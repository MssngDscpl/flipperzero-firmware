#include "gap.h"

#include "ble.h"

#include <furi_hal.h>
#include <furi.h>

#define TAG "BtGap"

#define FAST_ADV_TIMEOUT 30000
#define INITIAL_ADV_TIMEOUT 60000

#define GAP_INTERVAL_TO_MS(x) (uint16_t)((x)*1.25)

typedef struct {
    uint16_t gap_svc_handle;
    uint16_t dev_name_char_handle;
    uint16_t appearance_char_handle;
    uint16_t connection_handle;
    uint8_t adv_svc_uuid_len;
    uint8_t adv_svc_uuid[20];
    char* adv_name;
} GapSvc;

typedef struct {
    GapSvc service;
    GapConfig* config;
    GapConnectionParams connection_params;
    GapState state;
    osMutexId_t state_mutex;
    GapEventCallback on_event_cb;
    void* context;
    osTimerId_t advertise_timer;
    FuriThread* thread;
    osMessageQueueId_t command_queue;
    bool enable_adv;
} Gap;

typedef enum {
    GapCommandAdvFast,
    GapCommandAdvLowPower,
    GapCommandAdvStop,
    GapCommandKillThread,
} GapCommand;

typedef struct {
    GapScanCallback callback;
    void* context;
} GapScan;

