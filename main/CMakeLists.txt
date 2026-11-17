idf_component_register(
    SRCS "knot_main.c" "knot_model.c" "knot_store.c"
         "buddy_ble.c" "buddy_ble_store.c" "buddy_ble_lifecycle.c" "buddy_line.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_timer esp_hw_support json nvs_flash
)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_att_svr_register")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,__wrap_ble_att_svr_register")
