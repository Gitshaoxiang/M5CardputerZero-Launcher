#include "lvgl/lvgl.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace {

static lv_obj_t *g_input = nullptr;
static lv_obj_t *g_chat = nullptr;
static lv_obj_t *g_state = nullptr;
static lv_group_t *g_group = nullptr;
static lv_timer_t *g_poll_timer = nullptr;
static lv_timer_t *g_init_timer = nullptr;

static int g_uart_fd = -1;
static const char *k_uart_dev = "/dev/serial0";
static int g_screen_w = 320;
static int g_screen_h = 170;
static bool g_input_has_focus = false;
static bool g_uart_init_started = false;

struct UartConfig {
    int baud;
    int data_bits;
    char parity;
    int stop_bits;
};

static constexpr UartConfig kDefaultConfig = {115200, 8, 'N', 1};

static bool is_chat_full()
{
    if (g_chat == nullptr)
    {
        return false;
    }

    return lv_obj_get_child_count(g_chat) > 18;
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
    printf("[GroveUART] %s\n", line ? line : "");
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

static void append_rx(const char *text)
{
    append_chat_line("< ", text, lv_color_hex(0x80D8B8));
    append_log(text);
}

static void append_error(const char *text)
{
    append_chat_line("! ", text, lv_color_hex(0xFFB86B));
    append_log(text);
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
    snprintf(cmd, sizeof(cmd), "timeout 1 stty -F /dev/ttyS0 %d 2>/dev/null", cfg.baud);
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

    g_uart_fd = open(k_uart_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
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
             "%d %d%c%d",
             cfg.baud,
             cfg.data_bits,
             cfg.parity,
             cfg.stop_bits);
    lv_label_set_text(g_state, line);
    log_status("SYS UART ready 115200 8N1");
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

static void uart_poll_cb(lv_timer_t *)
{
    if (g_uart_fd < 0)
    {
        return;
    }

    char buf[256];
    while (true)
    {
        ssize_t n = read(g_uart_fd, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            std::string line;
            line.append(buf, static_cast<size_t>(n));
            append_rx(line.c_str());
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            break;
        }
        break;
    }
}

static void send_uart_action()
{
    const char *msg = lv_textarea_get_text(g_input);
    if (msg == nullptr || msg[0] == '\0')
    {
        append_error("empty message");
        return;
    }

    if (g_uart_fd < 0)
    {
        log_status("SYS UART not open, retry");
        open_or_reopen_uart();
    }
    if (g_uart_fd < 0)
    {
        return;
    }

    std::string packet(msg);
    packet.push_back('\n');

    ssize_t n = write(g_uart_fd, packet.c_str(), packet.size());
    if (n < 0)
    {
        char line[96] = {0};
        snprintf(line, sizeof(line), "send failed: %s", strerror(errno));
        append_error(line);
    }
    else
    {
        append_tx(msg);
        lv_textarea_set_text(g_input, "");
    }
}

static void input_key_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ENTER)
    {
        send_uart_action();
        lv_event_stop_bubbling(e);
    }
    else if (code == LV_EVENT_READY)
    {
        lv_event_stop_bubbling(e);
    }
}

static void input_focus_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED)
    {
        if (g_input_has_focus)
        {
            return;
        }
        g_input_has_focus = true;
        lv_obj_set_style_border_color(g_input, lv_color_hex(0x7CC7FF), 0);
    }
    else if (code == LV_EVENT_DEFOCUSED)
    {
        if (!g_input_has_focus)
        {
            return;
        }
        g_input_has_focus = false;
        lv_obj_set_style_border_color(g_input, lv_color_hex(0x3D6FA3), 0);
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
    lv_obj_t *title = create_label(screen, "Grove UART", &lv_font_montserrat_20, lv_color_hex(0xF4F8FF));
    lv_obj_set_pos(title, pad, 5);

    g_state = create_label(screen, "opening", &lv_font_montserrat_12, lv_color_hex(0x80D8B8));
    lv_obj_set_width(g_state, 118);
    lv_obj_set_style_text_align(g_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_state, g_screen_w - pad - 118, 10);

    g_chat = lv_obj_create(screen);
    lv_obj_set_pos(g_chat, pad, 32);
    lv_obj_set_size(g_chat, g_screen_w - pad * 2, g_screen_h - 72);
    lv_obj_set_style_bg_color(g_chat, lv_color_hex(0x101925), 0);
    lv_obj_set_style_bg_opa(g_chat, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_chat, lv_color_hex(0x294055), 0);
    lv_obj_set_style_border_width(g_chat, 1, 0);
    lv_obj_set_style_radius(g_chat, 6, 0);
    lv_obj_set_style_pad_all(g_chat, 5, 0);
    lv_obj_set_style_pad_row(g_chat, 3, 0);
    lv_obj_set_scrollbar_mode(g_chat, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(g_chat, LV_FLEX_FLOW_COLUMN);
    log_status("SYS Grove UART starting");

    g_input = lv_textarea_create(screen);
    lv_obj_set_pos(g_input, pad, g_screen_h - 32);
    lv_obj_set_size(g_input, g_screen_w - pad * 2, 26);
    lv_textarea_set_one_line(g_input, true);
    lv_textarea_set_placeholder_text(g_input, "Type and press Enter");
    lv_obj_set_style_text_font(g_input, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(g_input, lv_color_hex(0x172130), 0);
    lv_obj_set_style_border_color(g_input, lv_color_hex(0x3D6FA3), 0);
    lv_obj_set_style_border_width(g_input, 1, 0);
    lv_obj_set_style_radius(g_input, 6, 0);
    lv_obj_set_style_text_color(g_input, lv_color_hex(0xE8F3FF), 0);
    lv_obj_add_flag(g_input, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(g_input, input_key_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(g_input, input_key_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(g_input, input_focus_cb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(g_input, input_focus_cb, LV_EVENT_DEFOCUSED, nullptr);

    g_group = lv_group_create();
    lv_group_add_obj(g_group, g_input);
    lv_group_focus_obj(g_input);
    lv_group_set_editing(g_group, true);

    if (!g_input_has_focus)
    {
        lv_obj_send_event(g_input, LV_EVENT_FOCUSED, nullptr);
    }

    g_init_timer = lv_timer_create(init_uart_timer_cb, 100, nullptr);
    lv_timer_set_repeat_count(g_init_timer, 1);
    g_poll_timer = lv_timer_create(uart_poll_cb, 150, nullptr);
    (void)g_poll_timer;
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
    if (g_input == nullptr)
    {
        return;
    }

    (void)key_state;
    if (key_code == KEY_ENTER || key_code == KEY_KPENTER)
    {
        send_uart_action();
    }
    else if (key_code == KEY_BACKSPACE)
    {
        lv_textarea_delete_char(g_input);
    }
    else if (key_code == KEY_DELETE)
    {
        lv_textarea_delete_char_forward(g_input);
    }
    else if (key_code == KEY_LEFT)
    {
        lv_textarea_cursor_left(g_input);
    }
    else if (key_code == KEY_RIGHT)
    {
        lv_textarea_cursor_right(g_input);
    }
    else if (utf8 != nullptr && utf8[0] >= 0x20)
    {
        lv_textarea_add_text(g_input, utf8);
    }
}
