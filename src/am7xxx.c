/* am7xxx - communication with AM7xxx based USB Pico Projectors and DPFs
 *
 * Copyright (C) 2012  Antonio Ospite <ospite@studenti.unina.it>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libusb-1.0/libusb.h>

#include "am7xxx.h"
#include "serialize.h"

#define AM7XXX_VENDOR_ID  0x1de1
#define AM7XXX_PRODUCT_ID 0xc101

/* The header size on the wire is known to be always 24 bytes, regardless of
 * the memory configuration enforced by different architechtures or compilers
 * for struct am7xxx_header
 */
#define AM7XXX_HEADER_WIRE_SIZE 24

struct _am7xxx_device {
	libusb_device_handle *usb_device;
	uint8_t buffer[AM7XXX_HEADER_WIRE_SIZE];
};

typedef enum {
	AM7XXX_PACKET_TYPE_DEVINFO = 0x01,
	AM7XXX_PACKET_TYPE_IMAGE   = 0x02,
	AM7XXX_PACKET_TYPE_POWER   = 0x04,
	AM7XXX_PACKET_TYPE_UNKNOWN = 0x05,
} am7xxx_packet_type;

struct am7xxx_generic_header {
	uint32_t field0;
	uint32_t field1;
	uint32_t field2;
	uint32_t field3;
};

struct am7xxx_devinfo_header {
	uint32_t native_width;
	uint32_t native_height;
	uint32_t unknown0;
	uint32_t unknown1;
};

struct am7xxx_image_header {
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t image_size;
};

struct am7xxx_power_header {
	uint32_t bit2;
	uint32_t bit1;
	uint32_t bit0;
};

/*
 * Examples of packet headers:
 *
 * Image header:
 * 02 00 00 00 00 10 3e 10 01 00 00 00 20 03 00 00 e0 01 00 00 53 E8 00 00
 *
 * Power header:
 * 04 00 00 00 00 0c ff ff 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
 */

struct am7xxx_header {
	uint32_t packet_type;
	uint8_t unknown0;
	uint8_t header_data_len;
	uint8_t unknown2;
	uint8_t unknown3;
	union {
		struct am7xxx_generic_header data;
		struct am7xxx_devinfo_header devinfo;
		struct am7xxx_image_header image;
		struct am7xxx_power_header power;
	} header_data;
};


static void dump_devinfo_header(struct am7xxx_devinfo_header *d)
{
	if (d == NULL)
		return;

	printf("Info header:\n");
	printf("\tnative_width:  0x%08x (%u)\n", d->native_width, d->native_width);
	printf("\tnative_height: 0x%08x (%u)\n", d->native_height, d->native_height);
	printf("\tunknown0:      0x%08x (%u)\n", d->unknown0, d->unknown0);
	printf("\tunknown1:      0x%08x (%u)\n", d->unknown1, d->unknown1);
}

static void dump_image_header(struct am7xxx_image_header *i)
{
	if (i == NULL)
		return;

	printf("Image header:\n");
	printf("\tformat:     0x%08x (%u)\n", i->format, i->format);
	printf("\twidth:      0x%08x (%u)\n", i->width, i->width);
	printf("\theight:     0x%08x (%u)\n", i->height, i->height);
	printf("\timage size: 0x%08x (%u)\n", i->image_size, i->image_size);
}

static void dump_power_header(struct am7xxx_power_header *p)
{
	if (p == NULL)
		return;

	printf("Power header:\n");
	printf("\tbit2: 0x%08x (%u)\n", p->bit2, p->bit2);
	printf("\tbit1: 0x%08x (%u)\n", p->bit1, p->bit1);
	printf("\tbit0: 0x%08x (%u)\n", p->bit0, p->bit0);
}

static void dump_header(struct am7xxx_header *h)
{
	if (h == NULL)
		return;

	printf("packet_type:     0x%08x (%u)\n", h->packet_type, h->packet_type);
	printf("unknown0:        0x%02hhx (%hhu)\n", h->unknown0, h->unknown0);
	printf("header_data_len: 0x%02hhx (%hhu)\n", h->header_data_len, h->header_data_len);
	printf("unknown2:        0x%02hhx (%hhu)\n", h->unknown2, h->unknown2);
	printf("unknown3:        0x%02hhx (%hhu)\n", h->unknown3, h->unknown3);

	switch(h->packet_type) {
	case AM7XXX_PACKET_TYPE_DEVINFO:
		dump_devinfo_header(&(h->header_data.devinfo));
		break;

	case AM7XXX_PACKET_TYPE_IMAGE:
		dump_image_header(&(h->header_data.image));
		break;

	case AM7XXX_PACKET_TYPE_POWER:
		dump_power_header(&(h->header_data.power));
		break;

	default:
		printf("Packet type not supported!\n");
		break;
	}

	fflush(stdout);
}

