#include "lvgl/lvgl.h"

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

static constexpr int kLineDisabled = -1;
static constexpr int kDefaultPowerLine = 17;
static constexpr int kDefaultRouteLine = 0;
static constexpr int kDefaultLed1Line = 23;
static constexpr int kDefaultLed2Line = kLineDisabled;
static constexpr int kDefaultButtonLine = 26;
static constexpr int kMaxLineNum = 4096;
static constexpr int kDebounceSamples = 3;
static constexpr int kInputPollMs = 90;
static constexpr int kAutoToggleMs = 2000;
static constexpr uint32_t kHighColor = 0x5AE08D;
static constexpr uint32_t kLowColor = 0xA2AFBD;
static constexpr int kDotPulseMinAlpha = 80;
static constexpr int kDotPulseMaxAlpha = 255;
static constexpr int kDotBaseSize = 26;

struct GpioPin {
    std::string chip = "gpiochip0";
    int line = -1;
};

static GpioPin g_power_pin = {"gpiochip0", kDefaultPowerLine};
static GpioPin g_route_pin = {"gpiochip1", kDefaultRouteLine};
static GpioPin g_led1_pin = {"gpiochip0", kDefaultLed1Line};
static GpioPin g_led2_pin = {"gpiochip0", kDefaultLed2Line};
static GpioPin g_button_pin = {"gpiochip0", kDefaultButtonLine};

static bool g_button_active_low = true;
static bool g_led1_state = false;
static bool g_led2_state = false;
static int g_last_button_sample = -1;
static int g_stable_button_sample = -1;
static int g_button_sample_count = 0;
static int g_screen_w = 320;
static int g_screen_h = 170;

static lv_obj_t *g_status_label = nullptr;
static lv_obj_t *g_output_panel = nullptr;
static lv_obj_t *g_input_panel = nullptr;

static lv_obj_t *g_led1_pin_label = nullptr;
static lv_obj_t *g_led2_pin_label = nullptr;
static lv_obj_t *g_led1_level_label = nullptr;
static lv_obj_t *g_led2_level_label = nullptr;
static lv_obj_t *g_btn_pin_label = nullptr;
static lv_obj_t *g_btn_level_label = nullptr;
static lv_obj_t *g_log_label = nullptr;

static lv_obj_t *g_led1_dot = nullptr;
static lv_obj_t *g_led2_dot = nullptr;
static lv_obj_t *g_input_dot = nullptr;

static lv_group_t *g_group = nullptr;
static std::string g_last_log_text;

struct HeldChip {
    pid_t pid = -1;
    std::string chip;
};

enum class GpioSetVariant {
    ChipOptionAssign,
    ChipOptionSplit,
    PositionalAssign,
    PositionalSplit,
};

enum class GpioGetVariant {
    ChipOptionBiasLong,
    ChipOptionBiasShort,
    ChipOptionBiasLegacy,
    ChipOptionBiasEq,
    Positional,
    ChipOption,
    PositionalBiasLong,
};

static std::array<HeldChip, 6> g_output_holds;
static GpioSetVariant g_gpioset_variant = GpioSetVariant::ChipOptionAssign;
static bool g_gpioset_variant_locked = false;
static GpioGetVariant g_gpioget_variant = GpioGetVariant::ChipOption;
static bool g_gpioget_variant_locked = false;
static bool g_power_state = true;
static bool g_route_state = false;
static bool g_auto_toggle_enabled = true;
static bool g_button_pullup_enabled = true;
static std::string g_led1_issue;
static std::string g_led2_issue;
static bool g_led1_ready = false;
static bool g_led2_ready = false;
static bool g_button_ready = false;
static bool g_auto_toggle_error_reported = false;
static bool g_button_error_reported = false;

static bool is_gpio_line_valid(int line)
{
    return line >= 0 && line < kMaxLineNum;
}

static bool chip_is_safe(const std::string &chip)
{
    if (chip.empty())
    {
        return false;
    }

    for (char ch : chip)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
        {
            continue;
        }
        return false;
    }
    return true;
}

static std::string getenv_or_default(const char *name, const char *dflt)
{
    const char *value = getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return dflt ? dflt : "";
    }
    return value;
}

