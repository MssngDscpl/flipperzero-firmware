#include <furi_hal_bt.h>
#include <ble.h>
#include <stm32wbxx.h>
#include <shci.h>
#include <cmsis_os2.h>

#include <furi_hal_version.h>
#include <furi_hal_bt_hid.h>
#include <furi_hal_bt_serial.h>
#include "battery_service.h"

#include <furi.h>

#define TAG "FuriHalBt"

#define FURI_HAL_BT_DEFAULT_MAC_ADDR \
    { 0x6c, 0x7a, 0xd8, 0xac, 0x57, 0x72 }

osMutexId_t furi_hal_bt_core2_mtx = NULL;
static FuriHalBtStack furi_hal_bt_stack = FuriHalBtStackUnknown;

typedef void (*FuriHalBtProfileStart)(void);
typedef void (*FuriHalBtProfileStop)(void);

typedef struct {
    FuriHalBtProfileStart start;
    FuriHalBtProfileStart stop;
    GapConfig config;
    uint16_t appearance_char;
    uint16_t advertise_service_uuid;
} FuriHalBtProfileConfig;

FuriHalBtProfileConfig profile_config[FuriHalBtProfileNumber] = {
    [FuriHalBtProfileSerial] =
        {
            .start = furi_hal_bt_serial_start,
            .stop = furi_hal_bt_serial_stop,
            .config =
                {
                    .adv_service_uuid = 0x3080,
                    .appearance_char = 0x8600,
                    .bonding_mode = true,
                    .pairing_method = GapPairingPinCodeShow,
                    .mac_address = FURI_HAL_BT_DEFAULT_MAC_ADDR,
                    .conn_param =
                        {
                            .conn_int_min = 0x18, // 30 ms
                            .conn_int_max = 0x24, // 45 ms
                            .slave_latency = 0,
                            .supervisor_timeout = 0,
                        },
                },
        },
    [FuriHalBtProfileHidKeyboard] =
        {
            .start = furi_hal_bt_hid_start,
            .stop = furi_hal_bt_hid_stop,
            .config =
                {
                    .adv_service_uuid = HUMAN_INTERFACE_DEVICE_SERVICE_UUID,
                    .appearance_char = GAP_APPEARANCE_KEYBOARD,
                    .bonding_mode = true,
                    .pairing_method = GapPairingPinCodeVerifyYesNo,
                    .mac_address = FURI_HAL_BT_DEFAULT_MAC_ADDR,
                    .conn_param =
                        {
                            .conn_int_min = 0x18, // 30 ms
                            .conn_int_max = 0x24, // 45 ms
                            .slave_latency = 0,
                            .supervisor_timeout = 0,
                        },
                },
        },
};
FuriHalBtProfileConfig* current_profile = NULL;

void furi_hal_bt_init() {
    if(!furi_hal_bt_core2_mtx) {
        furi_hal_bt_core2_mtx = osMutexNew(NULL);
        furi_assert(furi_hal_bt_core2_mtx);
    }

    // Explicitly tell that we are in charge of CLK48 domain
    if(!LL_HSEM_IsSemaphoreLocked(HSEM, CFG_HW_CLK48_CONFIG_SEMID)) {
        furi_check(LL_HSEM_1StepLock(HSEM, CFG_HW_CLK48_CONFIG_SEMID) == 0);
    }

    // Start Core2
    ble_glue_init();
}

void furi_hal_bt_lock_core2() {
    furi_assert(furi_hal_bt_core2_mtx);
    furi_check(osMutexAcquire(furi_hal_bt_core2_mtx, osWaitForever) == osOK);
}

void furi_hal_bt_unlock_core2() {
    furi_assert(furi_hal_bt_core2_mtx);
    furi_check(osMutexRelease(furi_hal_bt_core2_mtx) == osOK);
}

static bool furi_hal_bt_radio_stack_is_supported(WirelessFwInfo_t* info) {
    bool supported = false;
    if(info->StackType == INFO_STACK_TYPE_BLE_HCI) {
        furi_hal_bt_stack = FuriHalBtStackHciLayer;
        supported = true;
    } else if(info->StackType == INFO_STACK_TYPE_BLE_LIGHT) {
        if(info->VersionMajor >= FURI_HAL_BT_STACK_VERSION_MAJOR &&
           info->VersionMinor >= FURI_HAL_BT_STACK_VERSION_MINOR) {
            furi_hal_bt_stack = FuriHalBtStackLight;
            supported = true;
        }
    } else {
        furi_hal_bt_stack = FuriHalBtStackUnknown;
    }
    return supported;
}

