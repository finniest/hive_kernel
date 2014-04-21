/* driver/i2c/chip/tap2026.c
 *
 * TI tpa2026 Speaker Amp
 *
 * Copyright (C) 2010 HTC Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/freezer.h>
#include <mach/tpa2026.h>
#include <linux/mfd/pmic8058.h>
#include <linux/mutex.h>

#define DEBUG (0)
#define AMP_ON_CMD_LEN 7
#define RETRY_CNT 5

static struct i2c_client *this_client;
static struct tpa2026_platform_data *pdata;
static char *config_data;
static int tpa2026_mode_cnt;
struct mutex spk_amp_lock_2026;
static int tpa2026_opened;
static int last_spkamp_state;
static int mfg_mode_2026;
static char SPK_AMP_ON[] = {0xC2, 0x05, 0x01, 0x00, 0x16, 0x9A, 0xC0};
static char HEADSET_AMP_ON[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static char RING_AMP_ON[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static char HANDSET_AMP_ON[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static char AMP_0FF[] = {0x00, 0x90};
static char MFG_LOOPBACK_ON[]	= {0xC2, 0x05, 0x01, 0x00, 0x06, 0x9A, 0xC0};
static char MFG_LOOPBACK_OFF[]	= {0x02, 0x05, 0x01, 0x00, 0x06, 0x9A, 0xC0};
static char MFG_LOOPBACK_L[]	= {0x42, 0x05, 0x01, 0x00, 0x06, 0x9A, 0xC0};
static char MFG_LOOPBACK_R[]	= {0x82, 0x05, 0x01, 0x00, 0x06, 0x9A, 0xC0};

struct pm8058_gpio tpa2026pwr = {
	.direction      = PM_GPIO_DIR_OUT,
	.output_buffer  = PM_GPIO_OUT_BUF_CMOS,
	.output_value   = 0,
	.pull           = PM_GPIO_PULL_NO,
	.out_strength   = PM_GPIO_STRENGTH_HIGH,
	.function       = PM_GPIO_FUNC_NORMAL,
	.vin_sel        = PM_GPIO_VIN_L7,
	.inv_int_pol	= 0,
};

static int tpa2026_write_reg(u8 reg, u8 val)
{
	int err;
	struct i2c_msg msg[1];
	unsigned char data[2];

	msg->addr = this_client->addr;
	msg->flags = 0;
	msg->len = 2;
	msg->buf = data;
	data[0] = reg;
	data[1] = val;

	err = i2c_transfer(this_client->adapter, msg, 1);
	if (err >= 0)
		return 0;

	return err;
}

static int tpa2026_i2c_write(char *txData, int length)
{
	int i, retry, pass = 0;
	char buf[2];
	struct i2c_msg msg[] = {
		{
		 .addr = this_client->addr,
		 .flags = 0,
		 .len = 2,
		 .buf = buf,
		},
	};
	for (i = 0; i < length; i++) {
		if (i == 2)  /* According to tpa2026 Spec */
			mdelay(1);
		buf[0] = (i+1);
		buf[1] = txData[i];
/* #if DEBUG */
		pr_info("i2c_write %d=%x\n", i, buf[1]);
/* #endif */
		msg->buf = buf;
		retry = RETRY_CNT;
		pass = 0;
		while (retry--) {
			if (i2c_transfer(this_client->adapter, msg, 1) < 0) {
				pr_err("%s: I2C transfer error %d retry %d\n",
						__func__, i, retry);
				msleep(20);
			} else {
				pass = 1;
				break;
			}
		}
		if (pass == 0) {
			pr_err("I2C transfer error, retry fail\n");
			return -EIO;
		}
	}
	return 0;
}

