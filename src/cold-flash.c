/*
    cold-flash.c - Cold flashing
    Copyright (C) 2011-2012  Pali Rohár <pali.rohar@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "global.h"
#include "cold-flash.h"
#include "image.h"
#include "usb-device.h"
#include "printf-utils.h"

#define READ_TIMEOUT		500
#define WRITE_TIMEOUT		3000
#define XLOADER_FINAL_ACK_TIMEOUT	5000
#define OMAP_ROM_COMMAND_DELAY_MS	10
#define OMAP_ROM_PAYLOAD_DELAY_MS	10
#define XLOADER_STAGE_DELAY_MS		10
#define USB_INTERWRITE_DELAY_MS		1
#define OMAP_ROM_TRANSFER_CHUNK		512
#define XLOADER_TRANSFER_CHUNK		0x10000
#define USB_ZERO_WRITE_RETRIES		10
#define USB_ZERO_WRITE_RETRY_DELAY_MS	2

/*
 * Old OMAP3 mask-ROM USB implementations can STALL when a modern xHCI host
 * submits consecutive OUT transfers too aggressively.  Keep the workaround
 * local to cold flashing and report the actual libusb result; upstream's
 * ERROR_RETURN macro deliberately clears errno and hides this diagnostic.
 */
static int cold_bulk_write(usb_dev_handle * udev, const char * bytes, int size,
	int timeout, int delay_ms, const char * step) {
	int ret;
	int saved_errno;
	const char * usb_error;

	if ( delay_ms > 0 )
		MSLEEP(delay_ms);

	errno = 0;
	ret = usb_bulk_write(udev, USB_WRITE_EP, bytes, size, timeout);
	saved_errno = errno;
	usb_error = usb_strerror();

	if ( ret != size ) {
		fprintf(stderr,
			"Error: %s: usb_bulk_write returned %d, expected %d; "
			"errno=%d (%s); libusb-global=%s\n",
			step, ret, size, saved_errno,
			saved_errno ? strerror(saved_errno) : "none",
			usb_error ? usb_error : "unknown");
	}

	return ret;
}

/*
 * Nokia's writer keeps submitting the unsent tail of payload data when
 * libusb-0.1 reports a positive short write or a zero-byte write.  Restrict
 * that behaviour to payload bytes: protocol words are never retried here.
 */
static int cold_bulk_write_payload(usb_dev_handle * udev, const char * bytes,
	int size, int timeout, int delay_ms, const char * step) {
	int ret = 0;
	int total = 0;
	int zero_retries = 0;
	int clear_ret;
	int saved_errno = 0;
	const char * usb_error;

	while ( total < size ) {
		if ( delay_ms > 0 )
			MSLEEP(delay_ms);

		errno = 0;
		ret = usb_bulk_write(udev, USB_WRITE_EP, bytes + total,
			size - total, timeout);
		saved_errno = errno;

		if ( ret > 0 ) {
			if ( ret > size - total ) {
				fprintf(stderr,
					"Error: %s: impossible usb_bulk_write result %d "
					"for %d remaining bytes\n",
					step, ret, size - total);
				return -1;
			}
			total += ret;
			zero_retries = 0;
			continue;
		}

		if ( ret == 0 && zero_retries < USB_ZERO_WRITE_RETRIES ) {
			++zero_retries;
			clear_ret = usb_clear_halt(udev, USB_WRITE_EP);
			if ( clear_ret != 0 ) {
				fprintf(stderr,
					"Error: %s: could not clear EP 0x%02x halt "
					"after zero-byte write (ret=%d)\n",
					step, USB_WRITE_EP, clear_ret);
				return total;
			}
			VERBOSE("Zero-byte USB payload write during %s; cleared EP "
				"0x%02x halt, retry %d/%d\n", step, USB_WRITE_EP,
				zero_retries, USB_ZERO_WRITE_RETRIES);
			MSLEEP(USB_ZERO_WRITE_RETRY_DELAY_MS);
			continue;
		}

		usb_error = usb_strerror();
		fprintf(stderr,
			"Error: %s: usb_bulk_write returned %d after %d/%d bytes; "
			"errno=%d (%s); libusb-global=%s\n",
			step, ret, total, size, saved_errno,
			saved_errno ? strerror(saved_errno) : "none",
			usb_error ? usb_error : "unknown");
		return ret < 0 ? ret : total;
	}

	return total;
}

