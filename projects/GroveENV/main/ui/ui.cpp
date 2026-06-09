#include "lvgl/lvgl.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace {

static constexpr uint8_t kSht30Addr = 0x44;
static constexpr int kSampleIntervalMs = 2000;
static constexpr int kChartPoints = 56;

struct EnvSample {
    bool sht_ok = false;
    float temperature_c = NAN;
    float humidity_rh = NAN;
};

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
        bool ok = ioctl(fd_, I2C_RDWR, &packet) >= 0;
        last_errno_ = ok ? 0 : errno;
        return ok;
    }

    bool read_bytes(uint8_t addr, uint8_t *data, size_t len)
    {
        if (fd_ < 0 || data == nullptr || len == 0)
        {
            last_errno_ = EINVAL;
            return false;
        }

        struct i2c_msg msg {};
        msg.addr = addr;
        msg.flags = I2C_M_RD;
        msg.len = static_cast<uint16_t>(len);
        msg.buf = data;

        struct i2c_rdwr_ioctl_data packet {};
        packet.msgs = &msg;
        packet.nmsgs = 1;
        bool ok = ioctl(fd_, I2C_RDWR, &packet) >= 0;
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

private:
    int fd_ = -1;
    int last_errno_ = 0;
    std::string path_;
};

static lv_obj_t *g_temp_value = nullptr;
static lv_obj_t *g_hum_value = nullptr;
static lv_obj_t *g_chart = nullptr;
static lv_chart_series_t *g_temp_series = nullptr;
static lv_chart_series_t *g_hum_series = nullptr;
static lv_timer_t *g_ui_timer = nullptr;
static lv_group_t *g_group = nullptr;
static std::mutex g_sample_mutex;
static EnvSample g_latest;
static std::atomic<bool> g_sample_ready{false};
static std::atomic<bool> g_sampling{false};
static std::atomic<bool> g_grove_i2c_ready{false};
static int g_elapsed_ms = 0;
static int g_screen_w = 320;
static int g_screen_h = 170;

static const char *getenv_default(const char *name, const char *dflt)
{
    const char *value = getenv(name);
    return value && value[0] ? value : dflt;
}

static void switch_grove_to_i2c()
{
    bool expected = false;
    if (!g_grove_i2c_ready.compare_exchange_strong(expected, true))
    {
        return;
    }

    printf("[GroveENV] init: switch power on (gpio17=1)\n");
    system("timeout 1 gpioset -c gpiochip0 17=1 >/dev/null 2>&1");
    printf("[GroveENV] init: switch mux to I2C (gpio4=1)\n");
    system("timeout 1 gpioset -c gpiochip0 4=1 >/dev/null 2>&1");
}

static uint8_t sht_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

static bool read_sht30(I2CBus &bus, float *temperature_c, float *humidity_rh, char *err, size_t err_len)
{
    const uint8_t cmd[2] = {0x24, 0x00};
    if (!bus.write_bytes(kSht30Addr, cmd, sizeof(cmd)))
    {
        std::snprintf(err, err_len, "SHT30 cmd fail: %s", bus.last_error());
        return false;
    }

    usleep(16000);

    uint8_t data[6] = {};
    if (!bus.read_bytes(kSht30Addr, data, sizeof(data)))
    {
        std::snprintf(err, err_len, "SHT30 read fail: %s", bus.last_error());
        return false;
    }

    if (sht_crc8(data, 2) != data[2] || sht_crc8(data + 3, 2) != data[5])
    {
        std::snprintf(err,
                      err_len,
                      "SHT30 crc fail: %02X %02X %02X %02X %02X %02X",
                      data[0],
                      data[1],
                      data[2],
                      data[3],
                      data[4],
                      data[5]);
        return false;
    }

    uint16_t raw_t = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    uint16_t raw_h = (static_cast<uint16_t>(data[3]) << 8) | data[4];
    if (temperature_c)
    {
        *temperature_c = -45.0f + 175.0f * static_cast<float>(raw_t) / 65535.0f;
    }
    if (humidity_rh)
    {
        *humidity_rh = 100.0f * static_cast<float>(raw_h) / 65535.0f;
    }
    std::snprintf(err, err_len, "SHT30 ok %.1fC %.0f%%", temperature_c ? *temperature_c : NAN, humidity_rh ? *humidity_rh : NAN);
    return true;
}

static void sample_worker()
{
    EnvSample sample;
    const char *i2c_dev = getenv_default("GROVE_ENV_I2C_DEV", "/dev/i2c-1");

    I2CBus bus;
    if (!bus.open_bus(i2c_dev))
    {
        printf("[GroveENV] open %s failed: %s\n", i2c_dev, strerror(errno));
    }
    else
    {
        char sht_status[80] = {};
        sample.sht_ok = read_sht30(bus, &sample.temperature_c, &sample.humidity_rh, sht_status, sizeof(sht_status));
        printf("[GroveENV] %s\n", sht_status);
    }

    {
        std::lock_guard<std::mutex> lock(g_sample_mutex);
        g_latest = sample;
    }
    g_sample_ready.store(true);
    g_sampling.store(false);
}

static void start_sample()
{
    if (g_sampling.exchange(true))
    {
        return;
    }

    std::thread worker(sample_worker);
    worker.detach();
}