static int tpa2026_i2c_write_for_read(char *txData, int length)
{
	int i, retry, pass = 0;
	char buf[2];
	struct i2c_msg msg[] = {
		{
		 .addr = this_client->addr,
		 .flags = 0,
		 .len = 2,
		 .buf = buf,
		},
	};
	for (i = 0; i < length; i++) {
		if (i == 2)  /* According to tpa2026 Spec */
			mdelay(1);
		buf[0] = i;
		buf[1] = txData[i];
/* #if DEBUG */
		pr_info("i2c_write %d=%x\n", i, buf[1]);
/* #endif */
		msg->buf = buf;
		retry = RETRY_CNT;
		pass = 0;
		while (retry--) {
			if (i2c_transfer(this_client->adapter, msg, 1) < 0) {
				pr_err("%s: I2C transfer error %d retry %d\n",
						__func__, i, retry);
				msleep(20);
			} else {
				pass = 1;
				break;
			}
		}
		if (pass == 0) {
			pr_err("I2C transfer error, retry fail\n");
			return -EIO;
		}
	}
	return 0;
}

static int tpa2026_i2c_read(char *rxData, int length)
{
	int rc;
	struct i2c_msg msgs[] = {
		{
			.addr = this_client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = rxData,
		},
	};
	rc = i2c_transfer(this_client->adapter, msgs, 1);
	if (rc < 0) {
		pr_err("%s: transfer error %d\n", __func__, rc);
		return rc;
	}

#if DEBUG
	{
		int i = 0;
		for (i = 0; i < length; i++)
			pr_info("i2c_read %s: rx[%d] = %2x\n", __func__, i, \
				rxData[i]);
	}
#endif

	return 0;
}

static int tpa2026_open(struct inode *inode, struct file *file)
{
	int rc = 0;

	mutex_lock(&spk_amp_lock_2026);

	if (tpa2026_opened) {
		pr_err("%s: busy\n", __func__);
		rc = -EBUSY;
		goto done;
	}
	tpa2026_opened = 1;
done:
	mutex_unlock(&spk_amp_lock_2026);
	return rc;
}

static int tpa2026_release(struct inode *inode, struct file *file)
{
	mutex_lock(&spk_amp_lock_2026);
	tpa2026_opened = 0;
	mutex_unlock(&spk_amp_lock_2026);

	return 0;
}
void set_amp_2026(int on, char *i2c_command)
{
	pr_info("%s: %d\n", __func__, on);
	mutex_lock(&spk_amp_lock_2026);
	if (on && !last_spkamp_state) {
		if (tpa2026_i2c_write(i2c_command, AMP_ON_CMD_LEN) == 0) {
			last_spkamp_state = 1;
			pr_info("%s: ON reg1=%x, reg2=%x\n",
				__func__, i2c_command[1], i2c_command[2]);
		}
	} else if (!on && last_spkamp_state) {
		if (tpa2026_i2c_write(AMP_0FF, sizeof(AMP_0FF)) == 0) {
			last_spkamp_state = 0;
			pr_info("%s: OFF\n", __func__);
		}
	}
	mutex_unlock(&spk_amp_lock_2026);
}

void set_speaker_amp_2026(int on)
{
	switch (mfg_mode_2026) {
	case 1:
		pr_info("%s: MFG_LOOPBACK_ON\n", __func__);
		set_amp_2026(on, MFG_LOOPBACK_ON);
		break;
	case 2:
		pr_info("%s: MFG_LOOPBACK_OFF\n", __func__);
		set_amp_2026(on, MFG_LOOPBACK_OFF);
		break;
	case 3:
		pr_info("%s: MFG_LOOPBACK_L\n", __func__);
		set_amp_2026(on, MFG_LOOPBACK_L);
		break;
	case 4:
		pr_info("%s: MFG_LOOPBACK_R\n", __func__);
		set_amp_2026(on, MFG_LOOPBACK_R);
		break;
	case 0:
	default:
		set_amp_2026(on, SPK_AMP_ON);
		break;
	}
}

void set_headset_amp_2026(int on)
{
	set_amp_2026(on, HEADSET_AMP_ON);
}

void set_speaker_headset_amp_2026(int on)
{
	set_amp_2026(on, RING_AMP_ON);
}