static uint32_t tab[256];

static void crc32_gentab(void) {

	int i, j;
	uint32_t crc;
	uint32_t poly = 0xEDB88320L;

	for ( i = 0; i < 256; i++) {

		crc = i;

		for ( j = 8; j > 0; j-- ) {

			if ( crc & 1 )
				crc = (crc >> 1) ^ poly;
			else
				crc >>= 1;

		}

		tab[i] = crc;

	}

}

static uint32_t crc32(unsigned char * bytes, size_t size, uint32_t crc) {

	static int gen = 0;
	uint32_t i;

	if ( ! gen ) {
		crc32_gentab();
		gen = 1;
	}

	for ( i = 0; i < size; ++i )
		crc = (crc >> 8) ^ tab[(crc ^ bytes[i]) & 0xff];

	return crc;

}

/* Omap Boot Messages */
/* See spruf98v.pdf (page 3444): OMAP35x Technical Reference Manual - 25.4.5 Peripheral Booting */

/* Omap Peripheral boot message */
static const uint32_t omap_peripheral_msg = 0xF0030002;

/* Unused */
#if 0
/* Omap Void (no device) boot message */
static const uint32_t omap_void_msg = 0xF0030006;

/* Omap XIP memory boot message */
static const uint32_t omap_xip_msg = 0xF0030106;

/* Omap Nand boot message */
static const uint32_t omap_nand_msg = 0xF0030206;

/* Omap OneNAND boot message */
static const uint32_t omap_onenand_msg = 0xF0030306;

/* Omap DOC boot message */
static const uint32_t omap_doc_msg = 0xF0030406;

/* Omap MMC/SD2 boot message */
static const uint32_t omap_mmc2_msg = 0xF0030506;

/* Omap MMC/SD1 boot message */
static const uint32_t omap_mmc1_msg = 0xF0030606;

/* Omap XIP memory with wait monitoring boot message */
static const uint32_t omap_xipwait_msg = 0xF0030706;

/* Omap UART boot message */
static const uint32_t omap_uart_msg = 0xF0031006;

/* Omap HS USB boot message */
static const uint32_t omap_hsusb_msg = 0xF0031106;

/* Omap next device boot message */
static const uint32_t omap_next_msg = 0xFFFFFFFF;
#endif

/* Omap memory boot message */
static const uint32_t omap_memory_msg = 0;


/* Nokia X-Loader messages */

/* Structure of X-Loader message */
struct xloader_msg {
	uint32_t type; /* 4 bytes - type of message */
	uint32_t size; /* 4 bytes - size of file */
	uint32_t crc1; /* 4 bytes - crc32 of file */
	uint32_t crc2; /* 4 bytes - crc32 of first 12 bytes of message */
} __attribute__((__packed__));

#define XLOADER_MSG_TYPE_PING	0x6301326E
#define XLOADER_MSG_TYPE_SEND	0x6302326E
#define XLOADER_MSG_TYPE_LOAD_APE	0x6304326E
#define XLOADER_RESPONSE_OK	0x00006FAA

struct xloader_msg xloader_msg_create(uint32_t type, struct image * image) {

	struct xloader_msg msg;
	uint32_t need, sent;
	int ret;
	uint8_t buffer[1024];

	msg.type = type;
	msg.size = 0;
	msg.crc1 = 0;

