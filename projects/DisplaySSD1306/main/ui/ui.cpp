#include "lvgl/lvgl.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

static constexpr int kOledMaxWidth = 128;
static constexpr int kOledMaxHeight = 64;
static constexpr int kOledMaxPages = kOledMaxHeight / 8;
static constexpr int kOledBufferSize = kOledMaxWidth * kOledMaxPages;
static constexpr int kFrameIntervalMs = 120;
static constexpr int kSlideIntervalMs = 2600;
static constexpr int kRetryIntervalMs = 2000;
static constexpr uint8_t kDefaultAddr = 0x3C;
static constexpr uint8_t kAltAddr = 0x3D;

class I2CBus {
public:
    ~I2CBus()
    {
        close_bus();
    }

    bool open_bus(const char *path)
    {
        close_bus();
        path_ = path ? path : "/dev/i2c-1";
        fd_ = open(path_.c_str(), O_RDWR | O_CLOEXEC);
        last_errno_ = fd_ >= 0 ? 0 : errno;
        return fd_ >= 0;
    }

    void close_bus()
    {
        if (fd_ >= 0)
        {
            close(fd_);
            fd_ = -1;
        }
    }

    bool write_bytes(uint8_t addr, const uint8_t *data, size_t len)
    {
        if (fd_ < 0 || data == nullptr || len == 0)
        {
            last_errno_ = EINVAL;
            return false;
        }

        struct i2c_msg msg {};
        msg.addr = addr;
        msg.flags = 0;
        msg.len = static_cast<uint16_t>(len);
        msg.buf = const_cast<uint8_t *>(data);

        struct i2c_rdwr_ioctl_data packet {};
        packet.msgs = &msg;
        packet.nmsgs = 1;
        const bool ok = ioctl(fd_, I2C_RDWR, &packet) >= 0;
        last_errno_ = ok ? 0 : errno;
        return ok;
    }

    int last_errno() const
    {
        return last_errno_;
    }

    const char *last_error() const
    {
        return last_errno_ ? strerror(last_errno_) : "ok";
    }

    const char *path() const
    {
        return path_.c_str();
    }

private:
    int fd_ = -1;
    int last_errno_ = 0;
    std::string path_;
};

enum class DemoScene : int {
    Pulse = 0,
    Bars,
    Orbit,
    Count
};

class Ssd1306Display {
public:
    bool connect(const char *path,
                 uint8_t preferred_addr,
                 uint8_t panel_width,
                 uint8_t column_offset,
                 uint8_t page_offset,
                 uint8_t row_offset,
                 uint8_t start_line,
                 uint8_t panel_height,
                 char *err,
                 size_t err_len)
    {
        ready_ = false;
        panel_width_ = panel_width;
        column_offset_ = column_offset;
        page_offset_ = page_offset & 0x07;
        row_offset_ = row_offset;
        start_line_ = start_line & 0x3F;
        panel_height_ = panel_height;
        panel_pages_ = panel_height_ / 8;

        if (!bus_.open_bus(path))
        {
            std::snprintf(err, err_len, "open %s failed: %s", path ? path : "/dev/i2c-1", bus_.last_error());
            return false;
        }

        const uint8_t candidates[2] = {preferred_addr, preferred_addr == kDefaultAddr ? kAltAddr : kDefaultAddr};
        for (uint8_t candidate : candidates)
        {
            addr_ = candidate;
            if (init_panel(err, err_len))
            {
                ready_ = true;
                clear();
                if (flush(err, err_len))
                {
                    return true;
                }
                ready_ = false;
            }
        }

        bus_.close_bus();
        return false;
    }

    void disconnect()
    {
        ready_ = false;
        bus_.close_bus();
    }

    bool ready() const
    {
        return ready_;
    }

    uint8_t addr() const
    {
        return addr_;
    }

    const char *bus_path() const
    {
        return bus_.path();
    }

    int panel_height() const
    {
        return panel_height_;
    }

    int panel_width() const
    {
        return panel_width_;
    }

    void clear()
    {
        framebuffer_.fill(0);
    }