void set_handset_amp_2026(int on)
{
	set_amp_2026(on, HANDSET_AMP_ON);
}

int update_amp_parameter_2026(int mode)
{
	if (mode > tpa2026_mode_cnt)
		return EINVAL;
	if (*(config_data + mode * MODE_CMD_LEM + 1) == SPKR_OUTPUT)
		memcpy(SPK_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(SPK_AMP_ON));
	else if (*(config_data + mode * MODE_CMD_LEM + 1) == HEADSET_OUTPUT)
		memcpy(HEADSET_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(HEADSET_AMP_ON));
	else if (*(config_data + mode * MODE_CMD_LEM + 1) == DUAL_OUTPUT)
		memcpy(RING_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(RING_AMP_ON));
	else if (*(config_data + mode * MODE_CMD_LEM + 1) == HANDSET_OUTPUT)
		memcpy(HANDSET_AMP_ON, config_data + mode * MODE_CMD_LEM + 2,
				sizeof(HANDSET_AMP_ON));
	else {
		pr_err("wrong mode id %d\n", mode);
		return -EINVAL;
	}
	return 0;
}
static int
tpa2026_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
	   unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int rc = 0, modeid = 0;
#if DEBUG
	int i = 0;
#endif
	unsigned char tmp[7];
	unsigned char reg_idx[1] = {0x01};
	unsigned char spk_cfg[8];
	unsigned char reg_value[2];
	struct tpa2026_config_data cfg;

	switch (cmd) {
	case TPA2026_WRITE_REG:
		pr_info("%s: TPA2026_WRITE_REG\n", __func__);
		mutex_lock(&spk_amp_lock_2026);
		if (!last_spkamp_state) {
			tpa2026pwr.output_value = 1;
			rc = pm8058_gpio_config(pdata->gpio_tpa2026_spk_en,
							&tpa2026pwr);

			/* According to tpa2026 Spec */
			mdelay(30);
		}
		if (copy_from_user(reg_value, argp, sizeof(reg_value)))
			goto err1;
		pr_info("%s: reg_value[0]=%2x, reg_value[1]=%2x\n", __func__,  \
				reg_value[0], reg_value[1]);
		rc = tpa2026_write_reg(reg_value[0], reg_value[1]);

err1:
		if (!last_spkamp_state) {
			tpa2026pwr.output_value = 0;
			pm8058_gpio_config(pdata->gpio_tpa2026_spk_en,
						&tpa2026pwr);
		}
		mutex_unlock(&spk_amp_lock_2026);
		break;
	case TPA2026_SET_CONFIG:
		if (copy_from_user(spk_cfg, argp, sizeof(spk_cfg)))
			return -EFAULT;
		if (spk_cfg[0] == SPKR_OUTPUT)
			memcpy(SPK_AMP_ON, spk_cfg + 1,
					sizeof(SPK_AMP_ON));
		else if (spk_cfg[0] == HEADSET_OUTPUT)
			memcpy(HEADSET_AMP_ON, spk_cfg + 1,
					sizeof(HEADSET_AMP_ON));
		else if (spk_cfg[0] == DUAL_OUTPUT)
			memcpy(RING_AMP_ON, spk_cfg + 1,
					sizeof(RING_AMP_ON));
		else
			return -EINVAL;
		break;
	case TPA2026_READ_CONFIG:
		mutex_lock(&spk_amp_lock_2026);
		if (!last_spkamp_state) {
			tpa2026pwr.output_value = 1;
			rc = pm8058_gpio_config(pdata->gpio_tpa2026_spk_en,
							&tpa2026pwr);

			/* According to tpa2026 Spec */
			mdelay(30);
		}
		rc = tpa2026_i2c_write_for_read(reg_idx, sizeof(reg_idx));
		if (rc < 0)
			goto err2;

		rc = tpa2026_i2c_read(tmp, sizeof(tmp));
		if (rc < 0)
			goto err2;

		if (copy_to_user(argp, &tmp, sizeof(tmp)))
			rc = -EFAULT;
err2:
		if (!last_spkamp_state) {
			tpa2026pwr.output_value = 0;
			pm8058_gpio_config(pdata->gpio_tpa2026_spk_en,
						&tpa2026pwr);
		}
		mutex_unlock(&spk_amp_lock_2026);
		break;
	case TPA2026_SET_MODE:
		if (copy_from_user(&modeid, argp, sizeof(modeid)))
			return -EFAULT;

		if (modeid > tpa2026_mode_cnt || modeid <= 0) {
			pr_err("unsupported tpa2026 mode %d\n", modeid);
			return -EINVAL;
		}
		rc = update_amp_parameter_2026(modeid);
		pr_info("set tpa2026 mode to %d\n", modeid);
		break;
	case TPA2026_SET_PARAM:
		cfg.cmd_data = 0;
		tpa2026_mode_cnt = 0;
		if (copy_from_user(&cfg, argp, sizeof(cfg))) {
			pr_err("%s: copy from user failed.\n", __func__);
			return -EFAULT;
		}

		if (cfg.data_len <= 0) {
			pr_err("%s: invalid data length %d\n",
					__func__, cfg.data_len);
			return -EINVAL;
		}

		config_data = kmalloc(cfg.data_len, GFP_KERNEL);
		if (!config_data) {
			pr_err("%s: out of memory\n", __func__);
			return -ENOMEM;
		}
		if (copy_from_user(config_data, cfg.cmd_data, cfg.data_len)) {
			pr_err("%s: copy data from user failed.\n", __func__);
			kfree(config_data);
			return -EFAULT;
		}
		tpa2026_mode_cnt = cfg.mode_num;
		pr_info("%s: update tpa2026 i2c commands #%d success.\n",
				__func__, cfg.data_len);
		/* update default paramater from csv*/
		update_amp_parameter_2026(TPA2026_MODE_PLAYBACK_SPKR);
		update_amp_parameter_2026(TPA2026_MODE_PLAYBACK_HEADSET);
		update_amp_parameter_2026(TPA2026_MODE_RING);
		update_amp_parameter_2026(TPA2026_MODE_HANDSET);
		rc = 0;
		break;
	case TPA2026_SET_MFG_LOOPBACK_ON:
		mutex_lock(&spk_amp_lock_2026);
		/* LOOPBACK mode left and right both ON */
		mfg_mode_2026 = 1;
		pr_info("%s: TPA2026_SET_MFG_LOOPBACK_ON\n", __func__);
		mutex_unlock(&spk_amp_lock_2026);
		break;
	case TPA2026_SET_MFG_LOOPBACK_OFF:
		mutex_lock(&spk_amp_lock_2026);
		/* LOOPBACK mode left and right both mute */
		mfg_mode_2026 = 2;
		pr_info("%s: TPA2026_SET_MFG_LOOPBACK_OFF\n", __func__);
		mutex_unlock(&spk_amp_lock_2026);
		break;
	case TPA2026_SET_MFG_LOOPBACK_L:
		mutex_lock(&spk_amp_lock_2026);
		/* LOOPBACK mode left on and right mute */
		mfg_mode_2026 = 3;
		pr_info("%s: TPA2026_SET_MFG_LOOPBACK_L\n", __func__);
		mutex_unlock(&spk_amp_lock_2026);
		break;
	case TPA2026_SET_MFG_LOOPBACK_R:
		mutex_lock(&spk_amp_lock_2026);
		/* LOOPBACK mode left mute and right on */
		mfg_mode_2026 = 4;
		pr_info("%s: TPA2026_SET_MFG_LOOPBACK_R\n", __func__);
		mutex_unlock(&spk_amp_lock_2026);
		break;
	case TPA2026_SET_MFG_LOOPBACK_STOP:
		mutex_lock(&spk_amp_lock_2026);
		/* LOOPBACK mode END */
		mfg_mode_2026 = 0;
		pr_info("%s: TPA2026_SET_MFG_LOOPBACK_STOP\n", __func__);
		mutex_unlock(&spk_amp_lock_2026);
		break;
	case TPA2026_RESET:
		tpa2026pwr.output_value = 0;
		rc = pm8058_gpio_config(pdata->gpio_tpa2026_spk_en, \
				&tpa2026pwr);
		pr_info("%s: TPA2026_RESET!\n", __func__);
		mdelay(100);
		if (last_spkamp_state) {
			pr_info("%s: after TPA2026_RESET, turn gpio on\n", \
					__func__);
			tpa2026pwr.output_value = 1;
			rc = pm8058_gpio_config( \
				pdata->gpio_tpa2026_spk_en,	&tpa2026pwr);
		}
		break;
	default:
		pr_err("%s: Invalid command\n", __func__);
		rc = -EINVAL;
		break;
	}

	return rc;
}

