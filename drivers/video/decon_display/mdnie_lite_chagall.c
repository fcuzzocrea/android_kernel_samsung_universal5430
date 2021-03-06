/* linux/drivers/video/mdnie.c
 *
 * Register interface file for Samsung mDNIe driver
 *
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/lcd.h>
#include <linux/fb.h>
#include <linux/pm_runtime.h>

#include "mdnie_chagall.h"

#if defined(CONFIG_DECON_LCD_S6E3HA1)
#include "mdnie_lite_table_kl.h"
#elif defined(CONFIG_DECON_LCD_S6TNMR7)
#include "mdnie_lite_table_ch.h"
#endif

#if defined(CONFIG_TDMB)
#include "mdnie_lite_table_dmb.h"
#endif

#define MDNIE_SYSFS_PREFIX		"/sdcard/mdnie/"
#define PANEL_COORDINATE_PATH	"/sys/class/lcd/panel/color_coordinate"

#define IS_DMB(idx)				(idx == DMB_NORMAL_MODE)
#define IS_SCENARIO(idx)		(idx < SCENARIO_MAX && !(idx > VIDEO_NORMAL_MODE && idx < CAMERA_MODE))
#define IS_ACCESSIBILITY(idx)	(idx && idx < ACCESSIBILITY_MAX)
#define IS_HBM(idx)				(idx && idx < HBM_MAX)

#define SCENARIO_IS_VALID(idx)	(IS_DMB(idx) || IS_SCENARIO(idx))

/* Split 16 bit as 8bit x 2 */
#define GET_MSB_8BIT(x)		((x >> 8) & (BIT(8) - 1))
#define GET_LSB_8BIT(x)		((x >> 0) & (BIT(8) - 1))

static struct class *mdnie_class;

#ifdef ASCR_BIT_SHIFT
static void inline store_ascr(struct mdnie_table *table, int pos, mdnie_t data)
{
	mdnie_t * wbuf = &(table->tune[ASCR_CMD].sequence[pos]);
	unsigned short * tmp = (unsigned short *)wbuf;
	int bit_shift = ASCR_BIT_SHIFT;

	*tmp &= ~(cpu_to_be16(0xFF << bit_shift));
	*tmp |= cpu_to_be16(((u16)data) << bit_shift);
}

static mdnie_t inline read_ascr(struct mdnie_table *table, int pos)
{
	mdnie_t * wbuf = &(table->tune[ASCR_CMD].sequence[pos]);
	unsigned short * tmp = (unsigned short *)wbuf;
	int bit_shift = ASCR_BIT_SHIFT;

	return (mdnie_t) (be16_to_cpu(*tmp) >> bit_shift);
}
#else
static void inline store_ascr(struct mdnie_table *table, int pos, mdnie_t data)
{
	mdnie_t * wbuf = &(table->tune[ASCR_CMD].sequence[pos]);

	*wbuf = data;
}

static mdnie_t inline read_ascr(struct mdnie_table *table, int pos)
{
	mdnie_t * wbuf = &(table->tune[ASCR_CMD].sequence[pos]);

	return *wbuf;
}
#endif

/* Do not call mdnie write directly */
static int mdnie_write(struct mdnie_info *mdnie, struct mdnie_table *table)
{
	struct mdnie_device *md = to_mdnie_device(mdnie->dev);
	int ret = 0;
#ifndef MDNIE_USE_SET_ADDR
	int i = 0;
#endif

	if (!mdnie->enable || !md->ops->write)
		return -EIO;

#ifdef MDNIE_USE_SET_ADDR
	ret = md->ops->set_addr(md->dev.parent, MDNIE_SEQUENCE_OFFSET_1);
	ret = md->ops->write(md->dev.parent, table->tune[MDNIE_CMD1].sequence,
							table->tune[MDNIE_CMD1].size);
	ret = md->ops->set_addr(md->dev.parent, MDNIE_SEQUENCE_OFFSET_2);
	ret = md->ops->write(md->dev.parent, table->tune[MDNIE_CMD2].sequence,
							table->tune[MDNIE_CMD2].size);
#else
	for (i = 0; i < ARRAY_SIZE(table->tune); i++) {
			ret = md->ops->write(md->dev.parent, table->tune[i].sequence, table->tune[i].size);
	}
#endif

	return ret;
}