    void set_pixel(int x, int y, bool on = true)
    {
        if (x < 0 || x >= panel_width_ || y < 0 || y >= panel_height_)
        {
            return;
        }

        uint8_t &cell = framebuffer_[x + (y / 8) * kOledMaxWidth];
        const uint8_t mask = static_cast<uint8_t>(1U << (y & 0x7));
        if (on)
        {
            cell |= mask;
        }
        else
        {
            cell &= static_cast<uint8_t>(~mask);
        }
    }

    void draw_hline(int x, int y, int w, bool on = true)
    {
        for (int i = 0; i < w; ++i)
        {
            set_pixel(x + i, y, on);
        }
    }

    void draw_vline(int x, int y, int h, bool on = true)
    {
        for (int i = 0; i < h; ++i)
        {
            set_pixel(x, y + i, on);
        }
    }

    void draw_rect(int x, int y, int w, int h, bool on = true)
    {
        if (w <= 0 || h <= 0)
        {
            return;
        }

        draw_hline(x, y, w, on);
        draw_hline(x, y + h - 1, w, on);
        draw_vline(x, y, h, on);
        draw_vline(x + w - 1, y, h, on);
    }

    void fill_rect(int x, int y, int w, int h, bool on = true)
    {
        for (int yy = 0; yy < h; ++yy)
        {
            for (int xx = 0; xx < w; ++xx)
            {
                set_pixel(x + xx, y + yy, on);
            }
        }
    }

    void draw_line(int x0, int y0, int x1, int y1, bool on = true)
    {
        int dx = std::abs(x1 - x0);
        int sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0);
        int sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;

        while (true)
        {
            set_pixel(x0, y0, on);
            if (x0 == x1 && y0 == y1)
            {
                break;
            }

            int e2 = err * 2;
            if (e2 >= dy)
            {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx)
            {
                err += dx;
                y0 += sy;
            }
        }
    }

    void draw_text(int x, int y, const char *text, int scale = 1, bool on = true)
    {
        if (text == nullptr || scale <= 0)
        {
            return;
        }

        int cursor_x = x;
        while (*text)
        {
            draw_char(cursor_x, y, *text, scale, on);
            cursor_x += 6 * scale;
            ++text;
        }
    }

    bool flush(char *err, size_t err_len)
    {
        if (!ready_)
        {
            std::snprintf(err, err_len, "display not ready");
            return false;
        }

        for (int page = 0; page < panel_pages_; ++page)
        {
            const uint8_t cmds[3] = {
                static_cast<uint8_t>(0xB0 | ((page + page_offset_) & 0x07)),
                static_cast<uint8_t>(0x00 | (column_offset_ & 0x0F)),
                static_cast<uint8_t>(0x10 | ((column_offset_ >> 4) & 0x0F)),
            };
            if (!send_commands(cmds, sizeof(cmds), err, err_len))
            {
                ready_ = false;
                bus_.close_bus();
                return false;
            }

            if (!send_data(&framebuffer_[page * kOledMaxWidth], panel_width_, err, err_len))
            {
                ready_ = false;
                bus_.close_bus();
                return false;
            }
        }

        std::snprintf(err, err_len, "flush ok");
        return true;
    }

