#include "furi/check.h"
#include "furi/common_defines.h"
#include "sys/_stdint.h"
#include "infrared_worker.h"
#include <infrared.h>
#include <furi_hal_infrared.h>
#include <limits.h>
#include <stdint.h>
#include <furi.h>

#include <notification/notification_messages.h>
#include <stream_buffer.h>

#define INFRARED_WORKER_RX_TIMEOUT INFRARED_RAW_RX_TIMING_DELAY_US

#define INFRARED_WORKER_RX_RECEIVED 0x01
#define INFRARED_WORKER_RX_TIMEOUT_RECEIVED 0x02
#define INFRARED_WORKER_OVERRUN 0x04
#define INFRARED_WORKER_EXIT 0x08
#define INFRARED_WORKER_TX_FILL_BUFFER 0x10
#define INFRARED_WORKER_TX_MESSAGE_SENT 0x20

#define INFRARED_WORKER_ALL_RX_EVENTS                                    \
    (INFRARED_WORKER_RX_RECEIVED | INFRARED_WORKER_RX_TIMEOUT_RECEIVED | \
     INFRARED_WORKER_OVERRUN | INFRARED_WORKER_EXIT)

#define INFRARED_WORKER_ALL_TX_EVENTS \
    (INFRARED_WORKER_TX_FILL_BUFFER | INFRARED_WORKER_TX_MESSAGE_SENT | INFRARED_WORKER_EXIT)

#define INFRARED_WORKER_ALL_EVENTS (INFRARED_WORKER_ALL_RX_EVENTS | INFRARED_WORKER_ALL_TX_EVENTS)

typedef enum {
    InfraredWorkerStateIdle,
    InfraredWorkerStateRunRx,
    InfraredWorkerStateRunTx,
    InfraredWorkerStateWaitTxEnd,
    InfraredWorkerStateStopTx,
    InfraredWorkerStateStartTx,
} InfraredWorkerState;

struct InfraredWorkerSignal {
    bool decoded;
    size_t timings_cnt;
    union {
        InfraredMessage message;
        /* +1 is for pause we add at the beginning */
        uint32_t timings[MAX_TIMINGS_AMOUNT + 1];
    };
};

struct InfraredWorker {
    FuriThread* thread;
    StreamBufferHandle_t stream;

    InfraredWorkerSignal signal;
    InfraredWorkerState state;
    InfraredEncoderHandler* infrared_encoder;
    InfraredDecoderHandler* infrared_decoder;
    NotificationApp* notification;
    bool blink_enable;

    union {
        struct {
            InfraredWorkerGetSignalCallback get_signal_callback;
            InfraredWorkerMessageSentCallback message_sent_callback;
            void* get_signal_context;
            void* message_sent_context;
            uint32_t frequency;
            float duty_cycle;
            uint32_t tx_raw_cnt;
            bool need_reinitialization;
            bool steady_signal_sent;
        } tx;
        struct {
            InfraredWorkerReceivedSignalCallback received_signal_callback;
            void* received_signal_context;
            bool overrun;
        } rx;
    };
};

typedef struct {
    uint32_t duration;
    bool level;
    FuriHalInfraredTxGetDataState state;
} InfraredWorkerTiming;

static int32_t infrared_worker_tx_thread(void* context);
static FuriHalInfraredTxGetDataState
    infrared_worker_furi_hal_data_isr_callback(void* context, uint32_t* duration, bool* level);
static void infrared_worker_furi_hal_message_sent_isr_callback(void* context);

static void infrared_worker_rx_timeout_callback(void* context) {
    InfraredWorker* instance = context;
    uint32_t flags_set = osThreadFlagsSet(
        furi_thread_get_thread_id(instance->thread), INFRARED_WORKER_RX_TIMEOUT_RECEIVED);
    furi_check(flags_set & INFRARED_WORKER_RX_TIMEOUT_RECEIVED);
}

