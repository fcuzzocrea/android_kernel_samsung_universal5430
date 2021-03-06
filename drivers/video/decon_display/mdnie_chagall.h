#ifndef __MDNIE_H__
#define __MDNIE_H__

#define END_SEQ	0xffff

#define MDNIE_LITE

#if defined(MDNIE_LITE)
typedef u8 mdnie_t;
#else
typedef u16 mdnie_t;
#endif

enum MODE {
	DYNAMIC,
	STANDARD,
	NATURAL,
	MOVIE,
	AUTO,
	MODE_MAX
};

enum SCENARIO {
	UI_MODE,
	VIDEO_NORMAL_MODE,
	CAMERA_MODE = 4,
	NAVI_MODE,
	GALLERY_MODE,
	VT_MODE,
	BROWSER_MODE,
	EBOOK_MODE,
	EMAIL_MODE,
	HMT_8_MODE,
	HMT_16_MODE,
	SCENARIO_MAX,
	DMB_NORMAL_MODE = 20,
	DMB_MODE_MAX
};

enum BYPASS {
	BYPASS_OFF,
	BYPASS_ON,
	BYPASS_MAX
};

enum ACCESSIBILITY {
	ACCESSIBILITY_OFF,
	NEGATIVE,
	COLOR_BLIND,
	SCREEN_CURTAIN,
	ACCESSIBILITY_MAX
};

enum HBM {
	HBM_OFF,
	HBM_ON,
	HBM_ON_TEXT,
	HBM_MAX
};

enum COLOR_OFFSET_FUNC {
	COLOR_OFFSET_FUNC_NONE,
	COLOR_OFFSET_FUNC_F1,
	COLOR_OFFSET_FUNC_F2,
	COLOR_OFFSET_FUNC_F3,
	COLOR_OFFSET_FUNC_F4,
	COLOR_OFFSET_FUNC_MAX
};

#ifdef CONFIG_LCD_HMT
enum hmt_mode {
	HMT_MDNIE_OFF,
	HMT_MDNIE_ON,
	HMT_3000K = HMT_MDNIE_ON,
	HMT_4000K,
	HMT_6400K,
	HMT_7500K,
	HMT_MDNIE_MAX
};
#endif

enum MDNIE_CMD {
#if defined(MDNIE_LITE)
	LEVEL1_KEY_UNLOCK,
	MDNIE_CMD1,
	MDNIE_CMD2,
	LEVEL1_KEY_LOCK,
	MDNIE_CMD_MAX,
#else
	MDNIE_CMD1,
	MDNIE_CMD2 = MDNIE_CMD1,
	MDNIE_CMD_MAX,
#endif
};

struct mdnie_command {
	mdnie_t *sequence;
	unsigned int size;
	unsigned int sleep;
};

struct mdnie_table {
	char *name;
	struct mdnie_command tune[MDNIE_CMD_MAX];
};

#define MDNIE_SET(id)	\
{							\
	.name		= #id,				\
	.tune		= {				\
		{	.sequence = LEVEL1_UNLOCK, .size = ARRAY_SIZE(LEVEL1_UNLOCK),	.sleep = 0,},	\
		{	.sequence = id##_1, .size = ARRAY_SIZE(id##_1),			.sleep = 0,},	\
		{	.sequence = id##_2, .size = ARRAY_SIZE(id##_2),			.sleep = 0,},	\
		{	.sequence = LEVEL1_LOCK, .size = ARRAY_SIZE(LEVEL1_LOCK),	.sleep = 0,},	\
	}	\
}

typedef int (*mdnie_w)(void *devdata, const u8 *seq, u32 len);
typedef int (*mdnie_r)(void *devdata, u8 addr, u8 *buf, u32 len);

struct mdnie_device;

struct mdnie_ops {
	int (*write)(struct device *, const u8 *seq, u32 len);
	int (*read)(struct device *, u8 addr, u8 *buf, u32 len);
	/* Only for specific hardware */
	int (*set_addr)(struct device *, int mdnie_addr);
};

struct mdnie_device {
	/* This protects the 'ops' field. If 'ops' is NULL, the driver that
	   registered this device has been unloaded, and if class_get_devdata()
	   points to something in the body of that driver, it is also invalid. */
	struct mutex ops_lock;
	/* If this is NULL, the backing module is unloaded */
	struct mdnie_ops *ops;
	/* The framebuffer notifier block */
	/* Serialise access to set_power method */
	struct mutex update_lock;
	struct notifier_block fb_notif;

	struct device dev;
};

#define to_mdnie_device(obj) container_of(obj, struct mdnie_device, dev)

static inline void * mdnie_get_data(struct mdnie_device *md_dev)
{
	return dev_get_drvdata(&md_dev->dev);
}

extern struct mdnie_device *mdnie_device_register(const char *name,
		struct device *parent, struct mdnie_ops *ops);
extern void mdnie_device_unregister(struct mdnie_device *md);


struct platform_mdnie_data {
	unsigned int	display_type;
    unsigned int    support_pwm;
#if defined (CONFIG_S5P_MDNIE_PWM)
    int pwm_out_no;
	int pwm_out_func;
    char *name;
    int *br_table;
    int dft_bl;
#endif
	int (*trigger_set)(struct device *fimd);
	struct device *fimd1_device;
	struct lcd_platform_data	*lcd_pd;
};

#ifdef CONFIG_FB_I80IF
extern int s3c_fb_enable_trigger_by_mdnie(struct device *fimd);
#endif

struct rgb_info {
	int r;
	int g;
	int b;
};

struct mdnie_info {
	struct clk		*bus_clk;
	struct clk		*clk;

	struct device		*dev;
	struct mutex		dev_lock;
	struct mutex		lock;

	unsigned int		enable;

	enum SCENARIO		scenario;
	enum MODE		mode;
	enum BYPASS		bypass;
	enum HBM		hbm;
#ifdef CONFIG_LCD_HMT
	enum hmt_mode	hmt_mode;
#endif
	unsigned int		tuning;
	unsigned int		accessibility;
	unsigned int		color_correction;

	char			path[50];

	void			*data;

	struct notifier_block	fb_notif;

	struct rgb_info		wrgb_current;

	struct mdnie_table table_buffer;
	mdnie_t sequence_buffer[256];
};

extern int mdnie_open_file(const char *path, char **fp);
extern int mdnie_register(struct device *p, void *data, mdnie_w w, mdnie_r r);
#ifdef CONFIG_EXYNOS_DECON_DUAL_DISPLAY
extern int mdnie2_register(struct device *p, void *data, mdnie_w w, mdnie_r r);
#endif
extern ssize_t attr_store_for_each(struct class *cls, const char *name, const char *buf, size_t size);
extern struct class *get_mdnie_class(void);

#endif /* __MDNIE_H__ */
