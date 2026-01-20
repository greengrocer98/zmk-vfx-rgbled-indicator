#if IS_ENABLED(CONFIG_REPORT_ATTR)
struct attr_report
{
    uint32_t attr;
    uint32_t val;
	int ret;
};
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
void indicate_battery();
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
void indicate_connection();
#endif
