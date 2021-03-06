/******************************************************************************
 * console.c
 * 
 * Virtual console driver.
 * 
 * Copyright (c) 2002-2004, K A Fraser.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/major.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/sysrq.h>
#include <linux/vt.h>
#include <xen/interface/xen.h>
#include <xen/interface/event_channel.h>
#include "xencons.h"

/*
 * Modes:
 *  'xencons=off'  [XC_OFF]:     Console is disabled.
 *  'xencons=tty'  [XC_TTY]:     Console attached to '/dev/tty[0-9]+'.
 *  'xencons=ttyS' [XC_SERIAL]:  Console attached to '/dev/ttyS[0-9]+'.
 *  'xencons=xvc'  [XC_XVC]:     Console attached to '/dev/xvc0'.
 *  'xencons=hvc'  [XC_HVC]:     Console attached to '/dev/hvc0'.
 *  default:                     XC_XVC
 * 
 * NB. In mode XC_TTY, we create dummy consoles for tty2-63. This suppresses
 * warnings from standard distro startup scripts.
 */
static enum {
	XC_OFF, XC_TTY, XC_SERIAL, XC_XVC, XC_HVC
} xc_mode = XC_XVC;
static int xc_num = -1;

/* /dev/xvc0 device number allocated by lanana.org. */
#define XEN_XVC_MAJOR 204
#define XEN_XVC_MINOR 191

/* /dev/hvc0 device number */
#define XEN_HVC_MAJOR 229
#define XEN_HVC_MINOR 0

static int __init xencons_setup(char *str)
{
	char *q;
	int n;

	console_use_vt = -1;
	if (!strncmp(str, "ttyS", 4)) {
		xc_mode = XC_SERIAL;
		str += 4;
	} else if (!strncmp(str, "tty", 3)) {
		xc_mode = XC_TTY;
		str += 3;
		console_use_vt = 0;
	} else if (!strncmp(str, "xvc", 3)) {
		xc_mode = XC_XVC;
		str += 3;
	} else if (!strncmp(str, "hvc", 3)) {
		xc_mode = XC_HVC;
		str += 3;
	} else if (!strncmp(str, "off", 3)) {
		xc_mode = XC_OFF;
		str += 3;
	}

	n = simple_strtol(str, &q, 10);
	if (q != str)
		xc_num = n;

	return 1;
}
__setup("xencons=", xencons_setup);

/* The kernel and user-land drivers share a common transmit buffer. */
static unsigned int wbuf_size = 4096;
#define WBUF_MASK(_i) ((_i)&(wbuf_size-1))
static char *wbuf;
static unsigned int wc, wp; /* write_cons, write_prod */

static int __init xencons_bufsz_setup(char *str)
{
	unsigned int goal;
	goal = simple_strtoul(str, NULL, 0);
	if (goal) {
		goal = roundup_pow_of_two(goal);
		if (wbuf_size < goal)
			wbuf_size = goal;
	}
	return 1;
}
__setup("xencons_bufsz=", xencons_bufsz_setup);

/* This lock protects accesses to the common transmit buffer. */
static DEFINE_SPINLOCK(xencons_lock);

/* Common transmit-kick routine. */
static void __xencons_tx_flush(void);

static struct tty_driver *xencons_driver;

/******************** Kernel console driver ********************************/

static void kcons_write(struct console *c, const char *s, unsigned int count)
{
	int           i = 0;
	unsigned long flags;

	spin_lock_irqsave(&xencons_lock, flags);

	while (i < count) {
		for (; i < count; i++) {
			if ((wp - wc) >= (wbuf_size - 1))
				break;
			if ((wbuf[WBUF_MASK(wp++)] = s[i]) == '\n')
				wbuf[WBUF_MASK(wp++)] = '\r';
		}

		__xencons_tx_flush();
	}

	spin_unlock_irqrestore(&xencons_lock, flags);
}

static void kcons_write_dom0(struct console *c, const char *s, unsigned int count)
{

	while (count > 0) {
		int rc;
		rc = HYPERVISOR_console_io( CONSOLEIO_write, count, (char *)s);
		if (rc <= 0)
			break;
		count -= rc;
		s += rc;
	}
}

static struct tty_driver *kcons_device(struct console *c, int *index)
{
	*index = 0;
	return xencons_driver;
}

static struct console kcons_info = {
	.device	= kcons_device,
	.flags	= CON_PRINTBUFFER | CON_ENABLED,
	.index	= -1,
};

