/*
 * SSHTerminal Implementation
 * Core SSH terminal functionality including WiFi connectivity, libssh2 session management,
 * command execution, terminal display rendering with LVGL, and command history management.
 */

#include "ssh_terminal.hpp"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "bsp/esp-bsp.h"
#include <cstring>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>

static const char *TAG = "SSH_TERMINAL";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;
#define WIFI_MAXIMUM_RETRY 5

static esp_event_handler_instance_t s_instance_any_id = NULL;
static esp_event_handler_instance_t s_instance_got_ip = NULL;

SSHTerminal::SSHTerminal() 
    : terminal_screen(NULL), 
      terminal_output(NULL), 
      input_label(NULL),
      status_bar(NULL),
      byte_counter_label(NULL),
      side_panel(NULL),
      cursor_pos(0),
      bytes_received(0),
      history_index(-1),
      cursor_blink_timer(NULL),
      cursor_visible(true),
      battery_update_timer(NULL),
      history_needs_save(false),
      history_save_timer(NULL),
      last_display_update(0),
      wifi_connected(false),
      ssh_connected(false),
      battery_initialized(false),
      ssh_socket(-1),
      session(NULL),
      channel(NULL),
      hostname(NULL),
      port_number(22)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "Initializing battery measurement...");
    esp_err_t battery_ret = battery.init();
    if (battery_ret == ESP_OK) {
        battery_initialized = true;
        ESP_LOGI(TAG, "Battery measurement initialized successfully");
        float test_voltage = battery.readBatteryVoltage();
        ESP_LOGI(TAG, "Test battery read: %.2fV", test_voltage);
    } else {
        battery_initialized = false;
        ESP_LOGE(TAG, "Battery measurement initialization FAILED: %s", esp_err_to_name(battery_ret));
    }
    
    load_history_from_nvs();
}

SSHTerminal::~SSHTerminal() 
{
    disconnect();
    if (hostname) {
        free(hostname);
    }
    if (cursor_blink_timer) {
        lv_timer_del(cursor_blink_timer);
    }
    if (battery_update_timer) {
        lv_timer_del(battery_update_timer);
    }
    if (history_save_timer) {
        lv_timer_del(history_save_timer);
        if (history_needs_save) {
            save_history_to_nvs();
        }
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    SSHTerminal* terminal = (SSHTerminal*)arg;
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        
        s_retry_num = 0;
        ESP_LOGI(TAG, "Setting WIFI_CONNECTED_BIT in event group %p", s_wifi_event_group);
        EventBits_t result = xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Event group bits after setting: 0x%x", result);
        
        if (terminal) {
            char ip_str[64];
            snprintf(ip_str, sizeof(ip_str), "WiFi Connected! IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
            
            if (bsp_display_lock(0)) {
                terminal->append_text(ip_str);
                bsp_display_unlock();
            }
        }
    }
}

esp_err_t SSHTerminal::init_wifi(const char* ssid, const char* password)
{
    ESP_LOGI(TAG, "Initializing WiFi...");
    
    static bool wifi_initialized = false;
    if (wifi_initialized) {
        ESP_LOGI(TAG, "Cleaning up previous WiFi instance...");
        
        if (s_instance_any_id) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_instance_any_id);
            s_instance_any_id = NULL;
        }
        if (s_instance_got_ip) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_instance_got_ip);
            s_instance_got_ip = NULL;
        }
        
        esp_wifi_stop();
        esp_wifi_deinit();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    s_retry_num = 0;
    
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    
    if (!wifi_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        wifi_initialized = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        this,
                                                        &s_instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        this,
                                                        &s_instance_got_ip));

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init finished. Event group: %p", s_wifi_event_group);

    const int max_wait_ms = 15000;
    const int check_interval_ms = 500;
    int elapsed_ms = 0;
    
    while (elapsed_ms < max_wait_ms) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(check_interval_ms));
        
        if (elapsed_ms % 5000 == 0) {
            ESP_LOGI(TAG, "Waiting... bits: 0x%x, elapsed: %d ms", bits, elapsed_ms);
        }

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to AP SSID:%s", ssid);
            wifi_connected = true;
            
            if (bsp_display_lock(0)) {
                update_status_bar();
                bsp_display_unlock();
            }
            return ESP_OK;
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGI(TAG, "Failed to connect to SSID:%s", ssid);
            wifi_connected = false;
            
            if (bsp_display_lock(0)) {
                update_status_bar();
                bsp_display_unlock();
            }
            return ESP_FAIL;
        }
        
        if (bsp_display_lock(0)) {
            append_text(".");
            bsp_display_unlock();
        }
        elapsed_ms += check_interval_ms;
    }
    
    ESP_LOGE(TAG, "Connection timeout");
    wifi_connected = false;
    s_retry_num = 0;
    
    if (bsp_display_lock(0)) {
        update_status_bar();
        bsp_display_unlock();
    }
    return ESP_FAIL;
}

bool SSHTerminal::is_wifi_connected()
{
    return wifi_connected;
}

