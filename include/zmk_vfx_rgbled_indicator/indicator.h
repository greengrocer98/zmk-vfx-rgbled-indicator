#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
void indicate_battery();
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
void indicate_connection();
#endif