static int getenv_to_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return fallback;
    }

    char *end = nullptr;
    errno = 0;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || errno == ERANGE || parsed < kLineDisabled || parsed > kMaxLineNum)
    {
        return fallback;
    }
    return static_cast<int>(parsed);
}

static bool getenv_to_bool(const char *name, bool fallback)
{
    const char *value = getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return fallback;
    }

    if (std::strcmp(value, "1") == 0 || value[0] == 'y' || value[0] == 'Y' || value[0] == 't' || value[0] == 'T')
    {
        return true;
    }

    if (std::strcmp(value, "0") == 0 || value[0] == 'n' || value[0] == 'N' || value[0] == 'f' || value[0] == 'F')
    {
        return false;
    }

    return fallback;
}

static void set_pin(GpioPin &pin, const char *chip_env, const char *line_env, const char *chip_dflt, int line_dflt)
{
    pin.chip = getenv_or_default(chip_env, chip_dflt);
    if (!chip_is_safe(pin.chip))
    {
        pin.chip = chip_dflt;
    }
    pin.line = getenv_to_int(line_env, line_dflt);
}

static std::string format_pin(const GpioPin &pin)
{
    if (pin.chip.empty() || !is_gpio_line_valid(pin.line))
    {
        return "invalid";
    }
    return pin.chip + ":" + std::to_string(pin.line);
}

static std::string format_gpio_name(const GpioPin &pin)
{
    if (!is_gpio_line_valid(pin.line))
    {
        return "--";
    }
    return "G" + std::to_string(pin.line);
}

static std::string trim_text(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
    {
        text.pop_back();
    }

    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
    {
        start++;
    }

    return text.substr(start);
}

static std::string shorten_text(const std::string &text, size_t max_len = 28)
{
    if (text.size() <= max_len)
    {
        return text;
    }
    if (max_len < 4)
    {
        return text.substr(0, max_len);
    }
    return text.substr(0, max_len - 3) + "...";
}

static void log_pin_map()
{
    std::fprintf(stderr,
                 "[ExtBusLED] MAP P=%s(%s) R=%s(%s) L1=%s L2=%s BTN=%s\n",
                 format_pin(g_power_pin).c_str(),
                 g_power_state ? "HIGH" : "LOW",
                 format_pin(g_route_pin).c_str(),
                 g_route_state ? "HIGH" : "LOW",
                 format_pin(g_led1_pin).c_str(),
                 format_pin(g_led2_pin).c_str(),
                 format_pin(g_button_pin).c_str());
}

static void show_log(const char *text)
{
    std::string next = text ? text : "";
    if (next == g_last_log_text)
    {
        return;
    }

    g_last_log_text = next;
    std::fprintf(stderr, "[ExtBusLED] %s\n", next.c_str());

    if (g_log_label != nullptr)
    {
        lv_label_set_text(g_log_label, next.c_str());
    }
}

static void show_logf(const char *fmt, ...)
{
    std::array<char, 180> buf{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    va_end(args);
    show_log(buf.data());
}

static std::string chip_basename(const std::string &chip)
{
    if (chip.empty())
    {
        return "gpiochip0";
    }

    size_t slash = chip.find_last_of('/');
    if (slash == std::string::npos)
    {
        return chip;
    }
    return chip.substr(slash + 1);
}

static std::string extract_consumer_name(const std::string &line)
{
    size_t pos = line.find("consumer=");
    if (pos == std::string::npos)
    {
        return "";
    }

    pos += std::strlen("consumer=");
    if (pos >= line.size())
    {
        return "";
    }

    if (line[pos] == '"')
    {
        size_t end = line.find('"', pos + 1);
        if (end != std::string::npos)
        {
            return line.substr(pos + 1, end - pos - 1);
        }
    }

    size_t end = line.find(' ', pos);
    if (end == std::string::npos)
    {
        end = line.size();
    }
    return line.substr(pos, end - pos);
}

static bool gpio_line_has_consumer(const GpioPin &pin, std::string &consumer_name)
{
    consumer_name.clear();
    if (!is_gpio_line_valid(pin.line) || !chip_is_safe(pin.chip))
    {
        return false;
    }

    std::string cmd = "gpioinfo " + chip_basename(pin.chip) + " 2>/dev/null";
    FILE *fp = popen(cmd.c_str(), "r");
    if (fp == nullptr)
    {
        return false;
    }

    bool found = false;
    std::array<char, 256> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr)
    {
        std::string text = buf.data();
        int line_no = -1;
        if (std::sscanf(text.c_str(), " line %d:", &line_no) == 1 && line_no == pin.line)
        {
            consumer_name = extract_consumer_name(text);
            found = !consumer_name.empty();
            break;
        }
    }
    pclose(fp);
    return found;
}