	if ( image ) {
		msg.size = image->size;
		image_seek(image, 0);
		sent = 0;
		while ( sent < image->size ) {
			need = image->size - sent;
			if ( need > sizeof(buffer) )
				need = sizeof(buffer);
			ret = image_read(image, buffer, need);
			if ( ret == 0 )
				break;
			msg.crc1 = crc32(buffer, ret, msg.crc1);
			sent += ret;
		}
	}

	msg.crc2 = crc32((unsigned char *)&msg, 12, 0);

	return msg;

}

static int read_asic(usb_dev_handle * udev, uint8_t * asic_buffer, int size, int asic_size) {

	int ret;

	printf("Waiting for ASIC ID...\n");
	ret = usb_bulk_read(udev, USB_READ_EP, (char *)asic_buffer, size, READ_TIMEOUT);
	if ( ret != asic_size )
		ERROR_RETURN("Invalid size of ASIC ID", -1);

	return 0;

}

static int send_2nd(usb_dev_handle * udev, struct image * image) {

	uint8_t buffer[OMAP_ROM_TRANSFER_CHUNK];
	uint32_t need, sent;
	int ret;

	printf("Sending OMAP peripheral boot message...\n");
	ret = cold_bulk_write(udev, (char *)&omap_peripheral_msg,
		sizeof(omap_peripheral_msg), WRITE_TIMEOUT, 0,
		"Sending OMAP peripheral boot message failed");
	if ( ret != sizeof(omap_peripheral_msg) )
		return -1;

	MSLEEP(OMAP_ROM_COMMAND_DELAY_MS);

	printf("Sending 2nd X-Loader image size...\n");
	ret = cold_bulk_write(udev, (char *)&image->size, 4, WRITE_TIMEOUT,
		USB_INTERWRITE_DELAY_MS,
		"Sending 2nd X-Loader image size failed");
	if ( ret != 4 )
		return -1;

	MSLEEP(OMAP_ROM_PAYLOAD_DELAY_MS);

	printf("Sending 2nd X-Loader image...\n");
	printf_progressbar(0, image->size);
	image_seek(image, 0);
	sent = 0;
	while ( sent < image->size ) {
		need = image->size - sent;
		if ( need > sizeof(buffer) )
			need = sizeof(buffer);
		ret = image_read(image, buffer, need);
		if ( ret == 0 )
			break;
		if ( cold_bulk_write(udev, (char *)buffer, ret, WRITE_TIMEOUT,
			USB_INTERWRITE_DELAY_MS,
			"Sending 2nd X-Loader image failed") != ret )
			return -1;
		sent += ret;
		printf_progressbar(sent, image->size);
	}

	MSLEEP(50);
	return 0;

}

