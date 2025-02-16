// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/*
 * tty0tty - linux null modem emulator (module) for kernel > 3.8
 *
 * Copyright (c) : 2013  Luis Claudio Gambôa Lopes
 *
 *  Based on Tiny TTY driver -
 *         Copyright (C) 2002-2004 Greg Kroah-Hartman (greg@kroah.com)
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/sched.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/signal.h>
#endif
#include <linux/uaccess.h>

#define DRIVER_VERSION "v1.3"

//Default number of pairs of devices
short pairs = 4;
module_param(pairs, short, 0440);
MODULE_PARM_DESC(pairs,
		 "Number of pairs of devices to be created, maximum of 128");

#define TTY0TTY_MAJOR		0	/* dynamic allocation */
#define TTY0TTY_MINOR		0

/* fake UART values */
//out
#define MCR_DTR		0x01
#define MCR_RTS		0x02
#define MCR_LOOP	0x04
//in
#define MSR_CTS		0x10
#define MSR_CD		0x20
#define MSR_DSR		0x40
#define MSR_RI		0x80

static struct tty_port *tport;

struct tty0tty_serial {
	struct tty_struct *tty;	/* pointer to the tty for this device */
	int open_count;		/* number of times this port has been opened */
	struct semaphore sem;	/* locks this structure */

	/* for tiocmget and tiocmset functions */
	int msr;		/* MSR shadow */
	int mcr;		/* MCR shadow */

	/* for ioctl fun */
	struct serial_struct serial;
	wait_queue_head_t wait;
	struct async_icount icount;
};

static struct tty0tty_serial **tty0tty_table;	/* initially all NULL */

static struct tty0tty_serial *get_counterpart(struct tty0tty_serial *tts)
{
	int idx = tts->tty->index;
	int counterpart_idx = idx + 1;

	if (idx % 2)
		counterpart_idx = idx - 1;

	if (tty0tty_table[counterpart_idx] &&
				tty0tty_table[counterpart_idx]->open_count > 0)
		return tty0tty_table[counterpart_idx];

	return NULL;
}

static int tty0tty_open(struct tty_struct *tty, struct file *file)
{
	struct tty0tty_serial *tty0tty;
	int index;
	int msr = 0;
	int mcr = 0;
	struct tty0tty_serial *tts;

	dev_dbg(tty->dev, "%s -\n", __func__);

	/* initialize the pointer in case something fails */
	tty->driver_data = NULL;

	/* get the serial object associated with this tty pointer */
	index = tty->index;
	tty0tty = tty0tty_table[index];
	if (tty0tty == NULL) {
		/* first time accessing this device, let's create it */
		tty0tty = kzalloc(sizeof(*tty0tty), GFP_KERNEL);
		if (!tty0tty)
			return -ENOMEM;

		sema_init(&tty0tty->sem, 1);
		tty0tty->open_count = 0;

		tty0tty_table[index] = tty0tty;
	}

	tport[index].tty = tty;
	tty->port = &tport[index];

	tts = get_counterpart(tty0tty);
	if (tts)
		mcr = tts->mcr;

	//null modem connection

	if (mcr & MCR_RTS)
		msr |= MSR_CTS;

	if (mcr & MCR_DTR) {
		msr |= MSR_DSR;
		msr |= MSR_CD;
	}

	tty0tty->msr = msr;
	tty0tty->mcr = 0;

	/* register the tty driver */

	down(&tty0tty->sem);

	/* save our structure within the tty structure */
	tty->driver_data = tty0tty;
	tty0tty->tty = tty;

	++tty0tty->open_count;

	up(&tty0tty->sem);
	return 0;
}

static void do_close(struct tty0tty_serial *tty0tty)
{
	unsigned int msr = 0;

	struct tty0tty_serial *tts = get_counterpart(tty0tty);

	dev_dbg(tty0tty->tty->dev, "%s -\n", __func__);

	if (tts)
		tts->msr = msr;

	down(&tty0tty->sem);

	/* port was never opened */
	if (!tty0tty->open_count)
		goto exit;

	--tty0tty->open_count;
exit:
	up(&tty0tty->sem);
}

static void tty0tty_close(struct tty_struct *tty, struct file *file)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;

	dev_dbg(tty->dev, "%s -\n", __func__);
	if (tty0tty)
		do_close(tty0tty);
}

static int tty0tty_write(struct tty_struct *tty, const unsigned char *buffer,
			 int count)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;
	int retval = -EINVAL;
	struct tty_struct *ttyx = NULL;
	struct tty0tty_serial *tts;

	if (!tty0tty)
		return -ENODEV;

	down(&tty0tty->sem);

	/* port was not opened */
	if (!tty0tty->open_count)
		goto exit;

	tts = get_counterpart(tty0tty);

	if (tts)
		ttyx = tts->tty;

	//tty->low_latency=1;

	if (ttyx != NULL) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
		tty_insert_flip_string(ttyx->port, buffer, count);
		tty_flip_buffer_push(ttyx->port);