private:
    bool init_panel(char *err, size_t err_len)
    {
        const uint8_t init_cmds[] = {
            0xAE,
            0xD5, 0x80,
            0xA8, static_cast<uint8_t>(panel_height_ - 1),
            0xD3, row_offset_,
            static_cast<uint8_t>(0x40 | start_line_),
            0x8D, 0x14,
            0x20, 0x02,
            0xA1,
            0xC8,
            0xDA, static_cast<uint8_t>(panel_height_ == 32 ? 0x02 : 0x12),
            0x81, 0xCF,
            0xD9, 0xF1,
            0xDB, 0x40,
            0xA4,
            0xA6,
            0x2E,
            0xAF,
        };

        if (!send_commands(init_cmds, sizeof(init_cmds), err, err_len))
        {
            return false;
        }

        std::snprintf(err, err_len, "oled ok @ 0x%02X", addr_);
        return true;
    }

    bool send_commands(const uint8_t *cmds, size_t len, char *err, size_t err_len)
    {
        if (cmds == nullptr || len == 0)
        {
            std::snprintf(err, err_len, "empty command");
            return false;
        }

        std::array<uint8_t, 32> payload {};
        if (len + 1 > payload.size())
        {
            std::snprintf(err, err_len, "command batch too large");
            return false;
        }

        payload[0] = 0x00;
        std::memcpy(payload.data() + 1, cmds, len);
        if (!bus_.write_bytes(addr_, payload.data(), len + 1))
        {
            std::snprintf(err, err_len, "cmd 0x%02X failed: %s", addr_, bus_.last_error());
            return false;
        }

        return true;
    }

    bool send_data(const uint8_t *data, size_t len, char *err, size_t err_len)
    {
        if (data == nullptr || len == 0)
        {
            std::snprintf(err, err_len, "empty data");
            return false;
        }

        std::array<uint8_t, kOledMaxWidth + 1> payload {};
        if (len + 1 > payload.size())
        {
            std::snprintf(err, err_len, "data batch too large");
            return false;
        }

        payload[0] = 0x40;
        std::memcpy(payload.data() + 1, data, len);
        if (!bus_.write_bytes(addr_, payload.data(), len + 1))
        {
            std::snprintf(err, err_len, "data 0x%02X failed: %s", addr_, bus_.last_error());
            return false;
        }

        return true;
    }

    void draw_char(int x, int y, char raw, int scale, bool on)
    {
        const char c = normalize_char(raw);
        const std::array<uint8_t, 5> glyph = glyph_for(c);

        for (int col = 0; col < 5; ++col)
        {
            uint8_t bits = glyph[static_cast<size_t>(col)];
            for (int row = 0; row < 7; ++row)
            {
                if ((bits & (1U << row)) == 0)
                {
                    continue;
                }

                const int px = x + col * scale;
                const int py = y + row * scale;
                fill_rect(px, py, scale, scale, on);
            }
        }
    }

    static char normalize_char(char c)
    {
        if (c >= 'a' && c <= 'z')
        {
            return static_cast<char>(c - 'a' + 'A');
        }
        return c;
    }

    static std::array<uint8_t, 5> glyph_for(char c)
    {
        switch (c)
        {
        case '0': return {0x3E, 0x51, 0x49, 0x45, 0x3E};
        case '1': return {0x00, 0x42, 0x7F, 0x40, 0x00};
        case '2': return {0x42, 0x61, 0x51, 0x49, 0x46};
        case '3': return {0x21, 0x41, 0x45, 0x4B, 0x31};
        case '4': return {0x18, 0x14, 0x12, 0x7F, 0x10};
        case '5': return {0x27, 0x45, 0x45, 0x45, 0x39};
        case '6': return {0x3C, 0x4A, 0x49, 0x49, 0x30};
        case '7': return {0x01, 0x71, 0x09, 0x05, 0x03};
        case '8': return {0x36, 0x49, 0x49, 0x49, 0x36};
        case '9': return {0x06, 0x49, 0x49, 0x29, 0x1E};
        case 'A': return {0x7E, 0x11, 0x11, 0x11, 0x7E};
        case 'B': return {0x7F, 0x49, 0x49, 0x49, 0x36};
        case 'C': return {0x3E, 0x41, 0x41, 0x41, 0x22};
        case 'D': return {0x7F, 0x41, 0x41, 0x22, 0x1C};
        case 'E': return {0x7F, 0x49, 0x49, 0x49, 0x41};
        case 'F': return {0x7F, 0x09, 0x09, 0x09, 0x01};
        case 'G': return {0x3E, 0x41, 0x49, 0x49, 0x7A};
        case 'H': return {0x7F, 0x08, 0x08, 0x08, 0x7F};
        case 'I': return {0x00, 0x41, 0x7F, 0x41, 0x00};
        case 'J': return {0x20, 0x40, 0x41, 0x3F, 0x01};
        case 'K': return {0x7F, 0x08, 0x14, 0x22, 0x41};
        case 'L': return {0x7F, 0x40, 0x40, 0x40, 0x40};
        case 'M': return {0x7F, 0x02, 0x0C, 0x02, 0x7F};
        case 'N': return {0x7F, 0x04, 0x08, 0x10, 0x7F};
        case 'O': return {0x3E, 0x41, 0x41, 0x41, 0x3E};
        case 'P': return {0x7F, 0x09, 0x09, 0x09, 0x06};
        case 'Q': return {0x3E, 0x41, 0x51, 0x21, 0x5E};
        case 'R': return {0x7F, 0x09, 0x19, 0x29, 0x46};
        case 'S': return {0x46, 0x49, 0x49, 0x49, 0x31};
        case 'T': return {0x01, 0x01, 0x7F, 0x01, 0x01};
        case 'U': return {0x3F, 0x40, 0x40, 0x40, 0x3F};
        case 'V': return {0x1F, 0x20, 0x40, 0x20, 0x1F};
        case 'W': return {0x7F, 0x20, 0x18, 0x20, 0x7F};
        case 'X': return {0x63, 0x14, 0x08, 0x14, 0x63};
        case 'Y': return {0x03, 0x04, 0x78, 0x04, 0x03};
        case 'Z': return {0x61, 0x51, 0x49, 0x45, 0x43};
        case '-': return {0x08, 0x08, 0x08, 0x08, 0x08};
        case '.': return {0x00, 0x60, 0x60, 0x00, 0x00};
        case ':': return {0x00, 0x36, 0x36, 0x00, 0x00};
        case '/': return {0x20, 0x10, 0x08, 0x04, 0x02};
        case ' ': return {0x00, 0x00, 0x00, 0x00, 0x00};
        default: return {0x02, 0x01, 0x51, 0x09, 0x06};
        }
    }

    I2CBus bus_;
    std::array<uint8_t, kOledBufferSize> framebuffer_ {};
    uint8_t addr_ = kDefaultAddr;
    uint8_t panel_width_ = 128;
    uint8_t column_offset_ = 0;
    uint8_t page_offset_ = 0;
    uint8_t row_offset_ = 0;
    uint8_t start_line_ = 0;
    uint8_t panel_height_ = 64;
    uint8_t panel_pages_ = 8;
    bool ready_ = false;
};