static void append_split_assignment_args(std::vector<std::string> &args, const std::string &assignment)
{
    const char *eq = std::strchr(assignment.c_str(), '=');
    std::string line = assignment;
    std::string value = "0";
    if (eq != nullptr)
    {
        line.assign(assignment.c_str(), static_cast<size_t>(eq - assignment.c_str()));
        value = eq + 1;
    }

    args.push_back(line);
    args.push_back(value);
}

static void exec_gpioset_variant(GpioSetVariant variant, const char *chip_name, const std::vector<std::string> &assignments)
{
    std::vector<std::string> args;
    args.push_back("gpioset");

    switch (variant)
    {
    case GpioSetVariant::ChipOptionAssign:
        args.push_back("-c");
        args.push_back(chip_name);
        for (const auto &assignment : assignments)
        {
            args.push_back(assignment);
        }
        break;
    case GpioSetVariant::ChipOptionSplit:
        args.push_back("-c");
        args.push_back(chip_name);
        for (const auto &assignment : assignments)
        {
            append_split_assignment_args(args, assignment);
        }
        break;
    case GpioSetVariant::PositionalAssign:
        args.push_back(chip_name);
        for (const auto &assignment : assignments)
        {
            args.push_back(assignment);
        }
        break;
    case GpioSetVariant::PositionalSplit:
        args.push_back(chip_name);
        for (const auto &assignment : assignments)
        {
            append_split_assignment_args(args, assignment);
        }
        break;
    }

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (auto &arg : args)
    {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);

    execvp("gpioset", argv.data());
    std::fprintf(stderr, "exec gpioset failed: %s", std::strerror(errno));
    _exit(127);
}

static std::string make_gpioget_command(GpioGetVariant variant, const GpioPin &pin)
{
    const std::string chip_name = chip_basename(pin.chip);
    const std::string line = std::to_string(pin.line);
    switch (variant)
    {
    case GpioGetVariant::ChipOptionBiasLong:
        return "gpioget --bias pull-up -c " + chip_name + " " + line;
    case GpioGetVariant::ChipOptionBiasShort:
        return "gpioget -b pull-up -c " + chip_name + " " + line;
    case GpioGetVariant::ChipOptionBiasLegacy:
        return "gpioget -B pull-up -c " + chip_name + " " + line;
    case GpioGetVariant::ChipOptionBiasEq:
        return "gpioget --bias=pull-up -c " + chip_name + " " + line;
    case GpioGetVariant::ChipOption:
        return "gpioget -c " + chip_name + " " + line;
    case GpioGetVariant::PositionalBiasLong:
        return "gpioget --bias pull-up " + chip_name + " " + line;
    case GpioGetVariant::Positional:
    default:
        return "gpioget " + chip_name + " " + line;
    }
}

