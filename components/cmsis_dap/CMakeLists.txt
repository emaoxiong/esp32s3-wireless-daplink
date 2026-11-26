idf_component_register(
    SRCS "src/DAP.c" "src/SW_DP.c" "src/JTAG_DP.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)