lv_obj_t* SSHTerminal::create_terminal_screen()
{
    terminal_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(terminal_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(terminal_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(terminal_screen, LV_OBJ_FLAG_SCROLLABLE);

    status_bar = lv_label_create(terminal_screen);
    lv_label_set_text(status_bar, "Status: Disconnected");
    lv_obj_set_style_text_color(status_bar, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(status_bar, &lv_font_montserrat_12, 0);
    lv_obj_align(status_bar, LV_ALIGN_TOP_LEFT, 5, 5);
    
    byte_counter_label = lv_label_create(terminal_screen);
    lv_label_set_text(byte_counter_label, "0 B");
    lv_obj_set_style_text_color(byte_counter_label, lv_color_hex(0x00FFFF), 0);
    lv_obj_set_style_text_font(byte_counter_label, &lv_font_montserrat_12, 0);
    lv_obj_align(byte_counter_label, LV_ALIGN_TOP_RIGHT, -5, 5);

    terminal_output = lv_textarea_create(terminal_screen);
    lv_obj_set_size(terminal_output, lv_pct(100), lv_pct(75));
    lv_obj_align(terminal_output, LV_ALIGN_TOP_MID, 0, 25);
    lv_obj_set_style_bg_color(terminal_output, lv_color_black(), 0);
    lv_obj_set_style_text_color(terminal_output, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(terminal_output, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_color(terminal_output, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(terminal_output, 2, 0);
    lv_textarea_set_cursor_click_pos(terminal_output, false);
    lv_textarea_set_one_line(terminal_output, false);
    lv_obj_set_scrollbar_mode(terminal_output, LV_SCROLLBAR_MODE_OFF);
    
    lv_obj_clear_flag(terminal_output, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_anim_time(terminal_output, 0, LV_PART_CURSOR);
    lv_obj_set_style_opa(terminal_output, LV_OPA_TRANSP, LV_PART_CURSOR);
    
    lv_obj_set_scroll_snap_x(terminal_output, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(terminal_output, LV_SCROLL_SNAP_NONE);
    lv_obj_clear_flag(terminal_output, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(terminal_output, LV_OBJ_FLAG_SCROLL_ELASTIC);

    lv_obj_t* input_container = lv_obj_create(terminal_screen);
    lv_obj_set_size(input_container, lv_pct(100) - 10, 25);
    lv_obj_set_style_bg_opa(input_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_container, 0, 0);
    lv_obj_set_style_pad_all(input_container, 0, 0);
    lv_obj_set_scrollbar_mode(input_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(input_container, LV_DIR_HOR);
    lv_obj_align(input_container, LV_ALIGN_BOTTOM_LEFT, 5, -5);
    
    input_label = lv_label_create(input_container);
    lv_label_set_text(input_label, "> ");
    lv_obj_set_style_text_color(input_label, lv_color_hex(0xFFFF00), 0);
    lv_obj_set_style_text_font(input_label, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(input_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(input_label, LV_ALIGN_LEFT_MID, 0, 0);
    
    // Enable touch events on input label
    lv_obj_add_flag(input_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(input_label, input_touch_event_cb, LV_EVENT_CLICKED, this);

    create_side_panel();
    
    lv_obj_add_event_cb(terminal_screen, gesture_event_cb, LV_EVENT_GESTURE, this);
    lv_obj_clear_flag(terminal_screen, LV_OBJ_FLAG_GESTURE_BUBBLE);
    
    cursor_blink_timer = lv_timer_create(cursor_blink_cb, 500, this);
    
    battery_update_timer = lv_timer_create(battery_update_cb, 60000, this);
    
    history_save_timer = lv_timer_create(history_save_cb, 5000, this);

    const char* logo = 
        "\n"
        "        ==================================\n"
        "        Portable SSH Terminal for ESP32-S3\n"
        "        ==================================\n"
        "\n"
        "Commands:\n"
        "  connect <SSID> <PASSWORD>  - Connect to WiFi\n"
        "  ssh <HOST> <PORT> <USER> <PASS> - Connect via SSH\n"
        "  disconnect - Close WiFi connection\n"
        "  exit - Close SSH connection\n"
        "  clear - Clear terminal\n"
        "\n"
        "Ready. Type 'connect' to start...\n\n";
    
    lv_textarea_set_text(terminal_output, logo);

    return terminal_screen;
}

void SSHTerminal::append_text(const char* text)
{
    if (!terminal_output || !text) {
        return;
    }
    
    int64_t start_time = esp_timer_get_time() / 1000;
    
    const char* current_text = lv_textarea_get_text(terminal_output);
    size_t current_len = current_text ? strlen(current_text) : 0;
    size_t new_len = strlen(text);
    
    const size_t MAX_BUFFER_SIZE = 2048;
    
    if (current_len > MAX_BUFFER_SIZE * 0.8) {
        lv_textarea_set_text(terminal_output, "...[cleared]\n");
        current_len = 14;
        
        int64_t elapsed = (esp_timer_get_time() / 1000) - start_time;
        if (elapsed > 500) {
            ESP_LOGW(TAG, "Text clear took %lld ms, skipping append", elapsed);
            return;
        }
    }
    
    const size_t MAX_CHUNK = 256;
    if (new_len > MAX_CHUNK) {
        text = text + (new_len - MAX_CHUNK);
        new_len = MAX_CHUNK;
    }
    
    if (current_len + new_len < MAX_BUFFER_SIZE) {
        lv_textarea_add_text(terminal_output, text);
    }
    
    int64_t total_time = (esp_timer_get_time() / 1000) - start_time;
    if (total_time > 1000) {
        ESP_LOGW(TAG, "append_text took %lld ms - LVGL heap may be fragmented", total_time);
    }
}

void SSHTerminal::clear_terminal()
{
    if (terminal_output) {
        lv_textarea_set_text(terminal_output, "");
    }
}

void SSHTerminal::handle_key_input(char key)
{
    if (key == '\n' || key == '\r') {
        if (!current_input.empty()) {
            append_text("\n> ");
            append_text(current_input.c_str());
            append_text("\n");
            
            if (current_input.rfind("connect ", 0) == 0) {
                size_t first_space = current_input.find(' ');
                size_t second_space = current_input.find(' ', first_space + 1);
                
                if (second_space != std::string::npos) {
                    std::string ssid = current_input.substr(first_space + 1, second_space - first_space - 1);
                    std::string password = current_input.substr(second_space + 1);
                    
                    append_text("Connecting to WiFi: ");
                    append_text(ssid.c_str());
                    append_text("\n");
                    
                    if (init_wifi(ssid.c_str(), password.c_str()) == ESP_OK) {
                        append_text("WiFi connected successfully!\n");
                    } else {
                        append_text("WiFi connection failed!\n");
                    }
                } else {
                    append_text("Usage: connect <SSID> <PASSWORD>\n");
                }
            }
            else if (current_input.rfind("ssh ", 0) == 0) {
                std::vector<std::string> parts;
                size_t pos = 0;
                std::string temp = current_input;
                
                while ((pos = temp.find(' ')) != std::string::npos) {
                    parts.push_back(temp.substr(0, pos));
                    temp.erase(0, pos + 1);
                }
                parts.push_back(temp);
                
                if (parts.size() >= 5) {
                    std::string host = parts[1];
                    int port = std::atoi(parts[2].c_str());
                    std::string user = parts[3];
                    std::string pass = parts[4];
                    
                    connect(host.c_str(), port, user.c_str(), pass.c_str());
                } else {
                    append_text("Usage: ssh <HOST> <PORT> <USER> <PASS>\n");
                }
            }
            else if (current_input == "disconnect") {
                if (wifi_connected) {
                    append_text("Disconnecting WiFi...\n");
                    s_retry_num = WIFI_MAXIMUM_RETRY;
                    esp_wifi_disconnect();
                    wifi_connected = false;
                    update_status_bar();
                    append_text("WiFi disconnected\n");
                } else {
                    append_text("WiFi not connected\n");
                }
            }
            else if (current_input == "exit") {
                disconnect();
            }
            else if (current_input == "clear") {
                clear_terminal();
            }
            else if (current_input == "help") {
                append_text("Available commands:\n");
                append_text("  connect <SSID> <PASSWORD> - Connect to WiFi\n");
                append_text("  ssh <HOST> <PORT> <USER> <PASS> - Connect via SSH\n");
                append_text("  disconnect - Disconnect WiFi\n");
                append_text("  exit - Disconnect SSH\n");
                append_text("  clear - Clear terminal\n");
                append_text("  help - Show this help\n");
            }
            else if (ssh_connected) {
                send_command(current_input.c_str());
            } else {
                append_text("Unknown command. Type 'help' for commands.\n");
            }
            
            auto it = std::find(command_history.begin(), command_history.end(), current_input);
            if (it != command_history.end()) {
                command_history.erase(it);
            }
            command_history.push_back(current_input);
            history_needs_save = true;
            current_input.clear();
            cursor_pos = 0;
            history_index = -1;
        }
    } else if (key == 8 || key == 127) {
        // Backspace - delete character before cursor
        if (cursor_pos > 0 && !current_input.empty()) {
            current_input.erase(cursor_pos - 1, 1);
            cursor_pos--;
        }
    } else if (key >= 32 && key <= 126) {
        // Insert character at cursor position
        current_input.insert(cursor_pos, 1, key);
        cursor_pos++;
    }
    
    update_input_display();
}

void SSHTerminal::update_input_display()
{
    if (!input_label) {
        return;
    }
    
    // Ensure cursor_pos is within bounds
    if (cursor_pos > current_input.length()) {
        cursor_pos = current_input.length();
    }
    
    std::string full_text = "> " + current_input;
    
    // Insert cursor at correct position
    if (cursor_visible) {
        size_t display_pos = 2 + cursor_pos; // 2 = length of "> "
        full_text.insert(display_pos, "|");
    }
    
    lv_label_set_text(input_label, full_text.c_str());
    
    lv_obj_t* container = lv_obj_get_parent(input_label);
    if (container) {
        lv_obj_scroll_to_x(container, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void SSHTerminal::input_touch_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_obj_t* input_label = (lv_obj_t* )lv_event_get_target(e);
    
    if (!terminal || !input_label) {
        return;
    }
    
    // Get the touch point relative to the label
    lv_point_t point;
    lv_indev_get_point(lv_indev_get_act(), &point);
    
    // Convert to label coordinates
    lv_obj_t* label = input_label;
    lv_area_t label_coords;
    lv_obj_get_coords(label, &label_coords);
    
    int32_t click_x = point.x - label_coords.x1;
    
    // Get font and calculate character position
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    
    // Account for "> " prefix (2 characters)
    const char* prefix = "> ";
    lv_point_t prefix_size;
    lv_txt_get_size(&prefix_size, prefix, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t prefix_width = prefix_size.x;
    
    if (click_x <= prefix_width) {
        // Clicked on prefix, set cursor to start
        terminal->cursor_pos = 0;
    } else {
        // Calculate which character was clicked
        int32_t text_x = click_x - prefix_width;
        size_t best_pos = 0;
        int32_t min_distance = INT32_MAX;
        
        // Check each character position
        for (size_t i = 0; i <= terminal->current_input.length(); i++) {
            std::string substr = terminal->current_input.substr(0, i);
            lv_point_t substr_size;
            lv_txt_get_size(&substr_size, substr.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int32_t char_x = substr_size.x;
            int32_t distance = abs(text_x - char_x);
            
            if (distance < min_distance) {
                min_distance = distance;
                best_pos = i;
            }
        }
        
        terminal->cursor_pos = best_pos;
    }
    
    // Reset cursor blink to make it visible
    terminal->cursor_visible = true;
    terminal->update_input_display();
    
    ESP_LOGI(TAG, "Cursor moved to position: %d", terminal->cursor_pos);
}

void SSHTerminal::cursor_blink_cb(lv_timer_t* timer)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_timer_get_user_data(timer);
    if (terminal) {
        terminal->cursor_visible = !terminal->cursor_visible;
        terminal->update_input_display();
    }
}

void SSHTerminal::battery_update_cb(lv_timer_t* timer)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_timer_get_user_data(timer);
    if (terminal) {
        if (bsp_display_lock(0)) {
            terminal->update_status_bar();
            bsp_display_unlock();
        }
    }
}

void SSHTerminal::history_save_cb(lv_timer_t* timer)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_timer_get_user_data(timer);
    if (terminal && terminal->history_needs_save) {
        terminal->history_needs_save = false;
        terminal->save_history_to_nvs();
    }
}

void SSHTerminal::navigate_history(int direction)
{
    if (command_history.empty()) {
        return;
    }
    
    if (direction > 0) {
        if (history_index < (int)command_history.size() - 1) {
            history_index++;
            current_input = command_history[command_history.size() - 1 - history_index];
        }
    } else if (direction < 0) {
        if (history_index > 0) {
            history_index--;
            current_input = command_history[command_history.size() - 1 - history_index];
        } else if (history_index == 0) {
            history_index = -1;
            current_input.clear();
        }
    }
    
    // Move cursor to end of input
    cursor_pos = current_input.length();
    update_input_display();
}

void SSHTerminal::move_cursor_left()
{
    if (cursor_pos > 0) {
        cursor_pos--;
        cursor_visible = true;
        update_input_display();
    }
}

void SSHTerminal::move_cursor_right()
{
    if (cursor_pos < current_input.length()) {
        cursor_pos++;
        cursor_visible = true;
        update_input_display();
    }
}

void SSHTerminal::move_cursor_home()
{
    cursor_pos = 0;
    cursor_visible = true;
    update_input_display();
}

void SSHTerminal::move_cursor_end()
{
    cursor_pos = current_input.length();
    cursor_visible = true;
    update_input_display();
}

void SSHTerminal::delete_current_history_entry()
{
    if (command_history.empty() || history_index < 0) {
        ESP_LOGW(TAG, "No history entry to delete (empty or not navigating)");
        return;
    }
    
    size_t actual_index = command_history.size() - 1 - history_index;
    
    ESP_LOGI(TAG, "Deleting history entry: '%s' (index %d)", 
             command_history[actual_index].c_str(), (int)actual_index);
    
    command_history.erase(command_history.begin() + actual_index);
    
    history_needs_save = true;
    
    if (command_history.empty()) {
        history_index = -1;
        current_input.clear();
    } else if (history_index >= (int)command_history.size()) {
        history_index = command_history.size() - 1;
        current_input = command_history[command_history.size() - 1 - history_index];
    } else if (actual_index < command_history.size()) {
        current_input = command_history[command_history.size() - 1 - history_index];
    } else {
        history_index = -1;
        current_input.clear();
    }
    
    std::string display_text = "> " + current_input;
    if (input_label) {
        lv_label_set_text(input_label, display_text.c_str());
    }
    
    ESP_LOGI(TAG, "History entry deleted. Remaining entries: %d", (int)command_history.size());
}

void SSHTerminal::send_current_history_command()
{
    if (command_history.empty() || history_index < 0) {
        ESP_LOGW(TAG, "No history command to send (empty or not navigating)");
        return;
    }
    
    size_t actual_index = command_history.size() - 1 - history_index;
    std::string cmd_to_send = command_history[actual_index];
    
    ESP_LOGI(TAG, "Sending history command: '%s'", cmd_to_send.c_str());
    
    current_input = cmd_to_send;
    
    std::string display_text = "> " + current_input;
    if (input_label) {
        lv_label_set_text(input_label, display_text.c_str());
    }
    
    send_command(cmd_to_send.c_str());
    
    auto it = std::find(command_history.begin(), command_history.end(), current_input);
    if (it != command_history.end()) {
        command_history.erase(it);
    }
    command_history.push_back(current_input);
    history_needs_save = true;
    current_input.clear();
    history_index = -1;
    
    display_text = "> ";
    if (input_label) {
        lv_label_set_text(input_label, display_text.c_str());
    }
}

void SSHTerminal::load_history_from_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading history: %s", esp_err_to_name(err));
        return;
    }
    
    uint32_t history_count = 0;
    err = nvs_get_u32(nvs_handle, "hist_count", &history_count);
    if (err != ESP_OK || history_count == 0) {
        ESP_LOGI(TAG, "No command history found in NVS");
        nvs_close(nvs_handle);
        return;
    }
    
    ESP_LOGI(TAG, "Loading %lu commands from NVS...", history_count);
    
    command_history.clear();
    for (uint32_t i = 0; i < history_count && i < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist_%lu", i);
        
        size_t required_size = 0;
        err = nvs_get_str(nvs_handle, key, NULL, &required_size);
        if (err != ESP_OK) {
            continue;
        }
        
        char* cmd = (char*)malloc(required_size);
        if (cmd) {
            err = nvs_get_str(nvs_handle, key, cmd, &required_size);
            if (err == ESP_OK) {
                command_history.push_back(std::string(cmd));
            }
            free(cmd);
        }
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded %d commands from NVS", (int)command_history.size());
}

void SSHTerminal::save_history_to_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing history: %s", esp_err_to_name(err));
        return;
    }
    
    size_t start_idx = 0;
    if (command_history.size() > 100) {
        start_idx = command_history.size() - 100;
    }
    
    uint32_t history_count = command_history.size() - start_idx;
    err = nvs_set_u32(nvs_handle, "hist_count", history_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save history count: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }
    
    int saved_count = 0;
    for (size_t i = start_idx; i < command_history.size() && saved_count < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist_%lu", (unsigned long)(i - start_idx));
        
        err = nvs_set_str(nvs_handle, key, command_history[i].c_str());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save command %zu: %s", i, esp_err_to_name(err));
        } else {
            saved_count++;
        }
        
        if (saved_count % 10 == 0) {
            vTaskDelay(1);
        }
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved %d commands to NVS", saved_count);
    }
    
    nvs_close(nvs_handle);
}

void SSHTerminal::clear_history_nvs()
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return;
    }
    
    uint32_t history_count = 0;
    nvs_get_u32(nvs_handle, "hist_count", &history_count);
    
    for (uint32_t i = 0; i < history_count && i < 100; i++) {
        char key[16];
        snprintf(key, sizeof(key), "hist_%lu", i);
        nvs_erase_key(nvs_handle, key);
    }
    
    nvs_erase_key(nvs_handle, "hist_count");
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

int SSHTerminal::waitsocket(int socket_fd, LIBSSH2_SESSION *session)
{
    struct timeval timeout;
    int rc;
    fd_set fd;
    fd_set *writefd = NULL;
    fd_set *readfd = NULL;
    int dir;

    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    FD_ZERO(&fd);
    FD_SET(socket_fd, &fd);

    dir = libssh2_session_block_directions(session);

    if(dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        readfd = &fd;

    if(dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        writefd = &fd;

    rc = select(socket_fd + 1, readfd, writefd, NULL, &timeout);

    return rc;
}

esp_err_t SSHTerminal::connect(const char* host, int port, const char* username, const char* password)
{
    if (!wifi_connected) {
        ESP_LOGE(TAG, "WiFi not connected");
        append_text("ERROR: WiFi not connected\n");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Connecting to %s:%d", host, port);
    append_text("Connecting to ");
    append_text(host);
    append_text("...\n");

    int rc = libssh2_init(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "libssh2 initialization failed (%d)", rc);
        append_text("ERROR: libssh2 init failed\n");
        return ESP_FAIL;
    }

    struct sockaddr_in sin;
    ssh_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ssh_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        append_text("ERROR: Failed to create socket\n");
        libssh2_exit();
        return ESP_FAIL;
    }

    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = inet_addr(host);

    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(ssh_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(ssh_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (::connect(ssh_socket, (struct sockaddr*)(&sin), sizeof(struct sockaddr_in)) != 0) {
        ESP_LOGE(TAG, "Failed to connect socket");
        append_text("ERROR: Failed to connect socket\n");
        close(ssh_socket);
        ssh_socket = -1;
        libssh2_exit();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Socket connected");
    append_text("Socket connected, initializing SSH session...\n");

    session = libssh2_session_init();
    if (!session) {
        ESP_LOGE(TAG, "Failed to create SSH session");
        append_text("ERROR: Failed to create SSH session\n");
        close(ssh_socket);
        ssh_socket = -1;
        libssh2_exit();
        return ESP_FAIL;
    }

    libssh2_session_set_blocking(session, 0);

    append_text("Performing SSH handshake...\n");
    while ((rc = libssh2_session_handshake(session, ssh_socket)) == LIBSSH2_ERROR_EAGAIN);
    
    if (rc) {
        ESP_LOGE(TAG, "SSH handshake failed: %d", rc);
        append_text("ERROR: SSH handshake failed\n");
        disconnect();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SSH handshake successful");
    append_text("SSH handshake successful\n");

    if (ssh_authenticate(username, password) != ESP_OK) {
        append_text("ERROR: Authentication failed\n");
        disconnect();
        return ESP_FAIL;
    }

    append_text("Authentication successful\n");

    if (ssh_open_channel() != ESP_OK) {
        append_text("ERROR: Failed to open channel\n");
        disconnect();
        return ESP_FAIL;
    }

    append_text("SSH channel opened - connected!\n");
    ssh_connected = true;
    update_status_bar();

    xTaskCreate(ssh_receive_task, "ssh_rx", 8192, this, 5, NULL);

    return ESP_OK;
}

esp_err_t SSHTerminal::ssh_authenticate(const char* username, const char* password)
{
    append_text("Authenticating as ");
    append_text(username);
    append_text("...\n");

    int rc;
    while ((rc = libssh2_userauth_password(session, username, password)) == LIBSSH2_ERROR_EAGAIN);
    
    if (rc) {
        char *err_msg;
        int err_len;
        libssh2_session_last_error(session, &err_msg, &err_len, 0);
        ESP_LOGE(TAG, "Authentication failed: %s", err_msg);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Authentication successful");
    return ESP_OK;
}

esp_err_t SSHTerminal::ssh_open_channel()
{
    append_text("Opening SSH channel...\n");

    int rc;
    while ((channel = libssh2_channel_open_session(session)) == NULL &&
           libssh2_session_last_error(session, NULL, NULL, 0) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(ssh_socket, session);
    }

    if (channel == NULL) {
        ESP_LOGE(TAG, "Failed to open channel");
        return ESP_FAIL;
    }

    while ((rc = libssh2_channel_request_pty(channel, "vt100")) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(ssh_socket, session);
    }
    
    if (rc) {
        ESP_LOGE(TAG, "Failed to request PTY");
        return ESP_FAIL;
    }

    while ((rc = libssh2_channel_shell(channel)) == LIBSSH2_ERROR_EAGAIN) {
        waitsocket(ssh_socket, session);
    }
    
    if (rc) {
        ESP_LOGE(TAG, "Failed to start shell");
        return ESP_FAIL;
    }

    libssh2_channel_set_blocking(channel, 0);

    ESP_LOGI(TAG, "SSH channel opened successfully");
    return ESP_OK;
}

esp_err_t SSHTerminal::disconnect()
{
    ssh_connected = false;
    
    if (channel) {
        libssh2_channel_free(channel);
        channel = NULL;
    }

    if (session) {
        libssh2_session_disconnect(session, "Normal Shutdown");
        libssh2_session_free(session);
        session = NULL;
    }

    if (ssh_socket >= 0) {
        close(ssh_socket);
        ssh_socket = -1;
    }

    libssh2_exit();
    
    if (bsp_display_lock(0)) {
        update_status_bar();
        append_text("\nDisconnected\n");
        bsp_display_unlock();
    }
    
    ESP_LOGI(TAG, "Disconnected");
    
    return ESP_OK;
}

bool SSHTerminal::is_connected()
{
    return ssh_connected;
}

void SSHTerminal::send_command(const char* cmd)
{
    if (!channel) {
        return;
    }
    
    bytes_received = 0;
    if (byte_counter_label && bsp_display_lock(0)) {
        lv_label_set_text(byte_counter_label, "0 B");
        bsp_display_unlock();
    }

    std::string full_cmd = std::string(cmd) + "\n";
    ssize_t nwritten = 0;
    int retry_count = 0;
    const int MAX_RETRIES = 50;
    
    ESP_LOGI(TAG, "Sending command: %s", cmd);
    
    while (nwritten < (ssize_t)full_cmd.length() && retry_count < MAX_RETRIES) {
        ssize_t n = libssh2_channel_write(channel, full_cmd.c_str() + nwritten, 
                                          full_cmd.length() - nwritten);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            int wait_result = waitsocket(ssh_socket, session);
            if (wait_result <= 0) {
                retry_count++;
                if (retry_count >= MAX_RETRIES) {
                    ESP_LOGE(TAG, "Send command timeout after %d retries", retry_count);
                    break;
                }
            }
            continue;
        }
        if (n < 0) {
            ESP_LOGE(TAG, "Failed to write to channel: %d", (int)n);
            break;
        }
        nwritten += n;
        retry_count = 0;
    }
    
    ESP_LOGI(TAG, "Command sent: %d bytes", (int)nwritten);
}

void SSHTerminal::ssh_receive_task(void* param)
{
    SSHTerminal* terminal = (SSHTerminal*)param;
    char buffer[1024];
    ssize_t rc;

    ESP_LOGI(TAG, "SSH receive task started");

    while (terminal->ssh_connected && terminal->channel) {
        rc = libssh2_channel_read(terminal->channel, buffer, sizeof(buffer) - 1);
        
        if (rc > 0) {
            buffer[rc] = '\0';
            terminal->process_received_data(buffer, rc);
            vTaskDelay(1);
        } else if (rc == LIBSSH2_ERROR_EAGAIN) {
            terminal->flush_display_buffer();
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (rc < 0) {
            ESP_LOGE(TAG, "Read error: %d", (int)rc);
            break;
        }

        if (libssh2_channel_eof(terminal->channel)) {
            ESP_LOGI(TAG, "Channel EOF");
            terminal->flush_display_buffer();
            break;
        }
        
        vTaskDelay(1);
    }

    ESP_LOGI(TAG, "SSH receive task ended");
    terminal->disconnect();
    vTaskDelete(NULL);
}

std::string SSHTerminal::strip_ansi_codes(const char* data, size_t len)
{
    std::string result;
    result.reserve(len);
    
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && i % 1024 == 0) {
            vTaskDelay(1);
        }
        
        if (data[i] == '\x1B' || data[i] == '\033') {
            i++;
            if (i >= len) break;
            
            if (data[i] == '[') {
                i++;
                while (i < len && !((data[i] >= 'A' && data[i] <= 'Z') || 
                                    (data[i] >= 'a' && data[i] <= 'z'))) {
                    i++;
                }
            } else if (data[i] == ']') {
                i++;
                while (i < len) {
                    if (data[i] == '\007') break;
                    if (data[i] == '\x1B' && i + 1 < len && data[i + 1] == '\\') {
                        i++;
                        break;
                    }
                    i++;
                }
            } else if (data[i] == '(' || data[i] == ')') {
                i++;
            }
        } else if (data[i] == '\r') {
            continue;
        } else {
            result += data[i];
        }
    }
    
    return result;
}

void SSHTerminal::process_received_data(const char* data, size_t len)
{
    bytes_received += len;
    
    std::string cleaned = strip_ansi_codes(data, len);
    text_buffer += cleaned;
    
    int64_t current_time = esp_timer_get_time() / 1000;
    
    if (text_buffer.size() > 2048) {
        text_buffer = text_buffer.substr(text_buffer.size() - 1024);
    }
    
    if (current_time - last_display_update >= 1000) {
        flush_display_buffer();
    }
    
    vTaskDelay(1);
}

void SSHTerminal::flush_display_buffer()
{
    if (text_buffer.empty() && bytes_received == 0) {
        return;
    }
    
    const size_t CHUNK_SIZE = 256;
    size_t offset = 0;
    
    while (offset < text_buffer.size()) {
        if (bsp_display_lock(0)) {
            size_t chunk_len = std::min(CHUNK_SIZE, text_buffer.size() - offset);
            std::string chunk = text_buffer.substr(offset, chunk_len);
            append_text(chunk.c_str());
            bsp_display_unlock();
            offset += chunk_len;
            
            vTaskDelay(1);
        } else {
            break;
        }
    }
    
    if (offset > 0) {
        text_buffer = text_buffer.substr(offset);
    }
    
    if (bytes_received > 0 && byte_counter_label && bsp_display_lock(0)) {
        char counter_text[32];
        if (bytes_received < 1024) {
            snprintf(counter_text, sizeof(counter_text), "%zu B", bytes_received);
        } else if (bytes_received < 1024 * 1024) {
            snprintf(counter_text, sizeof(counter_text), "%.1f KB", bytes_received / 1024.0);
        } else {
            snprintf(counter_text, sizeof(counter_text), "%.2f MB", bytes_received / (1024.0 * 1024.0));
        }
        lv_label_set_text(byte_counter_label, counter_text);
        bsp_display_unlock();
    }
    
    if (text_buffer.empty()) {
        last_display_update = esp_timer_get_time() / 1000;
    }
    
    vTaskDelay(1);
}

void SSHTerminal::update_status_bar()
{
    if (!status_bar) return;
    
    if (!bsp_display_lock(0)) {
        return;
    }

    std::string status;
    
    if (battery_initialized) {
        float voltage = battery.readBatteryVoltage();
        if (voltage > 0.1f) {
            char voltage_text[32];
            snprintf(voltage_text, sizeof(voltage_text), "%.2fV | ", voltage);
            status = voltage_text;
            ESP_LOGD(TAG, "Battery voltage displayed: %.2fV", voltage);
        } else {
            ESP_LOGW(TAG, "Battery voltage too low or invalid: %.2fV", voltage);
        }
    } else {
        ESP_LOGD(TAG, "Battery not initialized, skipping voltage display");
    }
    
    if (!wifi_connected) {
        status += LV_SYMBOL_WIFI " OFF";
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFF0000), 0);
    } else if (!ssh_connected) {
        status += LV_SYMBOL_WIFI " | " LV_SYMBOL_CLOSE " SSH";
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0xFFFF00), 0);
    } else {
        status += LV_SYMBOL_WIFI " | " LV_SYMBOL_OK " SSH";
        lv_obj_set_style_text_color(status_bar, lv_color_hex(0x00FF00), 0);
    }
    
    lv_label_set_text(status_bar, status.c_str());
    
    bsp_display_unlock();
}

void SSHTerminal::update_terminal_display()
{
}

void SSHTerminal::create_side_panel()
{
    side_panel = lv_obj_create(terminal_screen);
    lv_obj_set_size(side_panel, 100, lv_pct(100));
    lv_obj_set_style_bg_color(side_panel, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(side_panel, LV_OPA_80, 0);
    lv_obj_set_style_border_color(side_panel, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(side_panel, 2, 0);
    lv_obj_set_scrollbar_mode(side_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(side_panel, LV_DIR_VER);
    lv_obj_align(side_panel, LV_ALIGN_TOP_RIGHT, 100, 0);
    lv_obj_add_flag(side_panel, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t* title = lv_label_create(side_panel);
    lv_label_set_text(title, "Keys");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    
    auto create_key_button = [this](const char* label, const char* key_seq, int y_offset) {
        lv_obj_t* btn = lv_btn_create(side_panel);
        lv_obj_set_size(btn, 85, 30);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y_offset);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x00CC00), LV_STATE_PRESSED);
        
        lv_obj_t* btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, label);
        lv_obj_set_style_text_color(btn_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_10, 0);
        lv_obj_center(btn_label);
        
        lv_obj_set_user_data(btn, (void*)key_seq);
        lv_obj_add_event_cb(btn, special_key_event_cb, LV_EVENT_CLICKED, this);
        
        return btn;
    };

    create_key_button("<-", "LEFT", 35);
    create_key_button("->", "RIGHT", 70);
    create_key_button("Line <", "HOME", 105);
    create_key_button("> Line", "END", 140);
    create_key_button("Ctrl+C", "\x03", 175);
    create_key_button("Ctrl+Z", "\x1A", 210);
    create_key_button("Ctrl+D", "\x04", 245);
    create_key_button("Ctrl+L", "\x0C", 280);
    create_key_button("Tab", "\t", 315);
    create_key_button("Esc", "\x1B", 350);
    create_key_button("Exit SSH", "EXIT", 385);
    create_key_button("Clear", "CLEAR", 420);
}

void SSHTerminal::toggle_side_panel()
{
    if (!side_panel) return;
    
    if (lv_obj_has_flag(side_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(side_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(side_panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        lv_obj_add_flag(side_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(side_panel, LV_ALIGN_TOP_RIGHT, 100, 0);
    }
}

void SSHTerminal::send_special_key(const char* sequence)
{
    if (!sequence || strlen(sequence) == 0) {
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "EXIT") == 0) {
        disconnect();
        toggle_side_panel();
        return;
    }
    
    if (strcmp(sequence, "CLEAR") == 0) {
        clear_terminal();
        toggle_side_panel();
        return;
    }
    
    // Handle cursor movement keys
    if (strcmp(sequence, "LEFT") == 0) {
        move_cursor_left();
        return;
    }
    
    if (strcmp(sequence, "RIGHT") == 0) {
        move_cursor_right();
        return;
    }
    
    if (strcmp(sequence, "HOME") == 0) {
        move_cursor_home();
        return;
    }
    
    if (strcmp(sequence, "END") == 0) {
        move_cursor_end();
        return;
    }
    
    if (ssh_connected && channel) {
        libssh2_channel_write(channel, sequence, strlen(sequence));
        ESP_LOGI(TAG, "Sent special key sequence");
    } else {
        ESP_LOGW(TAG, "Cannot send special key - not connected");
    }
    
    toggle_side_panel();
}

void SSHTerminal::gesture_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    
    if (dir == LV_DIR_LEFT) {
        ESP_LOGI(TAG, "Swipe left detected - showing special keys panel");
        if (terminal->side_panel && lv_obj_has_flag(terminal->side_panel, LV_OBJ_FLAG_HIDDEN)) {
            terminal->toggle_side_panel();
        }
    } else if (dir == LV_DIR_RIGHT) {
        ESP_LOGI(TAG, "Swipe right detected - hiding special keys panel");
        if (terminal->side_panel && !lv_obj_has_flag(terminal->side_panel, LV_OBJ_FLAG_HIDDEN)) {
            terminal->toggle_side_panel();
        }
    }
}

void SSHTerminal::special_key_event_cb(lv_event_t* e)
{
    SSHTerminal* terminal = (SSHTerminal*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t* ) lv_event_get_target(e);
    const char* key_seq = (const char*)lv_obj_get_user_data(btn);
    
    if (key_seq) {
        terminal->send_special_key(key_seq);
    }
}