static int mdnie_write_table(struct mdnie_info *mdnie, struct mdnie_table *table)
{
	int i, ret = 0;
	struct mdnie_table *buf = NULL;

	for (i = 0; i < MDNIE_CMD_MAX; i++) {
		if (IS_ERR_OR_NULL(table->tune[i].sequence)) {
			dev_err(mdnie->dev, "mdnie sequence %s is null, %x\n", table->name, (u32)table->tune[i].sequence);
			return -EPERM;
		}
	}

	mutex_lock(&mdnie->dev_lock);

	buf = table;

	ret = mdnie_write(mdnie, buf);

	mutex_unlock(&mdnie->dev_lock);

	return ret;
}

static struct mdnie_table *mdnie_find_table(struct mdnie_info *mdnie)
{
	struct mdnie_table *table = NULL;

	mutex_lock(&mdnie->lock);

	if (IS_ACCESSIBILITY(mdnie->accessibility)) {
		table = &accessibility_table[mdnie->accessibility];
		goto exit;
	} else if (IS_HBM(mdnie->hbm)) {
#if defined(CONFIG_LCD_MIPI_S6E3HA1) || defined(CONFIG_LCD_MIPI_S6TNMR7)
		if ((mdnie->scenario == BROWSER_MODE)|| (mdnie->scenario == EBOOK_MODE))
			table = &hbm_table[HBM_ON_TEXT];
		else
#endif
		table = &hbm_table[mdnie->hbm];
		goto exit;
#if defined(CONFIG_TDMB)
	} else if (IS_DMB(mdnie->scenario)) {
		table = &dmb_table[mdnie->mode];
		goto exit;
#endif
	} else if (IS_SCENARIO(mdnie->scenario)) {
		table = &tuning_table[mdnie->scenario][mdnie->mode];
		goto exit;
	}

exit:
	mutex_unlock(&mdnie->lock);

	return table;
}

static void mdnie_update_sequence(struct mdnie_info *mdnie, struct mdnie_table *table)
{
	mdnie_write_table(mdnie, table);
}

static void mdnie_update(struct mdnie_info *mdnie)
{
	struct mdnie_table *table = NULL;

	if (!mdnie->enable) {
		dev_err(mdnie->dev, "mdnie state is off\n");
		return;
	}

	table = mdnie_find_table(mdnie);
	if (!IS_ERR_OR_NULL(table) && !IS_ERR_OR_NULL(table->name)) {
		mdnie_update_sequence(mdnie, table);
		dev_info(mdnie->dev, "%s\n", table->name);

		mdnie->wrgb_current.r = read_ascr(table, MDNIE_WHITE_R);
		mdnie->wrgb_current.g = read_ascr(table, MDNIE_WHITE_G);
		mdnie->wrgb_current.b = read_ascr(table, MDNIE_WHITE_B);
	}
}

static void update_color_position(struct mdnie_info *mdnie, unsigned int idx)
{
	u8 mode, scenario;
	struct mdnie_table *table = NULL;
	mdnie_t *wbuf;

	dev_info(mdnie->dev, "%s: idx=%d\n", __func__, idx);

	mutex_lock(&mdnie->lock);

	for (mode = 0; mode < MODE_MAX; mode++) {
		for (scenario = 0; scenario <= EMAIL_MODE; scenario++) {
			wbuf = tuning_table[scenario][mode].tune[ASCR_CMD].sequence;
			if (IS_ERR_OR_NULL(wbuf))
				continue;
			table = &tuning_table[scenario][mode];
			if ((read_ascr(table, MDNIE_WHITE_R) == 0xff)
				&& (read_ascr(table, MDNIE_WHITE_G) == 0xff)
				&& (read_ascr(table, MDNIE_WHITE_B) == 0xff)) {
				store_ascr(table, MDNIE_WHITE_R, coordinate_data[idx][0]);
				store_ascr(table, MDNIE_WHITE_G, coordinate_data[idx][1]);
				store_ascr(table, MDNIE_WHITE_B, coordinate_data[idx][2]);
			}
		}
	}

	mutex_unlock(&mdnie->lock);
}