bool furi_hal_bt_start_radio_stack() {
    bool res = false;
    furi_assert(furi_hal_bt_core2_mtx);

    osMutexAcquire(furi_hal_bt_core2_mtx, osWaitForever);

    // Explicitly tell that we are in charge of CLK48 domain
    if(!LL_HSEM_IsSemaphoreLocked(HSEM, CFG_HW_CLK48_CONFIG_SEMID)) {
        furi_check(LL_HSEM_1StepLock(HSEM, CFG_HW_CLK48_CONFIG_SEMID) == 0);
    }

    do {
        // Wait until FUS is started or timeout
        WirelessFwInfo_t info = {};
        if(!ble_glue_wait_for_fus_start(&info)) {
            FURI_LOG_E(TAG, "FUS start failed");
            LL_C2_PWR_SetPowerMode(LL_PWR_MODE_SHUTDOWN);
            ble_glue_thread_stop();
            break;
        }
        // If FUS is running, start radio stack fw
        if(ble_glue_radio_stack_fw_launch_started()) {
            // If FUS is running do nothing and wait for system reset
            furi_crash("Waiting for FUS to launch radio stack firmware");
        }
        // Check weather we support radio stack
        if(!furi_hal_bt_radio_stack_is_supported(&info)) {
            FURI_LOG_E(TAG, "Unsupported radio stack");
            // Don't stop SHCI for crypto enclave support
            break;
        }
        // Starting radio stack
        if(!ble_glue_start()) {
            FURI_LOG_E(TAG, "Failed to start radio stack");
            LL_C2_PWR_SetPowerMode(LL_PWR_MODE_SHUTDOWN);
            ble_glue_thread_stop();
            ble_app_thread_stop();
            break;
        }
        res = true;
    } while(false);
    osMutexRelease(furi_hal_bt_core2_mtx);

    return res;
}

FuriHalBtStack furi_hal_bt_get_radio_stack() {
    return furi_hal_bt_stack;
}

bool furi_hal_bt_start_app(FuriHalBtProfile profile, GapEventCallback event_cb, void* context) {
    furi_assert(event_cb);
    furi_assert(profile < FuriHalBtProfileNumber);
    bool ret = false;

    do {
        if(!ble_glue_is_radio_stack_ready()) {
            FURI_LOG_E(TAG, "Can't start BLE App - radio stack did not start");
            break;
        }
        if(furi_hal_bt_stack != FuriHalBtStackLight) {
            FURI_LOG_E(TAG, "Can't start Ble App - unsupported radio stack");
            break;
        }
        // Set mac address
        memcpy(
            profile_config[profile].config.mac_address,
            furi_hal_version_get_ble_mac(),
            sizeof(profile_config[profile].config.mac_address));
        // Set advertise name
        strlcpy(
            profile_config[profile].config.adv_name,
            furi_hal_version_get_ble_local_device_name_ptr(),
            FURI_HAL_VERSION_DEVICE_NAME_LENGTH);
        // Configure GAP
        GapConfig* config = &profile_config[profile].config;
        if(profile == FuriHalBtProfileSerial) {
            config->adv_service_uuid |= furi_hal_version_get_hw_color();
        } else if(profile == FuriHalBtProfileHidKeyboard) {
            // Change MAC address for HID profile
            config->mac_address[2]++;
            // Change name Flipper -> Keynote
            const char* clicker_str = "Keynote";
            memcpy(&config->adv_name[1], clicker_str, strlen(clicker_str));
        }
        if(!gap_init(config, event_cb, context)) {
            gap_thread_stop();
            FURI_LOG_E(TAG, "Failed to init GAP");
            break;
        }
        // Start selected profile services
        if(furi_hal_bt_stack == FuriHalBtStackLight) {
            profile_config[profile].start();
        }
        ret = true;
    } while(false);
    current_profile = &profile_config[profile];

    return ret;
}

bool furi_hal_bt_change_app(FuriHalBtProfile profile, GapEventCallback event_cb, void* context) {
    furi_assert(event_cb);
    furi_assert(profile < FuriHalBtProfileNumber);
    bool ret = true;

    FURI_LOG_I(TAG, "Stop current profile services");
    current_profile->stop();
    FURI_LOG_I(TAG, "Disconnect and stop advertising");
    furi_hal_bt_stop_advertising();
    FURI_LOG_I(TAG, "Shutdow 2nd core");
    LL_C2_PWR_SetPowerMode(LL_PWR_MODE_SHUTDOWN);
    FURI_LOG_I(TAG, "Stop BLE related RTOS threads");
    ble_app_thread_stop();
    gap_thread_stop();
    FURI_LOG_I(TAG, "Reset SHCI");
    SHCI_C2_Reinit();
    osDelay(100);
    ble_glue_thread_stop();
    FURI_LOG_I(TAG, "Start BT initialization");
    furi_hal_bt_init();
    furi_hal_bt_start_radio_stack();
    ret = furi_hal_bt_start_app(profile, event_cb, context);
    if(ret) {
        current_profile = &profile_config[profile];
    }
    return ret;
}

bool furi_hal_bt_is_active() {
    return gap_get_state() > GapStateIdle;
}

void furi_hal_bt_start_advertising() {
    if(gap_get_state() == GapStateIdle) {
        gap_start_advertising();
    }
}