// Identity root key
static const uint8_t gap_irk[16] =
    {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
// Encryption root key
static const uint8_t gap_erk[16] =
    {0xfe, 0xdc, 0xba, 0x09, 0x87, 0x65, 0x43, 0x21, 0xfe, 0xdc, 0xba, 0x09, 0x87, 0x65, 0x43, 0x21};

static Gap* gap = NULL;
static GapScan* gap_scan = NULL;

static void gap_advertise_start(GapState new_state);
static int32_t gap_app(void* context);

static void gap_verify_connection_parameters(Gap* gap) {
    furi_assert(gap);

    FURI_LOG_I(
        TAG,
        "Connection parameters: Connection Interval: %d (%d ms), Slave Latency: %d, Supervision Timeout: %d",
        gap->connection_params.conn_interval,
        GAP_INTERVAL_TO_MS(gap->connection_params.conn_interval),
        gap->connection_params.slave_latency,
        gap->connection_params.supervisor_timeout);

    // Send connection parameters request update if necessary
    GapConnectionParamsRequest* params = &gap->config->conn_param;
    if(params->conn_int_min > gap->connection_params.conn_interval ||
       params->conn_int_max < gap->connection_params.conn_interval) {
        FURI_LOG_W(TAG, "Unsupported connection interval. Request connection parameters update");
        if(aci_l2cap_connection_parameter_update_req(
               gap->service.connection_handle,
               params->conn_int_min,
               params->conn_int_max,
               gap->connection_params.slave_latency,
               gap->connection_params.supervisor_timeout)) {
            FURI_LOG_E(TAG, "Failed to request connection parameters update");
        }
    }
}

SVCCTL_UserEvtFlowStatus_t SVCCTL_App_Notification(void* pckt) {
    hci_event_pckt* event_pckt;
    evt_le_meta_event* meta_evt;
    evt_blue_aci* blue_evt;
    hci_le_phy_update_complete_event_rp0* evt_le_phy_update_complete;
    uint8_t tx_phy;
    uint8_t rx_phy;
    tBleStatus ret = BLE_STATUS_INVALID_PARAMS;

    event_pckt = (hci_event_pckt*)((hci_uart_pckt*)pckt)->data;

    if(gap) {
        osMutexAcquire(gap->state_mutex, osWaitForever);
    }
    switch(event_pckt->evt) {
    case EVT_DISCONN_COMPLETE: {
        hci_disconnection_complete_event_rp0* disconnection_complete_event =
            (hci_disconnection_complete_event_rp0*)event_pckt->data;
        if(disconnection_complete_event->Connection_Handle == gap->service.connection_handle) {
            gap->service.connection_handle = 0;
            gap->state = GapStateIdle;
            FURI_LOG_I(
                TAG, "Disconnect from client. Reason: %02X", disconnection_complete_event->Reason);
        }
        if(gap->enable_adv) {
            // Restart advertising
            gap_advertise_start(GapStateAdvFast);
            furi_hal_power_insomnia_exit();
        }
        GapEvent event = {.type = GapEventTypeDisconnected};
        gap->on_event_cb(event, gap->context);
    } break;

    case EVT_LE_META_EVENT:
        meta_evt = (evt_le_meta_event*)event_pckt->data;
        switch(meta_evt->subevent) {
        case EVT_LE_CONN_UPDATE_COMPLETE: {
            hci_le_connection_update_complete_event_rp0* event =
                (hci_le_connection_update_complete_event_rp0*)meta_evt->data;
            gap->connection_params.conn_interval = event->Conn_Interval;
            gap->connection_params.slave_latency = event->Conn_Latency;
            gap->connection_params.supervisor_timeout = event->Supervision_Timeout;
            FURI_LOG_I(TAG, "Connection parameters event complete");
            gap_verify_connection_parameters(gap);
            break;
        }

        case EVT_LE_PHY_UPDATE_COMPLETE:
            evt_le_phy_update_complete = (hci_le_phy_update_complete_event_rp0*)meta_evt->data;
            if(evt_le_phy_update_complete->Status) {
                FURI_LOG_E(
                    TAG, "Update PHY failed, status %d", evt_le_phy_update_complete->Status);
            } else {
                FURI_LOG_I(TAG, "Update PHY succeed");
            }
            ret = hci_le_read_phy(gap->service.connection_handle, &tx_phy, &rx_phy);
            if(ret) {
                FURI_LOG_E(TAG, "Read PHY failed, status: %d", ret);
            } else {
                FURI_LOG_I(TAG, "PHY Params TX = %d, RX = %d ", tx_phy, rx_phy);
            }
            break;

        case EVT_LE_CONN_COMPLETE:
            furi_hal_power_insomnia_enter();
            hci_le_connection_complete_event_rp0* event =
                (hci_le_connection_complete_event_rp0*)meta_evt->data;
            gap->connection_params.conn_interval = event->Conn_Interval;
            gap->connection_params.slave_latency = event->Conn_Latency;
            gap->connection_params.supervisor_timeout = event->Supervision_Timeout;

            // Stop advertising as connection completed
            osTimerStop(gap->advertise_timer);

            // Update connection status and handle
            gap->state = GapStateConnected;
            gap->service.connection_handle = event->Connection_Handle;

            gap_verify_connection_parameters(gap);
            // Start pairing by sending security request
            aci_gap_slave_security_req(event->Connection_Handle);
            break;

        case EVT_LE_ADVERTISING_REPORT: {
            if(gap_scan) {
                GapAddress address;
                hci_le_advertising_report_event_rp0* evt =
                    (hci_le_advertising_report_event_rp0*)meta_evt->data;
                for(uint8_t i = 0; i < evt->Num_Reports; i++) {
                    Advertising_Report_t* rep = &evt->Advertising_Report[i];
                    address.type = rep->Address_Type;
                    // Original MAC addres is in inverted order
                    for(uint8_t j = 0; j < sizeof(address.mac); j++) {
                        address.mac[j] = rep->Address[sizeof(address.mac) - j - 1];
                    }
                    gap_scan->callback(address, gap_scan->context);
                }
            }
        } break;

        default:
            break;
        }
        break;

    case EVT_VENDOR:
        blue_evt = (evt_blue_aci*)event_pckt->data;
        switch(blue_evt->ecode) {
            aci_gap_pairing_complete_event_rp0* pairing_complete;

        case EVT_BLUE_GAP_LIMITED_DISCOVERABLE:
            FURI_LOG_I(TAG, "Limited discoverable event");
            break;

        case EVT_BLUE_GAP_PASS_KEY_REQUEST: {
            // Generate random PIN code
            uint32_t pin = rand() % 999999;
            aci_gap_pass_key_resp(gap->service.connection_handle, pin);
            if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock)) {
                FURI_LOG_I(TAG, "Pass key request event. Pin: ******");
            } else {
                FURI_LOG_I(TAG, "Pass key request event. Pin: %06d", pin);
            }
            GapEvent event = {.type = GapEventTypePinCodeShow, .data.pin_code = pin};
            gap->on_event_cb(event, gap->context);
        } break;

        case EVT_BLUE_ATT_EXCHANGE_MTU_RESP: {
            aci_att_exchange_mtu_resp_event_rp0* pr = (void*)blue_evt->data;
            FURI_LOG_I(TAG, "Rx MTU size: %d", pr->Server_RX_MTU);
            // Set maximum packet size given header size is 3 bytes
            GapEvent event = {
                .type = GapEventTypeUpdateMTU, .data.max_packet_size = pr->Server_RX_MTU - 3};
            gap->on_event_cb(event, gap->context);
        } break;

        case EVT_BLUE_GAP_AUTHORIZATION_REQUEST:
            FURI_LOG_D(TAG, "Authorization request event");
            break;

        case EVT_BLUE_GAP_SLAVE_SECURITY_INITIATED:
            FURI_LOG_D(TAG, "Slave security initiated");
            break;

        case EVT_BLUE_GAP_BOND_LOST:
            FURI_LOG_D(TAG, "Bond lost event. Start rebonding");
            aci_gap_allow_rebond(gap->service.connection_handle);
            break;

        case EVT_BLUE_GAP_DEVICE_FOUND:
            FURI_LOG_D(TAG, "Device found event");
            break;

        case EVT_BLUE_GAP_ADDR_NOT_RESOLVED:
            FURI_LOG_D(TAG, "Address not resolved event");
            break;

        case EVT_BLUE_GAP_KEYPRESS_NOTIFICATION:
            FURI_LOG_D(TAG, "Key press notification event");
            break;

        case EVT_BLUE_GAP_NUMERIC_COMPARISON_VALUE: {
            uint32_t pin =
                ((aci_gap_numeric_comparison_value_event_rp0*)(blue_evt->data))->Numeric_Value;
            FURI_LOG_I(TAG, "Verify numeric comparison: %06d", pin);
            GapEvent event = {.type = GapEventTypePinCodeVerify, .data.pin_code = pin};
            bool result = gap->on_event_cb(event, gap->context);
            aci_gap_numeric_comparison_value_confirm_yesno(gap->service.connection_handle, result);
            break;
        }

        case EVT_BLUE_GAP_PAIRING_CMPLT:
            pairing_complete = (aci_gap_pairing_complete_event_rp0*)blue_evt->data;
            if(pairing_complete->Status) {
                FURI_LOG_E(
                    TAG,
                    "Pairing failed with status: %d. Terminating connection",
                    pairing_complete->Status);
                aci_gap_terminate(gap->service.connection_handle, 5);
            } else {
                FURI_LOG_I(TAG, "Pairing complete");
                GapEvent event = {.type = GapEventTypeConnected};
                gap->on_event_cb(event, gap->context);
            }
            break;

        case EVT_BLUE_GAP_PROCEDURE_COMPLETE:
            FURI_LOG_D(TAG, "Procedure complete event");
            break;

        case EVT_BLUE_L2CAP_CONNECTION_UPDATE_RESP: {
            uint16_t result =
                ((aci_l2cap_connection_update_resp_event_rp0*)(blue_evt->data))->Result;
            if(result == 0) {
                FURI_LOG_D(TAG, "Connection parameters accepted");
            } else if(result == 1) {
                FURI_LOG_D(TAG, "Connection parameters denied");
            }
            break;
        }
        }
    default:
        break;
    }
    if(gap) {
        osMutexRelease(gap->state_mutex);
    }
    return SVCCTL_UserEvtFlowEnable;
}