static int mdnie_calibration(int *r)
{
	int ret = 0;

	if (r[1] > 0) {
		if (r[3] > 0)
			ret = 3;
		else
			ret = (r[4] < 0) ? 1 : 2;
	} else {
		if (r[2] < 0) {
			if (r[3] > 0)
				ret = 9;
			else
				ret = (r[4] < 0) ? 7 : 8;
		} else {
			if (r[3] > 0)
				ret = 6;
			else
				ret = (r[4] < 0) ? 4 : 5;
		}
	}

	ret = (ret > 0) ? ret : 1;
	ret = (ret > 9) ? 1 : ret;

	pr_info("%d, %d, %d, %d, tune%d\n", r[1], r[2], r[3], r[4], ret);

	return ret;
}

static int get_panel_coordinate(struct mdnie_info *mdnie, int *result)
{
	int ret = 0;
	char *fp = NULL;
	unsigned short x, y;

	ret = mdnie_open_file(PANEL_COORDINATE_PATH, &fp);
	if (IS_ERR_OR_NULL(fp) || ret <= 0) {
		dev_info(mdnie->dev, "%s: open skip: %s, %d\n", __func__, PANEL_COORDINATE_PATH, ret);
		ret = -EINVAL;
		goto skip_color_correction;
	}

	ret = sscanf(fp, "%4hu, %4hu", &x, &y);
	if ((ret != 2) || !(x || y)) {
		dev_info(mdnie->dev, "%s: %d, %d\n", __func__, x, y);
		ret = -EINVAL;
		goto skip_color_correction;
	}

	result[COLOR_OFFSET_FUNC_F1] = COLOR_OFFSET_F1(x, y);
	result[COLOR_OFFSET_FUNC_F2] = COLOR_OFFSET_F2(x, y);
	result[COLOR_OFFSET_FUNC_F3] = COLOR_OFFSET_F3(x, y);
	result[COLOR_OFFSET_FUNC_F4] = COLOR_OFFSET_F4(x, y);

	ret = mdnie_calibration(result);
	dev_info(mdnie->dev, "%s: %d, %d, %d\n", __func__, x, y, ret);

skip_color_correction:
	mdnie->color_correction = 1;
	if (!IS_ERR_OR_NULL(fp))
		kfree(fp);

	return ret;
}

static ssize_t mode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->mode);
}

static ssize_t mode_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value = 0;
	int ret, idx, result[COLOR_OFFSET_FUNC_MAX] = {0,};

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s: %d\n", __func__, value);

	if (value >= MODE_MAX)
		return -EINVAL;

	mutex_lock(&mdnie->lock);
	mdnie->mode = value;
	mutex_unlock(&mdnie->lock);

	if (!mdnie->color_correction) {
		idx = get_panel_coordinate(mdnie, result);
		if (idx > 0)
			update_color_position(mdnie, idx);
	}

	mdnie_update(mdnie);

	return count;
}


static ssize_t scenario_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->scenario);
}

static ssize_t scenario_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s: %d\n", __func__, value);

	if (!SCENARIO_IS_VALID(value))
		value = UI_MODE;

	mutex_lock(&mdnie->lock);
	mdnie->scenario = value;
	mutex_unlock(&mdnie->lock);

	mdnie_update(mdnie);

	return count;
}

static ssize_t accessibility_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->accessibility);
}