static void infrared_worker_rx_callback(void* context, bool level, uint32_t duration) {
    InfraredWorker* instance = context;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    furi_assert(duration != 0);
    LevelDuration level_duration = level_duration_make(level, duration);

    size_t ret = xStreamBufferSendFromISR(
        instance->stream, &level_duration, sizeof(LevelDuration), &xHigherPriorityTaskWoken);
    uint32_t events = (ret == sizeof(LevelDuration)) ? INFRARED_WORKER_RX_RECEIVED :
                                                       INFRARED_WORKER_OVERRUN;
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);

    uint32_t flags_set = osThreadFlagsSet(furi_thread_get_thread_id(instance->thread), events);
    furi_check(flags_set & events);
}

static void infrared_worker_process_timeout(InfraredWorker* instance) {
    if(instance->signal.timings_cnt < 2) return;

    const InfraredMessage* message_decoded =
        infrared_check_decoder_ready(instance->infrared_decoder);
    if(message_decoded) {
        instance->signal.message = *message_decoded;
        instance->signal.timings_cnt = 0;
        instance->signal.decoded = true;
    } else {
        instance->signal.decoded = false;
    }
    if(instance->rx.received_signal_callback)
        instance->rx.received_signal_callback(
            instance->rx.received_signal_context, &instance->signal);
}

static void
    infrared_worker_process_timings(InfraredWorker* instance, uint32_t duration, bool level) {
    const InfraredMessage* message_decoded =
        infrared_decode(instance->infrared_decoder, level, duration);
    if(message_decoded) {
        instance->signal.message = *message_decoded;
        instance->signal.timings_cnt = 0;
        instance->signal.decoded = true;
        if(instance->rx.received_signal_callback)
            instance->rx.received_signal_callback(
                instance->rx.received_signal_context, &instance->signal);
    } else {
        /* Skip first timing if it starts from Space */
        if((instance->signal.timings_cnt == 0) && !level) {
            return;
        }

        if(instance->signal.timings_cnt < MAX_TIMINGS_AMOUNT) {
            instance->signal.timings[instance->signal.timings_cnt] = duration;
            ++instance->signal.timings_cnt;
        } else {
            uint32_t flags_set = osThreadFlagsSet(
                furi_thread_get_thread_id(instance->thread), INFRARED_WORKER_OVERRUN);
            furi_check(flags_set & INFRARED_WORKER_OVERRUN);
            instance->rx.overrun = true;
        }
    }
}

static int32_t infrared_worker_rx_thread(void* thread_context) {
    InfraredWorker* instance = thread_context;
    uint32_t events = 0;
    LevelDuration level_duration;
    TickType_t last_blink_time = 0;

    while(1) {
        events = osThreadFlagsWait(INFRARED_WORKER_ALL_RX_EVENTS, 0, osWaitForever);
        furi_check(events & INFRARED_WORKER_ALL_RX_EVENTS); /* at least one caught */

        if(events & INFRARED_WORKER_RX_RECEIVED) {
            if(!instance->rx.overrun && instance->blink_enable &&
               ((xTaskGetTickCount() - last_blink_time) > 80)) {
                last_blink_time = xTaskGetTickCount();
                notification_message(instance->notification, &sequence_blink_blue_10);
            }
            if(instance->signal.timings_cnt == 0)
                notification_message(instance->notification, &sequence_display_on);
            while(sizeof(LevelDuration) ==
                  xStreamBufferReceive(
                      instance->stream, &level_duration, sizeof(LevelDuration), 0)) {
                if(!instance->rx.overrun) {
                    bool level = level_duration_get_level(level_duration);
                    uint32_t duration = level_duration_get_duration(level_duration);
                    infrared_worker_process_timings(instance, duration, level);
                }
            }
        }
        if(events & INFRARED_WORKER_OVERRUN) {
            printf("#");
            infrared_reset_decoder(instance->infrared_decoder);
            instance->signal.timings_cnt = 0;
            if(instance->blink_enable)
                notification_message(instance->notification, &sequence_set_red_255);
        }
        if(events & INFRARED_WORKER_RX_TIMEOUT_RECEIVED) {
            if(instance->rx.overrun) {
                printf("\nOVERRUN, max samples: %d\n", MAX_TIMINGS_AMOUNT);
                instance->rx.overrun = false;
                if(instance->blink_enable)
                    notification_message(instance->notification, &sequence_reset_red);
            } else {
                infrared_worker_process_timeout(instance);
            }
            instance->signal.timings_cnt = 0;
        }
        if(events & INFRARED_WORKER_EXIT) break;
    }

    return 0;
}

