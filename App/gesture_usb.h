#ifndef GESTURE_USB_H
#define GESTURE_USB_H
#include <stdint.h>

typedef void (*gesture_command_fn)(const char *line);
void gesture_usb_init(gesture_command_fn handler);
void gesture_usb_receive(const uint8_t *data, uint32_t length);
void gesture_usb_session_changed(void);
void gesture_usb_process(void);
void gesture_usb_log(int raw, const char *format, ...);
uint32_t gesture_usb_drops(void);
int gesture_usb_connected(void);
#endif