static Ssd1306Display g_display;
static lv_obj_t *g_status_value = nullptr;
static lv_obj_t *g_bus_value = nullptr;
static lv_obj_t *g_slide_value = nullptr;
static lv_obj_t *g_hint_value = nullptr;
static lv_group_t *g_group = nullptr;
static lv_timer_t *g_ui_timer = nullptr;
static int g_screen_w = 320;
static int g_screen_h = 170;
static int g_retry_ms = kRetryIntervalMs;
static int g_elapsed_ms = 0;
static int g_frame = 0;
static DemoScene g_scene = DemoScene::Pulse;
static bool g_grove_i2c_ready = false;
static char g_status_text[96] = "WAITING FOR OLED";
static char g_bus_text[96] = "BUS --";
static char g_slide_text[64] = "PULSE";
static char g_hint_text[96] = "SAFE AREA ANIMATION LOOP";

static const char *getenv_default(const char *name, const char *dflt)
{
    const char *value = std::getenv(name);
    return value && value[0] ? value : dflt;
}

static uint8_t getenv_u8(const char *name, uint8_t fallback)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return fallback;
    }

    char *end = nullptr;
    long parsed = std::strtol(value, &end, 0);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 255)
    {
        return fallback;
    }
    return static_cast<uint8_t>(parsed);
}

static uint8_t normalize_panel_height(uint8_t value)
{
    if (value < 8)
    {
        return 32;
    }
    if (value > 64)
    {
        return 64;
    }

    value = static_cast<uint8_t>((value / 8) * 8);
    return value < 8 ? 8 : value;
}

static uint8_t normalize_panel_width(uint8_t value)
{
    if (value < 16)
    {
        return 64;
    }
    if (value > 128)
    {
        return 128;
    }
    return value;
}