static ssize_t accessibility_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int value, s[9] = {0, }, i = 0;
	int ret;
	struct mdnie_table *table;

	ret = sscanf(buf, "%8d %8x %8x %8x %8x %8x %8x %8x %8x %8x",
		&value, &s[0], &s[1], &s[2], &s[3],
		&s[4], &s[5], &s[6], &s[7], &s[8]);

	if (ret < 0)
		return ret;

	dev_info(dev, "%s: %d, %d\n", __func__, value, ret);

	if (value >= ACCESSIBILITY_MAX)
		return -EINVAL;

	mutex_lock(&mdnie->lock);
	mdnie->accessibility = value;
	if (value == COLOR_BLIND) {
		if (ret > ARRAY_SIZE(s) + 1) {
			mutex_unlock(&mdnie->lock);
			return -EINVAL;
		}
		table = &accessibility_table[COLOR_BLIND];
		while (i < ret - 1) {
			store_ascr( table, MDNIE_COLOR_BLIND_OFFSET + i * 2 + 0,
					GET_LSB_8BIT(s[i]));
			store_ascr( table, MDNIE_COLOR_BLIND_OFFSET + i * 2 + 1,
					GET_MSB_8BIT(s[i]));
			i++;
		}

		dev_info(dev, "%s: %s\n", __func__, buf);
	}
	mutex_unlock(&mdnie->lock);

	mdnie_update(mdnie);

	return count;
}

static ssize_t color_correct_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	char *pos = buf;
	int i, idx, result[COLOR_OFFSET_FUNC_MAX] = {0,};

	if (!mdnie->color_correction)
		return -EINVAL;

	idx = get_panel_coordinate(mdnie, result);

	for (i = COLOR_OFFSET_FUNC_F1; i < COLOR_OFFSET_FUNC_MAX; i++)
		pos += sprintf(pos, "f%d: %d, ", i, result[i]);
	pos += sprintf(pos, "tune%d\n", idx);

	return pos - buf;
}

static ssize_t bypass_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->bypass);
}

static ssize_t bypass_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	struct mdnie_table *table = NULL;
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 0, &value);
	if (ret < 0)
		return ret;

	dev_info(dev, "%s: %d\n", __func__, value);

	if (value >= BYPASS_MAX)
		return -EINVAL;

	value = (value) ? BYPASS_ON : BYPASS_OFF;

	mutex_lock(&mdnie->lock);
	mdnie->bypass = value;
	mutex_unlock(&mdnie->lock);

	table = &bypass_table[value];
	if (!IS_ERR_OR_NULL(table)) {
		mdnie_write_table(mdnie, table);
		dev_info(mdnie->dev, "%s\n", table->name);
	}

	return count;
}

static ssize_t lux_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", mdnie->hbm);
}

static ssize_t lux_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	unsigned int hbm = 0, update = 0;
	int ret, value;

	ret = kstrtoint(buf, 0, &value);
	if (ret < 0)
		return ret;

	mutex_lock(&mdnie->lock);
	hbm = get_hbm_index(value);
	update = (mdnie->hbm != hbm) ? 1 : 0;
	mdnie->hbm = update ? hbm : mdnie->hbm;
	mutex_unlock(&mdnie->lock);

	if (update) {
		dev_info(dev, "%s: %d\n", __func__, value);
		mdnie_update(mdnie);
	}

	return count;
}

