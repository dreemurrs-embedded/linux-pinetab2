extern struct device *global_dev;

#ifdef CONFIG_BES2600_ENABLE_DEVEL_LOGS
#define bes_devel(fmt, ...) dev_debug(global_dev, fmt, ##__VA_ARGS__)
#else
#define bes_devel(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif
#define bes_info(fmt, ...) dev_info(global_dev, fmt, ##__VA_ARGS__)
#define bes_warn(fmt, ...) dev_warn(global_dev, fmt, ##__VA_ARGS__)
#define bes_err(fmt, ...) dev_err(global_dev, fmt, ##__VA_ARGS__)
