```
╔══════════════════════════════════════════════════════════════════════╗
║                                                                      ║
║   ██████╗  ██████╗  ██████╗██╗  ██╗███████╗████████╗                 ║
║   ██╔══██╗██╔═══██╗██╔════╝██║ ██╔╝██╔════╝╚══██╔══╝                 ║
║   ██████╔╝██║   ██║██║     █████╔╝ █████╗     ██║                    ║
║   ██╔═══╝ ██║   ██║██║     ██╔═██╗ ██╔══╝     ██║                    ║
║   ██║     ╚██████╔╝╚██████╗██║  ██╗███████╗   ██║                    ║
║   ╚═╝      ╚═════╝  ╚═════╝╚═╝  ╚═╝╚══════╝   ╚═╝                    ║
║                                                                      ║
║    ███████╗███████╗██╗  ██╗    ████████╗███████╗██████╗ ███╗   ███╗  ║
║    ██╔════╝██╔════╝██║  ██║    ╚══██╔══╝██╔════╝██╔══██╗████╗ ████║  ║
║    ███████╗███████╗███████║       ██║   █████╗  ██████╔╝██╔████╔██║  ║
║    ╚════██║╚════██║██╔══██║       ██║   ██╔══╝  ██╔══██╗██║╚██╔╝██║  ║
║    ███████║███████║██║  ██║       ██║   ███████╗██║  ██║██║ ╚═╝ ██║  ║
║    ╚══════╝╚══════╝╚═╝  ╚═╝       ╚═╝   ╚══════╝╚═╝  ╚═╝╚═╝     ╚═╝  ║
║                                                                      ║
╚══════════════════════════════════════════════════════════════════════╝
```

# PocketSSH

A portable SSH terminal client for the ESP32-S3 T-Deck Plus, featuring a hardware keyboard, trackball navigation, and touch gesture controls. Connect to remote servers, execute commands, and navigate your terminal with ease on this compact handheld device.

![PocketSSH Screenshot](misc/screenshot_01.jpeg)

## Key Features

### SSH Terminal
- Full SSH2 protocol support with password authentication
- PTY terminal emulation (vt100) for proper shell interaction
- Real-time command execution and output display
- Handles large data transfers without freezing

### Hardware Controls
- **Physical Keyboard**: Full typing capability via C3 keyboard module
- **Trackball Navigation**: 
  - Up/Down: Navigate through command history
  - Click: Select/execute (when applicable)
- **Touch Screen Gestures**:
  - Swipe right-to-left: Show special keys panel
  - Swipe left-to-right: Hide special keys panel

### Special Keys Panel
Quick access to commonly used control sequences:
- **Ctrl+C**: Interrupt running process
- **Ctrl+Z**: Suspend process
- **Ctrl+D**: EOF signal / logout
- **Ctrl+L**: Clear screen
- **Tab**: Command completion
- **Esc**: Escape key
- **Exit SSH**: Close SSH session
- **Clear**: Clear terminal display

### Display Features
- 320x240 color LCD with LVGL graphics
- Real-time byte counter for data transfer monitoring
- Battery voltage indicator
- Connection status icons (WiFi, SSH)
- Non-blocking display updates prioritize rendering over input

## Hardware Requirements

- **ESP32-S3 T-Deck Plus**
  - ST7789 LCD Display (320x240)
  - GT911 Touch Controller
  - C3 Keyboard Module
  - Trackball (GPIO 1,2,3,15)
  - Battery monitoring (GPIO 4)

## Quick Start

### Prerequisites
- ESP-IDF v5.5.1
- ESP32-S3 T-Deck/T-Deck Plus

### First-Time Setup

1. **Connect to WiFi**:
   ```
   connect <SSID> <PASSWORD>
   ```
   Example: `connect MyNetwork MyPassword123`

2. **Connect to SSH Server**:
   ```
   ssh <HOST> <PORT> <USER> <PASSWORD>
   ```
   Example: `ssh 192.168.1.100 22 pi raspberry`

3. **Use Interactive Shell**:
   Once connected, any command you type is sent to the remote server:
   ```
   ls -la
   cd /home
   top
   vim myfile.txt
   ```

4. **Disconnect**:
   - Type `disconnect` command, or
   - Use Disconnect button in special keys panel

