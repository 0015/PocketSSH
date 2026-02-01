/*
 * SSHTerminal Header
 * Provides LVGL-based SSH terminal interface with WiFi connectivity.
 * Supports command history, battery monitoring, and interactive shell sessions.
 */

#ifndef SSH_TERMINAL_HPP
#define SSH_TERMINAL_HPP

#include "lvgl.h"
#include "esp_err.h"
#include <string>
#include <vector>
#include <map>
#include "libssh2.h"
#include "battery_measurement.hpp"

#define SSH_MAX_LINE_LENGTH 128
#define SSH_MAX_LINES 100
#define SSH_BUFFER_SIZE 4096

class SSHTerminal
{
public:
    SSHTerminal();
    ~SSHTerminal();

    lv_obj_t* create_terminal_screen();
    
    esp_err_t connect(const char* host, int port, const char* username, const char* password);
    esp_err_t connect_with_key(const char* host, int port, const char* username, const char* privkey_data, size_t privkey_len);
    esp_err_t disconnect();
    bool is_connected();
    
    void append_text(const char* text);
    void clear_terminal();
    void handle_key_input(char key);
    void send_command(const char* cmd);
    void navigate_history(int direction);
    void delete_current_history_entry();
    void send_current_history_command();
    void move_cursor_left();
    void move_cursor_right();
    void move_cursor_home();
    void move_cursor_end();
    
    esp_err_t init_wifi(const char* ssid, const char* password);
    bool is_wifi_connected();
    
    lv_obj_t* get_screen() { return terminal_screen; }
    
    void update_status_bar();
    
    // SSH key management
    void load_key_from_memory(const char* keyname, const char* key_data, size_t key_len);
    const char* get_loaded_key(const char* keyname, size_t* len);
    std::vector<std::string> get_loaded_key_names();
    
private:
    lv_obj_t* terminal_screen;
    lv_obj_t* terminal_output;
    lv_obj_t* input_label;
    lv_obj_t* status_bar;
    lv_obj_t* byte_counter_label;
    lv_obj_t* side_panel;
    
    std::string current_input;
    size_t cursor_pos;
    size_t bytes_received;
    std::vector<std::string> command_history;
    int history_index;
    
    lv_timer_t* cursor_blink_timer;
    bool cursor_visible;
    
    lv_timer_t* battery_update_timer;
    
    bool history_needs_save;
    lv_timer_t* history_save_timer;
    
    std::string text_buffer;
    int64_t last_display_update;
    
    bool wifi_connected;
    bool ssh_connected;
    bool battery_initialized;
    
    BatteryMeasurement battery;
    
    int ssh_socket;
    LIBSSH2_SESSION *session;
    LIBSSH2_CHANNEL *channel;
    
    char* hostname;
    int port_number;
    
    // SSH key storage: keyname -> key content
    std::map<std::string, std::string> loaded_keys;
    
    void update_terminal_display();
    void update_input_display();
    void process_received_data(const char* data, size_t len);
    void flush_display_buffer();
    
    void load_history_from_nvs();
    void save_history_to_nvs();
    void clear_history_nvs();
    std::string strip_ansi_codes(const char* data, size_t len);
    void send_special_key(const char* sequence);
    void create_side_panel();
    void toggle_side_panel();
    static void gesture_event_cb(lv_event_t* e);
    static void special_key_event_cb(lv_event_t* e);
    static void input_touch_event_cb(lv_event_t* e);
    static void cursor_blink_cb(lv_timer_t* timer);
    static void battery_update_cb(lv_timer_t* timer);
    static void history_save_cb(lv_timer_t* timer);
    static void ssh_receive_task(void* param);
    
    static int waitsocket(int socket_fd, LIBSSH2_SESSION *session);
    esp_err_t ssh_authenticate(const char* username, const char* password);
    esp_err_t ssh_authenticate_pubkey(const char* username, const char* privkey_data, size_t privkey_len);
    esp_err_t ssh_open_channel();
};

#endif
