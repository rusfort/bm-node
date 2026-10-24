idf_component_register(
    SRCS "main.c" "node_periph.c" "node_central.c" "e2e_test.c" "box_proto.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash esp_timer
)