static int __init xen_console_init(void)
{
	if (!is_running_on_xen())
		return 0;

	if (is_initial_xendomain()) {
		kcons_info.write = kcons_write_dom0;
	} else {
		if (!xen_start_info->console.domU.evtchn)
			return 0;
		kcons_info.write = kcons_write;
	}

	switch (xc_mode) {
	case XC_XVC:
		strcpy(kcons_info.name, "xvc");
		if (xc_num == -1)
			xc_num = 0;
		break;

	case XC_HVC:
		strcpy(kcons_info.name, "hvc");
		if (xc_num == -1)
			xc_num = 0;
		break;

	case XC_SERIAL:
		strcpy(kcons_info.name, "ttyS");
		if (xc_num == -1)
			xc_num = 0;
		break;

	case XC_TTY:
		strcpy(kcons_info.name, "tty");
		if (xc_num == -1)
			xc_num = 1;
		break;

	default:
		return 0;
	}

	wbuf = kmalloc(wbuf_size, GFP_KERNEL);
	if (!wbuf) {
		xc_mode = XC_OFF;
		pr_err("Xen virtual console turned off - no memory\n");
		return 0;
	}

	if (console_use_vt <= 0 || !is_initial_xendomain())
		add_preferred_console(kcons_info.name, xc_num, NULL);
	else
		kcons_info.index = xc_num;
	register_console(&kcons_info);

	return 0;
}
console_initcall(xen_console_init);

#ifdef CONFIG_XEN_PRIVILEGED_GUEST
/*** Useful function for console debugging -- goes straight to Xen. ***/
int xprintk(const char *fmt, ...)
{
	va_list args;
	int printk_len;
	static char printk_buf[1024];

	/* Emit the output into the temporary buffer */
	va_start(args, fmt);
	printk_len = vsnprintf(printk_buf, sizeof(printk_buf), fmt, args);
	va_end(args);

	/* Send the processed output directly to Xen. */
	kcons_write_dom0(NULL, printk_buf, printk_len);

	return 0;
}
#endif

/*** Forcibly flush console data before dying. ***/
void xencons_force_flush(void)
{
	int sz;

	/* Emergency console is synchronous, so there's nothing to flush. */
	if (!is_running_on_xen() ||
	    is_initial_xendomain() ||
	    !xen_start_info->console.domU.evtchn)
		return;

	/* Spin until console data is flushed through to the daemon. */
	while (wc != wp) {
		int sent = 0;
		if ((sz = wp - wc) == 0)
			continue;
		sent = xencons_ring_send(&wbuf[WBUF_MASK(wc)], sz);
		if (sent > 0)
			wc += sent;
	}
}


#ifdef CONFIG_XEN_PRIVILEGED_GUEST
#include <linux/screen_info.h>

void __init dom0_init_screen_info(const struct dom0_vga_console_info *info, size_t size)
{
	/* This is drawn from a dump from vgacon:startup in
	 * standard Linux. */
	screen_info.orig_video_mode = 3;
	screen_info.orig_video_isVGA = 1;
	screen_info.orig_video_lines = 25;
	screen_info.orig_video_cols = 80;
	screen_info.orig_video_ega_bx = 3;
	screen_info.orig_video_points = 16;
	screen_info.orig_y = screen_info.orig_video_lines - 1;

	switch (info->video_type) {
	case XEN_VGATYPE_TEXT_MODE_3:
		if (size < offsetof(struct dom0_vga_console_info, u.text_mode_3)
		           + sizeof(info->u.text_mode_3))
			break;
		screen_info.orig_video_lines = info->u.text_mode_3.rows;
		screen_info.orig_video_cols = info->u.text_mode_3.columns;
		screen_info.orig_x = info->u.text_mode_3.cursor_x;
		screen_info.orig_y = info->u.text_mode_3.cursor_y;
		screen_info.orig_video_points =
			info->u.text_mode_3.font_height;
		break;

	case XEN_VGATYPE_VESA_LFB:
	case XEN_VGATYPE_EFI_LFB:
		if (size < offsetof(struct dom0_vga_console_info,
		                    u.vesa_lfb.gbl_caps))
			break;
		screen_info.orig_video_isVGA = VIDEO_TYPE_VLFB;
		screen_info.lfb_width = info->u.vesa_lfb.width;
		screen_info.lfb_height = info->u.vesa_lfb.height;
		screen_info.lfb_depth = info->u.vesa_lfb.bits_per_pixel;
		screen_info.lfb_base = info->u.vesa_lfb.lfb_base;
		screen_info.lfb_size = info->u.vesa_lfb.lfb_size;
		screen_info.lfb_linelength = info->u.vesa_lfb.bytes_per_line;
		screen_info.red_size = info->u.vesa_lfb.red_size;
		screen_info.red_pos = info->u.vesa_lfb.red_pos;
		screen_info.green_size = info->u.vesa_lfb.green_size;
		screen_info.green_pos = info->u.vesa_lfb.green_pos;
		screen_info.blue_size = info->u.vesa_lfb.blue_size;
		screen_info.blue_pos = info->u.vesa_lfb.blue_pos;
		screen_info.rsvd_size = info->u.vesa_lfb.rsvd_size;
		screen_info.rsvd_pos = info->u.vesa_lfb.rsvd_pos;
		if (info->video_type == XEN_VGATYPE_EFI_LFB) {
			screen_info.orig_video_isVGA = VIDEO_TYPE_EFI;
			break;
		}
		if (size >= offsetof(struct dom0_vga_console_info,
		                     u.vesa_lfb.gbl_caps)
		            + sizeof(info->u.vesa_lfb.gbl_caps))
			screen_info.capabilities = info->u.vesa_lfb.gbl_caps;
		if (size >= offsetof(struct dom0_vga_console_info,
		                     u.vesa_lfb.mode_attrs)
		            + sizeof(info->u.vesa_lfb.mode_attrs))
			screen_info.vesa_attributes = info->u.vesa_lfb.mode_attrs;
		break;
	}
}
#endif


