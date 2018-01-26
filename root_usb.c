/**
 * Root USB Hub Driver
 *
 * Copyright (C) 2018 Paulus Gandung Prakosa (rvn.plvhx@gmail.com)
 *
 * This program is a free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>

static struct usb_device_id root_usb_id_table[] = {
	{ USB_DEVICE(0x80ee, 0x0021) },
	{ }
};

MODULE_DEVICE_TABLE(usb, root_usb_id_table);

struct root_usb {
	struct usb_device *usb_dev;
	int ctx;
	unsigned char *int_in_buffer;
	struct urb *int_in_urb;
};

struct measurement_packet {
	u8	measurements_in_packet;
	u8	rolling_counter;
	__le16	measurement0;
	__le16	measurement1;
	__le16	measurement2;
} __attribute__((packed));

#define CMD_ID_START_MEASUREMENTS	0x18
#define CMD_ID_INIT	0x1a

struct output_packet {
	u8	cmd;
	u8	params[7];
} __attribute__((packed));

static int send_cmd(struct root_usb *ru, u8 cmd) {
	struct output_packet *pack;
	int ret;

	pack = kmalloc(sizeof(pack), GFP_KERNEL);
	if (pack == NULL) {
		return -ENOMEM;
	}
	memset(pack, 0, sizeof(pack));
	pack->cmd = cmd;

	ret = usb_control_msg(
		ru->usb_dev,
		usb_sndctrlpipe(ru->usb_dev, 0),
		0x09,
		0x21,
		0x0200,
		0x0000,
		pack,
		sizeof(pack),
		10000
	);

	if (ret == sizeof(pack)) {
		ret = 0;
	}

	kfree(pack);

	return ret;
}

static void init_dev(struct root_usb *ru) {
	int ret;
	send_cmd(ru, CMD_ID_INIT);

	ret = usb_submit_urb(ru->int_in_urb, GFP_KERNEL);
	if (ret) {
		dev_err(&ru->usb_dev->dev,
			"%s - Error %d submitting interrupt urb.\n",
			__func__, ret);
	}

	send_cmd(ru, CMD_ID_START_MEASUREMENTS);
}

static ssize_t show_temperature(
	struct device *c_dev,
	struct device_attribute *c_dev_attr,
	char *buf
) {
	struct usb_interface *usb_if = to_usb_interface(c_dev);
	struct root_usb *ru_handler = usb_get_intfdata(usb_if);

	return sprintf(buf, "%d\n", ru_handler->ctx);
}

static DEVICE_ATTR(temperature, S_IRUGO, show_temperature, NULL);

static void read_int_callback(struct urb *urb) {
	struct root_usb *ru_handler = urb->context;
	struct measurement_packet *measurement = urb->transfer_buffer;
	int status = urb->status;
	int ret;

	switch (status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		dbg("%s - urb shutting down with status: %d", __func__, status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d", __func__, status);
		goto exit;
	}

	dev_info(&urb->dev->dev, "counter %d, temperature: %d\n",
		measurement->rolling_counter,
		measurement->measurement0
	);
	ru_handler->ctx = le16_to_cpu(measurement->measurement0);

exit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		dev_err(&urb->dev->dev,
			"%s - Error %d submitting interrupt urb\n",
			__func__, ret);
	}
}
 
static int root_usb_driver_probe(
	struct usb_interface *usb_if,
	const struct usb_device_id *usb_id
) {
	struct usb_device *usb_dev = interface_to_usbdev(usb_if);
	struct root_usb *ru_handler;
	struct usb_endpoint_descriptor *endpoint;
	int ret = -ENOMEM;
	size_t buffer_size;

	ru_handler = kzalloc(sizeof(ru_handler), GFP_KERNEL);
	if (ru_handler == NULL) {
		dev_err(&usb_if->dev, "Kernel virtual memory exhausted.\n");
		return -ENOMEM;
	}

	ru_handler->usb_dev = usb_get_dev(usb_dev);

	endpoint = &usb_if->cur_altsetting->endpoint[0].desc;
	buffer_size = le16_to_cpu(endpoint->wMaxPacketSize);
	ru_handler->int_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
	if (!ru_handler->int_in_buffer) {
		dev_err(&usb_if->dev, "Could not allocate buffer.\n");
		goto error;
	}

	ru_handler->int_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ru_handler->int_in_urb) {
		dev_err(&usb_if->dev, "No free urb's available.\n");
		goto error;
	}

	usb_fill_int_urb(ru_handler->int_in_urb, usb_dev,
		usb_rcvintpipe(usb_dev, endpoint->bEndpointAddress),
		ru_handler->int_in_buffer, buffer_size,
		read_int_callback, ru_handler,
		endpoint->bInterval);

	usb_set_intfdata(usb_if, ru_handler);

	init_dev(ru_handler);

	ret = device_create_file(&usb_if->dev, &dev_attr_temperature);
	if (ret) {
		goto error;
	}
	
	dev_info(&usb_if->dev, "Root USB device has attached.\n");
	return 0;

error:
	usb_free_urb(ru_handler->int_in_urb);
	kfree(ru_handler->int_in_buffer);
	kfree(ru_handler);
	return ret;
}

static void root_usb_driver_disconnect(struct usb_interface *usb_if) {
	struct root_usb *ru_handler;

	ru_handler = usb_get_intfdata(usb_if);
	usb_set_intfdata(usb_if, NULL);
	device_remove_file(&usb_if->dev, &dev_attr_temperature);
	usb_put_dev(ru_handler->usb_dev);
	usb_kill_urb(ru_handler->int_in_urb);
	usb_free_urb(ru_handler->int_in_urb);
	kfree(ru_handler->int_in_buffer);
	kfree(ru_handler);

	dev_info(&usb_if->dev, "Root USB device has detached.\n");
}

static struct usb_driver root_usb_driver = {
	.name = "root_usb",
	.probe = root_usb_driver_probe,
	.disconnect = root_usb_driver_disconnect,
	.id_table = root_usb_id_table
};

static int __init root_usb_driver_init(void)
{
	return usb_register(&root_usb_driver);
}

static void __exit root_usb_driver_exit(void)
{
	usb_deregister(&root_usb_driver);
}

module_init(root_usb_driver_init);
module_exit(root_usb_driver_exit);

MODULE_AUTHOR("Paulus Gandung Prakosa");
MODULE_DESCRIPTION("Root USB Hub Driver");
MODULE_LICENSE("GPL");