static inline unsigned int in_80chars(unsigned int i)
{
	/* The 3 below is the length of "xx " where xx is the hex string
	 * representation of a byte */
	return ((i+1) % (80/3));
}

static void dump_buffer(uint8_t *buffer, unsigned int len)
{
	unsigned int i;

	if (buffer == NULL || len == 0)
		return;

	for (i = 0; i < len; i++) {
		printf("%02hhX%c", buffer[i], (in_80chars(i) && (i < len - 1)) ? ' ' : '\n');
	}
	fflush(stdout);
}

static int read_data(am7xxx_device *dev, uint8_t *buffer, unsigned int len)
{
	int ret;
	int transferred = 0;

	ret = libusb_bulk_transfer(dev->usb_device, 0x81, buffer, len, &transferred, 0);
	if (ret != 0 || (unsigned int)transferred != len) {
		fprintf(stderr, "Error: ret: %d\ttransferred: %d (expected %u)\n",
			ret, transferred, len);
		return ret;
	}

#if DEBUG
	printf("\n<-- received\n");
	dump_buffer(buffer, len);
	printf("\n");
#endif

	return 0;
}

static int send_data(am7xxx_device *dev, uint8_t *buffer, unsigned int len)
{
	int ret;
	int transferred = 0;

#if DEBUG
	printf("\nsending -->\n");
	dump_buffer(buffer, len);
	printf("\n");
#endif

	ret = libusb_bulk_transfer(dev->usb_device, 1, buffer, len, &transferred, 0);
	if (ret != 0 || (unsigned int)transferred != len) {
		fprintf(stderr, "Error: ret: %d\ttransferred: %d (expected %u)\n",
			ret, transferred, len);
		return ret;
	}

	return 0;
}

static void serialize_header(struct am7xxx_header *h, uint8_t *buffer)
{
	uint8_t **buffer_iterator = &buffer;

	put_le32(h->packet_type, buffer_iterator);
	put_8(h->unknown0, buffer_iterator);
	put_8(h->header_data_len, buffer_iterator);
	put_8(h->unknown2, buffer_iterator);
	put_8(h->unknown3, buffer_iterator);
	put_le32(h->header_data.data.field0, buffer_iterator);
	put_le32(h->header_data.data.field1, buffer_iterator);
	put_le32(h->header_data.data.field2, buffer_iterator);
	put_le32(h->header_data.data.field3, buffer_iterator);
}

static void unserialize_header(uint8_t *buffer, struct am7xxx_header *h)
{
	uint8_t **buffer_iterator = &buffer;

	h->packet_type = get_le32(buffer_iterator);
	h->unknown0 = get_8(buffer_iterator);
	h->header_data_len = get_8(buffer_iterator);
	h->unknown2 = get_8(buffer_iterator);
	h->unknown3 = get_8(buffer_iterator);
	h->header_data.data.field0 = get_le32(buffer_iterator);
	h->header_data.data.field1 = get_le32(buffer_iterator);
	h->header_data.data.field2 = get_le32(buffer_iterator);
	h->header_data.data.field3 = get_le32(buffer_iterator);
}

static int read_header(am7xxx_device *dev, struct am7xxx_header *h)
{
	int ret;

	ret = read_data(dev, dev->buffer, AM7XXX_HEADER_WIRE_SIZE);
	if (ret < 0)
		goto out;

	unserialize_header(dev->buffer, h);

#if DEBUG
	printf("\n");
	dump_header(h);
	printf("\n");
#endif

	ret = 0;

out:
	return ret;
}

static int send_header(am7xxx_device *dev, struct am7xxx_header *h)
{
	int ret;

#if DEBUG
	printf("\n");
	dump_header(h);
	printf("\n");
#endif

	serialize_header(h, dev->buffer);
	ret = send_data(dev, dev->buffer, AM7XXX_HEADER_WIRE_SIZE);
	if (ret < 0)
		fprintf(stderr, "send_header: failed to send data.\n");

	return ret;
}

