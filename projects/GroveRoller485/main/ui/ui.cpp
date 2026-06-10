#include "lvgl/lvgl.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace {

static lv_obj_t *g_chat = nullptr;
static lv_obj_t *g_state = nullptr;
static lv_obj_t *g_speed_value = nullptr;
static lv_obj_t *g_mode_value = nullptr;
static lv_obj_t *g_hint_line = nullptr;
static lv_group_t *g_group = nullptr;
static lv_timer_t *g_init_timer = nullptr;

static int g_uart_fd = -1;
static const char *k_uart_dev = "/dev/serial0";
static int g_screen_w = 320;
static int g_screen_h = 170;
static bool g_uart_init_started = false;
static bool g_motor_running = false;
static int32_t g_speed = 100;

static constexpr uint8_t kRollerId = 1;
static constexpr int32_t kSpeedMin = -210000;
static constexpr int32_t kSpeedMax = 210000;
static constexpr int32_t kSpeedStep = 10;
static constexpr int32_t kCurrentLimit = 1200;
static constexpr useconds_t kCommandGapUs = 20000;
static constexpr int kModeRepeatCount = 3;
static constexpr int kOutputRepeatCount = 2;

struct UartConfig {
    int baud;
    int data_bits;
    char parity;
    int stop_bits;
};

enum RollerMode : uint8_t {
    kModeSpeed = 1,
};

enum RollerCommand : uint8_t {
    kCmdSetOutput = 0x00,
    kCmdSetMode = 0x01,
    kCmdSpeedMode = 0x20,
};

static constexpr UartConfig kDefaultConfig = {115200, 8, 'N', 1};

static bool is_chat_full()
{
    if (g_chat == nullptr)
    {
        return false;
    }

    return lv_obj_get_child_count(g_chat) > 12;
}

static void append_chat_line(const char *prefix, const char *text, lv_color_t color)
{
    if (g_chat == nullptr)
    {
        return;
    }

    if (is_chat_full())
    {
        lv_obj_t *first = lv_obj_get_child(g_chat, 0);
        if (first != nullptr)
        {
            lv_obj_delete(first);
        }
    }

    std::string line(prefix ? prefix : "");
    if (text != nullptr)
    {
        line.append(text);
    }

    lv_obj_t *label = lv_label_create(g_chat);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, line.c_str());
    lv_obj_scroll_to_view(label, LV_ANIM_OFF);
}

static void append_log(const char *line)
{
    printf("[GroveRoller485] %s\n", line ? line : "");
    fflush(stdout);
}

static void log_status(const char *line)
{
    append_log(line);
}

static void append_tx(const char *text)
{
    append_chat_line("> ", text, lv_color_hex(0x7CC7FF));
    append_log(text);
}

static void append_error(const char *text)
{
    append_chat_line("! ", text, lv_color_hex(0xFFB86B));
    append_log(text);
}

static void append_info(const char *text)
{
    append_chat_line("* ", text, lv_color_hex(0x80D8B8));
    append_log(text);
}

static void update_speed_label()
{
    if (g_speed_value == nullptr)
    {
        return;
    }

    char line[32] = {};
    snprintf(line, sizeof(line), "%ld", static_cast<long>(g_speed));
    lv_label_set_text(g_speed_value, line);
}

static void update_mode_label()
{
    if (g_mode_value == nullptr)
    {
        return;
    }

    lv_label_set_text(g_mode_value, g_motor_running ? "Running" : "Stopped");
}

static void update_hint_label()
{
    if (g_hint_line == nullptr)
    {
        return;
    }

    char line[96] = {};
    snprintf(line, sizeof(line), "Enter %s   Z -%ld   C +%ld",
             g_motor_running ? "Stop" : "Run",
             static_cast<long>(kSpeedStep),
             static_cast<long>(kSpeedStep));
    lv_label_set_text(g_hint_line, line);
}

static speed_t baud_to_termios(int baud)
{
    switch (baud)
    {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
    default:
        return B115200;
    }
}

