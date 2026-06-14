import sys

with open('src/main.c', 'r', encoding='utf-8') as f:
    content = f.read()

old_usb = """    usb_device_init();
    usb_device_register_callback(USB_EVENT_ENUMERATED, usb_device_enumerated_cb);
    usb_device_register_callback(USB_EVENT_CONFIG_REQUESTED, usb_device_config_requested_cb);
    usb_hid_register_out_callback(usb_hid_report_out_cb);
    usb_device_enable();"""
content = content.replace(old_usb, "")

usb_start_func = """
static void start_usb_and_wait(void) {
    usb_device_init();
    usb_device_register_callback(USB_EVENT_ENUMERATED, usb_device_enumerated_cb);
    usb_device_register_callback(USB_EVENT_CONFIG_REQUESTED, usb_device_config_requested_cb);
    usb_hid_register_out_callback(usb_hid_report_out_cb);
    usb_device_enable();
    for (int i = 0; i < 5000; i++) {
        uint32_t start = get_cpu_count();
        while ((get_cpu_count() - start) < 48000) {}
    }
}

"""
content = content.replace("int main(void)", usb_start_func + "int main(void)")
content = content.replace("                // Execution loop", "                start_usb_and_wait();\n                // Execution loop")
content = content.replace("            type_string(\"failed to open INJECT.BIN. error: \");", "            start_usb_and_wait();\n            type_string(\"failed to open INJECT.BIN. error: \");")
content = content.replace("        type_string(\"failed to mount sd card. error: \");", "        start_usb_and_wait();\n        type_string(\"failed to mount sd card. error: \");")

with open('src/main.c', 'w', encoding='utf-8') as f:
    f.write(content)