static void set_advertisment_service_uid(uint8_t* uid, uint8_t uid_len) {
    if(uid_len == 2) {
        gap->service.adv_svc_uuid[0] = AD_TYPE_16_BIT_SERV_UUID;
    } else if(uid_len == 4) {
        gap->service.adv_svc_uuid[0] = AD_TYPE_32_BIT_SERV_UUID;
    } else if(uid_len == 16) {
        gap->service.adv_svc_uuid[0] = AD_TYPE_128_BIT_SERV_UUID_CMPLT_LIST;
    }
    memcpy(&gap->service.adv_svc_uuid[gap->service.adv_svc_uuid_len], uid, uid_len);
    gap->service.adv_svc_uuid_len += uid_len;
}

static void gap_init_svc(Gap* gap) {
    tBleStatus status;
    uint32_t srd_bd_addr[2];

    // HCI Reset to synchronise BLE Stack
    hci_reset();
    // Configure mac address
    aci_hal_write_config_data(
        CONFIG_DATA_PUBADDR_OFFSET, CONFIG_DATA_PUBADDR_LEN, gap->config->mac_address);

    /* Static random Address
     * The two upper bits shall be set to 1
     * The lowest 32bits is read from the UDN to differentiate between devices
     * The RNG may be used to provide a random number on each power on
     */
    srd_bd_addr[1] = 0x0000ED6E;
    srd_bd_addr[0] = LL_FLASH_GetUDN();
    aci_hal_write_config_data(
        CONFIG_DATA_RANDOM_ADDRESS_OFFSET, CONFIG_DATA_RANDOM_ADDRESS_LEN, (uint8_t*)srd_bd_addr);
    // Set Identity root key used to derive LTK and CSRK
    aci_hal_write_config_data(CONFIG_DATA_IR_OFFSET, CONFIG_DATA_IR_LEN, (uint8_t*)gap_irk);
    // Set Encryption root key used to derive LTK and CSRK
    aci_hal_write_config_data(CONFIG_DATA_ER_OFFSET, CONFIG_DATA_ER_LEN, (uint8_t*)gap_erk);
    // Set TX Power to 0 dBm
    aci_hal_set_tx_power_level(1, 0x19);
    // Initialize GATT interface
    aci_gatt_init();
    // Initialize GAP interface
    // Skip fist symbol AD_TYPE_COMPLETE_LOCAL_NAME
    char* name = gap->service.adv_name + 1;
    aci_gap_init(
        GAP_PERIPHERAL_ROLE,
        0,
        strlen(name),
        &gap->service.gap_svc_handle,
        &gap->service.dev_name_char_handle,
        &gap->service.appearance_char_handle);

    // Set GAP characteristics
    status = aci_gatt_update_char_value(
        gap->service.gap_svc_handle,
        gap->service.dev_name_char_handle,
        0,
        strlen(name),
        (uint8_t*)name);
    if(status) {
        FURI_LOG_E(TAG, "Failed updating name characteristic: %d", status);
    }
    uint8_t gap_appearence_char_uuid[2] = {
        gap->config->appearance_char & 0xff, gap->config->appearance_char >> 8};
    status = aci_gatt_update_char_value(
        gap->service.gap_svc_handle,
        gap->service.appearance_char_handle,
        0,
        2,
        gap_appearence_char_uuid);
    if(status) {
        FURI_LOG_E(TAG, "Failed updating appearence characteristic: %d", status);
    }
    // Set default PHY
    hci_le_set_default_phy(ALL_PHYS_PREFERENCE, TX_2M_PREFERRED, RX_2M_PREFERRED);
    // Set I/O capability
    bool keypress_supported = false;
    if(gap->config->pairing_method == GapPairingPinCodeShow) {
        aci_gap_set_io_capability(IO_CAP_DISPLAY_ONLY);
    } else if(gap->config->pairing_method == GapPairingPinCodeVerifyYesNo) {
        aci_gap_set_io_capability(IO_CAP_DISPLAY_YES_NO);
        keypress_supported = true;
    }
    // Setup  authentication
    aci_gap_set_authentication_requirement(
        gap->config->bonding_mode,
        CFG_MITM_PROTECTION,
        CFG_SC_SUPPORT,
        keypress_supported,
        CFG_ENCRYPTION_KEY_SIZE_MIN,
        CFG_ENCRYPTION_KEY_SIZE_MAX,
        CFG_USED_FIXED_PIN,
        0,
        PUBLIC_ADDR);
    // Configure whitelist
    aci_gap_configure_whitelist();
}