static uint8_t clamp_inset(uint8_t value, uint8_t limit)
{
    return value > limit ? limit : value;
}

static void switch_grove_to_i2c()
{
    if (g_grove_i2c_ready)
    {
        return;
    }

    g_grove_i2c_ready = true;
    std::printf("[DisplaySSD1306] init: switch power on (gpio17=1)\n");
    std::system("timeout 1 gpioset -c gpiochip0 17=1 >/dev/null 2>&1");
    std::printf("[DisplaySSD1306] init: switch mux to I2C (gpio4=1)\n");
    std::system("timeout 1 gpioset -c gpiochip0 4=1 >/dev/null 2>&1");
}

static void sync_lvgl_labels()
{
    if (g_status_value)
    {
        lv_label_set_text(g_status_value, g_status_text);
    }
    if (g_bus_value)
    {
        lv_label_set_text(g_bus_value, g_bus_text);
    }
    if (g_slide_value)
    {
        lv_label_set_text(g_slide_value, g_slide_text);
    }
    if (g_hint_value)
    {
        lv_label_set_text(g_hint_value, g_hint_text);
    }
}

static const char *scene_name(DemoScene scene)
{
    switch (scene)
    {
    case DemoScene::Pulse: return "PULSE";
    case DemoScene::Bars: return "BARS";
    case DemoScene::Orbit: return "ORBIT";
    case DemoScene::Count: break;
    }
    return "DEMO";
}

static void update_scene_label()
{
    std::snprintf(g_slide_text, sizeof(g_slide_text), "%s", scene_name(g_scene));
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static lv_obj_t *create_card(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t accent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x14202B), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2F4458), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, 5, h);
    lv_obj_set_style_bg_color(bar, accent, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    return card;
}

static void render_scene_pulse(int outer_x, int outer_y, int outer_w, int outer_h, int frame)
{
    const int cx = outer_x + outer_w / 2;
    const int cy = outer_y + outer_h / 2;
    const float t = static_cast<float>(frame) * 0.24f;
    const int radius = 4 + static_cast<int>((std::sin(t) + 1.0f) * 5.0f);

    for (int r = 0; r < 3; ++r)
    {
        const int rr = radius + r * 5;
        for (int a = 0; a < 360; a += 18)
        {
            const float rad = static_cast<float>(a) * 3.1415926f / 180.0f;
            const int x = cx + static_cast<int>(std::cos(rad) * rr);
            const int y = cy + static_cast<int>(std::sin(rad) * rr * 0.55f);
            g_display.set_pixel(x, y, true);
        }
    }

    g_display.fill_rect(cx - 3, cy - 3, 7, 7, true);
    g_display.draw_text(outer_x + 2, outer_y + 2, "CP0", 1);
}

static void render_scene_bars(int outer_x, int outer_y, int outer_w, int outer_h, int frame)
{
    const int bar_count = outer_w >= 64 ? 6 : 4;
    const int slot = outer_w / bar_count;
    const int bar_w = slot > 8 ? slot - 4 : slot - 2;
    const int bottom = outer_y + outer_h - 1;

    for (int i = 0; i < bar_count; ++i)
    {
        const float phase = static_cast<float>(frame) * 0.22f + static_cast<float>(i) * 0.7f;
        const int h = 4 + static_cast<int>((std::sin(phase) + 1.0f) * (outer_h - 8) * 0.45f);
        const int x = outer_x + i * slot + (slot - bar_w) / 2;
        const int y = bottom - h;
        g_display.fill_rect(x, y, bar_w, h, true);
    }

    g_display.draw_text(outer_x + 2, outer_y + 2, "WAVE", 1);
}

