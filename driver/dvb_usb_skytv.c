// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DVB USB driver for Skydigital SKYTV HD USB mini (USB 1c6c:04c9)
 *
 * Hardware: Cypress FX2 (CY7C68013, RAM-only firmware), LG LGDT3304 ATSC/QAM
 * demodulator @0x59, Xceive XC5000 tuner @0x61, Conexant CX25843 @0x44
 * (analog, unused here).
 *
 * The FX2 firmware (taken from the vendor Windows driver) speaks a small
 * command protocol on bulk EP1 OUT/IN; transport stream comes on bulk EP2 IN.
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/delay.h>
#include <linux/firmware.h>

#include "dvb_usb.h"
#include "lgdt3305.h"
#include "xc5000.h"

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

#define SKYTV_FIRMWARE		"dvb-usb-skytv-hdusbmini.fw"

#define SKYTV_EP_CMD_OUT	0x01
#define SKYTV_EP_CMD_IN		0x81
#define SKYTV_EP_TS		0x82

/* FX2 firmware commands (EP1 OUT) */
#define CMD_I2C_WRITE		0x08	/* [08 addr len data..]        -> [st]       */
#define CMD_I2C_WRRD		0x09	/* [09 wlen rlen addr wdata..] -> [st data..] */
#define CMD_PORT_READ		0x0d	/* [0d port]                   -> [val]      */
#define CMD_PORT_WRITE		0x0e	/* [0e port oe val]            -> [st]       */
#define CMD_STREAM_START	0x36	/* [36 03 00 clk]              (no reply)    */
#define CMD_STREAM_STOP		0x37	/* [37]                        (no reply)    */

#define I2C_STATUS_OK		0x08
#define EP1_MAX			64
#define I2C_MAX_WRITE		(EP1_MAX - 3)
#define I2C_MAX_WRRD_W		(EP1_MAX - 4)
#define I2C_MAX_READ		(EP1_MAX - 1)

/* FX2 GPIO ports used by the board */
#define PORT_A			0
#define PORT_D			3
#define PD_TUNER_PWR		6
#define PD_TUNER_RESET_N	1
#define PD_DEMOD_RESET_N	7

#define DEMOD_ADDR		0x59
#define TUNER_ADDR		0x61

static int skytv_debug;
module_param_named(debug, skytv_debug, int, 0644);
MODULE_PARM_DESC(debug, "enable debug messages");

struct skytv_state {
	u8 buf[EP1_MAX + 4];
	u8 oe[4];
	u8 val[4];
	int (*fe_set_frontend)(struct dvb_frontend *fe);
	int (*fe_init)(struct dvb_frontend *fe);
};

/* ---------------------------------------------------------------------- */
/* EP1 command transport                                                   */

static int skytv_cmd_locked(struct dvb_usb_device *d, const u8 *wbuf, int wlen,
			    u8 *rbuf, int rlen)
{
	struct skytv_state *st = d_to_priv(d);
	int ret, actual, try;

	if (wlen > EP1_MAX || rlen > EP1_MAX)
		return -EINVAL;

	for (try = 0; try < 4; try++) {
		memcpy(st->buf, wbuf, wlen);
		ret = usb_bulk_msg(d->udev,
				   usb_sndbulkpipe(d->udev, SKYTV_EP_CMD_OUT),
				   st->buf, wlen, &actual, 1000);
		if (ret)
			return ret;
		if (!rlen)
			return 0;

		/*
		 * The firmware occasionally drops the first reply after the
		 * device has been (re)opened; resend the command on timeout.
		 */
		ret = usb_bulk_msg(d->udev,
				   usb_rcvbulkpipe(d->udev, SKYTV_EP_CMD_IN),
				   st->buf, EP1_MAX, &actual, 300);
		if (ret == -ETIMEDOUT)
			continue;
		if (ret)
			return ret;
		if (actual < rlen)
			return -EIO;
		memcpy(rbuf, st->buf, rlen);
		return 0;
	}
	return -ETIMEDOUT;
}