static void gap_advertise_start(GapState new_state) {
    tBleStatus status;
    uint16_t min_interval;
    uint16_t max_interval;

    if(new_state == GapStateAdvFast) {
        min_interval = 0x80; // 80 ms
        max_interval = 0xa0; // 100 ms
    } else {
        min_interval = 0x0640; // 1 s
        max_interval = 0x0fa0; // 2.5 s
    }
    // Stop advertising timer
    osTimerStop(gap->advertise_timer);

    if((new_state == GapStateAdvLowPower) &&
       ((gap->state == GapStateAdvFast) || (gap->state == GapStateAdvLowPower))) {
        // Stop advertising
        status = aci_gap_set_non_discoverable();
        if(status) {
            FURI_LOG_E(TAG, "Stop Advertising Failed, result: %d", status);
        }
    }
    // Configure advertising
    status = aci_gap_set_discoverable(
        ADV_IND,
        min_interval,
        max_interval,
        PUBLIC_ADDR,
        0,
        strlen(gap->service.adv_name),
        (uint8_t*)gap->service.adv_name,
        gap->service.adv_svc_uuid_len,
        gap->service.adv_svc_uuid,
        0,
        0);
    if(status) {
        FURI_LOG_E(TAG, "Set discoverable err: %d", status);
    }
    gap->state = new_state;
    GapEvent event = {.type = GapEventTypeStartAdvertising};
    gap->on_event_cb(event, gap->context);
    osTimerStart(gap->advertise_timer, INITIAL_ADV_TIMEOUT);
}

