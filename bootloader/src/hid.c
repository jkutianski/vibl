/*
* STM32 HID Bootloader - USB HID bootloader for STM32F10X
* Copyright (c) 2018 Bruno Freitas - bruno@brunofreitas.com
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
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

#include <stm32f1xx.h>
#include <stddef.h>

#include "usb.h"
#include "hid.h"
#include "bitwise.h"
#include "boot.h"
#include "config.h"

// This should be <= MAX_EP_NUM defined in usb.h
#define EP_NUM 2

extern volatile uint8_t DeviceAddress;
extern volatile uint16_t DeviceConfigured, DeviceStatus;

static USB_SetupPacket *SetupPacket;

/* buffer table base address */
#define BTABLE_ADDRESS      (0x00)

/* EP0  */
/* rx/tx buffer base address */
#define ENDP0_RXADDR        (0x18)
#define ENDP0_TXADDR        (0x58)

/* EP1  */
/* tx buffer base address */
#define ENDP1_TXADDR        (0x100)

/* USB Descriptors */
static const uint8_t USB_DEVICE_DESC[] = {
	0x12,        // bLength
	0x01,        // bDescriptorType (Device)
	0x10, 0x01,  // bcdUSB 1.10
	0x00,        // bDeviceClass (Use class information in the Interface Descriptors)
	0x00,        // bDeviceSubClass
	0x00,        // bDeviceProtocol
	0x08,        // bMaxPacketSize0 8
	0xD0, 0x16,  // idVendor 0x16D0
	0x6C, 0x10,  // idProduct 0x106C
	0x01, 0x00,  // bcdDevice 0.01
	0x01,        // iManufacturer (String Index)
	0x01,        // iProduct (String Index)
	0x02,        // iSerialNumber (String Index)
	0x01         // bNumConfigurations 1
};

static const uint8_t USBD_DEVICE_CFG_DESCRIPTOR[] = {
	0x09,        // bLength
	0x02,        // bDescriptorType (Configuration)
	0x22, 0x00,  // wTotalLength 34
	0x01,        // bNumInterfaces 1
	0x01,        // bConfigurationValue
	0x00,        // iConfiguration (String Index)
	0xC0,        // bmAttributes Self Powered
	0x32,        // bMaxPower 100mA

	0x09,        // bLength
	0x04,        // bDescriptorType (Interface)
	0x00,        // bInterfaceNumber 0
	0x00,        // bAlternateSetting
	0x01,        // bNumEndpoints 1
	0x03,        // bInterfaceClass
	0x00,        // bInterfaceSubClass
	0x00,        // bInterfaceProtocol
	0x00,        // iInterface (String Index)

	0x09,        // bLength
	0x21,        // bDescriptorType (HID)
	0x11, 0x01,  // bcdHID 1.11
	0x00,        // bCountryCode
	0x01,        // bNumDescriptors
	0x22,        // bDescriptorType[0] (HID)
	0x20, 0x00,  // wDescriptorLength[0] 32

	0x07,        // bLength
	0x05,        // bDescriptorType (Endpoint)
	0x81,        // bEndpointAddress (IN/D2H)
	0x03,        // bmAttributes (Interrupt)
	0x08, 0x00,  // wMaxPacketSize 8
	0x05         // bInterval 5 (unit depends on device speed)
};

static const uint8_t usbHidReportDescriptor[32] = {
		0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
		0x09, 0x01,        // Usage (0x01)
		0xA1, 0x01,        // Collection (Application)
		0x09, 0x02,        //   Usage (0x02)
		0x15, 0x00,        //   Logical Minimum (0)
		0x25, 0xFF,        //   Logical Maximum (-1)
		0x75, 0x08,        //   Report Size (8)
		0x95, 0x08,        //   Report Count (8)
		0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
		0x09, 0x03,        //   Usage (0x03)
		0x15, 0x00,        //   Logical Minimum (0)
		0x25, 0xFF,        //   Logical Maximum (-1)
		0x75, 0x08,        //   Report Size (8)
		0x95, 0x40,        //   Report Count (64)
		0x91, 0x02,        //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
		0xC0               // End Collection
};

void HIDUSB_Reset() {
	_SetBTABLE(BTABLE_ADDRESS);

	/* Initialize Endpoint 0 */
	_SetEPType(ENDP0, EP_CONTROL);
	_SetEPRxAddr(ENDP0, ENDP0_RXADDR);
	_SetEPTxAddr(ENDP0, ENDP0_TXADDR);
	_ClearEP_KIND(ENDP0);
	_SetEPRxValid(ENDP0);

	/* Initialize Endpoint 1 */
	_SetEPType(ENDP1, EP_INTERRUPT);
	_SetEPTxAddr(ENDP1, ENDP1_TXADDR);
	_SetEPTxCount(ENDP1, 0x8);
	_SetEPRxStatus(ENDP1, EP_RX_DIS);
	_SetEPTxStatus(ENDP1, EP_TX_NAK);

	/* set address in every used endpoint */
	for (int i = 0; i < EP_NUM; i++) {
		_SetEPAddress((uint8_t )i, (uint8_t )i);
		RxTxBuffer[i].MaxPacketSize = 8;
	}

	_SetDADDR(0 | DADDR_EF); /* set device address and enable function */
}

