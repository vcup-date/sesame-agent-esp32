#include "sesame.h"
#include "agent.h"
#include "camera.h"
#include "cfg.h"
#include "display.h"
#include "led.h"
#include "net.h"
#include "py.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"

static const char *TAG = "sesame";

void console_start(void);

static esp_err_t mount_storage(void)
{
    static wl_handle_t wl = WL_INVALID_HANDLE;

    const esp_vfs_fat_mount_config_t cfg = {
        .max_files              = 8,
        .format_if_mount_failed = true,
        .allocation_unit_size   = CONFIG_WL_SECTOR_SIZE,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl(SESAME_MOUNT, "storage", &cfg, &wl);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(cfg_init());

    if (!cfg_is_stored(CFG_DEV_NAME)) {
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            char name[24];
            snprintf(name, sizeof(name), "sesame-%02x%02x", mac[4], mac[5]);
            cfg_set_ram(CFG_DEV_NAME, name);
            ESP_LOGI(TAG, "device name: %s (http://%s.local)", name, name);
        }
    }

    err = mount_storage();
    if (err != ESP_OK) {

        ESP_LOGE(TAG, "storage did not mount (%s); /sesame is unavailable",
                 esp_err_to_name(err));
    } else {
        uint64_t total = 0, avail = 0;
        if (esp_vfs_fat_info(SESAME_MOUNT, &total, &avail) == ESP_OK) {
            ESP_LOGI(TAG, "%s mounted, %llu KB free of %llu KB",
                     SESAME_MOUNT, avail / 1024, total / 1024);
        }
    }

    cmd_sys_register();
    cmd_fs_register();
    cmd_hw_register();
    cmd_periph_register();
    cmd_ble_register();
    cmd_extra_register();
    cmd_file_register();
    cmd_netcmds_register();
    cmd_peer_register();
    cmd_motor_register();
    cmd_sniff_register();
    cmd_cron_register();
    cmd_skill_register();
    cmd_pins_register();
    cmd_disp_register();
    cmd_net_register();
    cmd_cam_register();
    cmd_agent_register();
    cmd_led_register();
    cmd_ssh_register();
    cmd_py_register();
    ESP_LOGI(TAG, "%d commands registered", sesame_cmd_count());

    if (camera_init() != ESP_OK) {
        ESP_LOGW(TAG, "camera unavailable; 'snap' will retry on demand");
    }

    led_init();
    py_init();

    ESP_ERROR_CHECK(net_start());

    cron_start();

    console_start();
}