static bool try_start_held_chip_variant(pid_t &pid_out,
                                        std::string &err_text,
                                        const std::string &chip,
                                        const std::vector<std::string> &assignments,
                                        GpioSetVariant variant)
{
    pid_out = -1;
    err_text.clear();

    int err_pipe[2] = {-1, -1};
    if (pipe(err_pipe) != 0)
    {
        err_text = "pipe failed";
        return false;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(err_pipe[0]);
        close(err_pipe[1]);
        err_text = "fork failed";
        return false;
    }

    if (pid == 0)
    {
        close(err_pipe[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
        {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[1]);

        const std::string chip_name = chip_basename(chip);
        exec_gpioset_variant(variant, chip_name.c_str(), assignments);
    }

    close(err_pipe[1]);
    int flags = fcntl(err_pipe[0], F_GETFL, 0);
    if (flags >= 0)
    {
        fcntl(err_pipe[0], F_SETFL, flags | O_NONBLOCK);
    }

    usleep(60000);

    std::array<char, 160> buf{};
    ssize_t got = read(err_pipe[0], buf.data(), buf.size() - 1);
    if (got > 0)
    {
        err_text.assign(buf.data(), static_cast<size_t>(got));
        err_text = trim_text(err_text);
    }
    close(err_pipe[0]);

    int status = 0;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid)
    {
        return false;
    }

    pid_out = pid;
    return true;
}

static void stop_held_chip(HeldChip &hold)
{
    if (hold.pid <= 0)
    {
        return;
    }

    kill(hold.pid, SIGTERM);
    waitpid(hold.pid, nullptr, 0);
    hold.pid = -1;
    hold.chip.clear();
}

static void stop_all_output_holds()
{
    for (auto &hold : g_output_holds)
    {
        stop_held_chip(hold);
    }
}

static bool start_held_chip(HeldChip &hold, const std::string &chip, const std::vector<std::string> &assignments, const char *tag)
{
    if (assignments.empty() || !chip_is_safe(chip))
    {
        return false;
    }

    stop_held_chip(hold);

    std::string preferred_err;
    pid_t pid = -1;
    const std::array<GpioSetVariant, 4> variants = {
        g_gpioset_variant_locked ? g_gpioset_variant : GpioSetVariant::ChipOptionAssign,
        GpioSetVariant::ChipOptionSplit,
        GpioSetVariant::PositionalAssign,
        GpioSetVariant::PositionalSplit,
    };

    for (GpioSetVariant variant : variants)
    {
        if (g_gpioset_variant_locked && variant != g_gpioset_variant)
        {
            continue;
        }

        std::string err_text;
        if (try_start_held_chip_variant(pid, err_text, chip, assignments, variant))
        {
            hold.pid = pid;
            hold.chip = chip;
            g_gpioset_variant = variant;
            g_gpioset_variant_locked = true;
            return true;
        }

        if (preferred_err.empty())
        {
            preferred_err = err_text;
        }
        if (!err_text.empty() && err_text.find("invalid option") == std::string::npos)
        {
            preferred_err = err_text;
        }
    }

    show_logf("%s %s", tag, preferred_err.empty() ? "gpioset exited" : shorten_text(preferred_err).c_str());
    return false;
}

static bool apply_output_holds()
{
    stop_all_output_holds();
    g_led1_issue.clear();
    g_led2_issue.clear();
    g_led1_ready = false;
    g_led2_ready = false;

    size_t hold_idx = 0;
    auto start_single = [&](const GpioPin &pin, bool high, const char *tag, std::string *issue = nullptr) -> bool {
        if (!is_gpio_line_valid(pin.line) || !chip_is_safe(pin.chip))
        {
            return false;
        }

        std::string consumer_name;
        if (gpio_line_has_consumer(pin, consumer_name))
        {
            if (issue != nullptr)
            {
                *issue = consumer_name;
            }
            return false;
        }

        if (hold_idx >= g_output_holds.size())
        {
            return false;
        }

        std::vector<std::string> assignments = {
            std::to_string(pin.line) + "=" + (high ? "1" : "0")
        };
        bool ok = start_held_chip(g_output_holds[hold_idx], pin.chip, assignments, tag);
        hold_idx++;
        return ok;
    };

    bool power_ok = start_single(g_power_pin, g_power_state, "POWER open");
    bool route_ok = start_single(g_route_pin, g_route_state, "ROUTE open");
    g_led1_ready = start_single(g_led1_pin, g_led1_state, "LED1 open", &g_led1_issue);
    g_led2_ready = start_single(g_led2_pin, g_led2_state, "LED2 open", &g_led2_issue);

    return power_ok && route_ok;
}

static bool read_gpio_line(const GpioPin &pin, int &value, std::string *err_text = nullptr)
{
    value = 0;
    if (!is_gpio_line_valid(pin.line) || !chip_is_safe(pin.chip))
    {
        if (err_text != nullptr)
        {
            *err_text = "invalid pin";
        }
        return false;
    }

    std::string last_err = "read unavailable";
    const std::array<GpioGetVariant, 7> variants = {
        g_gpioget_variant_locked ? g_gpioget_variant
                                 : (g_button_pullup_enabled ? GpioGetVariant::ChipOptionBiasLong : GpioGetVariant::ChipOption),
        GpioGetVariant::ChipOptionBiasShort,
        GpioGetVariant::ChipOptionBiasLegacy,
        GpioGetVariant::ChipOptionBiasEq,
        GpioGetVariant::ChipOption,
        GpioGetVariant::PositionalBiasLong,
        GpioGetVariant::Positional,
    };

    for (GpioGetVariant variant : variants)
    {
        if (g_gpioget_variant_locked && variant != g_gpioget_variant)
        {
            continue;
        }
        if (!g_button_pullup_enabled &&
            (variant == GpioGetVariant::ChipOptionBiasLong || variant == GpioGetVariant::ChipOptionBiasShort ||
             variant == GpioGetVariant::ChipOptionBiasLegacy || variant == GpioGetVariant::ChipOptionBiasEq ||
             variant == GpioGetVariant::PositionalBiasLong))
        {
            continue;
        }

        std::string out;
        std::string cmd = make_gpioget_command(variant, pin);
        FILE *fp = popen((cmd + " 2>&1").c_str(), "r");
        if (fp == nullptr)
        {
            last_err = "popen failed";
            continue;
        }

        std::array<char, 64> buf{};
        while (fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr)
        {
            out += buf.data();
        }
        int status = pclose(fp);
        out = trim_text(out);
        if (status != 0)
        {
            last_err = out.empty() ? "read failed" : out;
            continue;
        }

        if (out.empty())
        {
            last_err = "empty read";
            continue;
        }

        char *end = nullptr;
        errno = 0;
        long raw = std::strtol(out.c_str(), &end, 10);
        if (end != out.c_str() && errno != ERANGE && raw >= 0 && raw <= 1)
        {
            value = static_cast<int>(raw);
            g_gpioget_variant = variant;
            g_gpioget_variant_locked = true;
            return true;
        }

        if (out == "active" || out == "\"active\"" || out.find("=active") != std::string::npos)
        {
            value = 1;
            g_gpioget_variant = variant;
            g_gpioget_variant_locked = true;
            return true;
        }

        if (out == "inactive" || out == "\"inactive\"" || out.find("=inactive") != std::string::npos)
        {
            value = 0;
            g_gpioget_variant = variant;
            g_gpioget_variant_locked = true;
            return true;
        }

        last_err = out;
    }

    if (err_text != nullptr)
    {
        *err_text = last_err;
    }
    return false;
}

static void set_label_text(lv_obj_t *label, const char *fmt, ...)
{
    if (label == nullptr)
    {
        return;
    }

    std::array<char, 180> buf{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf.data(), buf.size(), fmt, args);
    va_end(args);
    lv_label_set_text(label, buf.data());
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static lv_obj_t *create_state_dot(lv_obj_t *parent)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, kDotBaseSize, kDotBaseSize);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(0xEAF2FF), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(dot, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(dot, kDotPulseMinAlpha, 0);
    return dot;
}

static void style_panel(lv_obj_t *panel)
{
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0F1D2F), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2A415A), 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
}