### Available Local Commands

These commands are executed locally on the device (not sent to SSH):
- `help` - Display available commands and usage
- `clear` - Clear the terminal display
- `disconnect` - Close WiFi connection
- `exit` - Close SSH connection
- `connect <SSID> <PASS>` - Connect to WiFi network
- `ssh <HOST> <PORT> <USER> <PASS>` - Establish SSH connection

## Architecture

### Framework & Libraries
- **ESP-IDF**: v5.5.1 - Official Espressif IoT Development Framework
- **LVGL**: v9.x - Graphics library with thread-safe display locking (`bsp_display_lock/unlock`)
- **libssh2**: SSH2 protocol implementation (see details below)
- **FreeRTOS**: Multi-task architecture for concurrent operations

### System Tasks
- **keypad_task**: Hardware keyboard input processing with non-blocking display updates
- **trackball_task**: Trackball GPIO monitoring for history navigation (0ms timeout, drops input if display busy)
- **ssh_receive_task**: SSH data reception and terminal output rendering
- **LVGL task**: GUI rendering and touch gesture detection

### Display Performance
- Non-blocking input system: `bsp_display_lock(0)` prioritizes screen rendering over input
- Buffered display updates prevent watchdog timeouts during large transfers
- Zero-timeout locks drop input gracefully when display is busy

## Using libssh2

PocketSSH uses **libssh2** for SSH2 protocol implementation, providing secure remote shell access.

### Integration
- **Library**: `skuodi__libssh2_esp` component from ESP Component Registry
- **Configuration**: Managed via `idf_component.yml` in the `main/` directory
- **Version**: Compatible with ESP-IDF v5.x

### Implementation Details

#### Connection Flow
```cpp
1. libssh2_session_init() - Initialize SSH session
2. libssh2_session_handshake() - Perform SSH handshake
3. libssh2_userauth_password() - Authenticate with username/password
4. libssh2_channel_open_session() - Open SSH channel
5. libssh2_channel_request_pty() - Request PTY for terminal emulation
6. libssh2_channel_shell() - Start shell session
```

#### PTY Terminal Mode
- **Terminal Type**: vt100 emulation
- **PTY Size**: 80 columns × 24 rows (configurable)
- **Mode**: Provides proper terminal interaction with command prompts, colors, and cursor control

#### Data Handling
- **Non-blocking mode**: `libssh2_session_set_blocking(session, 0)`
- **Read buffer**: 4KB chunks processed by `ssh_receive_task`
- **Write operations**: Direct keyboard input → `libssh2_channel_write()`
- **Control sequences**: Special keys (Ctrl+C, Tab, etc.) sent as escape sequences

#### Security Features
- **Protocol**: SSH2 with encryption
- **Authentication**: Password-based (username/password)
- **Session**: Secure encrypted channel for all data transfer

### Component Configuration

Add to your `main/idf_component.yml`:
```yaml
dependencies:
  skuodi__libssh2_esp: "^1.1.0"
```

The library automatically handles:
- TCP socket management
- Encryption/decryption
- SSH protocol negotiation
- Keep-alive packets
- Session cleanup

### Troubleshooting libssh2

**Connection fails**: Check network connectivity and SSH server accessibility
```bash
# Test SSH server from another machine
ssh user@host -p port
```

**Authentication errors**: Verify username/password, check server auth methods:
```bash
ssh -v user@host  # Verbose output shows auth methods
```

**Data corruption**: Ensure PTY mode is enabled for proper terminal handling
```cpp
libssh2_channel_request_pty(channel, "vt100");
```

## Example Usage

### Typical Session Flow
```
> connect MyWiFi password123
Connecting to WiFi: MyWiFi
WiFi connected successfully!

> ssh 192.168.1.100 22 pi raspberry
Connecting to 192.168.1.100...
Socket connected, initializing SSH session...
Performing SSH handshake...
SSH handshake successful
Authenticating as pi...
Authentication successful
Opening SSH channel...
SSH channel opened - connected!

pi@raspberrypi:~ $ ls -la
total 32
drwxr-xr-x  4 pi   pi   4096 Jan  7 10:30 .
drwxr-xr-x  3 root root 4096 Jan  1 00:00 ..
-rw-r--r--  1 pi   pi    220 Jan  1 00:00 .bash_logout
...

pi@raspberrypi:~ $ uname -a
Linux raspberrypi 5.15.84-v8+ #1613 SMP PREEMPT Thu Jan 5 12:03:08 GMT 2023 aarch64 GNU/Linux

> disconnect
Disconnected from SSH server
```