static int send_secondary(usb_dev_handle * udev, struct image * image) {

	struct xloader_msg init_msg;
	/* Match Nokia's cold-flash writer.  libusb-0.1 splits this into
	 * contiguous 16 KiB usbfs URBs; small, separately submitted writes can
	 * make the running X-Loader stall endpoint 1 after the first packet. */
	uint8_t buffer[XLOADER_TRANSFER_CHUNK];
	uint32_t need, sent;
	uint32_t response;
	uint32_t command;
	const char * payload_name;
	const char * payload_error;
	int ret;

	/* Harmattan's RM-680/RM-696 2nd uses 0x6304 to load the signed
	 * ape-algo.  Keep 0x6302 for legacy Secondary images. */
	command = image->type == IMAGE_APE_ALGO ?
		XLOADER_MSG_TYPE_LOAD_APE : XLOADER_MSG_TYPE_SEND;
	payload_name = image->type == IMAGE_APE_ALGO ?
		"APE algorithm" : "Secondary";
	payload_error = image->type == IMAGE_APE_ALGO ?
		"Sending APE algorithm failed" : "Sending Secondary image failed";
	init_msg = xloader_msg_create(command, image);
	VERBOSE("X-Loader image header: command=0x%04X size=%u "
		"crc-data=0x%08X crc-header=0x%08X\n",
		(unsigned int)((command >> 16) & 0xffff), init_msg.size,
		init_msg.crc1, init_msg.crc2);

	printf("Sending X-Loader init message...\n");
	ret = cold_bulk_write(udev, (char *)&init_msg, sizeof(init_msg),
		WRITE_TIMEOUT, USB_INTERWRITE_DELAY_MS,
		"Sending X-Loader init message failed");
	if ( ret != sizeof(init_msg) )
		return -1;

	printf("Waiting for X-Loader response...\n");
	MSLEEP(20);
	ret = usb_bulk_read(udev, USB_READ_EP, (char *)&response,
		sizeof(response), READ_TIMEOUT);
	if ( ret != 4 )
		ERROR_RETURN("No response", -1);
	if ( response != XLOADER_RESPONSE_OK ) {
		fprintf(stderr, "Error: X-Loader rejected image command "
			"0x%04X: response 0x%08X, expected 0x%08X\n",
			(unsigned int)((command >> 16) & 0xffff), response,
			XLOADER_RESPONSE_OK);
		return -1;
	}

	MSLEEP(XLOADER_STAGE_DELAY_MS);

	printf("Sending %s image...\n", payload_name);
	printf_progressbar(0, image->size);
	image_seek(image, 0);
	sent = 0;
	while ( sent < image->size ) {
		need = image->size - sent;
		if ( need > sizeof(buffer) )
			need = sizeof(buffer);
		ret = image_read(image, buffer, need);
		if ( ret == 0 )
			break;
		if ( cold_bulk_write_payload(udev, (char *)buffer, ret,
			WRITE_TIMEOUT,
			USB_INTERWRITE_DELAY_MS,
			payload_error) != ret )
			return -1;
		sent += ret;
		printf_progressbar(sent, image->size);
	}

	printf("Waiting for X-Loader response...\n");
	MSLEEP(20);
	ret = usb_bulk_read(udev, USB_READ_EP, (char *)&response,
		sizeof(response), XLOADER_FINAL_ACK_TIMEOUT);
	if ( ret != 4 )
		ERROR_RETURN("No response", -1);
	if ( response != XLOADER_RESPONSE_OK ) {
		fprintf(stderr, "Error: X-Loader rejected image data: response "
			"0x%08X, expected 0x%08X\n",
			response, XLOADER_RESPONSE_OK);
		return -1;
	}

	return 0;

}

static int ping_timeout(usb_dev_handle * udev) {

	int ret;
	int pong = 0;
	int try_ping = 10;

	while ( try_ping > 0 ) {

		struct xloader_msg ping_msg = xloader_msg_create(XLOADER_MSG_TYPE_PING, NULL);
		int try_read = 4;

		printf("Sending X-Loader ping message\n");
		ret = cold_bulk_write(udev, (char *)&ping_msg, sizeof(ping_msg),
			WRITE_TIMEOUT, USB_INTERWRITE_DELAY_MS,
			"Sending X-Loader ping message failed");
		if ( ret != sizeof(ping_msg) )
			return -1;

		printf("Waiting for X-Loader pong response...\n");
		while ( try_read > 0 ) {

			uint32_t ping_read;
			ret = usb_bulk_read(udev, USB_READ_EP, (char *)&ping_read, sizeof(ping_read), READ_TIMEOUT);
			if ( ret == sizeof(ping_read) ) {
				printf("Got it\n");
				pong = 1;
				break;
			}

			MSLEEP(5);
			--try_read;

		}

		if ( pong )
			break;

		printf("Response timeout\n");
		--try_ping;

	}

	if ( pong )
		return 0;
	else
		return -1;

}