static int skytv_cmd(struct dvb_usb_device *d, const u8 *wbuf, int wlen,
		     u8 *rbuf, int rlen)
{
	int ret;

	mutex_lock(&d->usb_mutex);
	ret = skytv_cmd_locked(d, wbuf, wlen, rbuf, rlen);
	mutex_unlock(&d->usb_mutex);
	if (ret && skytv_debug)
		dev_warn(&d->udev->dev, "cmd %02x failed: %d\n", wbuf[0], ret);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* GPIO                                                                    */

static int skytv_gpio(struct dvb_usb_device *d, int port, int bit, int on)
{
	struct skytv_state *st = d_to_priv(d);
	u8 b[4], r;

	st->oe[port] |= BIT(bit);
	if (on)
		st->val[port] |= BIT(bit);
	else
		st->val[port] &= ~BIT(bit);

	b[0] = CMD_PORT_WRITE;
	b[1] = port;
	b[2] = st->oe[port];
	b[3] = st->val[port];
	return skytv_cmd(d, b, 4, &r, 1);
}

static int skytv_port_read(struct dvb_usb_device *d, int port, u8 *val)
{
	u8 b[2] = { CMD_PORT_READ, port };

	return skytv_cmd(d, b, 2, val, 1);
}

/* ---------------------------------------------------------------------- */
/* I2C                                                                     */

static int skytv_i2c_write(struct dvb_usb_device *d, u8 addr, const u8 *data,
			   int len)
{
	u8 b[EP1_MAX], r;
	int ret;

	if (len > I2C_MAX_WRITE)
		return -EOPNOTSUPP;
	b[0] = CMD_I2C_WRITE;
	b[1] = addr;
	b[2] = len;
	memcpy(&b[3], data, len);
	ret = skytv_cmd(d, b, len + 3, &r, 1);
	if (ret)
		return ret;
	return r == I2C_STATUS_OK ? 0 : -EREMOTEIO;
}

static int skytv_i2c_wrrd(struct dvb_usb_device *d, u8 addr, const u8 *wdata,
			  int wlen, u8 *rdata, int rlen)
{
	u8 b[EP1_MAX], r[EP1_MAX];
	int ret;

	if (wlen > I2C_MAX_WRRD_W || rlen > I2C_MAX_READ)
		return -EOPNOTSUPP;
	b[0] = CMD_I2C_WRRD;
	b[1] = wlen;
	b[2] = rlen;
	b[3] = addr;
	if (wlen)
		memcpy(&b[4], wdata, wlen);
	ret = skytv_cmd(d, b, wlen + 4, r, rlen + 1);
	if (ret)
		return ret;
	if (r[0] != I2C_STATUS_OK)
		return -EREMOTEIO;
	memcpy(rdata, &r[1], rlen);
	return 0;
}

/*
 * The FX2 EP1 buffer is 64 bytes, but xc5000 sends up to 64-byte firmware
 * chunks. Each xc5000 chunk starts with a 2-byte address header that the
 * xc5000 driver itself repeats when it splits; do the same for longer writes.
 */
static int skytv_i2c_write_split(struct dvb_usb_device *d, u8 addr,
				 const u8 *data, int len)
{
	u8 b[I2C_MAX_WRITE];
	int pos, n, ret;

	if (len <= I2C_MAX_WRITE)
		return skytv_i2c_write(d, addr, data, len);
	if (addr != TUNER_ADDR || len < 3)
		return -EOPNOTSUPP;

	b[0] = data[0];
	b[1] = data[1];
	for (pos = 2; pos < len; pos += n) {
		n = min(len - pos, I2C_MAX_WRITE - 2);
		memcpy(&b[2], &data[pos], n);
		ret = skytv_i2c_write(d, addr, b, n + 2);
		if (ret)
			return ret;
	}
	return 0;
}

static int skytv_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msg[],
			  int num)
{
	struct dvb_usb_device *d = i2c_get_adapdata(adap);
	int i = 0, ret = 0;

	if (mutex_lock_interruptible(&d->i2c_mutex) < 0)
		return -EAGAIN;

	while (i < num) {
		if (i + 1 < num && !(msg[i].flags & I2C_M_RD) &&
		    (msg[i + 1].flags & I2C_M_RD) &&
		    msg[i].addr == msg[i + 1].addr) {
			ret = skytv_i2c_wrrd(d, msg[i].addr, msg[i].buf,
					     msg[i].len, msg[i + 1].buf,
					     msg[i + 1].len);
			i += 2;
		} else if (msg[i].flags & I2C_M_RD) {
			ret = skytv_i2c_wrrd(d, msg[i].addr, NULL, 0,
					     msg[i].buf, msg[i].len);
			i++;
		} else {
			ret = skytv_i2c_write_split(d, msg[i].addr, msg[i].buf,
						    msg[i].len);
			i++;
		}
		if (ret)
			break;
	}

	mutex_unlock(&d->i2c_mutex);
	return ret ? ret : num;
}