void furi_hal_bt_stop_advertising() {
    if(furi_hal_bt_is_active()) {
        gap_stop_advertising();
        while(furi_hal_bt_is_active()) {
            osDelay(1);
        }
    }
}

void furi_hal_bt_update_battery_level(uint8_t battery_level) {
    if(battery_svc_is_started()) {
        battery_svc_update_level(battery_level);
    }
}

void furi_hal_bt_get_key_storage_buff(uint8_t** key_buff_addr, uint16_t* key_buff_size) {
    ble_app_get_key_storage_buff(key_buff_addr, key_buff_size);
}

void furi_hal_bt_set_key_storage_change_callback(
    BleGlueKeyStorageChangedCallback callback,
    void* context) {
    furi_assert(callback);
    ble_glue_set_key_storage_changed_callback(callback, context);
}

void furi_hal_bt_nvm_sram_sem_acquire() {
    while(LL_HSEM_1StepLock(HSEM, CFG_HW_BLE_NVM_SRAM_SEMID)) {
        osThreadYield();
    }
}

void furi_hal_bt_nvm_sram_sem_release() {
    LL_HSEM_ReleaseLock(HSEM, CFG_HW_BLE_NVM_SRAM_SEMID, 0);
}

bool furi_hal_bt_clear_white_list() {
    furi_hal_bt_nvm_sram_sem_acquire();
    tBleStatus status = aci_gap_clear_security_db();
    if(status) {
        FURI_LOG_E(TAG, "Clear while list failed with status %d", status);
    }
    furi_hal_bt_nvm_sram_sem_release();
    return status != BLE_STATUS_SUCCESS;
}

void furi_hal_bt_dump_state(string_t buffer) {
    if(furi_hal_bt_is_alive()) {
        uint8_t HCI_Version;
        uint16_t HCI_Revision;
        uint8_t LMP_PAL_Version;
        uint16_t Manufacturer_Name;
        uint16_t LMP_PAL_Subversion;

        tBleStatus ret = hci_read_local_version_information(
            &HCI_Version, &HCI_Revision, &LMP_PAL_Version, &Manufacturer_Name, &LMP_PAL_Subversion);

        string_cat_printf(
            buffer,
            "Ret: %d, HCI_Version: %d, HCI_Revision: %d, LMP_PAL_Version: %d, Manufacturer_Name: %d, LMP_PAL_Subversion: %d",
            ret,
            HCI_Version,
            HCI_Revision,
            LMP_PAL_Version,
            Manufacturer_Name,
            LMP_PAL_Subversion);
    } else {
        string_cat_printf(buffer, "BLE not ready");
    }
}

bool furi_hal_bt_is_alive() {
    return ble_glue_is_alive();
}

void furi_hal_bt_start_tone_tx(uint8_t channel, uint8_t power) {
    aci_hal_set_tx_power_level(0, power);
    aci_hal_tone_start(channel, 0);
}

void furi_hal_bt_stop_tone_tx() {
    aci_hal_tone_stop();
}

void furi_hal_bt_start_packet_tx(uint8_t channel, uint8_t pattern, uint8_t datarate) {
    hci_le_enhanced_transmitter_test(channel, 0x25, pattern, datarate);
}

void furi_hal_bt_start_packet_rx(uint8_t channel, uint8_t datarate) {
    hci_le_enhanced_receiver_test(channel, datarate, 0);
}

uint16_t furi_hal_bt_stop_packet_test() {
    uint16_t num_of_packets = 0;
    hci_le_test_end(&num_of_packets);
    return num_of_packets;
}

void furi_hal_bt_start_rx(uint8_t channel) {
    aci_hal_rx_start(channel);
}

float furi_hal_bt_get_rssi() {
    float val;
    uint8_t rssi_raw[3];

    if(aci_hal_read_raw_rssi(rssi_raw) != BLE_STATUS_SUCCESS) {
        return 0.0f;
    }

    // Some ST magic with rssi
    uint8_t agc = rssi_raw[2] & 0xFF;
    int rssi = (((int)rssi_raw[1] << 8) & 0xFF00) + (rssi_raw[0] & 0xFF);
    if(rssi == 0 || agc > 11) {
        val = -127.0;
    } else {
        val = agc * 6.0f - 127.0f;
        while(rssi > 30) {
            val += 6.0;
            rssi >>= 1;
        }
        val += (417 * rssi + 18080) >> 10;
    }
    return val;
}

uint32_t furi_hal_bt_get_transmitted_packets() {
    uint32_t packets = 0;
    aci_hal_le_tx_test_packet_number(&packets);
    return packets;
}

void furi_hal_bt_stop_rx() {
    aci_hal_rx_stop();
}

bool furi_hal_bt_start_scan(GapScanCallback callback, void* context) {
    if(furi_hal_bt_stack != FuriHalBtStackHciLayer) {
        return false;
    }
    gap_start_scan(callback, context);
    return true;
}

void furi_hal_bt_stop_scan() {
    if(furi_hal_bt_stack == FuriHalBtStackHciLayer) {
        gap_stop_scan();
    }
}
