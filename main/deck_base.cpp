/*
 * Deck Base Application
 * Main application entry point for PocketSSH. Initializes hardware peripherals including
 * GPIO, display, touch input, and manages FreeRTOS tasks for keyboard input and trackball navigation.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

#include "esp_lcd_touch_gt911.h"
#include "driver/gpio.h"

#include "utilities.h"
#include "c3_keyboard.hpp"
#include "ssh_terminal.hpp"

#include "lvgl.h"

// Pepboy splash screen images
LV_IMG_DECLARE(pepboy_0);
LV_IMG_DECLARE(pepboy_1);
LV_IMG_DECLARE(pepboy_2);
LV_IMG_DECLARE(pepboy_3);
LV_IMG_DECLARE(pepboy_4);
LV_IMG_DECLARE(pepboy_5);
LV_IMG_DECLARE(pepboy_6);
LV_IMG_DECLARE(pepboy_7);

#if defined(BSP_LCD_DRAW_BUFF_SIZE)
#define DRAW_BUF_SIZE BSP_LCD_DRAW_BUFF_SIZE
#else
#define DRAW_BUF_SIZE (BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT)
#endif

static const char *TAG = "main";

static i2c_master_bus_handle_t i2c_handle;
static lv_obj_t *ssh_screen;
static SSHTerminal *ssh_terminal = NULL;

// Splash screen variables
static lv_obj_t *splash_screen = NULL;
static lv_obj_t *splash_img = NULL;
static lv_timer_t *splash_timer = NULL;
static int splash_frame = 0;
static const lv_image_dsc_t* pepboy_frames[] = {
    &pepboy_0, &pepboy_1, &pepboy_2, &pepboy_3,
    &pepboy_4, &pepboy_5, &pepboy_6, &pepboy_7
};

// Dismiss splash screen (called by touch or keyboard)
void dismiss_splash_screen()
{
    if (splash_timer) {
        lv_timer_delete(splash_timer);
        splash_timer = NULL;
    }
    if (splash_screen) {
        bsp_display_lock(0);
        lv_scr_load(ssh_screen);
        lv_obj_delete(splash_screen);
        splash_screen = NULL;
        bsp_display_unlock();
    }
}

// Touch event handler for splash screen - skip on touch
void splash_touch_cb(lv_event_t * e)
{
    dismiss_splash_screen();
}

// Splash screen animation callback
void splash_timer_cb(lv_timer_t * timer)
{
    // Update to next frame
    splash_frame++;
    
    // Wrap frame index (8 frames total) - loop indefinitely
    if (splash_frame >= 8) {
        splash_frame = 0;
    }
    
    // Update image
    lv_image_set_src(splash_img, pepboy_frames[splash_frame]);
}

void show_splash_screen()
{
    // Create splash screen
    splash_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash_screen, lv_color_black(), 0);
    
    // Add touch event to skip splash screen
    lv_obj_add_event_cb(splash_screen, splash_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(splash_screen, LV_OBJ_FLAG_CLICKABLE);
    
    // Create image object centered on screen
    splash_img = lv_image_create(splash_screen);
    lv_image_set_src(splash_img, &pepboy_0);
    lv_obj_align(splash_img, LV_ALIGN_CENTER, 0, 0);
    
    // Load splash screen
    lv_scr_load(splash_screen);
    
    // Reset counter
    splash_frame = 0;
    
    // Create timer for animation (100ms per frame for smooth walking animation)
    splash_timer = lv_timer_create(splash_timer_cb, 150, NULL);
}

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

            // If splash screen is active, dismiss it on any key press
            if (splash_screen) {
                dismiss_splash_screen();
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;  // Skip processing this key
            }

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

// Forward declaration
void load_ssh_keys_from_sd(SSHTerminal* terminal);

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

    /* Load SSH keys from SD card BEFORE LVGL initialization */
    // Note: Create a temporary terminal instance just for loading keys
    SSHTerminal* temp_terminal = new SSHTerminal();
    load_ssh_keys_from_sd(temp_terminal);

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

    // Show splash screen animation
    show_splash_screen();

    // Use the terminal instance that already has loaded keys
    ssh_terminal = temp_terminal;
    ssh_screen = ssh_terminal->create_terminal_screen();
    
    // Display version and initial instructions
#ifdef POCKETSSH_VERSION
    ssh_terminal->append_text("PocketSSH v" POCKETSSH_VERSION "\n");
#else
    ssh_terminal->append_text("PocketSSH Terminal Ready\n");