void infrared_worker_rx_set_received_signal_callback(
    InfraredWorker* instance,
    InfraredWorkerReceivedSignalCallback callback,
    void* context) {
    furi_assert(instance);
    instance->rx.received_signal_callback = callback;
    instance->rx.received_signal_context = context;
}

InfraredWorker* infrared_worker_alloc() {
    InfraredWorker* instance = malloc(sizeof(InfraredWorker));

    instance->thread = furi_thread_alloc();
    furi_thread_set_name(instance->thread, "InfraredWorker");
    furi_thread_set_stack_size(instance->thread, 2048);
    furi_thread_set_context(instance->thread, instance);

    size_t buffer_size =
        MAX(sizeof(InfraredWorkerTiming) * (MAX_TIMINGS_AMOUNT + 1),
            sizeof(LevelDuration) * MAX_TIMINGS_AMOUNT);
    instance->stream = xStreamBufferCreate(buffer_size, sizeof(InfraredWorkerTiming));
    instance->infrared_decoder = infrared_alloc_decoder();
    instance->infrared_encoder = infrared_alloc_encoder();
    instance->blink_enable = false;
    instance->notification = furi_record_open("notification");
    instance->state = InfraredWorkerStateIdle;

    return instance;
}

void infrared_worker_free(InfraredWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->state == InfraredWorkerStateIdle);

    furi_record_close("notification");
    infrared_free_decoder(instance->infrared_decoder);
    infrared_free_encoder(instance->infrared_encoder);
    vStreamBufferDelete(instance->stream);
    furi_thread_free(instance->thread);

    free(instance);
}

void infrared_worker_rx_start(InfraredWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->state == InfraredWorkerStateIdle);

    xStreamBufferSetTriggerLevel(instance->stream, sizeof(LevelDuration));

    furi_thread_set_callback(instance->thread, infrared_worker_rx_thread);
    furi_thread_start(instance->thread);

    furi_hal_infrared_async_rx_set_capture_isr_callback(infrared_worker_rx_callback, instance);
    furi_hal_infrared_async_rx_set_timeout_isr_callback(
        infrared_worker_rx_timeout_callback, instance);
    furi_hal_infrared_async_rx_start();
    furi_hal_infrared_async_rx_set_timeout(INFRARED_WORKER_RX_TIMEOUT);

    instance->rx.overrun = false;
    instance->state = InfraredWorkerStateRunRx;
}

void infrared_worker_rx_stop(InfraredWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->state == InfraredWorkerStateRunRx);

    furi_hal_infrared_async_rx_set_timeout_isr_callback(NULL, NULL);
    furi_hal_infrared_async_rx_set_capture_isr_callback(NULL, NULL);
    furi_hal_infrared_async_rx_stop();

    osThreadFlagsSet(furi_thread_get_thread_id(instance->thread), INFRARED_WORKER_EXIT);
    furi_thread_join(instance->thread);

    BaseType_t xReturn = xStreamBufferReset(instance->stream);
    furi_assert(xReturn == pdPASS);
    (void)xReturn;

    instance->state = InfraredWorkerStateIdle;
}

bool infrared_worker_signal_is_decoded(const InfraredWorkerSignal* signal) {
    furi_assert(signal);
    return signal->decoded;
}