static void gap_advertise_stop() {
    if(gap->state > GapStateIdle) {
        if(gap->state == GapStateConnected) {
            // Terminate connection
            aci_gap_terminate(gap->service.connection_handle, 0x13);
        }
        // Stop advertising
        osTimerStop(gap->advertise_timer);
        aci_gap_set_non_discoverable();
        gap->state = GapStateIdle;
    }
    GapEvent event = {.type = GapEventTypeStopAdvertising};
    gap->on_event_cb(event, gap->context);
}

void gap_start_advertising() {
    osMutexAcquire(gap->state_mutex, osWaitForever);
    if(gap->state == GapStateIdle) {
        gap->state = GapStateStartingAdv;
        FURI_LOG_I(TAG, "Start advertising");
        gap->enable_adv = true;
        GapCommand command = GapCommandAdvFast;
        furi_check(osMessageQueuePut(gap->command_queue, &command, 0, 0) == osOK);
    }
    osMutexRelease(gap->state_mutex);
}

void gap_stop_advertising() {
    osMutexAcquire(gap->state_mutex, osWaitForever);
    if(gap->state > GapStateIdle) {
        FURI_LOG_I(TAG, "Stop advertising");
        gap->enable_adv = false;
        GapCommand command = GapCommandAdvStop;
        furi_check(osMessageQueuePut(gap->command_queue, &command, 0, 0) == osOK);
    }
    osMutexRelease(gap->state_mutex);
}

static void gap_advetise_timer_callback(void* context) {
    GapCommand command = GapCommandAdvLowPower;
    furi_check(osMessageQueuePut(gap->command_queue, &command, 0, 0) == osOK);
}