static struct file_operations tpa2026_fops = {
	.owner = THIS_MODULE,
	.open = tpa2026_open,
	.release = tpa2026_release,
	.ioctl = tpa2026_ioctl,
};

static struct miscdevice tpa2026_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "tpa2026",
	.fops = &tpa2026_fops,
};

int tpa2026_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret = 0;

	pr_info("%s\n", __func__);

	pdata = client->dev.platform_data;

	if (pdata == NULL) {
		pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
		if (pdata == NULL) {
			ret = -ENOMEM;
			pr_err("%s: platform data is NULL\n", __func__);
			goto err_alloc_data_failed;
		}
	}

	this_client = client;

	if (ret < 0) {
		pr_info("%s: pmic request aud_spk_en pin failed\n", __func__);
		goto err_free_gpio_all;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_info("%s: i2c check functionality error\n", __func__);
		ret = -ENODEV;
		goto err_free_gpio_all;
	}

	ret = misc_register(&tpa2026_device);
	if (ret) {
		pr_info("%s: tpa2026_device register failed\n", __func__);
		goto err_free_gpio_all;
	}

	if (pdata->spkr_cmd[1] != 0)  /* path id != 0 */
		memcpy(SPK_AMP_ON, pdata->spkr_cmd, sizeof(SPK_AMP_ON));
	if (pdata->hsed_cmd[1] != 0)
		memcpy(HEADSET_AMP_ON, pdata->hsed_cmd, sizeof(HEADSET_AMP_ON));
	if (pdata->rece_cmd[1] != 0)
		memcpy(HANDSET_AMP_ON, pdata->rece_cmd, sizeof(HANDSET_AMP_ON));

	return 0;