/* Temporary solution: Do not use this sysfs as official purpose */
static ssize_t mdnie_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_device *md = to_mdnie_device(dev);
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	char *pos = buf;
	struct mdnie_table *table = NULL;
	int i, j;
	u8 *buffer;

	if (!mdnie->enable) {
		dev_err(mdnie->dev, "mdnie state is off\n");
		goto exit;
	}

	table = mdnie_find_table(mdnie);

	for (i = 0; i < MDNIE_CMD_MAX; i++) {
		if (IS_ERR_OR_NULL(table->tune[i].sequence)) {
			dev_err(mdnie->dev, "mdnie sequence %s is null, %x\n", table->name, (u32)table->tune[i].sequence);
			goto exit;
		}
	}

	md->ops->write(md->dev.parent, table->tune[LEVEL1_KEY_UNLOCK].sequence, table->tune[LEVEL1_KEY_UNLOCK].size);

	pos += sprintf(pos, "+ %s\n", table->name);

	for (j = MDNIE_CMD1; j <= MDNIE_CMD2; j++) {
		buffer = kzalloc(table->tune[j].size, GFP_KERNEL);

#ifdef MDNIE_USE_SET_ADDR
		if (j == MDNIE_CMD1)
			md->ops->set_addr(md->dev.parent, MDNIE_SEQUENCE_OFFSET_1);
		else if (j == MDNIE_CMD2)
			md->ops->set_addr(md->dev.parent, MDNIE_SEQUENCE_OFFSET_2);
#endif

		md->ops->read(md->dev.parent, table->tune[j].sequence[0], buffer, table->tune[j].size - 1);

		for (i = 0; i < table->tune[j].size - 1; i++) {
			pos += sprintf(pos, "%3d:\t0x%02x\t0x%02x", i + 1, table->tune[j].sequence[i+1], buffer[i]);
			if (table->tune[j].sequence[i+1] != buffer[i])
				pos += sprintf(pos, "\t(X)");
			pos += sprintf(pos, "\n");
		}

		kfree(buffer);
	}

	pos += sprintf(pos, "- %s\n", table->name);

	md->ops->write(md->dev.parent, table->tune[LEVEL1_KEY_LOCK].sequence, table->tune[LEVEL1_KEY_LOCK].size);

exit:
	return pos - buf;
}

static ssize_t sensorRGB_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);

	return sprintf(buf, "%d %d %d\n", mdnie->wrgb_current.r, mdnie->wrgb_current.g, mdnie->wrgb_current.b);
}

static ssize_t sensorRGB_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct mdnie_info *mdnie = dev_get_drvdata(dev);
	struct mdnie_table *table = NULL;
	unsigned int white_r, white_g, white_b;
	int ret;

	ret = sscanf(buf, "%8d %8d %8d", &white_r, &white_g, &white_b);
	if (ret < 0)
		return ret;

	if (mdnie->enable
		&& mdnie->accessibility == ACCESSIBILITY_OFF
		&& mdnie->mode == AUTO
		&& (mdnie->scenario == BROWSER_MODE || mdnie->scenario == EBOOK_MODE)) {
		dev_info(dev, "%s: %d, %d, %d\n", __func__, white_r, white_g, white_b);

		table = mdnie_find_table(mdnie);

		memcpy(&mdnie->table_buffer, table, sizeof(struct mdnie_table));
		memcpy(&mdnie->sequence_buffer, table->tune[ASCR_CMD].sequence, table->tune[ASCR_CMD].size);
		mdnie->table_buffer.tune[ASCR_CMD].sequence = mdnie->sequence_buffer;

		store_ascr(&mdnie->table_buffer, MDNIE_WHITE_R, (unsigned char)white_r);
		store_ascr(&mdnie->table_buffer, MDNIE_WHITE_G, (unsigned char)white_g);
		store_ascr(&mdnie->table_buffer, MDNIE_WHITE_B, (unsigned char)white_b);

		mdnie->wrgb_current.r = white_r;
		mdnie->wrgb_current.g = white_g;
		mdnie->wrgb_current.b = white_b;

		mdnie_update_sequence(mdnie, &mdnie->table_buffer);
	}

	return count;
}

static struct device_attribute mdnie_attributes[] = {
	__ATTR(mode, 0664, mode_show, mode_store),
	__ATTR(scenario, 0664, scenario_show, scenario_store),
	__ATTR(accessibility, 0664, accessibility_show, accessibility_store),
	__ATTR(color_correct, 0444, color_correct_show, NULL),
	__ATTR(bypass, 0664, bypass_show, bypass_store),
	__ATTR(lux, 0000, lux_show, lux_store),
	__ATTR(mdnie, 0444, mdnie_show, NULL),
	__ATTR(sensorRGB, 0664, sensorRGB_show, sensorRGB_store),
	__ATTR_NULL,
};