static int chart_lane_value(float value, float min_value, float max_value, int lane_min, int lane_max)
{
    if (!std::isfinite(value))
    {
        return lane_min;
    }
    if (value < min_value)
    {
        value = min_value;
    }
    if (value > max_value)
    {
        value = max_value;
    }

    float ratio = (value - min_value) / (max_value - min_value);
    return lane_min + static_cast<int>(ratio * static_cast<float>(lane_max - lane_min));
}

static void set_label_or_dash(lv_obj_t *label, const char *fmt, float value)
{
    if (label == nullptr)
    {
        return;
    }
    if (!std::isfinite(value))
    {
        lv_label_set_text(label, "--");
        return;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, value);
    lv_label_set_text(label, buf);
}

static void apply_sample(const EnvSample &sample)
{
    set_label_or_dash(g_temp_value, "%.1f C", sample.sht_ok ? sample.temperature_c : NAN);
    set_label_or_dash(g_hum_value, "%.0f %%", sample.sht_ok ? sample.humidity_rh : NAN);

    if (g_chart && g_temp_series && g_hum_series)
    {
        lv_chart_set_next_value(g_chart, g_temp_series, chart_lane_value(sample.temperature_c, 0.0f, 50.0f, 4, 30));
        lv_chart_set_next_value(g_chart, g_hum_series, chart_lane_value(sample.humidity_rh, 0.0f, 100.0f, 37, 63));
        lv_chart_refresh(g_chart);
    }
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (g_sample_ready.exchange(false))
    {
        EnvSample sample;
        {
            std::lock_guard<std::mutex> lock(g_sample_mutex);
            sample = g_latest;
        }
        apply_sample(sample);
    }

    g_elapsed_ms += 50;
    if (g_elapsed_ms >= kSampleIntervalMs)
    {
        g_elapsed_ms = 0;
        start_sample();
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

static lv_obj_t *create_metric_card(lv_obj_t *parent,
                                    const char *title,
                                    const char *initial_value,
                                    lv_color_t accent,
                                    int x,
                                    int y,
                                    int w,
                                    int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x172130), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x33475E), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, 4, h);
    lv_obj_set_style_bg_color(bar, accent, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    lv_obj_t *name = create_label(card, title, &lv_font_montserrat_12, lv_color_hex(0xA9B8C8));
    lv_obj_set_pos(name, 10, 5);

    lv_obj_t *value = create_label(card, initial_value, &lv_font_montserrat_20, lv_color_hex(0xF4F7FB));
    lv_obj_set_width(value, w - 12);
    lv_obj_set_pos(value, 10, 21);
    return value;
}

static void create_legend(lv_obj_t *parent, int x, int y, lv_color_t color, const char *text)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_pos(dot, x, y + 5);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t *label = create_label(parent, text, &lv_font_montserrat_12, lv_color_hex(0xC8D2DD));
    lv_obj_set_pos(label, x + 10, y);
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

    switch_grove_to_i2c();

    lv_obj_t *title = create_label(screen, "Grove ENV", &lv_font_montserrat_20, lv_color_hex(0xF7FAFF));
    lv_obj_set_pos(title, 10, 5);

    const int pad = 8;
    const int gap = 8;
    const int card_y = 30;
    const int card_h = 50;
    const int card_w = (g_screen_w - pad * 2 - gap) / 2;

    g_temp_value = create_metric_card(screen, "TEMP", "--", lv_color_hex(0xFF6B6B), pad, card_y, card_w, card_h);
    g_hum_value = create_metric_card(screen, "HUM", "--", lv_color_hex(0x4ECDC4), pad + card_w + gap, card_y, card_w, card_h);

    const int chart_y = card_y + card_h + 8;
    const int chart_h = g_screen_h - chart_y - 25;
    g_chart = lv_chart_create(screen);
    lv_obj_set_pos(g_chart, pad, chart_y);
    lv_obj_set_size(g_chart, g_screen_w - pad * 2, chart_h > 54 ? chart_h : 54);
    lv_obj_set_style_radius(g_chart, 6, 0);
    lv_obj_set_style_bg_color(g_chart, lv_color_hex(0x111A27), 0);
    lv_obj_set_style_bg_opa(g_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_chart, 1, 0);
    lv_obj_set_style_border_color(g_chart, lv_color_hex(0x2F4358), 0);
    lv_obj_set_style_line_color(g_chart, lv_color_hex(0x25384C), LV_PART_MAIN);
    lv_obj_set_style_line_width(g_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_chart, 4, 0);
    lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(g_chart, kChartPoints);
    lv_chart_set_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(g_chart, 4, 6);
    lv_chart_set_update_mode(g_chart, LV_CHART_UPDATE_MODE_SHIFT);

    g_temp_series = lv_chart_add_series(g_chart, lv_color_hex(0xFF6B6B), LV_CHART_AXIS_PRIMARY_Y);
    g_hum_series = lv_chart_add_series(g_chart, lv_color_hex(0x4ECDC4), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(g_chart, g_temp_series, 0);
    lv_chart_set_all_value(g_chart, g_hum_series, 0);

    const int footer_y = g_screen_h - 18;
    create_legend(screen, pad, footer_y, lv_color_hex(0xFF6B6B), "T");
    create_legend(screen, pad + 39, footer_y, lv_color_hex(0x4ECDC4), "RH");

    g_group = lv_group_create();
    g_ui_timer = lv_timer_create(ui_timer_cb, 50, nullptr);
    (void)g_ui_timer;

    g_elapsed_ms = kSampleIntervalMs - 300;
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}
