#ifndef C3_KEYBOARD_HPP
#define C3_KEYBOARD_HPP

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <cstdint>

class C3Keyboard
{
public:
    C3Keyboard(i2c_master_bus_handle_t i2c_handle);
    esp_err_t init();   // Initializes the keypad
    uint32_t get_key(); // Reads a key press from the keypad

private:
    i2c_master_bus_handle_t i2c_handle;
    i2c_master_dev_handle_t keypad_dev;
};

#endif // C3_KEYBOARD_HPP