void HIDUSB_GetDescriptor(USB_SetupPacket *SPacket) {

	switch (SPacket->wValue.H) {
		case USB_DEVICE_DESC_TYPE:
			USB_SendData(0, USB_DEVICE_DESC,
					SPacket->wLength > sizeof(USB_DEVICE_DESC) ?
							sizeof(USB_DEVICE_DESC) : SPacket->wLength);
			break;

		case USB_CFG_DESC_TYPE:
			USB_SendData(0, USBD_DEVICE_CFG_DESCRIPTOR,
					SPacket->wLength > sizeof(USBD_DEVICE_CFG_DESCRIPTOR) ?
							sizeof(USBD_DEVICE_CFG_DESCRIPTOR) : SPacket->wLength);
			break;

		case USB_REPORT_DESC_TYPE:
			USB_SendData(0, usbHidReportDescriptor,
					SPacket->wLength > sizeof(usbHidReportDescriptor) ?
							sizeof(usbHidReportDescriptor) : SPacket->wLength);
			break;

		case USB_STR_DESC_TYPE:

			switch (SPacket->wValue.L) {
			case 0x00:
				USB_SendData(0, sdLangID,
						SPacket->wLength > sizeof(sdLangID) ?
								sizeof(sdLangID) : SPacket->wLength);
				break;
			case 0x01:
				USB_SendData(0, sdProduct,
						SPacket->wLength > sizeof(sdProduct) ?
								sizeof(sdProduct) : SPacket->wLength);
				break;
			case 0x02:
					USB_SendData(0, sdSerial,
						SPacket->wLength > sizeof(sdSerial) ?
								sizeof(sdSerial) : SPacket->wLength);
				break;
			default:
				USB_SendData(0, 0, 0);
			}

			break;
		default:
			USB_SendData(0, 0, 0);
			break;
	}
}

static void HIDUSB_FlashUnlock() {
	FLASH->KEYR = FLASH_KEY1;
	FLASH->KEYR = FLASH_KEY2;
}

static void HIDUSB_FlashLock() {
	bit_set(FLASH->CR, FLASH_CR_LOCK);
}

static void HIDUSB_FormatFlashPage(uint32_t page) {
	while(FLASH->SR & FLASH_SR_BSY);

	bit_set(FLASH->CR, FLASH_CR_PER);

	FLASH->AR = page;

	bit_set(FLASH->CR, FLASH_CR_STRT);

	while(FLASH->SR & FLASH_SR_BSY);

	bit_clear(FLASH->CR, FLASH_CR_PER);
}

static void HIDUSB_WriteFlash(uint32_t page, uint8_t *data, uint16_t size) {
	while(FLASH->SR & FLASH_SR_BSY);

	bit_set(FLASH->CR, FLASH_CR_PG);

	for(uint16_t i = 0; i < size; i += 2) {
		uint16_t tmp = data[i] | (data[i + 1] << 8);
		*(volatile uint16_t *)(page + i) = tmp;

		while(FLASH->SR & FLASH_SR_BSY);
	}

	bit_clear(FLASH->CR, FLASH_CR_PG);
}

static uint8_t HIDUSB_PacketIsCommand(const uint8_t *page) {
	return (page[0] == 'V' && page[1] == 'C');
}

enum {
	STATE_INIT = 0,
	STATE_FLASH,
};