void infrared_worker_get_raw_signal(
    const InfraredWorkerSignal* signal,
    const uint32_t** timings,
    size_t* timings_cnt) {
    furi_assert(signal);
    furi_assert(timings);
    furi_assert(timings_cnt);

    *timings = signal->timings;
    *timings_cnt = signal->timings_cnt;
}

const InfraredMessage* infrared_worker_get_decoded_signal(const InfraredWorkerSignal* signal) {
    furi_assert(signal);
    return &signal->message;
}

void infrared_worker_rx_enable_blink_on_receiving(InfraredWorker* instance, bool enable) {
    furi_assert(instance);
    instance->blink_enable = enable;
}

void infrared_worker_tx_start(InfraredWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->state == InfraredWorkerStateIdle);
    furi_assert(instance->tx.get_signal_callback);

    // size have to be greater than api hal infrared async tx buffer size
    xStreamBufferSetTriggerLevel(instance->stream, sizeof(InfraredWorkerTiming));

    furi_thread_set_callback(instance->thread, infrared_worker_tx_thread);

    instance->tx.steady_signal_sent = false;
    instance->tx.need_reinitialization = false;
    furi_hal_infrared_async_tx_set_data_isr_callback(
        infrared_worker_furi_hal_data_isr_callback, instance);
    furi_hal_infrared_async_tx_set_signal_sent_isr_callback(
        infrared_worker_furi_hal_message_sent_isr_callback, instance);

    instance->state = InfraredWorkerStateStartTx;
    furi_thread_start(instance->thread);
}

static void infrared_worker_furi_hal_message_sent_isr_callback(void* context) {
    InfraredWorker* instance = context;
    uint32_t flags_set = osThreadFlagsSet(
        furi_thread_get_thread_id(instance->thread), INFRARED_WORKER_TX_MESSAGE_SENT);
    furi_check(flags_set & INFRARED_WORKER_TX_MESSAGE_SENT);
}

static FuriHalInfraredTxGetDataState
    infrared_worker_furi_hal_data_isr_callback(void* context, uint32_t* duration, bool* level) {
    furi_assert(context);
    furi_assert(duration);
    furi_assert(level);

    InfraredWorker* instance = context;
    InfraredWorkerTiming timing;
    FuriHalInfraredTxGetDataState state;

    if(sizeof(InfraredWorkerTiming) ==
       xStreamBufferReceiveFromISR(instance->stream, &timing, sizeof(InfraredWorkerTiming), 0)) {
        *level = timing.level;
        *duration = timing.duration;
        state = timing.state;
    } else {
        furi_assert(0);
        *level = 0;
        *duration = 100;
        state = FuriHalInfraredTxGetDataStateDone;
    }

    uint32_t flags_set = osThreadFlagsSet(
        furi_thread_get_thread_id(instance->thread), INFRARED_WORKER_TX_FILL_BUFFER);
    furi_check(flags_set & INFRARED_WORKER_TX_FILL_BUFFER);

    return state;
}