/******************** User-space console driver (/dev/console) ************/

#define DRV(_d)         (_d)
#define DUMMY_TTY(_tty) ((xc_mode == XC_TTY) &&		\
			 ((_tty)->index != (xc_num - 1)))

static struct tty_port *xencons_ports;
static struct tty_struct *xencons_tty;
static int xencons_priv_irq;
static char x_char;

static const struct tty_port_operations xencons_port_ops = {};

void xencons_rx(char *buf, unsigned len)
{
	int           i;
	unsigned long flags;
	struct tty_port *port;

	spin_lock_irqsave(&xencons_lock, flags);
	if (xencons_tty == NULL)
		goto out;
	port = &xencons_ports[xencons_tty->index];

	for (i = 0; i < len; i++) {
#ifdef CONFIG_MAGIC_SYSRQ
		static unsigned long sysrq_requested;

		if (buf[i] == '\x0f') { /* ^O */
			if (!sysrq_requested) {
				sysrq_requested = jiffies;
				continue; /* don't print sysrq key */
			}
			sysrq_requested = 0;
		} else if (sysrq_requested) {
			unsigned long sysrq_timeout = sysrq_requested + HZ*2;

			sysrq_requested = 0;
			if (time_before(jiffies, sysrq_timeout)) {
				spin_unlock_irqrestore(&xencons_lock, flags);
				handle_sysrq(buf[i]);
				spin_lock_irqsave(&xencons_lock, flags);
				continue;
			}
		}
#endif
		tty_insert_flip_char(port, buf[i], 0);
	}
	tty_flip_buffer_push(port);

 out:
	spin_unlock_irqrestore(&xencons_lock, flags);
}

static void __xencons_tx_flush(void)
{
	int sent, sz, work_done = 0;

	if (x_char) {
		if (is_initial_xendomain())
			kcons_write_dom0(NULL, &x_char, 1);
		else
			while (x_char)
				if (xencons_ring_send(&x_char, 1) == 1)
					break;
		x_char = 0;
		work_done = 1;
	}

	while (wc != wp) {
		sz = wp - wc;
		if (sz > (wbuf_size - WBUF_MASK(wc)))
			sz = wbuf_size - WBUF_MASK(wc);
		if (is_initial_xendomain()) {
			kcons_write_dom0(NULL, &wbuf[WBUF_MASK(wc)], sz);
			wc += sz;
		} else {
			sent = xencons_ring_send(&wbuf[WBUF_MASK(wc)], sz);
			if (sent == 0)
				break;
			wc += sent;
		}
		work_done = 1;
	}

	if (work_done && (xencons_tty != NULL)) {
		wake_up_interruptible(&xencons_tty->write_wait);
		tty_wakeup(xencons_tty);
	}
}

void xencons_tx(void)
{
	unsigned long flags;

	spin_lock_irqsave(&xencons_lock, flags);
	__xencons_tx_flush();
	spin_unlock_irqrestore(&xencons_lock, flags);
}

/* Privileged receive callback and transmit kicker. */
static irqreturn_t xencons_priv_interrupt(int irq, void *dev_id)
{
	static char rbuf[16];
	int         l;

	while ((l = HYPERVISOR_console_io(CONSOLEIO_read, 16, rbuf)) > 0)
		xencons_rx(rbuf, l);

	xencons_tx();

	return IRQ_HANDLED;
}