static uint8_t roller_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            if (crc & 0x01)
            {
                crc = static_cast<uint8_t>((crc >> 1) ^ 0x8C);
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static bool switch_to_uart_mode(const UartConfig &cfg)
{
    log_status("SYS switch Grove to UART");

    int rc_power = system("timeout 1 gpioset -c gpiochip0 17=1 >/dev/null 2>&1");
    char line[96] = {0};
    snprintf(line, sizeof(line), "SYS gpio17 power rc=%d", rc_power);
    log_status(line);

    int rc_mux = system("timeout 1 gpioset -c gpiochip0 4=0 >/dev/null 2>&1");
    snprintf(line, sizeof(line), "SYS gpio4 UART rc=%d", rc_mux);
    log_status(line);

    char cmd[96] = {0};
    snprintf(cmd, sizeof(cmd), "timeout 1 stty -F /dev/ttyS0 %d raw -echo -echoe -echok 2>/dev/null", cfg.baud);
    int rc_stty = system(cmd);
    snprintf(line, sizeof(line), "SYS stty rc=%d", rc_stty);
    log_status(line);

    return true;
}

static bool open_uart(const UartConfig &cfg)
{
    log_status("SYS open /dev/serial0");
    if (g_uart_fd >= 0)
    {
        close(g_uart_fd);
        g_uart_fd = -1;
    }

    g_uart_fd = open(k_uart_dev, O_RDWR | O_NOCTTY);
    if (g_uart_fd < 0)
    {
        char line[96] = {0};
        snprintf(line, sizeof(line), "ERR open failed: %s", strerror(errno));
        append_error(line);
        return false;
    }

    struct termios tty;
    if (tcgetattr(g_uart_fd, &tty) != 0)
    {
        char line[96] = {0};
        snprintf(line, sizeof(line), "ERR tcgetattr: %s", strerror(errno));
        append_error(line);
        close(g_uart_fd);
        g_uart_fd = -1;
        return false;
    }

    speed_t speed = baud_to_termios(cfg.baud);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= (cfg.data_bits == 7 ? CS7 : CS8);
    tty.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | IGNCR);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_oflag &= ~OPOST;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);

    if (cfg.parity == 'E')
    {
        tty.c_cflag |= PARENB;
    }
    else if (cfg.parity == 'O')
    {
        tty.c_cflag |= (PARENB | PARODD);
    }

    if (cfg.stop_bits == 2)
    {
        tty.c_cflag |= CSTOPB;
    }

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(g_uart_fd, TCSANOW, &tty) != 0)
    {
        char line[96] = {0};
        snprintf(line, sizeof(line), "ERR tcsetattr: %s", strerror(errno));
        append_error(line);
        close(g_uart_fd);
        g_uart_fd = -1;
        return false;
    }

    return true;
}

static bool write_packet(const uint8_t *data, size_t len, const char *label)
{
    if (g_uart_fd < 0)
    {
        log_status("SYS UART not open, retry");
        UartConfig cfg = kDefaultConfig;
        if (!switch_to_uart_mode(cfg) || !open_uart(cfg))
        {
            return false;
        }
    }

    size_t written = 0;
    while (written < len)
    {
        ssize_t n = write(g_uart_fd, data + written, len - written);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            char line[128] = {};
            snprintf(line, sizeof(line), "send failed: %s", strerror(errno));
            append_error(line);
            return false;
        }
        written += static_cast<size_t>(n);
    }

    if (tcdrain(g_uart_fd) != 0)
    {
        char line[128] = {};
        snprintf(line, sizeof(line), "drain failed: %s", strerror(errno));
        append_error(line);
        return false;
    }

    if (label != nullptr)
    {
        append_tx(label);
    }
    usleep(kCommandGapUs);
    return true;
}

static bool send_simple_command(uint8_t cmd, uint8_t value, const char *label)
{
    uint8_t packet[15] = {};
    packet[0] = cmd;
    packet[1] = kRollerId;
    packet[2] = value;
    packet[14] = roller_crc8(packet, 14);
    return write_packet(packet, sizeof(packet), label);
}

static bool send_simple_command_repeat(uint8_t cmd, uint8_t value, const char *label, int repeat_count)
{
    for (int i = 0; i < repeat_count; ++i)
    {
        if (!send_simple_command(cmd, value, label))
        {
            return false;
        }
    }
    return true;
}

