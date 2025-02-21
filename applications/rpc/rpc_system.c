#include <flipper.pb.h>
#include <furi_hal.h>
#include <power/power_service/power.h>
#include <notification/notification_messages.h>
#include <protobuf_version.h>

#include "rpc_i.h"

typedef struct {
    RpcSession* session;
    PB_Main* response;
} RpcSystemContext;

static void rpc_system_system_ping_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_ping_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    if(request->has_next) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    PB_Main response = PB_Main_init_default;
    response.has_next = false;
    response.command_status = PB_CommandStatus_OK;
    response.command_id = request->command_id;
    response.which_content = PB_Main_system_ping_response_tag;

    const PB_System_PingRequest* ping_request = &request->content.system_ping_request;
    PB_System_PingResponse* ping_response = &response.content.system_ping_response;
    if(ping_request->data && (ping_request->data->size > 0)) {
        ping_response->data = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(ping_request->data->size));
        memcpy(ping_response->data->bytes, ping_request->data->bytes, ping_request->data->size);
        ping_response->data->size = ping_request->data->size;
    }

    rpc_send_and_release(session, &response);
}

static void rpc_system_system_reboot_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_reboot_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    const int mode = request->content.system_reboot_request.mode;

    if(mode == PB_System_RebootRequest_RebootMode_OS) {
        power_reboot(PowerBootModeNormal);
    } else if(mode == PB_System_RebootRequest_RebootMode_DFU) {
        power_reboot(PowerBootModeDfu);
    } else {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
    }
}

static void rpc_system_system_device_info_callback(
    const char* key,
    const char* value,
    bool last,
    void* context) {
    furi_assert(key);
    furi_assert(value);
    RpcSystemContext* ctx = context;

    char* str_key = strdup(key);
    char* str_value = strdup(value);

    ctx->response->has_next = !last;
    ctx->response->content.system_device_info_response.key = str_key;
    ctx->response->content.system_device_info_response.value = str_value;

    rpc_send_and_release(ctx->session, ctx->response);
}

static void rpc_system_system_device_info_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_device_info_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_system_device_info_response_tag;
    response->command_status = PB_CommandStatus_OK;

    RpcSystemContext device_info_context = {
        .session = session,
        .response = response,
    };
    furi_hal_info_get(rpc_system_system_device_info_callback, &device_info_context);

    free(response);
}

static void rpc_system_system_get_datetime_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_get_datetime_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    FuriHalRtcDateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_system_get_datetime_response_tag;
    response->command_status = PB_CommandStatus_OK;
    response->content.system_get_datetime_response.has_datetime = true;
    response->content.system_get_datetime_response.datetime.hour = datetime.hour;
    response->content.system_get_datetime_response.datetime.minute = datetime.minute;
    response->content.system_get_datetime_response.datetime.second = datetime.second;
    response->content.system_get_datetime_response.datetime.day = datetime.day;
    response->content.system_get_datetime_response.datetime.month = datetime.month;
    response->content.system_get_datetime_response.datetime.year = datetime.year;
    response->content.system_get_datetime_response.datetime.weekday = datetime.weekday;

    rpc_send_and_release(session, response);
    free(response);
}

static void rpc_system_system_set_datetime_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_set_datetime_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    if(!request->content.system_set_datetime_request.has_datetime) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    FuriHalRtcDateTime datetime;
    datetime.hour = request->content.system_set_datetime_request.datetime.hour;
    datetime.minute = request->content.system_set_datetime_request.datetime.minute;
    datetime.second = request->content.system_set_datetime_request.datetime.second;
    datetime.day = request->content.system_set_datetime_request.datetime.day;
    datetime.month = request->content.system_set_datetime_request.datetime.month;
    datetime.year = request->content.system_set_datetime_request.datetime.year;
    datetime.weekday = request->content.system_set_datetime_request.datetime.weekday;
    furi_hal_rtc_set_datetime(&datetime);

    rpc_send_and_release_empty(session, request->command_id, PB_CommandStatus_OK);
}

static void rpc_system_system_factory_reset_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_factory_reset_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    furi_hal_rtc_set_flag(FuriHalRtcFlagFactoryReset);
    power_reboot(PowerBootModeNormal);

    (void)session;
}

static void
    rpc_system_system_play_audiovisual_alert_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_play_audiovisual_alert_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    NotificationApp* notification = furi_record_open("notification");
    notification_message(notification, &sequence_audiovisual_alert);
    furi_record_close("notification");

    rpc_send_and_release_empty(session, request->command_id, PB_CommandStatus_OK);
}

static void rpc_system_system_protobuf_version_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_protobuf_version_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->has_next = false;
    response->command_status = PB_CommandStatus_OK;
    response->which_content = PB_Main_system_protobuf_version_response_tag;
    /* build error here means something wrong with tags in
     * local repo https://github.com/flipperdevices/flipperzero-protobuf */
    response->content.system_protobuf_version_response.major = PROTOBUF_MAJOR_VERSION;
    response->content.system_protobuf_version_response.minor = PROTOBUF_MINOR_VERSION;

    rpc_send_and_release(session, response);
    free(response);
}

static void rpc_system_system_power_info_callback(
    const char* key,
    const char* value,
    bool last,
    void* context) {
    furi_assert(key);
    furi_assert(value);
    RpcSystemContext* ctx = context;

    char* str_key = strdup(key);
    char* str_value = strdup(value);

    ctx->response->has_next = !last;
    ctx->response->content.system_device_info_response.key = str_key;
    ctx->response->content.system_device_info_response.value = str_value;

    rpc_send_and_release(ctx->session, ctx->response);
}

static void rpc_system_system_get_power_info_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_power_info_request_tag);

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_system_power_info_response_tag;
    response->command_status = PB_CommandStatus_OK;

    RpcSystemContext power_info_context = {
        .session = session,
        .response = response,
    };
    furi_hal_power_info_get(rpc_system_system_power_info_callback, &power_info_context);

    free(response);
}

void* rpc_system_system_alloc(RpcSession* session) {
    RpcHandler rpc_handler = {
        .message_handler = NULL,
        .decode_submessage = NULL,
        .context = session,
    };

    rpc_handler.message_handler = rpc_system_system_ping_process;
    rpc_add_handler(session, PB_Main_system_ping_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_reboot_process;
    rpc_add_handler(session, PB_Main_system_reboot_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_device_info_process;
    rpc_add_handler(session, PB_Main_system_device_info_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_factory_reset_process;
    rpc_add_handler(session, PB_Main_system_factory_reset_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_get_datetime_process;
    rpc_add_handler(session, PB_Main_system_get_datetime_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_set_datetime_process;
    rpc_add_handler(session, PB_Main_system_set_datetime_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_play_audiovisual_alert_process;
    rpc_add_handler(session, PB_Main_system_play_audiovisual_alert_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_protobuf_version_process;
    rpc_add_handler(session, PB_Main_system_protobuf_version_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_get_power_info_process;
    rpc_add_handler(session, PB_Main_system_power_info_request_tag, &rpc_handler);

    return NULL;
}