static int xencons_write_room(struct tty_struct *tty)
{
	return wbuf_size - (wp - wc);
}

static int xencons_chars_in_buffer(struct tty_struct *tty)
{
	return wp - wc;
}

static void xencons_send_xchar(struct tty_struct *tty, char ch)
{
	unsigned long flags;

	if (DUMMY_TTY(tty))
		return;

	spin_lock_irqsave(&xencons_lock, flags);
	x_char = ch;
	__xencons_tx_flush();
	spin_unlock_irqrestore(&xencons_lock, flags);
}

static void xencons_throttle(struct tty_struct *tty)
{
	if (DUMMY_TTY(tty))
		return;

	if (I_IXOFF(tty))
		xencons_send_xchar(tty, STOP_CHAR(tty));
}

static void xencons_unthrottle(struct tty_struct *tty)
{
	if (DUMMY_TTY(tty))
		return;

	if (I_IXOFF(tty)) {
		if (x_char != 0)
			x_char = 0;
		else
			xencons_send_xchar(tty, START_CHAR(tty));
	}
}

static void xencons_flush_buffer(struct tty_struct *tty)
{
	unsigned long flags;

	if (DUMMY_TTY(tty))
		return;

	spin_lock_irqsave(&xencons_lock, flags);
	wc = wp = 0;
	spin_unlock_irqrestore(&xencons_lock, flags);
}

static inline int __xencons_put_char(int ch)
{
	char _ch = (char)ch;
	if ((wp - wc) == wbuf_size)
		return 0;
	wbuf[WBUF_MASK(wp++)] = _ch;
	return 1;
}

static int xencons_write(
	struct tty_struct *tty,
	const unsigned char *buf,
	int count)
{
	int i;
	unsigned long flags;

	if (DUMMY_TTY(tty))
		return count;

	spin_lock_irqsave(&xencons_lock, flags);

	for (i = 0; i < count; i++)
		if (!__xencons_put_char(buf[i]))
			break;

	if (i != 0)
		__xencons_tx_flush();

	spin_unlock_irqrestore(&xencons_lock, flags);

	return i;
}

static int xencons_put_char(struct tty_struct *tty, u_char ch)
{
	unsigned long flags;
	int ret;

	if (DUMMY_TTY(tty))
		return 0;

	spin_lock_irqsave(&xencons_lock, flags);
	ret = __xencons_put_char(ch);
	spin_unlock_irqrestore(&xencons_lock, flags);
	return ret;
}

static void xencons_flush_chars(struct tty_struct *tty)
{
	unsigned long flags;

	if (DUMMY_TTY(tty))
		return;

	spin_lock_irqsave(&xencons_lock, flags);
	__xencons_tx_flush();
	spin_unlock_irqrestore(&xencons_lock, flags);
}

static void xencons_wait_until_sent(struct tty_struct *tty, int timeout)
{
	unsigned long orig_jiffies = jiffies;

	if (DUMMY_TTY(tty))
		return;

	while (tty_chars_in_buffer(tty)) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(1);
		if (signal_pending(current))
			break;
		if (timeout && time_after(jiffies, orig_jiffies + timeout))
			break;
	}

	set_current_state(TASK_RUNNING);
}

static int xencons_install(struct tty_driver *drv, struct tty_struct *tty)
{
	struct tty_port *port = &xencons_ports[tty->index];
	int rc;

	tty->driver_data = NULL;

	tty_port_init(port);
	port->ops = &xencons_port_ops;

	rc = tty_port_install(port, drv, tty);
	if (rc)
		tty_port_put(port);
	return rc;
}

static int xencons_open(struct tty_struct *tty, struct file *filp)
{
	unsigned long flags;

	if (DUMMY_TTY(tty))
		return 0;

	spin_lock_irqsave(&xencons_lock, flags);
	if (xencons_tty == NULL)
		xencons_tty = tty;
	__xencons_tx_flush();
	spin_unlock_irqrestore(&xencons_lock, flags);

	return 0;
}

