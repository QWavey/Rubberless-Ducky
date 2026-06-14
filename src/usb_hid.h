/**
 * @file usb_hid.h
 * @brief USB HID layer interface for AT32UC3B1
 *
 * Thin wrapper around the AVR32 USB peripheral hardware registers.
 * For production use, replace with full ASF USB stack integration.
 */

#ifndef USB_HID_H_
#define USB_HID_H_

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * USB Device Events
 * ---------------------------------------------------------------------- */
typedef enum {
    USB_EVENT_RESET       = 0,
    USB_EVENT_SUSPEND     = 1,
    USB_EVENT_RESUME      = 2,
    USB_EVENT_ENUMERATED  = 3,
    USB_EVENT_CONFIG_REQUESTED = 4,
} usb_event_t;

/* -------------------------------------------------------------------------
 * Callback function type definitions
 * ---------------------------------------------------------------------- */
typedef void (*usb_event_callback_t)(void);
typedef void (*usb_hid_out_callback_t)(uint8_t *data, uint8_t length);

/* -------------------------------------------------------------------------
 * USB Device API
 * ---------------------------------------------------------------------- */

/**
 * @brief Initialize the USB device hardware and internal state.
 */
void usb_device_init(void);

/**
 * @brief Enable the USB device (connect D+ pull-up).
 */
void usb_device_enable(void);

/**
 * @brief Disable the USB device (disconnect D+ pull-up).
 */
void usb_device_disable(void);

/**
 * @brief Register an event callback.
 * @param event    The USB event to register for.
 * @param callback The function to call when the event occurs.
 */
void usb_device_register_callback(usb_event_t event, usb_event_callback_t callback);

/**
 * @brief USB device task - call this from the main loop to process USB events.
 *
 * This handles:
 *   - Control pipe requests (GET_DESCRIPTOR, SET_CONFIGURATION, SET_IDLE, etc.)
 *   - OUT endpoint data reception
 *   - State machine updates
 */
void usb_device_task(void);

/* -------------------------------------------------------------------------
 * USB HID API
 * ---------------------------------------------------------------------- */

/**
 * @brief Register a callback for received OUT reports (host -> device).
 * @param callback Function called with (data_ptr, length) when report arrives.
 */
void usb_hid_register_out_callback(usb_hid_out_callback_t callback);

/**
 * @brief Send an IN report to the host.
 * @param data   Pointer to report data (first byte must be Report ID).
 * @param length Number of bytes to send (must be HID_IN_REPORT_SIZE).
 * @return true if the data was queued successfully, false if busy.
 */
bool usb_hid_send_report(const uint8_t *data, uint8_t length);

/**
 * @brief Check if the IN endpoint is ready to accept a new report.
 */
bool usb_hid_in_endpoint_ready(void);

#endif /* USB_HID_H_ */

void ep0_send(const uint8_t *data, uint16_t length, uint16_t requested_length);

void ep0_ack_status_in(void);
