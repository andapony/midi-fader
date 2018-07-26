/**
 * Human interface device driver
 *
 * Kevin Cuzner
 */

#include "usb_hid.h"

#define HID_IN_ENDPOINT 1
#define HID_OUT_ENDPOINT 2

void __attribute__((weak)) hook_usb_hid_configured(void) { }
void __attribute__((weak)) hook_usb_hid_out_report(const USBTransferData *report) { }
void __attribute__((weak)) hook_usb_hid_in_report_sent(const USBTransferData *report) { }
void __attribute__((weak)) hook_usb_hid_out_report_received(const USBTransferData *report) { }

void usb_hid_send(const USBTransferData *report)
{
    usb_endpoint_send(HID_IN_ENDPOINT, report->addr, report->len);
}

void usb_hid_receive(const USBTransferData *report)
{
    usb_endpoint_receive(HID_OUT_ENDPOINT, report->addr, report->len);
}

/**
 * Implementation of hook_usb_handle_setup_request which implements HID class
 * requests
 */
USBControlResult hook_usb_handle_setup_request(USBSetupPacket const *setup, USBTransferData *nextTransfer)
{
    switch (setup->wRequestAndType)
    {
        /*case USB_REQ(0x01, USB_REQ_DIR_IN | USB_REQ_TYPE_CLS | USB_REQ_RCP_IFACE):
            //Get report request
            nextTransfer->addr = report_in;
            nextTransfer->len = sizeof(report_in);
            return USB_CTL_OK;*/
        case USB_REQ(0x0A, USB_REQ_DIR_OUT | USB_REQ_TYPE_CLS | USB_REQ_RCP_IFACE):
            return USB_CTL_OK;
    }
    return USB_CTL_STALL;
}

void hook_usb_set_configuration(uint16_t configuration)
{
    usb_endpoint_setup(HID_IN_ENDPOINT, 0x81, USB_HID_ENDPOINT_SIZE, USB_ENDPOINT_INTERRUPT, USB_FLAGS_NOZLP);
    usb_endpoint_setup(HID_OUT_ENDPOINT, 0x02, USB_HID_ENDPOINT_SIZE, USB_ENDPOINT_INTERRUPT, USB_FLAGS_NOZLP);

    hook_usb_hid_configured();
}

void hook_usb_endpoint_sent(uint8_t endpoint, void *buf, uint16_t len)
{
    USBTransferData report = { buf, len };
    if (endpoint == HID_IN_ENDPOINT)
    {
        hook_usb_hid_in_report_sent(&report);
    }
}

void hook_usb_endpoint_received(uint8_t endpoint, void *buf, uint16_t len)
{
    USBTransferData report = { buf, len };
    if (endpoint == HID_OUT_ENDPOINT)
    {
        hook_usb_hid_out_report_received(&report);
    }
}

