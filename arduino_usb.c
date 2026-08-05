#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/usb.h>

#define USB_ARD_MINOR_BASE	192
#define MAX_TRANSFER		(PAGE_SIZE - 512)
#define WRITES_IN_FLIGHT	8

/*
 * Replace or extend these IDs with the VID/PID shown by `lsusb` for your board.
 * Many classic Arduino boards expose CDC ACM serial interfaces and are already
 * handled by cdc_acm; this driver is intended for boards/firmware exposing bulk
 * IN and bulk OUT endpoints.
 */
#define USB_ARD_VENDOR_ID	0x2341
#define USB_ARD_PRODUCT_ID	0x0043

static const struct usb_device_id ard_table[] = {
	{ USB_DEVICE(USB_ARD_VENDOR_ID, USB_ARD_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, ard_table);

struct ard_usb {
	struct usb_device	*udev;
	struct usb_interface	*interface;
	struct semaphore	limit_sem;
	struct usb_anchor	submitted;
	struct urb		*bulk_in_urb;
	unsigned char		*bulk_in_buffer;
	size_t			bulk_in_size;
	size_t			bulk_in_filled;
	size_t			bulk_in_copied;
	__u8			bulk_in_endpoint_addr;
	__u8			bulk_out_endpoint_addr;
	int			errors;
	bool			ongoing_read;
	spinlock_t		err_lock;
	struct kref		kref;
	struct mutex		io_mutex;
	unsigned long		disconnected:1;
	wait_queue_head_t	bulk_in_wait;
	dma_addr_t		bulk_in_dma;
};

#define to_ard_dev(d) container_of(d, struct ard_usb, kref)

static struct usb_driver ard_driver;
static void ard_draw_down(struct ard_usb *dev);

static void ard_delete(struct kref *kref)
{
	struct ard_usb *dev = to_ard_dev(kref);

	usb_free_urb(dev->bulk_in_urb);
	if (dev->bulk_in_buffer)
		usb_free_coherent(dev->udev, dev->bulk_in_size,
				  dev->bulk_in_buffer, dev->bulk_in_dma);
	usb_put_intf(dev->interface);
	usb_put_dev(dev->udev);
	kfree(dev);
}

static int ard_open(struct inode *inode, struct file *file)
{
	struct ard_usb *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&ard_driver, subminor);
	if (!interface) {
		pr_err("%s - cannot find device for minor %d\n",
		       __func__, subminor);
		return -ENODEV;
	}

	dev = usb_get_intfdata(interface);
	if (!dev)
		return -ENODEV;

	retval = usb_autopm_get_interface(interface);
	if (retval)
		return retval;

	kref_get(&dev->kref);
	file->private_data = dev;

	return 0;
}

static int ard_release(struct inode *inode, struct file *file)
{
	struct ard_usb *dev = file->private_data;

	if (!dev)
		return -ENODEV;

	usb_autopm_put_interface(dev->interface);
	kref_put(&dev->kref, ard_delete);

	return 0;
}

static int ard_flush(struct file *file, fl_owner_t id)
{
	struct ard_usb *dev = file->private_data;
	int res;

	if (!dev)
		return -ENODEV;

	mutex_lock(&dev->io_mutex);
	ard_draw_down(dev);

	spin_lock_irq(&dev->err_lock);
	res = dev->errors ? (dev->errors == -EPIPE ? -EPIPE : -EIO) : 0;
	dev->errors = 0;
	spin_unlock_irq(&dev->err_lock);

	mutex_unlock(&dev->io_mutex);

	return res;
}

static void ard_read_bulk_callback(struct urb *urb)
{
	struct ard_usb *dev = urb->context;
	unsigned long flags;

	spin_lock_irqsave(&dev->err_lock, flags);
	if (urb->status) {
		if (urb->status != -ENOENT &&
		    urb->status != -ECONNRESET &&
		    urb->status != -ESHUTDOWN)
			dev_err(&dev->interface->dev,
				"%s - nonzero read bulk status: %d\n",
				__func__, urb->status);

		dev->errors = urb->status;
	} else {
		dev->bulk_in_filled = urb->actual_length;
	}
	dev->ongoing_read = false;
	spin_unlock_irqrestore(&dev->err_lock, flags);

	wake_up_interruptible(&dev->bulk_in_wait);
}

static int ard_do_read_io(struct ard_usb *dev, size_t count)
{
	int rv;

	usb_fill_bulk_urb(dev->bulk_in_urb,
			  dev->udev,
			  usb_rcvbulkpipe(dev->udev,
					  dev->bulk_in_endpoint_addr),
			  dev->bulk_in_buffer,
			  min(dev->bulk_in_size, count),
			  ard_read_bulk_callback,
			  dev);
	dev->bulk_in_urb->transfer_dma = dev->bulk_in_dma;
	dev->bulk_in_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	spin_lock_irq(&dev->err_lock);
	dev->ongoing_read = true;
	spin_unlock_irq(&dev->err_lock);

	dev->bulk_in_filled = 0;
	dev->bulk_in_copied = 0;

	rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (rv < 0) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting read urb, error %d\n",
			__func__, rv);
		rv = (rv == -ENOMEM) ? rv : -EIO;
		spin_lock_irq(&dev->err_lock);
		dev->ongoing_read = false;
		spin_unlock_irq(&dev->err_lock);
	}

	return rv;
}