static void dot_set_opacity_cb(void *obj, int32_t opa)
{
    lv_obj_t *dot = static_cast<lv_obj_t *>(obj);
    lv_obj_set_style_bg_opa(dot, static_cast<lv_opa_t>(opa), 0);
}

static void stop_dot_pulse(lv_obj_t *dot)
{
    if (dot == nullptr)
    {
        return;
    }
    lv_anim_del(dot, dot_set_opacity_cb);
}

static void start_dot_pulse(lv_obj_t *dot)
{
    if (dot == nullptr)
    {
        return;
    }

    stop_dot_pulse(dot);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, dot);
    lv_anim_set_values(&anim, kDotPulseMinAlpha, kDotPulseMaxAlpha);
    lv_anim_set_time(&anim, 700);
    lv_anim_set_playback_time(&anim, 700);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&anim, dot_set_opacity_cb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

static void set_level_text(lv_obj_t *label, bool high, bool ok)
{
    if (label == nullptr)
    {
        return;
    }

    if (!ok)
    {
        lv_label_set_text(label, "N/A");
        lv_obj_set_style_text_color(label, lv_color_hex(0xA2AFBD), 0);
        return;
    }

    lv_label_set_text(label, high ? "HIGH" : "LOW");
    lv_obj_set_style_text_color(label, lv_color_hex(high ? kHighColor : kLowColor), 0);
}