static bool infrared_get_new_signal(InfraredWorker* instance) {
    bool new_signal_obtained = false;

    InfraredWorkerGetSignalResponse response =
        instance->tx.get_signal_callback(instance->tx.get_signal_context, instance);
    if(response == InfraredWorkerGetSignalResponseNew) {
        uint32_t new_tx_frequency = 0;
        float new_tx_duty_cycle = 0;
        if(instance->signal.decoded) {
            new_tx_frequency = infrared_get_protocol_frequency(instance->signal.message.protocol);
            new_tx_duty_cycle =
                infrared_get_protocol_duty_cycle(instance->signal.message.protocol);
        } else {
            furi_assert(instance->signal.timings_cnt > 1);
            new_tx_frequency = INFRARED_COMMON_CARRIER_FREQUENCY;
            new_tx_duty_cycle = INFRARED_COMMON_DUTY_CYCLE;
        }

        instance->tx.tx_raw_cnt = 0;
        instance->tx.need_reinitialization = (new_tx_frequency != instance->tx.frequency) ||
                                             (new_tx_duty_cycle != instance->tx.duty_cycle);
        instance->tx.frequency = new_tx_frequency;
        instance->tx.duty_cycle = new_tx_duty_cycle;
        if(instance->signal.decoded) {
            infrared_reset_encoder(instance->infrared_encoder, &instance->signal.message);
        }
        new_signal_obtained = true;
    } else if(response == InfraredWorkerGetSignalResponseSame) {
        new_signal_obtained = true;
        /* no need to reinit */
    } else if(response == InfraredWorkerGetSignalResponseStop) {
        new_signal_obtained = false;
    } else {
        furi_assert(0);
    }

    return new_signal_obtained;
}

static bool infrared_worker_tx_fill_buffer(InfraredWorker* instance) {
    bool new_data_available = true;
    InfraredWorkerTiming timing;
    InfraredStatus status = InfraredStatusError;

    while(!xStreamBufferIsFull(instance->stream) && !instance->tx.need_reinitialization &&
          new_data_available) {
        if(instance->signal.decoded) {
            status = infrared_encode(instance->infrared_encoder, &timing.duration, &timing.level);
        } else {
            timing.duration = instance->signal.timings[instance->tx.tx_raw_cnt];
            /* raw always starts from Mark, but we fill it with space delay at start */
            timing.level = (instance->tx.tx_raw_cnt % 2);
            ++instance->tx.tx_raw_cnt;
            if(instance->tx.tx_raw_cnt >= instance->signal.timings_cnt) {
                instance->tx.tx_raw_cnt = 0;
                status = InfraredStatusDone;
            } else {
                status = InfraredStatusOk;
            }
        }

        if(status == InfraredStatusError) {
            furi_assert(0);
            new_data_available = false;
            break;
        } else if(status == InfraredStatusOk) {
            timing.state = FuriHalInfraredTxGetDataStateOk;
        } else if(status == InfraredStatusDone) {
            timing.state = FuriHalInfraredTxGetDataStateDone;

            new_data_available = infrared_get_new_signal(instance);
            if(instance->tx.need_reinitialization || !new_data_available) {
                timing.state = FuriHalInfraredTxGetDataStateLastDone;
            }
        } else {
            furi_assert(0);
        }
        uint32_t written_size =
            xStreamBufferSend(instance->stream, &timing, sizeof(InfraredWorkerTiming), 0);
        furi_assert(sizeof(InfraredWorkerTiming) == written_size);
        (void)written_size;
    }

    return new_data_available;
}