static ssize_t ard_read(struct file *file, char __user *buffer, size_t count,
			loff_t *ppos)
{
	struct ard_usb *dev = file->private_data;
	int rv;
	bool ongoing_io;

	if (!count)
		return 0;

	rv = mutex_lock_interruptible(&dev->io_mutex);
	if (rv < 0)
		return rv;

	if (dev->disconnected) {
		rv = -ENODEV;
		goto exit;
	}

retry:
	spin_lock_irq(&dev->err_lock);
	ongoing_io = dev->ongoing_read;
	spin_unlock_irq(&dev->err_lock);

	if (ongoing_io) {
		if (file->f_flags & O_NONBLOCK) {
			rv = -EAGAIN;
			goto exit;
		}

		rv = wait_event_interruptible(dev->bulk_in_wait,
					      !dev->ongoing_read);
		if (rv < 0)
			goto exit;
	}

	spin_lock_irq(&dev->err_lock);
	rv = dev->errors;
	if (rv < 0)
		dev->errors = 0;
	spin_unlock_irq(&dev->err_lock);
	if (rv < 0) {
		rv = (rv == -EPIPE) ? rv : -EIO;
		goto exit;
	}

	if (dev->bulk_in_filled) {
		size_t available = dev->bulk_in_filled - dev->bulk_in_copied;
		size_t chunk = min(available, count);

		if (!available) {
			rv = ard_do_read_io(dev, count);
			if (rv < 0)
				goto exit;
			goto retry;
		}

		if (copy_to_user(buffer,
				 dev->bulk_in_buffer + dev->bulk_in_copied,
				 chunk))
			rv = -EFAULT;
		else
			rv = chunk;

		dev->bulk_in_copied += chunk;

		if (available < count)
			ard_do_read_io(dev, count - chunk);
	} else {
		rv = ard_do_read_io(dev, count);
		if (rv < 0)
			goto exit;
		goto retry;
	}

exit:
	mutex_unlock(&dev->io_mutex);
	return rv;
}

static void ard_write_bulk_callback(struct urb *urb)
{
	struct ard_usb *dev = urb->context;
	unsigned long flags;

	if (urb->status) {
		if (urb->status != -ENOENT &&
		    urb->status != -ECONNRESET &&
		    urb->status != -ESHUTDOWN)
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status: %d\n",
				__func__, urb->status);

		spin_lock_irqsave(&dev->err_lock, flags);
		dev->errors = urb->status;
		spin_unlock_irqrestore(&dev->err_lock, flags);
	}

	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);
	up(&dev->limit_sem);
}

static ssize_t ard_write(struct file *file, const char __user *user_buffer,
			 size_t count, loff_t *ppos)
{
	struct ard_usb *dev = file->private_data;
	struct urb *urb = NULL;
	char *buf = NULL;
	size_t writesize = min_t(size_t, count, MAX_TRANSFER);
	int retval = 0;

	if (!count)
		return 0;

	if (file->f_flags & O_NONBLOCK) {
		if (down_trylock(&dev->limit_sem))
			return -EAGAIN;
	} else {
		if (down_interruptible(&dev->limit_sem))
			return -ERESTARTSYS;
	}

	spin_lock_irq(&dev->err_lock);
	retval = dev->errors;
	if (retval < 0)
		dev->errors = 0;
	spin_unlock_irq(&dev->err_lock);
	if (retval < 0) {
		retval = (retval == -EPIPE) ? retval : -EIO;
		goto error;
	}

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(dev->udev, writesize, GFP_KERNEL,
				 &urb->transfer_dma);
	if (!buf) {
		retval = -ENOMEM;
		goto error;
	}

	if (copy_from_user(buf, user_buffer, writesize)) {
		retval = -EFAULT;
		goto error;
	}

	mutex_lock(&dev->io_mutex);
	if (dev->disconnected) {
		mutex_unlock(&dev->io_mutex);
		retval = -ENODEV;
		goto error;
	}

	usb_fill_bulk_urb(urb, dev->udev,
			  usb_sndbulkpipe(dev->udev,
					  dev->bulk_out_endpoint_addr),
			  buf, writesize, ard_write_bulk_callback, dev);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &dev->submitted);

	retval = usb_submit_urb(urb, GFP_KERNEL);
	mutex_unlock(&dev->io_mutex);
	if (retval) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting write urb, error %d\n",
			__func__, retval);
		goto error_unanchor;
	}

	usb_free_urb(urb);

	return writesize;

