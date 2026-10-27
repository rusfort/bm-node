idf_component_register(
    SRCS "main.c" "link_glue.c" "relay.c" "e2e_test.c" "box_proto.c"
         "core_main.c" "core_central.c" "tx_periph.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES board_link bt nvs_flash esp_timer esp_driver_uart esp_driver_gpio bootloader_support
)