#else
		tty_insert_flip_string(ttyx, buffer, count);
		tty_flip_buffer_push(ttyx);
#endif
		retval = count;
	}

exit:
	up(&tty0tty->sem);
	return retval;
}

static int tty0tty_write_room(struct tty_struct *tty)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;
	int room = -EINVAL;

	if (!tty0tty)
		return -ENODEV;

	down(&tty0tty->sem);

	/* port was not opened */
	if (!tty0tty->open_count)
		goto exit;

	/* calculate how much room is left in the device */
	room = 255;

exit:
	up(&tty0tty->sem);
	return room;
}

#define RELEVANT_IFLAG(iflag) ((iflag) & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))

static void tty0tty_set_termios(struct tty_struct *tty,
				struct ktermios *old_termios)
{
	unsigned int cflag;
	unsigned int iflag;

	dev_dbg(tty->dev, "%s -\n", __func__);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0)
	cflag = tty->termios.c_cflag;
	iflag = tty->termios.c_iflag;
#else
	cflag = tty->termios->c_cflag;
	iflag = tty->termios->c_iflag;
#endif

	/* check that they really want us to change something */
	if (old_termios) {
		if ((cflag == old_termios->c_cflag) &&
		    (RELEVANT_IFLAG(iflag) ==
		     RELEVANT_IFLAG(old_termios->c_iflag))) {
			dev_dbg(tty->dev, " - nothing to change...\n");
			return;
		}
	}
	/* get the byte size */
	switch (cflag & CSIZE) {
	case CS5:
		dev_dbg(tty->dev, " - data bits = 5\n");
		break;
	case CS6:
		dev_dbg(tty->dev, " - data bits = 6\n");
		break;
	case CS7:
		dev_dbg(tty->dev, " - data bits = 7\n");
		break;
	default:
	case CS8:
		dev_dbg(tty->dev, " - data bits = 8\n");
		break;
	}

	/* determine the parity */
	if (cflag & PARENB)
		if (cflag & PARODD)
			dev_dbg(tty->dev, " - parity = odd\n");
		else
			dev_dbg(tty->dev, " - parity = even\n");
	else
		dev_dbg(tty->dev, " - parity = none\n");

	/* figure out the stop bits requested */
	if (cflag & CSTOPB)
		dev_dbg(tty->dev, " - stop bits = 2\n");
	else
		dev_dbg(tty->dev, " - stop bits = 1\n");

	/* figure out the hardware flow control settings */
	if (cflag & CRTSCTS)
		dev_dbg(tty->dev, " - RTS/CTS is enabled\n");
	else
		dev_dbg(tty->dev, " - RTS/CTS is disabled\n");

	/* determine software flow control */
	/* if we are implementing XON/XOFF, set the start and
	 * stop character in the device
	 */
	if (I_IXOFF(tty) || I_IXON(tty)) {
		unsigned char stop_char = STOP_CHAR(tty);
		unsigned char start_char = START_CHAR(tty);

		/* if we are implementing INBOUND XON/XOFF */
		if (I_IXOFF(tty))
			dev_dbg(tty->dev, " - INBOUND XON/XOFF is enabled, XON = %2x, XOFF = %2x\n",
					start_char, stop_char);
		else
			dev_dbg(tty->dev, " - INBOUND XON/XOFF is disabled\n");

		/* if we are implementing OUTBOUND XON/XOFF */
		if (I_IXON(tty))
			dev_dbg(tty->dev, " - OUTBOUND XON/XOFF is enabled, XON = %2x, XOFF = %2x\n",
					start_char, stop_char);
		else
			dev_dbg(tty->dev, " - OUTBOUND XON/XOFF is disabled\n");
	}

	/* get the baud rate wanted */
	dev_dbg(tty->dev, " - baud rate = %d\n", tty_get_baud_rate(tty));
}

static int tty0tty_tiocmget(struct tty_struct *tty)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;

	unsigned int result = 0;
	unsigned int msr = tty0tty->msr;
	unsigned int mcr = tty0tty->mcr;

	result = ((mcr & MCR_DTR) ? TIOCM_DTR : 0) |	/* DTR is set */
	    ((mcr & MCR_RTS) ? TIOCM_RTS : 0) |	/* RTS is set */
	    ((mcr & MCR_LOOP) ? TIOCM_LOOP : 0) |	/* LOOP is set */
	    ((msr & MSR_CTS) ? TIOCM_CTS : 0) |	/* CTS is set */
	    ((msr & MSR_CD) ? TIOCM_CAR : 0) |	/* Carrier detect is set */
	    ((msr & MSR_RI) ? TIOCM_RI : 0) |	/* Ring Indicator is set */
	    ((msr & MSR_DSR) ? TIOCM_DSR : 0);	/* DSR is set */

	return result;
}