static void set_state_dot(lv_obj_t *dot, bool high, bool ok)
{
    if (dot == nullptr)
    {
        return;
    }

    lv_color_t color = ok ? lv_color_hex(high ? kHighColor : kLowColor) : lv_color_hex(0xA2AFBD);
    lv_obj_set_style_bg_color(dot, color, 0);
    if (ok && high)
    {
        start_dot_pulse(dot);
    }
    else
    {
        stop_dot_pulse(dot);
        lv_obj_set_style_bg_opa(dot, ok ? kDotPulseMinAlpha : LV_OPA_50, 0);
    }
}

static void set_value_text(lv_obj_t *label, const char *on_text, const char *off_text, bool high, bool ok)
{
    if (label == nullptr)
    {
        return;
    }

    if (!ok)
    {
        lv_label_set_text(label, "N/A");
        lv_obj_set_style_text_color(label, lv_color_hex(0xA2AFBD), 0);
        return;
    }

    lv_label_set_text(label, high ? on_text : off_text);
    lv_obj_set_style_text_color(label, lv_color_hex(high ? kHighColor : kLowColor), 0);
}

static bool set_led_state(int idx, bool on, const char *source)
{
    bool ok = false;
    if (idx == 1)
    {
        bool prev = g_led1_state;
        g_led1_state = on;
        ok = apply_output_holds();
        if (ok)
        {
            set_value_text(g_led1_level_label, "HIGH", "LOW", g_led1_state, true);
            set_state_dot(g_led1_dot, g_led1_state, true);
        }
        else
        {
            g_led1_state = prev;
            apply_output_holds();
        }
    }
    else if (idx == 2)
    {
        bool prev = g_led2_state;
        g_led2_state = on;
        ok = apply_output_holds();
        if (ok)
        {
            set_value_text(g_led2_level_label, "HIGH", "LOW", g_led2_state, true);
            set_state_dot(g_led2_dot, g_led2_state, true);
        }
        else
        {
            g_led2_state = prev;
            apply_output_holds();
        }
    }

    if (!ok && source != nullptr && std::strcmp(source, "AUTO") != 0)
    {
        show_logf("%s L%d %s",
                  source ? source : "SET",
                  idx,
                  on ? "HIGH" : "LOW");
    }

    return ok;
}

static void init_ext_bus_and_pins()
{
    set_pin(g_power_pin, "EXT_POWER_GPIOCHIP", "EXT_POWER_GPIO", "gpiochip0", kDefaultPowerLine);
    set_pin(g_route_pin, "EXT_ROUTE_GPIOCHIP", "EXT_ROUTE_GPIO", "gpiochip1", kDefaultRouteLine);
    set_pin(g_led1_pin, "EXT_LED1_GPIOCHIP", "EXT_LED1_GPIO", "gpiochip0", kDefaultLed1Line);
    set_pin(g_led2_pin, "EXT_LED2_GPIOCHIP", "EXT_LED2_GPIO", "gpiochip0", kDefaultLed2Line);
    set_pin(g_button_pin, "EXT_BUTTON_GPIOCHIP", "EXT_BUTTON_GPIO", "gpiochip0", kDefaultButtonLine);

    g_button_active_low = getenv_to_bool("EXT_BUTTON_ACTIVE_LOW", true);
    g_button_pullup_enabled = getenv_to_bool("EXT_BUTTON_PULLUP", true);
    g_power_state = getenv_to_bool("EXT_POWER_HIGH", true);
    g_route_state = getenv_to_bool("EXT_ROUTE_HIGH", false);
    g_auto_toggle_enabled = getenv_to_bool("EXT_AUTO_TOGGLE", true);
    g_led1_state = false;
    g_led2_state = false;
    log_pin_map();

    bool outputs_ok = apply_output_holds();
    g_button_ready = is_gpio_line_valid(g_button_pin.line) && chip_is_safe(g_button_pin.chip);

    if (!g_led1_issue.empty())
    {
        show_logf("L1 busy %s", shorten_text(g_led1_issue, 18).c_str());
    }
    else if (!g_led2_issue.empty())
    {
        show_logf("L2 busy %s", shorten_text(g_led2_issue, 18).c_str());
    }
    else if (outputs_ok && (g_led1_ready || g_led2_ready) && g_auto_toggle_enabled)
    {
        show_log("AUTO 1s");
    }
    else if (!g_auto_toggle_enabled && (g_led1_ready || g_led2_ready))
    {
        show_log("AUTO off");
    }

    if (!is_gpio_line_valid(g_led1_pin.line))
    {
        g_led1_pin.line = kDefaultLed1Line;
    }
    if (!is_gpio_line_valid(g_led2_pin.line))
    {
        g_led2_pin.line = kDefaultLed2Line;
    }
    if (!is_gpio_line_valid(g_button_pin.line))
    {
        g_button_pin.line = kDefaultButtonLine;
    }

    if (g_led1_pin.line == g_led2_pin.line)
    {
        g_led2_pin.line = kDefaultLed2Line;
    }

    if (g_led1_pin.line == g_button_pin.line)
    {
        g_button_pin.line = kDefaultButtonLine;
    }

    bool led1_ok = g_led1_ready;
    bool led2_ok = g_led2_ready;
    if (!led1_ok && !led2_ok)
    {
        show_log("INIT LED failed");
    }

    set_state_dot(g_led1_dot, g_led1_state, led1_ok);
    set_state_dot(g_led2_dot, g_led2_state, led2_ok);
}