static void render_scene_orbit(int outer_x, int outer_y, int outer_w, int outer_h, int frame)
{
    const int cx = outer_x + outer_w / 2;
    const int cy = outer_y + outer_h / 2;
    const int rx = outer_w / 2 - 6;
    const int ry = outer_h / 2 - 6;

    for (int i = 0; i < 5; ++i)
    {
        const float phase = static_cast<float>(frame) * 0.16f + static_cast<float>(i) * 1.2f;
        const int x = cx + static_cast<int>(std::cos(phase) * rx);
        const int y = cy + static_cast<int>(std::sin(phase * 1.37f) * ry);
        g_display.fill_rect(x - 1, y - 1, 3, 3, true);
    }

    g_display.draw_line(outer_x + 2, outer_y + outer_h - 4,
                        outer_x + outer_w - 3, outer_y + 3, true);
    g_display.draw_text(outer_x + 2, outer_y + 2, "FLOW", 1);
}

static void render_demo_scene(int frame)
{
    const int oled_w = g_display.panel_width();
    const int oled_h = g_display.panel_height();
    const int inset_x = clamp_inset(getenv_u8("DISPLAY_SSD1306_INSET_X", 4),
                                    static_cast<uint8_t>(oled_w / 4));
    const int inset_y = clamp_inset(getenv_u8("DISPLAY_SSD1306_INSET_Y", 4),
                                    static_cast<uint8_t>(oled_h / 4));
    const int outer_x = inset_x;
    const int outer_y = inset_y;
    const int outer_w = oled_w - inset_x * 2;
    const int outer_h = oled_h - inset_y * 2;

    g_display.clear();
    if (outer_w < 12 || outer_h < 12)
    {
        return;
    }

    switch (g_scene)
    {
    case DemoScene::Pulse:
        render_scene_pulse(outer_x, outer_y, outer_w, outer_h, frame);
        break;
    case DemoScene::Bars:
        render_scene_bars(outer_x, outer_y, outer_w, outer_h, frame);
        break;
    case DemoScene::Orbit:
        render_scene_orbit(outer_x, outer_y, outer_w, outer_h, frame);
        break;
    case DemoScene::Count:
        break;
    }
}