err_free_gpio_all:
	return ret;
err_alloc_data_failed:
	return ret;
}

static int tpa2026_remove(struct i2c_client *client)
{
	struct tpa2026_platform_data *p2026data = i2c_get_clientdata(client);
	kfree(p2026data);

	return 0;
}

static int tpa2026_suspend(struct i2c_client *client, pm_message_t mesg)
{
	return 0;
}

static int tpa2026_resume(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id tpa2026_id[] = {
	{ TPA2026_I2C_NAME, 0 },
	{ }
};

static struct i2c_driver tpa2026_driver = {
	.probe = tpa2026_probe,
	.remove = tpa2026_remove,
	.suspend = tpa2026_suspend,
	.resume = tpa2026_resume,
	.id_table = tpa2026_id,
	.driver = {
		.name = TPA2026_I2C_NAME,
	},
};

static int __init tpa2026_init(void)
{
	pr_info("%s\n", __func__);
	mutex_init(&spk_amp_lock_2026);
	mfg_mode_2026 = 0;
	return i2c_add_driver(&tpa2026_driver);
}

static void __exit tpa2026_exit(void)
{
	i2c_del_driver(&tpa2026_driver);
}

module_init(tpa2026_init);
module_exit(tpa2026_exit);

MODULE_DESCRIPTION("tpa2026 Speaker Amp driver");
MODULE_LICENSE("GPL");
