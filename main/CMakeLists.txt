idf_component_register(
    SRCS
        "main.c"
        "registry.c"
        "ascii.c"
        "console.c"
        "cfg.c"
        "cmd_sys.c"
        "cmd_fs.c"
        "cmd_hw.c"
        "periph.c"
        "ble.c"
        "cmd_extra.c"
        "cmd_file.c"
        "cmd_net.c"
        "peer.c"
        "motor.c"
        "sniff.c"
        "display.c"
        "cmd_pins.c"
        "camera.c"
        "led.c"
        "wifi.c"
        "dns_hijack.c"
        "portal.c"
        "webui.c"
        "sshd.c"
        "py.c"
        "agent.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "portal.html" "index.html"
    REQUIRES
        nvs_flash
        fatfs
        console
        vfs
        json
        esp_timer
        esp_system
        esp_hw_support
        esp_app_format
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_http_client
        esp-tls
        mbedtls
        lwip
        esp_driver_uart
        esp_driver_gpio
        esp_driver_ledc
        bt
        esp_driver_tsens
        espressif__led_strip
        esp_driver_i2c
        esp_driver_spi
        esp_driver_i2s
        esp_driver_sdm
        esp_driver_gptimer
        esp_driver_rmt
        esp_driver_mcpwm
        esp_adc
        espressif__esp32-camera
        espressif__mdns
        wolfssl__wolfssl
        wolfssl__wolfssh
        micropython
)

# Command tables are written { name, usage, help, fn } and leave `raw` off.
# C zero-fills the rest, which is exactly what we want: raw is an opt-in flag
# for the two commands whose argument is a body. Spelling `, false` on twenty
# tables would be noise, and the warning is not catching a real mistake here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# wolfssh/ssh.h falls back to <wolfssl/options.h> — an autotools artefact that
# does not exist in the ESP-IDF build — unless it is told the configuration
# comes from user_settings.h. The wolfssl component defines this for itself;
# anything that includes its headers has to say so too.
target_compile_definitions(${COMPONENT_LIB} PRIVATE WOLFSSL_USER_SETTINGS)