static bool send_speed_command(int32_t speed, int32_t current, const char *label)
{
    uint8_t packet[15] = {};
    int32_t speed_bytes = speed * 100;
    int32_t current_bytes = current * 100;

    packet[0] = kCmdSpeedMode;
    packet[1] = kRollerId;
    packet[2] = static_cast<uint8_t>(speed_bytes & 0xFF);
    packet[3] = static_cast<uint8_t>((speed_bytes >> 8) & 0xFF);
    packet[4] = static_cast<uint8_t>((speed_bytes >> 16) & 0xFF);
    packet[5] = static_cast<uint8_t>((speed_bytes >> 24) & 0xFF);
    packet[6] = static_cast<uint8_t>(current_bytes & 0xFF);
    packet[7] = static_cast<uint8_t>((current_bytes >> 8) & 0xFF);
    packet[8] = static_cast<uint8_t>((current_bytes >> 16) & 0xFF);
    packet[9] = static_cast<uint8_t>((current_bytes >> 24) & 0xFF);
    packet[14] = roller_crc8(packet, 14);
    return write_packet(packet, sizeof(packet), label);
}

static bool ensure_speed_mode_ready()
{
    if (!send_simple_command_repeat(kCmdSetMode, kModeSpeed, "mode speed", kModeRepeatCount))
    {
        return false;
    }
    if (!send_simple_command_repeat(kCmdSetOutput, 1, "output on", kOutputRepeatCount))
    {
        return false;
    }
    return true;
}

static void apply_running_speed()
{
    char line[96] = {};
    if (g_motor_running)
    {
        if (!ensure_speed_mode_ready())
        {
            return;
        }
        snprintf(line, sizeof(line), "speed %ld", static_cast<long>(g_speed));
        if (send_speed_command(g_speed, kCurrentLimit, line))
        {
            append_info("motor speed updated");
        }
    }
    else
    {
        if (!send_simple_command_repeat(kCmdSetMode, kModeSpeed, "mode speed", kModeRepeatCount))
        {
            return;
        }
        if (send_speed_command(0, kCurrentLimit, "speed 0"))
        {
            append_info("motor stop command sent");
        }
    }
}

static void toggle_motor()
{
    g_motor_running = !g_motor_running;
    apply_running_speed();
    update_mode_label();
    update_hint_label();
}

static void change_speed(int32_t delta)
{
    int32_t next = g_speed + delta;
    if (next < kSpeedMin)
    {
        next = kSpeedMin;
    }
    if (next > kSpeedMax)
    {
        next = kSpeedMax;
    }

    if (next == g_speed)
    {
        append_info("speed unchanged");
        return;
    }

    g_speed = next;
    update_speed_label();
    update_hint_label();

    char line[96] = {};
    snprintf(line, sizeof(line), "target speed %ld", static_cast<long>(g_speed));
    append_info(line);

    if (g_motor_running)
    {
        apply_running_speed();
    }
}

static void open_or_reopen_uart()
{
    log_status("SYS init UART begin");
    UartConfig cfg = kDefaultConfig;

    if (!switch_to_uart_mode(cfg))
    {
        lv_label_set_text(g_state, "GPIO failed");
        append_error("ERR GPIO switch failed");
        return;
    }

    if (!open_uart(cfg))
    {
        lv_label_set_text(g_state, "UART failed");
        append_error("ERR UART open failed");
        return;
    }

    char line[128] = {0};
    snprintf(line,
             sizeof(line),
             "%d %d%c%d id=%u",
             cfg.baud,
             cfg.data_bits,
             cfg.parity,
             cfg.stop_bits,
             static_cast<unsigned>(kRollerId));
    lv_label_set_text(g_state, line);
    log_status("SYS UART ready 115200 8N1");

    if (ensure_speed_mode_ready())
    {
        append_info("roller speed mode ready");
    }
}

static void init_uart_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_uart_init_started)
    {
        return;
    }

    g_uart_init_started = true;
    open_or_reopen_uart();
    if (g_init_timer)
    {
        lv_timer_delete(g_init_timer);
        g_init_timer = nullptr;
    }
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

} // namespace