static void refresh_output_labels()
{
    set_label_text(g_led1_pin_label, "LED %s", format_gpio_name(g_led1_pin).c_str());
    set_value_text(g_led1_level_label, "HIGH", "LOW", g_led1_state, g_led1_ready);
    set_value_text(g_led2_level_label, "HIGH", "LOW", g_led2_state, g_led2_ready);
}

static void update_button_state_labels(int raw, bool read_ok)
{
    if (!read_ok)
    {
        set_label_text(g_btn_pin_label, "INPUT %s", format_gpio_name(g_button_pin).c_str());
        set_value_text(g_btn_level_label, "HIGH", "LOW", false, false);
        set_state_dot(g_input_dot, false, false);
        return;
    }

    set_label_text(g_btn_pin_label, "INPUT %s", format_gpio_name(g_button_pin).c_str());

    bool high = raw != 0;
    set_state_dot(g_input_dot, high, true);

    set_value_text(g_btn_level_label, "HIGH", "LOW", high, true);
}

static void poll_gpio_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    int raw = 0;
    std::string err_text;
    bool ok = g_button_ready && read_gpio_line(g_button_pin, raw, &err_text);
    if (!ok)
    {
        if (!g_button_error_reported)
        {
            std::string detail = shorten_text(err_text.empty() ? "read unavailable" : err_text, 22);
            show_logf("IN %s", detail.c_str());
            g_button_error_reported = true;
        }
        update_button_state_labels(0, false);
        return;
    }

    g_button_error_reported = false;

    raw = (raw != 0) ? 1 : 0;

    if (g_last_button_sample < 0)
    {
        g_last_button_sample = raw;
        g_stable_button_sample = raw;
        g_button_sample_count = 1;
    }
    else if (raw == g_last_button_sample)
    {
        if (g_button_sample_count < kDebounceSamples)
        {
            g_button_sample_count++;
        }
    }
    else
    {
        g_last_button_sample = raw;
        g_button_sample_count = 1;
    }

    if (g_button_sample_count >= kDebounceSamples)
    {
        g_stable_button_sample = g_last_button_sample;
    }

    update_button_state_labels(g_stable_button_sample, true);
}