static void xencons_close(struct tty_struct *tty, struct file *filp)
{
	unsigned long flags;

	if (DUMMY_TTY(tty))
		return;

	/*
	 * Must follow lock nesting; callers are prepared for this
	 * (__tty_hangup) or don't care as they drop the lock right after our
	 * return (tty_release) in order to then acquire both in proper order.
	 */
	tty_unlock(tty);
	mutex_lock(&tty_mutex);
	tty_lock(tty);

	if (tty->count != 1) {
		mutex_unlock(&tty_mutex);
		return;
	}

	mutex_unlock(&tty_mutex);

	tty->closing = 1;
	tty_wait_until_sent(tty, 0);
	tty_driver_flush_buffer(tty);
	if (tty->ldisc->ops->flush_buffer)
		tty->ldisc->ops->flush_buffer(tty);
	tty->closing = 0;
	spin_lock_irqsave(&xencons_lock, flags);
	xencons_tty = NULL;
	spin_unlock_irqrestore(&xencons_lock, flags);
}

static const struct tty_operations xencons_ops = {
	.install = xencons_install,
	.open = xencons_open,
	.close = xencons_close,
	.write = xencons_write,
	.write_room = xencons_write_room,
	.put_char = xencons_put_char,
	.flush_chars = xencons_flush_chars,
	.chars_in_buffer = xencons_chars_in_buffer,
	.send_xchar = xencons_send_xchar,
	.flush_buffer = xencons_flush_buffer,
	.throttle = xencons_throttle,
	.unthrottle = xencons_unthrottle,
	.wait_until_sent = xencons_wait_until_sent,
};

static int __init xencons_init(void)
{
	int rc;

	if (!is_running_on_xen())
		return -ENODEV;

	if (xc_mode == XC_OFF)
		return 0;

	if (!is_initial_xendomain()) {
		rc = xencons_ring_init();
		if (rc)
			return rc;
	}

	rc = xc_mode == XC_TTY ? MAX_NR_CONSOLES : 1;
	xencons_ports = kcalloc(rc, sizeof(*xencons_ports), GFP_KERNEL);
	if (!xencons_ports)
		return -ENOMEM;
	xencons_driver = alloc_tty_driver(rc);
	if (!xencons_driver) {
		kfree(xencons_ports);
		xencons_ports = NULL;
		return -ENOMEM;
	}

	DRV(xencons_driver)->name            = "xencons";
	DRV(xencons_driver)->major           = TTY_MAJOR;
	DRV(xencons_driver)->type            = TTY_DRIVER_TYPE_SERIAL;
	DRV(xencons_driver)->subtype         = SERIAL_TYPE_NORMAL;
	DRV(xencons_driver)->init_termios    = tty_std_termios;
	DRV(xencons_driver)->flags           =
		TTY_DRIVER_REAL_RAW |
		TTY_DRIVER_RESET_TERMIOS;

	switch (xc_mode) {
	case XC_XVC:
		DRV(xencons_driver)->name        = "xvc";
		DRV(xencons_driver)->major       = XEN_XVC_MAJOR;
		DRV(xencons_driver)->minor_start = XEN_XVC_MINOR;
		DRV(xencons_driver)->name_base   = xc_num;
		break;
	case XC_HVC:
		DRV(xencons_driver)->name        = "hvc";
		DRV(xencons_driver)->major       = XEN_HVC_MAJOR;
		DRV(xencons_driver)->minor_start = XEN_HVC_MINOR;
		DRV(xencons_driver)->name_base   = xc_num;
		break;
	case XC_SERIAL:
		DRV(xencons_driver)->name        = "ttyS";
		DRV(xencons_driver)->minor_start = 64 + xc_num;
		DRV(xencons_driver)->name_base   = xc_num;
		break;
	default:
		DRV(xencons_driver)->name        = "tty";
		DRV(xencons_driver)->minor_start = 1;
		DRV(xencons_driver)->name_base   = 1;
		break;
	}

	tty_set_operations(xencons_driver, &xencons_ops);

	if ((rc = tty_register_driver(DRV(xencons_driver))) != 0) {
		pr_warning("WARNING: Failed to register Xen virtual "
			   "console driver as '%s%d'\n",
			   DRV(xencons_driver)->name,
			   DRV(xencons_driver)->name_base);
		put_tty_driver(xencons_driver);
		xencons_driver = NULL;
		kfree(xencons_ports);
		xencons_ports = NULL;
		return rc;
	}

	if (is_initial_xendomain()) {
		xencons_priv_irq = bind_virq_to_irqhandler(
			VIRQ_CONSOLE,
			0,
			xencons_priv_interrupt,
			0,
			"console",
			NULL);
		BUG_ON(xencons_priv_irq < 0);
	}

	pr_info("Xen virtual console successfully installed as %s%d\n",
		DRV(xencons_driver)->name, xc_num);

	return 0;
}

module_init(xencons_init);

MODULE_LICENSE("Dual BSD/GPL");