void ui_init()
{
    lv_obj_t *screen = lv_screen_active();
    lv_display_t *disp = lv_display_get_default();
    if (disp != nullptr)
    {
        g_screen_w = lv_display_get_horizontal_resolution(disp);
        g_screen_h = lv_display_get_vertical_resolution(disp);
    }

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B111A), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    const int pad = 8;
    lv_obj_t *title = create_label(screen, "Grove Roller485", &lv_font_montserrat_20, lv_color_hex(0xF4F8FF));
    lv_obj_set_pos(title, pad, 5);

    g_state = create_label(screen, "opening", &lv_font_montserrat_12, lv_color_hex(0x80D8B8));
    lv_obj_set_width(g_state, 150);
    lv_obj_set_style_text_align(g_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_state, g_screen_w - pad - 150, 10);

    lv_obj_t *status_panel = lv_obj_create(screen);
    lv_obj_set_pos(status_panel, pad, 32);
    lv_obj_set_size(status_panel, g_screen_w - pad * 2, 46);
    lv_obj_set_style_bg_color(status_panel, lv_color_hex(0x101925), 0);
    lv_obj_set_style_bg_opa(status_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(status_panel, lv_color_hex(0x294055), 0);
    lv_obj_set_style_border_width(status_panel, 1, 0);
    lv_obj_set_style_radius(status_panel, 6, 0);
    lv_obj_set_style_pad_all(status_panel, 6, 0);
    lv_obj_clear_flag(status_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mode_label = create_label(status_panel, "Mode", &lv_font_montserrat_12, lv_color_hex(0x7A93AD));
    lv_obj_set_pos(mode_label, 2, 2);
    g_mode_value = create_label(status_panel, "Stopped", &lv_font_montserrat_16, lv_color_hex(0xF4F8FF));
    lv_obj_set_pos(g_mode_value, 2, 18);

    lv_obj_t *speed_label = create_label(status_panel, "Speed", &lv_font_montserrat_12, lv_color_hex(0x7A93AD));
    lv_obj_set_pos(speed_label, 170, 2);
    g_speed_value = create_label(status_panel, "100", &lv_font_montserrat_16, lv_color_hex(0x7CC7FF));
    lv_obj_set_pos(g_speed_value, 170, 18);

    g_hint_line = create_label(screen, "", &lv_font_montserrat_12, lv_color_hex(0xC9D7E6));
    lv_obj_set_pos(g_hint_line, pad, 83);

    g_chat = lv_obj_create(screen);
    lv_obj_set_pos(g_chat, pad, 102);
    lv_obj_set_size(g_chat, g_screen_w - pad * 2, g_screen_h - 110);
    lv_obj_set_style_bg_color(g_chat, lv_color_hex(0x101925), 0);
    lv_obj_set_style_bg_opa(g_chat, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_chat, lv_color_hex(0x294055), 0);
    lv_obj_set_style_border_width(g_chat, 1, 0);
    lv_obj_set_style_radius(g_chat, 6, 0);
    lv_obj_set_style_pad_all(g_chat, 5, 0);
    lv_obj_set_style_pad_row(g_chat, 3, 0);
    lv_obj_set_scrollbar_mode(g_chat, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(g_chat, LV_FLEX_FLOW_COLUMN);

    log_status("SYS Grove Roller485 starting");
    append_info("Enter start/stop");
    append_info("Z dec speed, C inc speed");

    g_group = lv_group_create();
    update_mode_label();
    update_speed_label();
    update_hint_label();

    g_init_timer = lv_timer_create(init_uart_timer_cb, 100, nullptr);
    lv_timer_set_repeat_count(g_init_timer, 1);
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}

void ui_debug_log(const char *line)
{
    append_log(line);
}

void ui_handle_key_item(uint32_t key_code, const char *utf8, int key_state)
{
    (void)utf8;
    (void)key_state;

    if (key_code == KEY_ENTER || key_code == KEY_KPENTER)
    {
        toggle_motor();
    }
    else if (key_code == KEY_Z)
    {
        change_speed(-kSpeedStep);
    }
    else if (key_code == KEY_C)
    {
        change_speed(kSpeedStep);
    }
}