error_unanchor:
	usb_unanchor_urb(urb);
error:
	if (urb) {
		usb_free_coherent(dev->udev, writesize, buf, urb->transfer_dma);
		usb_free_urb(urb);
	}
	up(&dev->limit_sem);

	return retval;
}

static const struct file_operations ard_fops = {
	.owner =	THIS_MODULE,
	.read =		ard_read,
	.write =	ard_write,
	.open =		ard_open,
	.release =	ard_release,
	.flush =	ard_flush,
	.llseek =	noop_llseek,
};

static struct usb_class_driver ard_class = {
	.name =		"ard%d",
	.fops =		&ard_fops,
	.minor_base =	USB_ARD_MINOR_BASE,
};

static int ard_probe(struct usb_interface *interface,
		     const struct usb_device_id *id)
{
	struct ard_usb *dev;
	struct usb_endpoint_descriptor *bulk_in;
	struct usb_endpoint_descriptor *bulk_out;
	int retval;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->kref);
	sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
	mutex_init(&dev->io_mutex);
	spin_lock_init(&dev->err_lock);
	init_usb_anchor(&dev->submitted);
	init_waitqueue_head(&dev->bulk_in_wait);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = usb_get_intf(interface);

	retval = usb_find_common_endpoints(interface->cur_altsetting,
					   &bulk_in, &bulk_out, NULL, NULL);
	if (retval) {
		dev_err(&interface->dev,
			"could not find both bulk-in and bulk-out endpoints\n");
		goto error;
	}

	dev->bulk_in_size = usb_endpoint_maxp(bulk_in);
	dev->bulk_in_endpoint_addr = bulk_in->bEndpointAddress;
	dev->bulk_out_endpoint_addr = bulk_out->bEndpointAddress;

	dev->bulk_in_buffer = usb_alloc_coherent(dev->udev, dev->bulk_in_size,
						 GFP_KERNEL, &dev->bulk_in_dma);
	if (!dev->bulk_in_buffer) {
		retval = -ENOMEM;
		goto error;
	}

	dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_in_urb) {
		retval = -ENOMEM;
		goto error;
	}

	usb_set_intfdata(interface, dev);

	retval = usb_register_dev(interface, &ard_class);
	if (retval) {
		dev_err(&interface->dev,
			"not able to get a minor for this device\n");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	dev_info(&interface->dev,
		 "Arduino USB device attached to /dev/ard%d\n",
		 interface->minor);

	return 0;

error:
	kref_put(&dev->kref, ard_delete);
	return retval;
}

static void ard_disconnect(struct usb_interface *interface)
{
	struct ard_usb *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);
	usb_deregister_dev(interface, &ard_class);

	if (!dev)
		return;

	mutex_lock(&dev->io_mutex);
	dev->disconnected = 1;
	mutex_unlock(&dev->io_mutex);

	usb_kill_urb(dev->bulk_in_urb);
	usb_kill_anchored_urbs(&dev->submitted);

	kref_put(&dev->kref, ard_delete);

	dev_info(&interface->dev, "Arduino USB #%d disconnected\n", minor);
}

static void ard_draw_down(struct ard_usb *dev)
{
	int time;

	time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
	if (!time)
		usb_kill_anchored_urbs(&dev->submitted);
	usb_kill_urb(dev->bulk_in_urb);
}

static int ard_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct ard_usb *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;

	ard_draw_down(dev);

	return 0;
}

static int ard_resume(struct usb_interface *intf)
{
	return 0;
}

static int ard_pre_reset(struct usb_interface *intf)
{
	struct ard_usb *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;

	mutex_lock(&dev->io_mutex);
	ard_draw_down(dev);

	return 0;
}

static int ard_post_reset(struct usb_interface *intf)
{
	struct ard_usb *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;

	dev->errors = -EPIPE;
	mutex_unlock(&dev->io_mutex);

	return 0;
}

static struct usb_driver ard_driver = {
	.name =			"arduino_usb",
	.probe =		ard_probe,
	.disconnect =		ard_disconnect,
	.suspend =		ard_suspend,
	.resume =		ard_resume,
	.pre_reset =		ard_pre_reset,
	.post_reset =		ard_post_reset,
	.id_table =		ard_table,
	.supports_autosuspend =	1,
};

module_usb_driver(ard_driver);

MODULE_AUTHOR("corn");
MODULE_DESCRIPTION("Arduino USB bulk endpoint character driver");
MODULE_LICENSE("GPL v2");