static bool ensure_display_ready()
{
    if (g_display.ready())
    {
        return true;
    }

    switch_grove_to_i2c();

    const char *i2c_dev = getenv_default("DISPLAY_SSD1306_I2C_DEV", "/dev/i2c-1");
    const uint8_t oled_addr = getenv_u8("DISPLAY_SSD1306_ADDR", kDefaultAddr);
    const uint8_t panel_width = normalize_panel_width(
        getenv_u8("DISPLAY_SSD1306_WIDTH", 64));
    const uint8_t column_offset = getenv_u8("DISPLAY_SSD1306_COL_OFFSET", 32);
    const uint8_t page_offset = getenv_u8("DISPLAY_SSD1306_PAGE_OFFSET", 1);
    const uint8_t row_offset = getenv_u8("DISPLAY_SSD1306_ROW_OFFSET", 0);
    const uint8_t start_line = getenv_u8("DISPLAY_SSD1306_START_LINE", 0);
    const uint8_t panel_height = normalize_panel_height(
        getenv_u8("DISPLAY_SSD1306_HEIGHT", 32));

    char err[96] = {};
    if (!g_display.connect(i2c_dev,
                           oled_addr,
                           panel_width,
                           column_offset,
                           page_offset,
                           row_offset,
                           start_line,
                           panel_height,
                           err,
                           sizeof(err)))
    {
        std::snprintf(g_status_text, sizeof(g_status_text), "OLED FAIL");
        std::snprintf(g_bus_text, sizeof(g_bus_text), "%s", err);
        std::printf("[DisplaySSD1306] %s\n", err);
        sync_lvgl_labels();
        return false;
    }

    std::snprintf(g_status_text, sizeof(g_status_text), "OLED OK 0X%02X", g_display.addr());
    std::snprintf(g_bus_text, sizeof(g_bus_text), "BUS %s", g_display.bus_path());
    std::snprintf(g_hint_text,
                  sizeof(g_hint_text),
                  "ADDR 0X%02X %uX%u C%u P%u R%u S%u",
                  g_display.addr(),
                  panel_width,
                  panel_height,
                  column_offset,
                  page_offset,
                  row_offset,
                  start_line);
    std::printf("[DisplaySSD1306] ready: addr=0x%02X bus=%s\n", g_display.addr(), g_display.bus_path());
    sync_lvgl_labels();
    return true;
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    g_frame += 1;
    g_retry_ms += kFrameIntervalMs;
    g_elapsed_ms += kFrameIntervalMs;

    if (!g_display.ready())
    {
        if (g_retry_ms >= kRetryIntervalMs)
        {
            g_retry_ms = 0;
            ensure_display_ready();
        }
        return;
    }

    if (g_elapsed_ms >= kSlideIntervalMs)
    {
        g_elapsed_ms = 0;
        g_scene = static_cast<DemoScene>((static_cast<int>(g_scene) + 1) % static_cast<int>(DemoScene::Count));
        update_scene_label();
        sync_lvgl_labels();
    }

    render_demo_scene(g_frame);

    char err[96] = {};
    if (!g_display.flush(err, sizeof(err)))
    {
        std::snprintf(g_status_text, sizeof(g_status_text), "OLED LOST");
        std::snprintf(g_bus_text, sizeof(g_bus_text), "%s", err);
        std::printf("[DisplaySSD1306] %s\n", err);
        sync_lvgl_labels();
    }
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

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x091018), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = create_label(screen, "Display SSD1306", &lv_font_montserrat_20, lv_color_hex(0xF5F7FA));
    lv_obj_set_pos(title, 10, 8);

    lv_obj_t *subtitle = create_label(screen, "Grove I2C safe-area animation demo", &lv_font_montserrat_12, lv_color_hex(0x9FB1C5));
    lv_obj_set_pos(subtitle, 10, 34);

    const int pad = 10;
    lv_obj_t *status_card = create_card(screen, pad, 56, g_screen_w - pad * 2, 46, lv_color_hex(0x29C4A9));
    lv_obj_t *status_title = create_label(status_card, "OLED STATUS", &lv_font_montserrat_12, lv_color_hex(0xA7BBCD));
    lv_obj_set_pos(status_title, 14, 6);
    g_status_value = create_label(status_card, g_status_text, &lv_font_montserrat_20, lv_color_hex(0xF5F7FA));
    lv_obj_set_pos(g_status_value, 14, 18);

    lv_obj_t *bus_card = create_card(screen, pad, 110, (g_screen_w - pad * 3) / 2, 42, lv_color_hex(0x5CA9FF));
    lv_obj_t *bus_title = create_label(bus_card, "BUS", &lv_font_montserrat_12, lv_color_hex(0xA7BBCD));
    lv_obj_set_pos(bus_title, 14, 6);
    g_bus_value = create_label(bus_card, g_bus_text, &lv_font_montserrat_12, lv_color_hex(0xF5F7FA));
    lv_obj_set_width(g_bus_value, (g_screen_w - pad * 3) / 2 - 22);
    lv_obj_set_pos(g_bus_value, 14, 20);

    lv_obj_t *slide_card = create_card(screen, pad + (g_screen_w - pad * 3) / 2 + pad, 110, (g_screen_w - pad * 3) / 2, 42, lv_color_hex(0xFF8A5B));
    lv_obj_t *slide_title = create_label(slide_card, "SLIDE", &lv_font_montserrat_12, lv_color_hex(0xA7BBCD));
    lv_obj_set_pos(slide_title, 14, 6);
    g_slide_value = create_label(slide_card, g_slide_text, &lv_font_montserrat_12, lv_color_hex(0xF5F7FA));
    lv_obj_set_pos(g_slide_value, 14, 20);

    g_hint_value = create_label(screen, g_hint_text, &lv_font_montserrat_12, lv_color_hex(0x7E94AA));
    lv_obj_set_pos(g_hint_value, 10, g_screen_h - 16);

    g_group = lv_group_create();
    g_ui_timer = lv_timer_create(ui_timer_cb, kFrameIntervalMs, nullptr);
    (void)g_ui_timer;

    g_retry_ms = kRetryIntervalMs;
    g_elapsed_ms = 0;
    g_frame = 0;
    update_scene_label();
    sync_lvgl_labels();
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}