static void auto_toggle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_led1_ready && !g_led2_ready)
    {
        if (!g_auto_toggle_error_reported)
        {
            show_log("AUTO disabled");
            g_auto_toggle_error_reported = true;
        }
        return;
    }

    bool ok1 = g_led1_ready ? set_led_state(1, !g_led1_state, "AUTO") : false;
    bool ok2 = g_led2_ready ? set_led_state(2, !g_led2_state, "AUTO") : false;
    if ((g_led1_ready && !ok1) || (g_led2_ready && !ok2))
    {
        if (!g_auto_toggle_error_reported)
        {
            show_log("AUTO write failed");
            g_auto_toggle_error_reported = true;
        }
        return;
    }

    g_auto_toggle_error_reported = false;
    refresh_output_labels();
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

    init_ext_bus_and_pins();

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1624), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    const int panel_x = 6;
    const int panel_w = g_screen_w - (panel_x * 2);
    const int output_panel_y = 34;
    const int output_panel_h = 56;
    const int input_panel_y = 98;
    const int input_panel_h = 56;
    const int output_level_w = 126;
    const int output_level_x = panel_w - output_level_w - 14;
    const int input_level_w = 126;
    const int input_level_x = panel_w - input_level_w - 14;

    auto title = create_label(screen, "ExtBus GPIO", &lv_font_montserrat_12, lv_color_hex(0xF4F8FF));
    lv_obj_set_pos(title, 8, 5);

    g_status_label = create_label(screen, "", &lv_font_montserrat_12, lv_color_hex(0x9FD4FF));
    lv_obj_set_pos(g_status_label, 102, 5);
    lv_obj_set_width(g_status_label, g_screen_w - 110);
    lv_obj_set_style_text_align(g_status_label, LV_TEXT_ALIGN_RIGHT, 0);
    set_label_text(g_status_label, "LED%s IN%s", format_gpio_name(g_led1_pin).c_str(),
                   format_gpio_name(g_button_pin).c_str());

    g_log_label = create_label(screen, "", &lv_font_montserrat_12, lv_color_hex(0x95A5A6));
    lv_obj_set_pos(g_log_label, 8, 19);
    lv_obj_set_width(g_log_label, g_screen_w - 16);
    lv_label_set_long_mode(g_log_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(g_log_label, g_last_log_text.c_str());

    g_output_panel = lv_obj_create(screen);
    lv_obj_set_pos(g_output_panel, panel_x, output_panel_y);
    lv_obj_set_size(g_output_panel, panel_w, output_panel_h);
    style_panel(g_output_panel);

    g_led1_pin_label = create_label(g_output_panel, "", &lv_font_montserrat_16, lv_color_hex(0xD6DFEE));
    g_led1_level_label = create_label(g_output_panel, "", &lv_font_montserrat_24, lv_color_hex(0x5AE08D));

    g_led1_dot = create_state_dot(g_output_panel);

    lv_obj_set_width(g_led1_pin_label, 118);
    lv_obj_set_width(g_led1_level_label, output_level_w);
    lv_obj_set_style_text_align(g_led1_level_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_set_pos(g_led1_dot, 14, 14);
    lv_obj_set_pos(g_led1_pin_label, 52, 14);
    lv_obj_set_pos(g_led1_level_label, output_level_x, 8);

    g_input_panel = lv_obj_create(screen);
    lv_obj_set_pos(g_input_panel, panel_x, input_panel_y);
    lv_obj_set_size(g_input_panel, panel_w, input_panel_h);
    style_panel(g_input_panel);

    g_btn_pin_label = create_label(g_input_panel, "", &lv_font_montserrat_16, lv_color_hex(0x8AC5FF));
    g_btn_level_label = create_label(g_input_panel, "", &lv_font_montserrat_24, lv_color_hex(0x5AE08D));
    g_input_dot = create_state_dot(g_input_panel);

    lv_obj_set_width(g_btn_pin_label, 120);
    lv_obj_set_width(g_btn_level_label, input_level_w);
    lv_obj_set_style_text_align(g_btn_level_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_set_pos(g_input_dot, 14, 14);
    lv_obj_set_pos(g_btn_pin_label, 52, 14);
    lv_obj_set_pos(g_btn_level_label, input_level_x, 8);

    refresh_output_labels();
    update_button_state_labels(0, false);
    set_state_dot(g_led1_dot, g_led1_state, g_led1_ready);

    lv_timer_create(poll_gpio_timer_cb, kInputPollMs, nullptr);
    if (g_auto_toggle_enabled)
    {
        lv_timer_create(auto_toggle_timer_cb, kAutoToggleMs, nullptr);
    }

    g_group = nullptr;
}

lv_group_t *ui_get_input_group()
{
    return g_group;
}

void ui_shutdown()
{
    stop_all_output_holds();
}