bool gap_init(GapConfig* config, GapEventCallback on_event_cb, void* context) {
    if(!ble_glue_is_radio_stack_ready()) {
        return false;
    }

    gap = malloc(sizeof(Gap));
    gap->config = config;
    srand(DWT->CYCCNT);
    // Create advertising timer
    gap->advertise_timer = osTimerNew(gap_advetise_timer_callback, osTimerOnce, NULL, NULL);
    // Initialization of GATT & GAP layer
    gap->service.adv_name = config->adv_name;
    gap_init_svc(gap);
    // Initialization of the BLE Services
    SVCCTL_Init();
    // Initialization of the GAP state
    gap->state_mutex = osMutexNew(NULL);
    gap->state = GapStateIdle;
    gap->service.connection_handle = 0xFFFF;
    gap->enable_adv = true;

    // Thread configuration
    gap->thread = furi_thread_alloc();
    furi_thread_set_name(gap->thread, "BleGapDriver");
    furi_thread_set_stack_size(gap->thread, 1024);
    furi_thread_set_context(gap->thread, gap);
    furi_thread_set_callback(gap->thread, gap_app);
    furi_thread_start(gap->thread);

    // Command queue allocation
    gap->command_queue = osMessageQueueNew(8, sizeof(GapCommand), NULL);

    uint8_t adv_service_uid[2];
    gap->service.adv_svc_uuid_len = 1;
    adv_service_uid[0] = gap->config->adv_service_uuid & 0xff;
    adv_service_uid[1] = gap->config->adv_service_uuid >> 8;
    set_advertisment_service_uid(adv_service_uid, sizeof(adv_service_uid));

    // Set callback
    gap->on_event_cb = on_event_cb;
    gap->context = context;
    return true;
}

GapState gap_get_state() {
    GapState state;
    if(gap) {
        osMutexAcquire(gap->state_mutex, osWaitForever);
        state = gap->state;
        osMutexRelease(gap->state_mutex);
    } else {
        state = GapStateUninitialized;
    }
    return state;
}

void gap_start_scan(GapScanCallback callback, void* context) {
    furi_assert(callback);
    gap_scan = malloc(sizeof(GapScan));
    gap_scan->callback = callback;
    gap_scan->context = context;
    // Scan interval 250 ms
    hci_le_set_scan_parameters(1, 4000, 200, 0, 0);
    hci_le_set_scan_enable(1, 1);
}

void gap_stop_scan() {
    furi_assert(gap_scan);
    hci_le_set_scan_enable(0, 1);
    free(gap_scan);
    gap_scan = NULL;
}

void gap_thread_stop() {
    if(gap) {
        osMutexAcquire(gap->state_mutex, osWaitForever);
        gap->enable_adv = false;
        GapCommand command = GapCommandKillThread;
        osMessageQueuePut(gap->command_queue, &command, 0, osWaitForever);
        osMutexRelease(gap->state_mutex);
        furi_thread_join(gap->thread);
        furi_thread_free(gap->thread);
        // Free resources
        osMutexDelete(gap->state_mutex);
        osMessageQueueDelete(gap->command_queue);
        osTimerStop(gap->advertise_timer);
        while(xTimerIsTimerActive(gap->advertise_timer) == pdTRUE) osDelay(1);
        furi_check(osTimerDelete(gap->advertise_timer) == osOK);
        free(gap);
        gap = NULL;
    }
}

static int32_t gap_app(void* context) {
    GapCommand command;
    while(1) {
        osStatus_t status = osMessageQueueGet(gap->command_queue, &command, NULL, osWaitForever);
        if(status != osOK) {
            FURI_LOG_E(TAG, "Message queue get error: %d", status);
            continue;
        }
        osMutexAcquire(gap->state_mutex, osWaitForever);
        if(command == GapCommandKillThread) {
            break;
        }
        if(command == GapCommandAdvFast) {
            gap_advertise_start(GapStateAdvFast);
        } else if(command == GapCommandAdvLowPower) {
            gap_advertise_start(GapStateAdvLowPower);
        } else if(command == GapCommandAdvStop) {
            gap_advertise_stop();
        }
        osMutexRelease(gap->state_mutex);
    }

    return 0;
}
