/*
 * Deck Base Application
 * Main application entry point for PocketSSH. Initializes hardware peripherals including
 * GPIO, display, touch input, and manages FreeRTOS tasks for keyboard input and trackball navigation.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"

#include "esp_lcd_touch_gt911.h"
#include "driver/gpio.h"

#include "utilities.h"
#include "c3_keyboard.hpp"
#include "ssh_terminal.hpp"

#include "lvgl.h"

#if defined(BSP_LCD_DRAW_BUFF_SIZE)
#define DRAW_BUF_SIZE BSP_LCD_DRAW_BUFF_SIZE
#else
#define DRAW_BUF_SIZE (BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT)
#endif

static const char *TAG = "main";

static i2c_master_bus_handle_t i2c_handle;
static lv_obj_t *ssh_screen;
static SSHTerminal *ssh_terminal = NULL;

void keypad_task(void *param)
{
    C3Keyboard keyboard(i2c_handle);
    if (keyboard.init() != ESP_OK)
    {
        ESP_LOGE("KEYPAD", "Failed to initialize keypad!");
        vTaskDelete(NULL);
        return;
    }

    while (1)
    {
        uint32_t key = keyboard.get_key();
        if (key)
        {
            ESP_LOGI("KEYPAD", "Key Pressed: %c", (unsigned char)key);

            // Send key input to SSH terminal with display lock
            if (ssh_terminal && ssh_screen) {
                if (bsp_display_lock(0)) {
                    ssh_terminal->handle_key_input((char)key);
                    bsp_display_unlock();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); // Shorter delay for better responsiveness
    }
}

void trackball_task(void *param)
{
    static bool last_up = true;
    static bool last_down = true;
    static bool last_press = true;
    static uint32_t press_start_time = 0;
    const uint32_t LONG_PRESS_MS = 1000; // 1 second for long press
    
    while (1) {
        bool up = gpio_get_level(BOARD_TBOX_G01);
        bool down = gpio_get_level(BOARD_TBOX_G03);
        bool press = gpio_get_level(BOARD_BOOT_PIN);
        
        // Detect falling edge (button press)
        if (!up && last_up) {
            // Non-blocking: prioritize screen updates, drop input if display is busy
            if (ssh_terminal && bsp_display_lock(0)) {
                ssh_terminal->navigate_history(1); // Older command
                bsp_display_unlock();
            }
        }
        if (!down && last_down) {
            // Non-blocking: prioritize screen updates, drop input if display is busy
            if (ssh_terminal && bsp_display_lock(0)) {
                ssh_terminal->navigate_history(-1); // Newer command
                bsp_display_unlock();
            }
        }
        
        // Trackball press button handling
        if (!press && last_press) {
            // Button just pressed - record start time
            press_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        } else if (press && !last_press) {
            // Button released - check duration
            uint32_t press_duration = (xTaskGetTickCount() * portTICK_PERIOD_MS) - press_start_time;
            
            if (ssh_terminal && bsp_display_lock(0)) {
                if (press_duration >= LONG_PRESS_MS) {
                    // Long press: Delete current history entry
                    ESP_LOGI("TRACKBALL", "Long press detected (%lu ms) - deleting command", press_duration);
                    ssh_terminal->delete_current_history_entry();
                } else {
                    // Short press: Execute current input (like Enter key)
                    ESP_LOGI("TRACKBALL", "Short press detected (%lu ms) - executing current input", press_duration);
                    ssh_terminal->handle_key_input('\n');
                }
                bsp_display_unlock();
            }
        }
        
        last_up = up;
        last_down = down;
        last_press = press;
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t _bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    /* Initilize I2C */
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Initialize I2C Fail");
        return ret;
    }

    /* Initialize touch */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_V_RES,
        .y_max = BSP_LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_16,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .process_coordinates = NULL,
        .interrupt_callback = NULL,
        .user_data = NULL,
        .driver_data = NULL,
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_LOGI("Touch", "Initialize LCD Touch: GT911");
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    #pragma GCC diagnostic pop
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;

    i2c_handle = bsp_i2c_get_handle();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), "TOuch", "");
    return esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, ret_touch);
}

void device_init(void)
{
    /* Initialize power on pin */
    gpio_reset_pin(BOARD_POWERON);
    gpio_set_direction(BOARD_POWERON, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_POWERON, 1);

    /* Initialize GPIO for SD card CS */
    gpio_reset_pin(BOARD_SDCARD_CS);
    gpio_set_direction(BOARD_SDCARD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_SDCARD_CS, 1);

    /* Initialize radio CS pin */
    gpio_reset_pin(RADIO_CS_PIN);
    gpio_set_direction(RADIO_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(RADIO_CS_PIN, 1);

    /* Initialize TFT CS pin */
    gpio_reset_pin(BOARD_TFT_CS);
    gpio_set_direction(BOARD_TFT_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_TFT_CS, 1);

    /* Configure MISO with pull-up for SD card (must be done before SPI bus init) */
    gpio_reset_pin(BOARD_SPI_MISO);
    gpio_set_direction(BOARD_SPI_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_SPI_MISO, GPIO_PULLUP_ONLY);

    // Initialize trackball GPIOs for command history navigation
    gpio_reset_pin(BOARD_TBOX_G01);
    gpio_set_direction(BOARD_TBOX_G01, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_TBOX_G01, GPIO_PULLUP_ONLY);
    
    gpio_reset_pin(BOARD_TBOX_G03);
    gpio_set_direction(BOARD_TBOX_G03, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_TBOX_G03, GPIO_PULLUP_ONLY);
    
    // Initialize trackball press button (BOOT button on GPIO 0)
    gpio_reset_pin(BOARD_BOOT_PIN);
    gpio_set_direction(BOARD_BOOT_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOARD_BOOT_PIN, GPIO_PULLUP_ONLY);
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize device GPIOs */
    device_init();

    /* Initialize display and LVGL */
    lv_display_t *disp = bsp_display_start();

    /* Set display brightness to 100% */
    bsp_display_backlight_on();

    /* Initialize touch */
    esp_lcd_touch_handle_t touch_handle = NULL;
    const bsp_touch_config_t bsp_touch_cfg = {};
    _bsp_touch_new(&bsp_touch_cfg, &touch_handle);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = touch_handle,
        .scale = {.x = 0, .y = 0},
    };

    lvgl_port_add_touch(&touch_cfg);

    bsp_display_lock(0);

    // Create and display PocketSSH terminal screen
    ssh_terminal = new SSHTerminal();
    ssh_screen = ssh_terminal->create_terminal_screen();
    
    // Display version and initial instructions
#ifdef POCKETSSH_VERSION
    ssh_terminal->append_text("PocketSSH v" POCKETSSH_VERSION "\n");
#else
    ssh_terminal->append_text("PocketSSH Terminal Ready\n");
#endif
   
    lv_scr_load(ssh_screen);
    
    // Update status bar to show initial battery voltage
    ssh_terminal->update_status_bar();

    bsp_display_unlock();

    xTaskCreate(keypad_task, "keypad_task", 4096, NULL, 5, NULL);
    
    xTaskCreate(trackball_task, "trackball_task", 4096, NULL, 5, NULL);
}