#endif

    // Display loaded SSH keys
    auto loaded_keys = ssh_terminal->get_loaded_key_names();
    if (!loaded_keys.empty()) {
        ssh_terminal->append_text("\nLoaded SSH keys:\n");
        for (const auto& keyname : loaded_keys) {
            ssh_terminal->append_text("  - ");
            ssh_terminal->append_text(keyname.c_str());
            ssh_terminal->append_text("\n");
        }
        ssh_terminal->append_text("\n");
    } else {
        ssh_terminal->append_text("\nNo SSH keys found on SD card.\n");
        ssh_terminal->append_text("Place .pem files in /sdcard/ssh_keys/\n\n");
    }
    
    // Update status bar to show initial battery voltage
    ssh_terminal->update_status_bar();

    bsp_display_unlock();

    xTaskCreate(keypad_task, "keypad_task", 4096, NULL, 5, NULL);
    
    xTaskCreate(trackball_task, "trackball_task", 4096, NULL, 5, NULL);
}

/*
 * Load SSH private keys from SD card
 * Reads all .pem files from /sdcard/ssh_keys/ directory and loads them into memory
 * Must be called BEFORE LVGL initialization to avoid SD card access conflicts
 */
void load_ssh_keys_from_sd(SSHTerminal* terminal)
{
    if (!terminal) {
        ESP_LOGE(TAG, "Terminal pointer is NULL");
        return;
    }

    ESP_LOGI(TAG, "Starting SD card key loading...");

    // SD card mount configuration
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false
    };

    sdmmc_card_t *card = NULL;
    const char mount_point[] = "/sdcard";
    
    ESP_LOGI(TAG, "Mounting SD card...");
    
    // Configure SPI bus for SD card (T-Deck Plus uses SPI mode, not SDMMC)
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_SPI_MOSI,
        .miso_io_num = BOARD_SPI_MISO,
        .sclk_io_num = BOARD_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus (%s)", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "SPI bus initialized");
    
    // Configure SPI host for SD card
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    
    // Configure SD card SPI device
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BOARD_SDCARD_CS;
    slot_config.host_id = SPI3_HOST;
    
    ESP_LOGI(TAG, "Attempting to mount SD card on SPI3...");
    
    // Mount SD card using SPI mode
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card (%s)", esp_err_to_name(ret));
        }
        return;
    }
    
    ESP_LOGI(TAG, "SD card mounted successfully");

    // Open the ssh_keys directory
    const char* keys_dir = "/sdcard/ssh_keys";
    ESP_LOGI(TAG, "Opening directory: %s", keys_dir);
    DIR* dir = opendir(keys_dir);
    
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open directory %s - creating it", keys_dir);
        mkdir(keys_dir, 0755);
        dir = opendir(keys_dir);
        if (!dir) {
            ESP_LOGE(TAG, "Still cannot open directory after creation");
            esp_vfs_fat_sdcard_unmount(mount_point, card);
            spi_bus_free(SPI3_HOST);
            return;
        }
    }

    ESP_LOGI(TAG, "Directory opened, scanning for .pem files...");

    // Read all .pem files from directory
    struct dirent* entry;
    int keys_loaded = 0;
    int files_found = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        files_found++;
        ESP_LOGI(TAG, "Found file: %s", entry->d_name);
        
        // Skip . and .. entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Check if file has .pem or .PEM extension (case-insensitive)
        size_t name_len = strlen(entry->d_name);
        if (name_len < 5) {
            ESP_LOGD(TAG, "Skipping file (name too short): %s", entry->d_name);
            continue;
        }
        
        const char* ext = entry->d_name + name_len - 4;
        if (strcasecmp(ext, ".pem") != 0) {
            ESP_LOGI(TAG, "Skipping non-PEM file: %s", entry->d_name);
            continue;
        }
        
        ESP_LOGI(TAG, "Processing PEM file: %s", entry->d_name);
        
        // Build full file path
        char filepath[256];
        snprintf(filepath, sizeof(filepath), "%s/%s", keys_dir, entry->d_name);
        
        // Open and read the key file
        FILE* f = fopen(filepath, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open key file: %s", filepath);
            continue;
        }
        
        // Get file size
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        if (file_size <= 0 || file_size > 16384) {
            ESP_LOGW(TAG, "Invalid key file size: %s (%ld bytes)", entry->d_name, file_size);
            fclose(f);
            continue;
        }
        
        // Allocate buffer and read key data
        char* key_data = (char*)malloc(file_size + 1);
        if (!key_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for key: %s", entry->d_name);
            fclose(f);
            continue;
        }
        
        size_t bytes_read = fread(key_data, 1, file_size, f);
        fclose(f);
        
        if (bytes_read != (size_t)file_size) {
            ESP_LOGE(TAG, "Failed to read complete key file: %s", entry->d_name);
            free(key_data);
            continue;
        }
        
        key_data[file_size] = '\0';  // Null terminate
        
        // Load key into terminal's memory
        terminal->load_key_from_memory(entry->d_name, key_data, file_size);
        free(key_data);
        keys_loaded++;
    }
    
    closedir(dir);
    
    ESP_LOGI(TAG, "Total files found: %d, Keys loaded: %d", files_found, keys_loaded);
    
    // Unmount SD card and deinitialize SPI bus
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    spi_bus_free(SPI3_HOST);
    ESP_LOGI(TAG, "SD card unmounted, %d SSH keys loaded", keys_loaded);
}