static int fb_notifier_callback(struct notifier_block *self,
				 unsigned long event, void *data)
{
	struct mdnie_device *md;
	struct mdnie_info *mdnie;
	struct fb_event *evdata = data;
	int fb_blank;

	switch (event) {
	case FB_EVENT_BLANK:
		break;
	default:
		return NOTIFY_DONE;
	}

	md = container_of(self, struct mdnie_device, fb_notif);
	if (!md)
		return NOTIFY_DONE;

	mdnie = (struct mdnie_info *)mdnie_get_data(md);

	fb_blank = *(int *)evdata->data;

	dev_info(mdnie->dev, "%s: %d\n", __func__, fb_blank);

	if (fb_blank == FB_BLANK_UNBLANK) {
		mutex_lock(&mdnie->lock);
		mdnie->enable = 1;
		mutex_unlock(&mdnie->lock);

		mdnie_update(mdnie);
	} else if (fb_blank == FB_BLANK_POWERDOWN) {
		mutex_lock(&mdnie->lock);
		mdnie->enable = 0;
		mutex_unlock(&mdnie->lock);
	}

	return NOTIFY_DONE;
}

static int mdnie_register_fb(struct mdnie_device *md)
{
	memset(&md->fb_notif, 0, sizeof(md->fb_notif));
	md->fb_notif.notifier_call = fb_notifier_callback;
	return fb_register_client(&md->fb_notif);
}

static void mdnie_unregister_fb(struct mdnie_device *md)
{
	fb_unregister_client(&md->fb_notif);
}

static void mdnie_device_release(struct device *dev)
{
	struct mdnie_device *md = to_mdnie_device(dev);
	kfree(md);
}

struct mdnie_device *mdnie_device_register(const char *name,
		struct device *parent, struct mdnie_ops *ops)
{
	struct mdnie_device *new_md;
	struct mdnie_info *mdnie;
	int rc;

	mdnie = kzalloc(sizeof(struct mdnie_info), GFP_KERNEL);
	if (!mdnie) {
		pr_err("failed to allocate mdnie\n");
		rc = -ENOMEM;
		goto error0;
	}

	new_md = kzalloc(sizeof(struct mdnie_device), GFP_KERNEL);
	if (!new_md) {
		pr_err("failed to allocate mdnie\n");
		rc = -ENOMEM;
		goto error1;
	}

	mutex_init(&new_md->ops_lock);
	mutex_init(&new_md->update_lock);

	new_md->dev.class = mdnie_class;
	new_md->dev.parent = parent;
	new_md->dev.release = mdnie_device_release;

	dev_set_name(&new_md->dev, name);

	mdnie->dev = &new_md->dev;
	mdnie->scenario = UI_MODE;
	mdnie->mode = AUTO;
	mdnie->enable = 0;
	mdnie->tuning = 0;
	mdnie->accessibility = ACCESSIBILITY_OFF;
	mdnie->bypass = BYPASS_OFF;

	mutex_init(&mdnie->lock);
	mutex_init(&mdnie->dev_lock);

	dev_set_drvdata(&new_md->dev, mdnie);

	rc = device_register(&new_md->dev);
	if (rc) {
		pr_err("failed to device_register mdnie\n");
		goto error2;
	}

	rc = mdnie_register_fb(new_md);
	if (rc) {
		device_unregister(&new_md->dev);
		goto error1;
	}

	new_md->ops = ops;

	mdnie->enable = 1;
	mdnie_update(mdnie);

	dev_info(mdnie->dev, "registered successfully\n");

	return new_md;

error2:
	kfree(new_md);
error1:
	kfree(mdnie);
error0:
	return ERR_PTR(rc);
}