int init_cold_flash(struct usb_device_info * dev) {

	uint8_t asic_buffer[127];
	int asic_size = 69;
	const char * chip = NULL;
	int revision;
	int i;

	if ( dev->flash_device->protocol != FLASH_COLD )
		ERROR_RETURN("Device is not in Cold Flash mode", -1);

	if ( read_asic(dev->udev, asic_buffer, sizeof(asic_buffer), asic_size) != 0 )
		ERROR_RETURN("Reading ASIC ID failed", -1);

	if ( verbose ) {
		printf("Got ASIC ID:");
		for ( i = 0; i < asic_size; ++i )
			printf(" 0x%.2X", (unsigned int)asic_buffer[i]);
		printf("\n");
	}

	/* TODO: Detect device from asic id */

	/* ASIC ID specification: http://processors.wiki.ti.com/index.php/OMAP35x_and_AM/DM37x_Initialization#UART.2FUSB_Booting */

	/* Number of subblocks */
	if ( asic_buffer[0] != 0x05 )
		ERROR_RETURN("Invalid ASIC ID", -1);

	/* 1. ID Subblock - header */
	if ( memcmp(asic_buffer+1, "\x01\x05\x01", 3) != 0 )
		ERROR_RETURN("Invalid ASIC ID", -1);

	/* 1. ID Subblock - OMAP chip version (check for OMAP3430 or 3630) */
	if ( memcmp(asic_buffer+4, "\x34\x30\x07", 3) == 0 )
		chip = "OMAP3430";
	else if ( memcmp(asic_buffer+4, "\x36\x30\x07", 3) == 0 )
		chip = "OMAP3630";
	else
		ERROR_RETURN("Invalid ASIC ID", -1);

	/* 1. ID Subblock - OMAP chip revision */
	revision = asic_buffer[7];

	/* 2. Secure Mode Subblock - header */
	if ( memcmp(asic_buffer+8, "\x13\x02\x01", 3) != 0 )
		ERROR_RETURN("Invalid ASIC ID", -1);

	/* 3. 2nd ID Subblock - header */
	if ( memcmp(asic_buffer+12, "\x12\x15\x01", 3) != 0 )
		ERROR_RETURN("Invalid ASIC ID", -1);

	/* 4. Root Key Hash Subblock - header */
	if ( memcmp(asic_buffer+35, "\x14\x15\x01", 3) != 0 )
		ERROR_RETURN("Invalid ASIC ID", -1);

	/* 5. Checksum subblock - header */
	if ( memcmp(asic_buffer+58, "\x15\x09\x01", 3) != 0 )
		ERROR_RETURN("Invalid ASIC ID", -1);

	printf("Detected %s chip (revision %d)\n", chip, revision);

	return 0;

}

int cold_flash(struct usb_device_info * dev, struct image * x2nd, struct image * secondary) {

	if ( x2nd->type != IMAGE_2ND )
		ERROR_RETURN("Image type is not 2nd X-Loader", -1);

	if ( secondary->type != IMAGE_SECONDARY &&
		secondary->type != IMAGE_APE_ALGO )
		ERROR_RETURN("Image type is not Secondary or APE algorithm", -1);

	if ( send_2nd(dev->udev, x2nd) != 0 )
		ERROR_RETURN("Sending 2nd X-Loader image failed", -1);

	if ( ping_timeout(dev->udev) != 0 )
		ERROR_RETURN("Sending X-Loader ping message failed", -1);

	if ( send_secondary(dev->udev, secondary) != 0 )
		ERROR_RETURN("Sending Secondary image failed", -1);

	printf("Done\n");
	return 0;

}

int leave_cold_flash(struct usb_device_info * dev) {

	int ret;

	printf("Sending OMAP memory boot message...\n");
	ret = cold_bulk_write(dev->udev, (char *)&omap_memory_msg,
		sizeof(omap_memory_msg), WRITE_TIMEOUT, USB_INTERWRITE_DELAY_MS,
		"Sending OMAP memory boot message failed");
	if ( ret != sizeof(omap_memory_msg) )
		return -1;

	MSLEEP(250);
	return 0;

}
