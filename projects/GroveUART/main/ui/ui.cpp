#include "lvgl/lvgl.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <termios.h>
#include <unistd.h>

namespace {

static lv_obj_t *g_input = nullptr;
static lv_obj_t *g_log = nullptr;
static lv_obj_t *g_state = nullptr;
static lv_group_t *g_group = nullptr;
static lv_timer_t *g_poll_timer = nullptr;

static int g_uart_fd = -1;
static const char *k_uart_dev = "/dev/ttyS0";

static void append_log(const char *line)
{
    lv_textarea_add_text(g_log, line);
    lv_textarea_add_text(g_log, "\n");
    lv_textarea_set_cursor_pos(g_log, LV_TEXTAREA_CURSOR_LAST);
}

static bool switch_to_uart_mode()
{
    int rc = 0;
    rc |= system("gpioset -c gpiochip0 17=1");
    rc |= system("gpioset -c gpiochip0 4=0");
    rc |= system("stty -F /dev/ttyS0 115200 2>/dev/null || true");
    return rc == 0;
}

static bool open_uart()
{
    if (g_uart_fd >= 0)
    {
        close(g_uart_fd);
        g_uart_fd = -1;
    }

    g_uart_fd = open(k_uart_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (g_uart_fd < 0)
    {
        return false;
    }

    struct termios tty;
    if (tcgetattr(g_uart_fd, &tty) != 0)
    {
        close(g_uart_fd);
        g_uart_fd = -1;
        return false;
    }

    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_oflag &= ~OPOST;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD | CSTOPB | CRTSCTS);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(g_uart_fd, TCSANOW, &tty) != 0)
    {
        close(g_uart_fd);
        g_uart_fd = -1;
        return false;
    }

    return true;
}

static void uart_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_uart_fd < 0)
    {
        return;
    }

    char buf[128];
    ssize_t n = read(g_uart_fd, buf, sizeof(buf) - 1);
    if (n > 0)
    {
        buf[n] = '\0';
        append_log(buf);
    }
}

static void init_uart_action()
{
    append_log("[GROVE EXT-001] Switch to UART");
    append_log("gpioset -c gpiochip0 17=1");
    append_log("gpioset -c gpiochip0 4=0");
    append_log("stty -F /dev/ttyS0 115200");

    if (!switch_to_uart_mode())
    {
        lv_label_set_text(g_state, "State: Switch command failed");
        append_log("Switch command failed.");
        return;
    }

    if (!open_uart())
    {
        lv_label_set_text(g_state, "State: Open /dev/ttyS0 failed");
        append_log("Open /dev/ttyS0 failed.");
        return;
    }

    lv_label_set_text(g_state, "State: UART ready @115200");
    append_log("UART ready.");
}

static void send_uart_action()
{
    const char *msg = lv_textarea_get_text(g_input);
    if (msg == nullptr || msg[0] == '\0')
    {
        return;
    }

    if (g_uart_fd < 0)
    {
        append_log("UART not ready, run Init UART first.");
        return;
    }

    std::string packet(msg);
    packet.push_back('\n');

    ssize_t n = write(g_uart_fd, packet.c_str(), packet.size());
    if (n < 0)
    {
        append_log("UART write failed.");
    }
    else
    {
        std::string line = "TX: ";
        line += msg;
        append_log(line.c_str());
        lv_textarea_set_text(g_input, "");
    }
}

static void init_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        init_uart_action();
    }
}

static void send_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        send_uart_action();
    }
}

static void clear_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED)
    {
        lv_textarea_set_text(g_log, "");
    }
}

static void input_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY)
    {
        send_uart_action();
    }
}

} // namespace

void ui_init()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111A26), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "GroveUART Debug Tool");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF4F8FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    g_state = lv_label_create(screen);
    lv_label_set_text(g_state, "State: idle");
    lv_obj_set_style_text_font(g_state, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g_state, lv_color_hex(0x9EC2E8), 0);
    lv_obj_align(g_state, LV_ALIGN_TOP_MID, 0, 31);

    lv_obj_t *init_btn = lv_button_create(screen);
    lv_obj_set_size(init_btn, 96, 30);
    lv_obj_align(init_btn, LV_ALIGN_TOP_LEFT, 10, 50);
    lv_obj_set_style_bg_color(init_btn, lv_color_hex(0x2C7ED1), 0);
    lv_obj_add_flag(init_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(init_btn, init_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *init_txt = lv_label_create(init_btn);
    lv_label_set_text(init_txt, "Init UART");
    lv_obj_center(init_txt);

    g_input = lv_textarea_create(screen);
    lv_obj_set_size(g_input, 134, 30);
    lv_obj_align(g_input, LV_ALIGN_TOP_MID, 0, 50);
    lv_textarea_set_one_line(g_input, true);
    lv_textarea_set_placeholder_text(g_input, "Type message");
    lv_obj_set_style_text_font(g_input, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(g_input, lv_color_hex(0x1A2A3C), 0);
    lv_obj_add_flag(g_input, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(g_input, input_key_cb, LV_EVENT_READY, nullptr);

    lv_obj_t *send_btn = lv_button_create(screen);
    lv_obj_set_size(send_btn, 72, 30);
    lv_obj_align(send_btn, LV_ALIGN_TOP_RIGHT, -10, 50);
    lv_obj_set_style_bg_color(send_btn, lv_color_hex(0x3B8FE2), 0);
    lv_obj_add_flag(send_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(send_btn, send_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *send_txt = lv_label_create(send_btn);
    lv_label_set_text(send_txt, "Send");
    lv_obj_center(send_txt);

    g_log = lv_textarea_create(screen);
    lv_obj_set_size(g_log, 300, 72);
    lv_obj_align(g_log, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_font(g_log, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(g_log, lv_color_hex(0x0F1A28), 0);
    lv_obj_set_style_text_color(g_log, lv_color_hex(0xD8E8FF), 0);
    lv_textarea_set_text(g_log, "UART log ready.\n");
    lv_textarea_set_cursor_click_pos(g_log, false);
    lv_obj_add_state(g_log, LV_STATE_DISABLED);

    lv_obj_t *clear_btn = lv_button_create(screen);
    lv_obj_set_size(clear_btn, 72, 24);
    lv_obj_align(clear_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -86);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(0x6D7E95), 0);
    lv_obj_add_flag(clear_btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(clear_btn, clear_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *clear_txt = lv_label_create(clear_btn);
    lv_label_set_text(clear_txt, "Clear");
    lv_obj_center(clear_txt);

    g_group = lv_group_create();
    lv_group_set_wrap(g_group, true);
    lv_group_add_obj(g_group, init_btn);
    lv_group_add_obj(g_group, g_input);
    lv_group_add_obj(g_group, send_btn);
    lv_group_add_obj(g_group, clear_btn);
    lv_group_focus_obj(g_input);

    g_poll_timer = lv_timer_create(uart_poll_cb, 120, nullptr);
    (void)g_poll_timer;
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}