## Security Considerations

**Current Security Features:**
- Uses SSH2 protocol with encryption (secure!)
- Passwords are encrypted during transmission
- Full cipher support (AES, 3DES, Blowfish, etc.)

**Security Notes:**
- Passwords stored in memory during session
- No host key verification (accepts any server)
- Recommended: Add host key verification for production
- Recommended: Use public key authentication instead of passwords
- Consider using ESP32's secure boot and flash encryption

## Memory Requirements

- Minimum 8KB stack for SSH task
- ~40-50KB heap for SSH session
- Additional memory for terminal display (LVGL)
- Optimized for ESP32 memory constraints

## Known Limitations

**Screen-Intensive Applications:**
- Applications with continuous screen updates (e.g., `htop`, `top`, `watch`) may be difficult to use
- The 320x240 display and rendering performance make real-time updates challenging
- **Recommended**: Use static commands like `ps`, `df`, `ls` instead of interactive/updating tools
- **Alternative**: Use `top -n 1` for single-iteration output instead of continuous mode

**Best Practices:**
- Standard shell commands work well: `ls`, `cd`, `cat`, `grep`, `find`
- Text editors like `nano` and `vim` are usable with patience
- Scripts and one-time commands execute perfectly
- Avoid continuous monitoring tools (`htop`, `tail -f`, `watch`)

## Troubleshooting

### WiFi Connection Issues
- Check SSID and password are correct
- Verify WiFi router is in range
- Ensure WPA2-PSK authentication is supported

### SSH Connection Issues
- Verify server IP and port (default SSH is port 22)
- Check firewall settings on target server
- Ensure SSH server is running (`sudo systemctl status ssh`)
- Verify username and password are correct
- Check SSH server allows password authentication (`PasswordAuthentication yes` in sshd_config)

### Keyboard Not Working
- Check I2C connection to keyboard
- Verify keyboard I2C address (0x55)
- Check I2C bus initialization

### Display Issues
- Ensure LVGL is properly initialized
- Check SPI connection to display
- Verify display driver configuration

## Hardware Pinout

### ESP32-S3 T-Deck Plus
- **Display**: ST7789 SPI LCD (320×240 pixels)
- **Touch**: GT911 I2C touch controller
- **Keyboard**: C3 module via I2C
- **Trackball GPIOs**:
  - GPIO 1: Trackball Up
  - GPIO 2: Trackball Left
  - GPIO 3: Trackball Down
  - GPIO 15: Trackball Right
- **Battery**: GPIO 4 (ADC1 CH3) - Voltage monitoring

## Deployment

### Release Firmware Image

A ready-to-deploy **merged binary** is available that includes bootloader, partition table, and application:
- **File**: `build/PocketSSH-v1.0.0-release.bin`
- **Flash Address**: `0x0` (single merged binary)
- **Flash Settings**: 
  - **Mode**: DIO (Dual I/O)
  - **Frequency**: 80MHz
  - **Size**: 16MB

### Web-Based Installation (Recommended)

Flash firmware via browser using ESP Web Flasher:
1. Visit: https://espressif.github.io/esptool-js/
2. Connect ESP32-S3 device via USB
3. Click "Connect" and select serial port
4. Add file: `PocketSSH-v1.0.0-release.bin` at offset `0x0`
5. Click "Program" to flash

### Command Line Installation

```bash
esptool.py --chip esp32s3 --baud 921600 write_flash 0x0 build/PocketSSH-v1.0.0-release.bin
```

## Credits

- ESP-IDF framework by Espressif Systems
- LVGL graphics library
- libssh2 SSH protocol library
- FreeRTOS real-time operating system

## License

This project uses various ESP-IDF components and open-source libraries:
- **ESP-IDF**: Apache License 2.0
- **LVGL**: MIT License
- **libssh2**: BSD-style license

See individual component licenses for complete details.