static int32_t infrared_worker_tx_thread(void* thread_context) {
    InfraredWorker* instance = thread_context;
    furi_assert(instance->state == InfraredWorkerStateStartTx);
    furi_assert(thread_context);

    uint32_t events = 0;
    bool new_data_available = true;
    bool exit = false;

    exit = !infrared_get_new_signal(instance);
    furi_assert(!exit);

    while(!exit) {
        switch(instance->state) {
        case InfraredWorkerStateStartTx:
            instance->tx.need_reinitialization = false;
            new_data_available = infrared_worker_tx_fill_buffer(instance);
            furi_hal_infrared_async_tx_start(instance->tx.frequency, instance->tx.duty_cycle);

            if(!new_data_available) {
                instance->state = InfraredWorkerStateStopTx;
            } else if(instance->tx.need_reinitialization) {
                instance->state = InfraredWorkerStateWaitTxEnd;
            } else {
                instance->state = InfraredWorkerStateRunTx;
            }

            break;
        case InfraredWorkerStateStopTx:
            furi_hal_infrared_async_tx_stop();
            exit = true;
            break;
        case InfraredWorkerStateWaitTxEnd:
            furi_hal_infrared_async_tx_wait_termination();
            instance->state = InfraredWorkerStateStartTx;

            events = osThreadFlagsGet();
            if(events & INFRARED_WORKER_EXIT) {
                exit = true;
                break;
            }

            break;
        case InfraredWorkerStateRunTx:
            events = osThreadFlagsWait(INFRARED_WORKER_ALL_TX_EVENTS, 0, osWaitForever);
            furi_check(events & INFRARED_WORKER_ALL_TX_EVENTS); /* at least one caught */

            if(events & INFRARED_WORKER_EXIT) {
                instance->state = InfraredWorkerStateStopTx;
                break;
            }

            if(events & INFRARED_WORKER_TX_FILL_BUFFER) {
                infrared_worker_tx_fill_buffer(instance);

                if(instance->tx.need_reinitialization) {
                    instance->state = InfraredWorkerStateWaitTxEnd;
                }
            }

            if(events & INFRARED_WORKER_TX_MESSAGE_SENT) {
                if(instance->tx.message_sent_callback)
                    instance->tx.message_sent_callback(instance->tx.message_sent_context);
            }
            break;
        default:
            furi_assert(0);
            break;
        }
    }

    return 0;
}

void infrared_worker_tx_set_get_signal_callback(
    InfraredWorker* instance,
    InfraredWorkerGetSignalCallback callback,
    void* context) {
    furi_assert(instance);
    instance->tx.get_signal_callback = callback;
    instance->tx.get_signal_context = context;
}

void infrared_worker_tx_set_signal_sent_callback(
    InfraredWorker* instance,
    InfraredWorkerMessageSentCallback callback,
    void* context) {
    furi_assert(instance);
    instance->tx.message_sent_callback = callback;
    instance->tx.message_sent_context = context;
}

void infrared_worker_tx_stop(InfraredWorker* instance) {
    furi_assert(instance);
    furi_assert(instance->state != InfraredWorkerStateRunRx);

    osThreadFlagsSet(furi_thread_get_thread_id(instance->thread), INFRARED_WORKER_EXIT);
    furi_thread_join(instance->thread);
    furi_hal_infrared_async_tx_set_data_isr_callback(NULL, NULL);
    furi_hal_infrared_async_tx_set_signal_sent_isr_callback(NULL, NULL);

    instance->signal.timings_cnt = 0;
    BaseType_t xReturn = pdFAIL;
    xReturn = xStreamBufferReset(instance->stream);
    furi_assert(xReturn == pdPASS);
    (void)xReturn;
    instance->state = InfraredWorkerStateIdle;
}

void infrared_worker_set_decoded_signal(InfraredWorker* instance, const InfraredMessage* message) {
    furi_assert(instance);
    furi_assert(message);

    instance->signal.decoded = true;
    instance->signal.message = *message;
}

void infrared_worker_set_raw_signal(
    InfraredWorker* instance,
    const uint32_t* timings,
    size_t timings_cnt) {
    furi_assert(instance);
    furi_assert(timings);
    furi_assert(timings_cnt > 0);
    size_t max_copy_num = COUNT_OF(instance->signal.timings) - 1;
    furi_check(timings_cnt <= max_copy_num);

    instance->signal.timings[0] = INFRARED_RAW_TX_TIMING_DELAY_US;
    memcpy(&instance->signal.timings[1], timings, timings_cnt * sizeof(uint32_t));
    instance->signal.decoded = false;
    instance->signal.timings_cnt = timings_cnt + 1;
}

InfraredWorkerGetSignalResponse
    infrared_worker_tx_get_signal_steady_callback(void* context, InfraredWorker* instance) {
    InfraredWorkerGetSignalResponse response = instance->tx.steady_signal_sent ?
                                                   InfraredWorkerGetSignalResponseSame :
                                                   InfraredWorkerGetSignalResponseNew;
    instance->tx.steady_signal_sent = true;
    return response;
}