static u32 skytv_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm skytv_i2c_algo = {
	.master_xfer   = skytv_i2c_xfer,
	.functionality = skytv_i2c_func,
};

/* ---------------------------------------------------------------------- */
/* firmware / power                                                        */

static int skytv_identify_state(struct dvb_usb_device *d, const char **name)
{
	/* the boot ROM descriptor has no strings; the firmware sets iManufacturer */
	return d->udev->descriptor.iManufacturer ? WARM : COLD;
}

static int skytv_download_firmware(struct dvb_usb_device *d,
				   const struct firmware *fw)
{
	u8 *buf;
	size_t pos, n;
	int ret;

	buf = kmalloc(64, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = 1;		/* CPUCS: hold 8051 in reset */
	ret = usb_control_msg(d->udev, usb_sndctrlpipe(d->udev, 0), 0xa0,
			      USB_TYPE_VENDOR | USB_DIR_OUT, 0xe600, 0, buf, 1,
			      1000);
	for (pos = 0; ret >= 0 && pos < fw->size; pos += n) {
		n = min_t(size_t, 64, fw->size - pos);
		memcpy(buf, fw->data + pos, n);
		ret = usb_control_msg(d->udev, usb_sndctrlpipe(d->udev, 0),
				      0xa0, USB_TYPE_VENDOR | USB_DIR_OUT, pos,
				      0, buf, n, 1000);
	}
	if (ret >= 0) {
		buf[0] = 0;	/* release reset; firmware re-enumerates */
		ret = usb_control_msg(d->udev, usb_sndctrlpipe(d->udev, 0),
				      0xa0, USB_TYPE_VENDOR | USB_DIR_OUT,
				      0xe600, 0, buf, 1, 1000);
	}
	kfree(buf);

	if (ret < 0) {
		dev_err(&d->udev->dev, "firmware download failed: %d\n", ret);
		return ret;
	}
	return RECONNECTS_USB;
}

static int skytv_read_config(struct dvb_usb_device *d)
{
	struct skytv_state *st = d_to_priv(d);
	int ret;

	ret = skytv_port_read(d, PORT_A, &st->val[PORT_A]);
	if (!ret)
		ret = skytv_port_read(d, PORT_D, &st->val[PORT_D]);
	if (ret)
		return ret;

	/* tuner power + out of reset, then pulse demod reset */
	ret = skytv_gpio(d, PORT_D, PD_TUNER_PWR, 1);
	if (!ret)
		ret = skytv_gpio(d, PORT_D, PD_TUNER_RESET_N, 1);
	msleep(50);
	if (!ret)
		ret = skytv_gpio(d, PORT_D, PD_DEMOD_RESET_N, 0);
	msleep(10);
	if (!ret)
		ret = skytv_gpio(d, PORT_D, PD_DEMOD_RESET_N, 1);
	msleep(50);
	return ret;
}

static int skytv_tuner_callback(void *priv, int component, int cmd, int arg)
{
	struct dvb_usb_adapter *adap = priv;
	struct dvb_usb_device *d = adap_to_d(adap);
	int ret;

	if (component != DVB_FRONTEND_COMPONENT_TUNER || cmd != XC5000_TUNER_RESET)
		return -EINVAL;

	ret = skytv_gpio(d, PORT_D, PD_TUNER_RESET_N, 0);
	msleep(10);
	if (!ret)
		ret = skytv_gpio(d, PORT_D, PD_TUNER_RESET_N, 1);
	msleep(10);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* frontend                                                                */

static const struct lgdt3305_config skytv_lgdt3304_config = {
	.i2c_addr           = DEMOD_ADDR,
	.demod_chip         = LGDT3304,
	.deny_i2c_rptr      = 1,
	.spectral_inversion = 0,
	.rf_agc_loop        = 0,
	.mpeg_mode          = LGDT3305_MPEG_PARALLEL,
	.tpclk_edge         = LGDT3305_TPCLK_FALLING_EDGE,
	.tpclk_mode         = LGDT3305_TPCLK_GATED,
	.tpvalid_polarity   = LGDT3305_TP_VALID_HIGH,
	.vsb_if_khz         = 6000,
	.qam_if_khz         = 6000,
};

static const struct xc5000_config skytv_xc5000_config = {
	.i2c_address = TUNER_ADDR,
	.if_khz      = 6000,
	.chip_id     = XC5000A,
};

static int skytv_demod_wr(struct dvb_usb_device *d, u16 reg, u8 val)
{
	u8 b[3] = { reg >> 8, reg & 0xff, val };

	return skytv_i2c_write(d, DEMOD_ADDR, b, 3);
}

static int skytv_demod_rd(struct dvb_usb_device *d, u16 reg, u8 *val)
{
	u8 b[2] = { reg >> 8, reg & 0xff };

	return skytv_i2c_wrrd(d, DEMOD_ADDR, b, 2, val, 1);
}

/*
 * lgdt3305.c programs the LGDT3304 for the KWorld 340U (fixed VSB carrier
 * NCO, and TP_CTRL_1 bit6 left set from the init table). On this board the
 * XC5000 runs at a 6 MHz IF and the TS port must stay in gated-clock mode,
 * as the vendor driver leaves it, so fix those up after every tune.
 */
static int skytv_set_frontend(struct dvb_frontend *fe)
{
	struct dvb_usb_device *d = fe_to_d(fe);
	struct skytv_state *st = d_to_priv(d);
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	u8 v;
	int ret, i;

	ret = st->fe_set_frontend(fe);
	if (ret)
		return ret;

	mutex_lock(&d->i2c_mutex);
	if (c->modulation == VSB_8) {
		for (i = 0; i < 4 && !ret; i++)
			ret = skytv_demod_wr(d, 0x0106 + i, 0x00);
	}
	if (!ret)
		ret = skytv_demod_wr(d, 0x050e, 0x1b);
	/* soft reset: GEN_CTRL_3 bit0 */
	if (!ret)
		ret = skytv_demod_rd(d, 0x0002, &v);
	if (!ret)
		ret = skytv_demod_wr(d, 0x0002, v & ~0x01);
	msleep(20);
	if (!ret)
		ret = skytv_demod_wr(d, 0x0002, v | 0x01);
	mutex_unlock(&d->i2c_mutex);

	return ret;
}

/*
 * lgdt3305_sleep() tristates the AGC pins (GEN_CTRL_3 bits 1-2) and turns off
 * the ADC/VSB blocks (GEN_CTRL_4 bits 0-1), but the LGDT3304 init table never
 * turns them back on, so the demod stays dead after the first sleep.
 */
static int skytv_fe_init(struct dvb_frontend *fe)
{
	struct dvb_usb_device *d = fe_to_d(fe);
	struct skytv_state *st = d_to_priv(d);
	u8 v;
	int ret;

	ret = st->fe_init(fe);
	if (ret)
		return ret;

	mutex_lock(&d->i2c_mutex);
	ret = skytv_demod_rd(d, 0x0002, &v);
	if (!ret)
		ret = skytv_demod_wr(d, 0x0002, v & ~0x06);
	if (!ret)
		ret = skytv_demod_rd(d, 0x000a, &v);
	if (!ret)
		ret = skytv_demod_wr(d, 0x000a, v | 0x03);
	mutex_unlock(&d->i2c_mutex);

	return ret;
}

static int skytv_frontend_attach(struct dvb_usb_adapter *adap)
{
	struct dvb_usb_device *d = adap_to_d(adap);
	struct skytv_state *st = d_to_priv(d);

	adap->fe[0] = dvb_attach(lgdt3305_attach, &skytv_lgdt3304_config,
				 &d->i2c_adap);
	if (!adap->fe[0])
		return -ENODEV;

	/*
	 * lgdt3305 advertises 54-858 MHz; the XC5000 covers up to 1023 MHz and
	 * the in-house cable plan goes to 861 MHz (CATV ch 135).
	 */
	adap->fe[0]->ops.info.frequency_max_hz = 1002 * 1000000;

	adap->fe[0]->callback = skytv_tuner_callback;
	st->fe_set_frontend = adap->fe[0]->ops.set_frontend;
	adap->fe[0]->ops.set_frontend = skytv_set_frontend;
	st->fe_init = adap->fe[0]->ops.init;
	adap->fe[0]->ops.init = skytv_fe_init;
	return 0;
}

static int skytv_tuner_attach(struct dvb_usb_adapter *adap)
{
	struct dvb_usb_device *d = adap_to_d(adap);

	if (!dvb_attach(xc5000_attach, adap->fe[0], &d->i2c_adap,
			&skytv_xc5000_config))
		return -ENODEV;
	return 0;
}

static int skytv_streaming_ctrl(struct dvb_frontend *fe, int onoff)
{
	struct dvb_usb_device *d = fe_to_d(fe);
	/* clk=1: FX2 internal IFCLK, async slave FIFO (what works with LGDT3304) */
	u8 start[4] = { CMD_STREAM_START, 0x03, 0x00, 0x01 };
	u8 stop[1] = { CMD_STREAM_STOP };

	if (onoff)
		return skytv_cmd(d, start, sizeof(start), NULL, 0);
	return skytv_cmd(d, stop, sizeof(stop), NULL, 0);
}

static struct dvb_usb_device_properties skytv_props = {
	.driver_name = KBUILD_MODNAME,
	.owner = THIS_MODULE,
	.adapter_nr = adapter_nr,
	.size_of_priv = sizeof(struct skytv_state),

	.identify_state = skytv_identify_state,
	.firmware = SKYTV_FIRMWARE,
	.download_firmware = skytv_download_firmware,

	.i2c_algo = &skytv_i2c_algo,
	.read_config = skytv_read_config,
	.frontend_attach = skytv_frontend_attach,
	.tuner_attach = skytv_tuner_attach,
	.streaming_ctrl = skytv_streaming_ctrl,

	.num_adapters = 1,
	.adapter = {
		{
			.stream = DVB_USB_STREAM_BULK(SKYTV_EP_TS, 8, 32 * 512),
		},
	},
};

static const struct usb_device_id skytv_id_table[] = {
	{ DVB_USB_DEVICE(0x1c6c, 0x04c9, &skytv_props,
			 "Skydigital SKYTV HD USB mini", NULL) },
	{ }
};
MODULE_DEVICE_TABLE(usb, skytv_id_table);

static struct usb_driver skytv_usb_driver = {
	.name = KBUILD_MODNAME,
	.id_table = skytv_id_table,
	.probe = dvb_usbv2_probe,
	.disconnect = dvb_usbv2_disconnect,
	.suspend = dvb_usbv2_suspend,
	.resume = dvb_usbv2_resume,
	.reset_resume = dvb_usbv2_reset_resume,
	.no_dynamic_id = 1,
	.soft_unbind = 1,
};

module_usb_driver(skytv_usb_driver);

MODULE_DESCRIPTION("Skydigital SKYTV HD USB mini ATSC driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(SKYTV_FIRMWARE);