am7xxx_device *am7xxx_init(void)
{
	unsigned int i;

	am7xxx_device *dev = malloc(sizeof(*dev));
	if (dev == NULL) {
		perror("malloc");
		goto out;
	}
	memset(dev, 0, sizeof(*dev));

	libusb_init(NULL);
	libusb_set_debug(NULL, 3);

	dev->usb_device = libusb_open_device_with_vid_pid(NULL,
					      AM7XXX_VENDOR_ID,
					      AM7XXX_PRODUCT_ID);
	if (dev->usb_device == NULL) {
		errno = ENODEV;
		perror("libusb_open_device_with_vid_pid");
		goto out_libusb_exit;
	}

	libusb_set_configuration(dev->usb_device, 1);
	libusb_claim_interface(dev->usb_device, 0);

	return dev;

out_libusb_exit:
	libusb_exit(NULL);
	free(dev);
out:
	return NULL;
}

void am7xxx_shutdown(am7xxx_device *dev)
{
	if (dev) {
		libusb_close(dev->usb_device);
		libusb_exit(NULL);
		free(dev);
		dev = NULL;
	}
}

int am7xxx_get_device_info(am7xxx_device *dev,
			   unsigned int *native_width,
			   unsigned int *native_height,
			   unsigned int *unknown0,
			   unsigned int *unknown1)
{
	int ret;
	struct am7xxx_header h = {
		.packet_type     = AM7XXX_PACKET_TYPE_DEVINFO,
		.unknown0        = 0x00,
		.header_data_len = 0x00,
		.unknown2        = 0x3e,
		.unknown3        = 0x10,
		.header_data = {
			.devinfo = {
				.native_width  = 0,
				.native_height = 0,
				.unknown0      = 0,
				.unknown1      = 0,
			},
		},
	};

	ret = send_header(dev, &h);
	if (ret < 0)
		return ret;

	ret = read_header(dev, &h);
	if (ret < 0)
		return ret;

	*native_width = h.header_data.devinfo.native_width;
	*native_height = h.header_data.devinfo.native_height;
	*unknown0 = h.header_data.devinfo.unknown0;
	*unknown1 = h.header_data.devinfo.unknown1;

	return 0;
}

int am7xxx_send_image(am7xxx_device *dev,
		      am7xxx_image_format format,
		      unsigned int width,
		      unsigned int height,
		      uint8_t *image,
		      unsigned int size)
{
	int ret;
	struct am7xxx_header h = {
		.packet_type     = AM7XXX_PACKET_TYPE_IMAGE,
		.unknown0        = 0x00,
		.header_data_len = sizeof(struct am7xxx_image_header),
		.unknown2        = 0x3e,
		.unknown3        = 0x10,
		.header_data = {
			.image = {
				.format     = format,
				.width      = width,
				.height     = height,
				.image_size = size,
			},
		},
	};

	ret = send_header(dev, &h);
	if (ret < 0)
		return ret;

	if (image == NULL || size == 0)
		return 0;

	return send_data(dev, image, size);
}

int am7xxx_set_power_mode(am7xxx_device *dev, am7xxx_power_mode mode)
{
	int ret;
	struct am7xxx_header h = {
		.packet_type     = AM7XXX_PACKET_TYPE_POWER,
		.unknown0        = 0x00,
		.header_data_len = sizeof(struct am7xxx_power_header),
		.unknown2        = 0x3e,
		.unknown3        = 0x10,
	};

	switch(mode) {
	case AM7XXX_POWER_OFF:
		h.header_data.power.bit2 = 0;
		h.header_data.power.bit1 = 0;
		h.header_data.power.bit0 = 0;
		break;

	case AM7XXX_POWER_LOW:
		h.header_data.power.bit2 = 0;
		h.header_data.power.bit1 = 0;
		h.header_data.power.bit0 = 1;

	case AM7XXX_POWER_MIDDLE:
		h.header_data.power.bit2 = 0;
		h.header_data.power.bit1 = 1;
		h.header_data.power.bit0 = 0;
		break;

	case AM7XXX_POWER_HIGH:
		h.header_data.power.bit2 = 0;
		h.header_data.power.bit1 = 1;
		h.header_data.power.bit0 = 1;
		break;

	case AM7XXX_POWER_TURBO:
		h.header_data.power.bit2 = 1;
		h.header_data.power.bit1 = 0;
		h.header_data.power.bit0 = 0;
		break;

	default:
		fprintf(stderr, "Unsupported power mode.\n");
		return -EINVAL;
	};

	ret = send_header(dev, &h);
	if (ret < 0)
		return ret;

	return 0;
}