/**
 * mdnie_device - unregisters a object of mdnie_device class.
 * @ld: the mdnie device object to be unregistered and freed.
 *
 * Unregisters a previously registered via mdnie_device_register object.
 */
void mdnie_device_unregister(struct mdnie_device *md)
{
	if (!md)
		return;

	mutex_lock(&md->ops_lock);
	md->ops = NULL;
	mutex_unlock(&md->ops_lock);
	mdnie_unregister_fb(md);

	device_unregister(&md->dev);
}

static void __exit mdnie_class_exit(void)
{
	class_destroy(mdnie_class);
}

static int __init mdnie_class_init(void)
{
	mdnie_class = class_create(THIS_MODULE, "mdnie");
	if (IS_ERR(mdnie_class)) {
		printk(KERN_WARNING "Unable to create mdnie class; errno = %ld\n",
				PTR_ERR(mdnie_class));
		return PTR_ERR(mdnie_class);
	}

	mdnie_class->dev_attrs = mdnie_attributes;
	return 0;
}

/*
 * if this is compiled into the kernel, we need to ensure that the
 * class is registered before users of the class try to register lcd's
 */
postcore_initcall(mdnie_class_init);
module_exit(mdnie_class_exit);

MODULE_DESCRIPTION("mDNIe Driver");
MODULE_LICENSE("GPL");

static int attr_store(struct device *dev,
	struct attribute *attr, const char *buf, size_t size)
{
	struct device_attribute *dev_attr = container_of(attr, struct device_attribute, attr);

	dev_attr->store(dev, dev_attr, buf, size);

	return 0;
}

static int attrs_store_iter(struct device *dev,
	const char *name, const char *buf, size_t size, struct attribute **attrs)
{
	int i;

	for (i = 0; attrs[i]; i++) {
		if (!strcmp(name, attrs[i]->name))
			attr_store(dev, attrs[i], buf, size);
	}

	return 0;
}

static int groups_store_iter(struct device *dev,
	const char *name, const char *buf, size_t size, const struct attribute_group **groups)
{
	int i;

	for (i = 0; groups[i]; i++)
		attrs_store_iter(dev, name, buf, size, groups[i]->attrs);

	return 0;
}

static int dev_attrs_store_iter(struct device *dev,
	const char *name, const char *buf, size_t size, struct device_attribute *dev_attrs)
{
	int i;

	for (i = 0; attr_name(dev_attrs[i]); i++) {
		if (!strcmp(name, attr_name(dev_attrs[i])))
			attr_store(dev, &dev_attrs[i].attr, buf, size);
	}

	return 0;
}

static int attr_find_and_store(struct device *dev,
	const char *name, const char *buf, size_t size)
{
	struct device_attribute *dev_attrs;
	const struct attribute_group **groups;

	if (dev->class && dev->class->dev_attrs) {
		dev_attrs = dev->class->dev_attrs;
		dev_attrs_store_iter(dev, name, buf, size, dev_attrs);
	}

	if (dev->type && dev->type->groups) {
		groups = dev->type->groups;
		groups_store_iter(dev, name, buf, size, groups);
	}

	if (dev->groups) {
		groups = dev->groups;
		groups_store_iter(dev, name, buf, size, groups);
	}

	return 0;
}

ssize_t attr_store_for_each(struct class *cls,
	const char *name, const char *buf, size_t size)
{
	struct class_dev_iter iter;
	struct device *dev;
	int error = 0;
	struct class *class = cls;

	if (!class)
		return -EINVAL;
	if (!class->p) {
		WARN(1, "%s called for class '%s' before it was initialized",
		     __func__, class->name);
		return -EINVAL;
	}

	class_dev_iter_init(&iter, class, NULL, NULL);
	while ((dev = class_dev_iter_next(&iter))) {
		error = attr_find_and_store(dev, name, buf, size);
		if (error)
			break;
	}
	class_dev_iter_exit(&iter);

	return error;
}

struct class *get_mdnie_class(void)
{
	return mdnie_class;
}