static int tty0tty_tiocmset(struct tty_struct *tty,
			    unsigned int set, unsigned int clear)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;
	unsigned int mcr = tty0tty->mcr;
	unsigned int msr = 0;
	struct tty0tty_serial *tts = get_counterpart(tty0tty);

	if (tts)
		msr = tts->msr;

	dev_dbg(tty->dev, "%s -\n", __func__);

	//null modem connection

	if (set & TIOCM_RTS) {
		mcr |= MCR_RTS;
		msr |= MSR_CTS;
	}

	if (set & TIOCM_DTR) {
		mcr |= MCR_DTR;
		msr |= MSR_DSR;
		msr |= MSR_CD;
	}

	if (clear & TIOCM_RTS) {
		mcr &= ~MCR_RTS;
		msr &= ~MSR_CTS;
	}

	if (clear & TIOCM_DTR) {
		mcr &= ~MCR_DTR;
		msr &= ~MSR_DSR;
		msr &= ~MSR_CD;
	}

	/* set the new MCR value in the device */
	tty0tty->mcr = mcr;

	if (tts)
		tts->msr = msr;

	return 0;
}

static int tty0tty_ioctl_tiocgserial(struct tty_struct *tty,
				     unsigned int cmd, unsigned long arg)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;

	dev_dbg(tty->dev, "%s -\n", __func__);
	if (cmd == TIOCGSERIAL) {
		struct serial_struct tmp;

		if (!arg)
			return -EFAULT;

		memset(&tmp, 0, sizeof(tmp));

		tmp.type = tty0tty->serial.type;
		tmp.line = tty0tty->serial.line;
		tmp.port = tty0tty->serial.port;
		tmp.irq = tty0tty->serial.irq;
		tmp.flags = ASYNC_SKIP_TEST | ASYNC_AUTO_IRQ;
		tmp.xmit_fifo_size = tty0tty->serial.xmit_fifo_size;
		tmp.baud_base = tty0tty->serial.baud_base;
		tmp.close_delay = 5 * HZ;
		tmp.closing_wait = 30 * HZ;
		tmp.custom_divisor = tty0tty->serial.custom_divisor;
		tmp.hub6 = tty0tty->serial.hub6;
		tmp.io_type = tty0tty->serial.io_type;

		if (copy_to_user
		    ((void __user *)arg, &tmp, sizeof(struct serial_struct)))
			return -EFAULT;
		return 0;
	}
	return -ENOIOCTLCMD;
}

static int tty0tty_ioctl_tiocmiwait(struct tty_struct *tty,
				    unsigned int cmd, unsigned long arg)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;

	dev_dbg(tty->dev, "%s -\n", __func__);

	if (cmd == TIOCMIWAIT) {
		DECLARE_WAITQUEUE(wait, current);
		struct async_icount cnow;
		struct async_icount cprev;

		cprev = tty0tty->icount;
		while (1) {
			add_wait_queue(&tty0tty->wait, &wait);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			remove_wait_queue(&tty0tty->wait, &wait);

			/* see if a signal woke us up */
			if (signal_pending(current))
				return -ERESTARTSYS;

			cnow = tty0tty->icount;
			if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr &&
			    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts)
				return -EIO;	/* no change => error */
			if (((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) ||
			    ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) ||
			    ((arg & TIOCM_CD) && (cnow.dcd != cprev.dcd)) ||
			    ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts))) {
				return 0;
			}
			cprev = cnow;
		}
	}
	return -ENOIOCTLCMD;
}

static int tty0tty_ioctl_tiocgicount(struct tty_struct *tty,
				     unsigned int cmd, unsigned long arg)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;

	dev_dbg(tty->dev, "%s -\n", __func__);

	if (cmd == TIOCGICOUNT) {
		struct async_icount cnow = tty0tty->icount;
		struct serial_icounter_struct icount;

		icount.cts = cnow.cts;
		icount.dsr = cnow.dsr;
		icount.rng = cnow.rng;
		icount.dcd = cnow.dcd;
		icount.rx = cnow.rx;
		icount.tx = cnow.tx;
		icount.frame = cnow.frame;
		icount.overrun = cnow.overrun;
		icount.parity = cnow.parity;
		icount.brk = cnow.brk;
		icount.buf_overrun = cnow.buf_overrun;

		if (copy_to_user((void __user *)arg, &icount, sizeof(icount)))
			return -EFAULT;
		return 0;
	}
	return -ENOIOCTLCMD;
}