void HIDUSB_HandleData(uint8_t *data) {
	static int state = STATE_INIT;
	static uint32_t pagesToFlash;
	static uint32_t currentPage;
	static uint32_t currentPageOffset;

	/* Will flash 64 bytes at a time */
	static uint8_t pageData[64];

	static uint8_t keyboard_id[8] = VIAL_KEYBOARD_UID;

	static uint8_t bootloader_ident[8] = { 1 };

	if (state == STATE_INIT) {
		for (size_t i = 0; i < 8; ++i)
			pageData[currentPageOffset + i] = data[i];
		currentPageOffset += 8;

		if (currentPageOffset == sizeof(pageData)) {
			currentPageOffset = 0;

			if (HIDUSB_PacketIsCommand(pageData)) {
				switch (pageData[2]) {
				case 0x00:
					/* Retrieve bootloader version, flags */
					USB_SendData(ENDP1, bootloader_ident, sizeof(bootloader_ident));
					break;
				case 0x01:
					/* Send vial keyboard ID */
					USB_SendData(ENDP1, keyboard_id, sizeof(keyboard_id));
					break;
				case 0x02:
					/* Flash */
					currentPage = 0;
					pagesToFlash = pageData[3] + 256 * pageData[4];
					/* Don't allow to pass a ridiculous value. 10 megs max */
					if (pagesToFlash < 10 * 1024 * 1024 / sizeof(pageData)) {
						state = STATE_FLASH;
						currentPageOffset = 0;
					}
					break;
				case 0x03:
					/* Reboot */
					NVIC_SystemReset();
					break;
				case 0x04:
					/* set insecure so that on first boot we can restore layout */
					setInsecureFlag();
					break;
				default:
					break;
				}
			}
		}
	} else if (state == STATE_FLASH) {
		for (size_t i = 0; i < 8; ++i)
			pageData[currentPageOffset + i] = data[i];
		currentPageOffset += 8;

		/* Flashing */
		if (currentPageOffset == sizeof(pageData)) {
			/* Received another page */
			uint32_t pageAddress = USER_PROGRAM + (currentPage * sizeof(pageData));

			HIDUSB_FlashUnlock();
			/* If we're at page boundary, we have to erase this page */
			if ((pageAddress & 0x3FF) == 0)
				HIDUSB_FormatFlashPage(pageAddress);
			/* Then proceed to write the data */
			HIDUSB_WriteFlash(pageAddress, pageData, sizeof(pageData));
			HIDUSB_FlashLock();

			currentPage++;
			currentPageOffset = 0;
		}

		/* Did we flash everything? */
		if (currentPage == pagesToFlash) {
			/* Back to processing commands */
			state = STATE_INIT;
		}
	}
}

void HIDUSB_EPHandler(uint16_t Status) {

	uint8_t EPn = Status & USB_ISTR_EP_ID;
	uint16_t EP = _GetENDPOINT(EPn);

	// OUT and SETUP packets (data reception)
	if (EP & EP_CTR_RX) {

		// Copy from packet area to user buffer
		USB_PMA2Buffer(EPn);

		if (EPn == 0) { //If control endpoint

			if (EP & USB_EP0R_SETUP) {

				SetupPacket = (USB_SetupPacket *) RxTxBuffer[EPn].RXB;

				switch (SetupPacket->bRequest) {
				case USB_REQUEST_SET_ADDRESS:
					DeviceAddress = SetupPacket->wValue.L;
					USB_SendData(0, 0, 0);
					break;

				case USB_REQUEST_GET_DESCRIPTOR:
					HIDUSB_GetDescriptor(SetupPacket);
					break;

				case USB_REQUEST_GET_STATUS:
					USB_SendData(0, (uint16_t *) &DeviceStatus, 2);
					break;

				case USB_REQUEST_GET_CONFIGURATION:
					USB_SendData(0, (uint16_t *) &DeviceConfigured, 1);
					break;

				case USB_REQUEST_SET_CONFIGURATION:
					DeviceConfigured = 1;
					USB_SendData(0, 0, 0);
					break;

				case USB_REQUEST_GET_INTERFACE:
					USB_SendData(0, 0, 0);
					break;

				default:
					USB_SendData(0, 0, 0);
					_SetEPTxStatus(0, EP_TX_STALL);
					break;
				}

			} else { // OUT packet
				if(RxTxBuffer[EPn].RXL) {
					HIDUSB_HandleData((uint8_t *) RxTxBuffer[EPn].RXB);
				}
			}

		} else { // Got data from another EP
			// Call user function
			HIDUSB_DataReceivedHandler(RxTxBuffer[EPn].RXB,
					RxTxBuffer[EPn].RXL);
		}

		_ClearEP_CTR_RX(EPn);
		_SetEPRxValid(EPn);
	}

	if (EP & EP_CTR_TX) { // something transmitted
		if (DeviceAddress) {
			_SetDADDR(DeviceAddress | DADDR_EF); /* set device address and enable function */
			DeviceAddress = 0;
		}

		if (RxTxBuffer[EPn].TXL) { // Have to transmit something?
			USB_Buffer2PMA(EPn);
		} else {
			_SetEPTxCount(EPn, 0);
		}

		_SetEPTxValid(EPn);
		_ClearEP_CTR_TX(EPn);

		if (EPn == ENDP1) {
			_SetEPTxStatus(ENDP1, EP_TX_NAK);
		}
	}
}

__attribute__((weak)) void HIDUSB_DataReceivedHandler(uint16_t *Data, uint16_t Length) {
	(void)Data;
	(void)Length;
}