static int tty0tty_ioctl(struct tty_struct *tty,
			 unsigned int cmd, unsigned long arg)
{
	dev_dbg(tty->dev, "%s - %04X\n", __func__, cmd);

	switch (cmd) {
	case TIOCGSERIAL:
		return tty0tty_ioctl_tiocgserial(tty, cmd, arg);
	case TIOCMIWAIT:
		return tty0tty_ioctl_tiocmiwait(tty, cmd, arg);
	case TIOCGICOUNT:
		return tty0tty_ioctl_tiocgicount(tty, cmd, arg);
	}

	return -ENOIOCTLCMD;
}

static const struct tty_operations serial_ops = {
	.open = tty0tty_open,
	.close = tty0tty_close,
	.write = tty0tty_write,
	.write_room = tty0tty_write_room,
	.set_termios = tty0tty_set_termios,
	.tiocmget = tty0tty_tiocmget,
	.tiocmset = tty0tty_tiocmset,
	.ioctl = tty0tty_ioctl,
};

static struct tty_driver *tty0tty_tty_driver;

static int __init tty0tty_init(void)
{
	int retval;
	int i;

	pairs = clamp_val(pairs, 1, 128);
	tport = kmalloc(2 * pairs * sizeof(struct tty_port), GFP_KERNEL);
	tty0tty_table =
	    kmalloc(2 * pairs * sizeof(struct tty0tty_serial *), GFP_KERNEL);

	for (i = 0; i < 2 * pairs; i++)
		tty0tty_table[i] = NULL;

	pr_debug("%s -\n", __func__);

	/* allocate the tty driver */
	tty0tty_tty_driver = alloc_tty_driver(2 * pairs);
	if (!tty0tty_tty_driver)
		return -ENOMEM;

	/* initialize the tty driver */
	tty0tty_tty_driver->owner = THIS_MODULE;
	tty0tty_tty_driver->driver_name = "tty0tty";
	tty0tty_tty_driver->name = "tnt";
	/* no more devfs subsystem */
	tty0tty_tty_driver->major = TTY0TTY_MAJOR;
	tty0tty_tty_driver->minor_start = TTY0TTY_MINOR;
	tty0tty_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	tty0tty_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	tty0tty_tty_driver->flags =
	    TTY_DRIVER_RESET_TERMIOS | TTY_DRIVER_REAL_RAW;
	/* no more devfs subsystem */
	tty0tty_tty_driver->init_termios = tty_std_termios;
	tty0tty_tty_driver->init_termios.c_iflag = 0;
	tty0tty_tty_driver->init_termios.c_oflag = 0;
	tty0tty_tty_driver->init_termios.c_cflag = B38400 | CS8 | CREAD;
	tty0tty_tty_driver->init_termios.c_lflag = 0;
	tty0tty_tty_driver->init_termios.c_ispeed = 38400;
	tty0tty_tty_driver->init_termios.c_ospeed = 38400;

	tty_set_operations(tty0tty_tty_driver, &serial_ops);

	for (i = 0; i < 2 * pairs; i++) {
		tty_port_init(&tport[i]);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0)
		tty_port_link_device(&tport[i], tty0tty_tty_driver, i);
#endif
	}

	retval = tty_register_driver(tty0tty_tty_driver);
	if (retval) {
		pr_err("failed to register tty0tty tty driver");
		put_tty_driver(tty0tty_tty_driver);
		return retval;
	}

	pr_info(DRIVER_DESC " " DRIVER_VERSION "\n");
	return retval;
}

static void __exit tty0tty_exit(void)
{
	struct tty0tty_serial *tty0tty;
	int i;

	pr_debug("%s -\n", __func__);

	for (i = 0; i < 2 * pairs; ++i) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0)
		tty_port_destroy(&tport[i]);
#endif
		tty_unregister_device(tty0tty_tty_driver, i);
	}
	tty_unregister_driver(tty0tty_tty_driver);

	/* shut down all of the timers and free the memory */
	for (i = 0; i < 2 * pairs; ++i) {
		tty0tty = tty0tty_table[i];
		if (tty0tty) {
			/* close the port */
			while (tty0tty->open_count)
				do_close(tty0tty);

			/* shut down our timer and free the memory */
			kfree(tty0tty);
			tty0tty_table[i] = NULL;
		}
	}
	kfree(tport);
	kfree(tty0tty_table);
}

module_init(tty0tty_init);
module_exit(tty0tty_exit);

MODULE_AUTHOR("Luis Claudio Gamboa Lopes <lcgamboa@yahoo.com>");
MODULE_DESCRIPTION("tty0tty null modem driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
