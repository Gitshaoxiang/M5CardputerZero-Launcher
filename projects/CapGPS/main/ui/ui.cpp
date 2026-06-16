#include "lvgl/lvgl.h"

#include "TinyGPS++.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <termios.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

static constexpr const char *kDefaultSerialDevice = "/dev/ttyS0";
static constexpr int kDefaultBaudRate = 115200;
static constexpr int kRefreshPeriodMs = 250;
static constexpr int kSerialPollTimeoutMs = 250;
static constexpr int kSerialReconnectDelayMs = 1200;
static constexpr int kActivityHeartbeatMs = 4000;
static constexpr const char *kExtPowerChip = "gpiochip0";
static constexpr int kExtPowerLine = 17;
static constexpr const char *kExtRouteChip = "gpiochip1";
static constexpr int kExtRouteLine = 0;
static constexpr bool kExtRouteHigh = true;
static constexpr lv_opa_t kOpaLightCard = (lv_opa_t)245;
static constexpr lv_opa_t kOpaMediumCard = (lv_opa_t)230;
static constexpr lv_opa_t kOpaChipFill = (lv_opa_t)64;
static constexpr size_t kLogTextMaxLen = 96;

struct GpsSnapshot {
    std::string serial_device = kDefaultSerialDevice;
    std::string status_title = "Booting GPS";
    std::string status_detail = "Opening /dev/ttyS0";
    std::string fix_badge = "INIT";
    std::string fix_mode = "No fix";
    std::string fix_quality = "Invalid";
    std::string utc_date = "--/--/----";
    std::string utc_time = "--:--:--";
    std::string last_sentence = "no NMEA yet";
    std::string activity_log = "LOG | Booting GPS reader";
    std::string error_message;
    int baud_rate = kDefaultBaudRate;
    int satellites_used = 0;
    int satellites_visible = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude_m = 0.0;
    double speed_kmh = 0.0;
    double hdop = 0.0;
    double course_deg = 0.0;
    uint64_t tx_bytes = 0;
    uint64_t rx_bytes = 0;
    uint32_t chars_processed = 0;
    uint32_t checksum_passed = 0;
    uint32_t checksum_failed = 0;
    uint64_t last_rx_ms = 0;
    uint64_t last_sentence_ms = 0;
    uint64_t last_fix_ms = 0;
    bool serial_ok = false;
    bool module_seen = false;
    bool nmea_seen = false;
    bool has_location = false;
    bool has_fix = false;
    bool date_valid = false;
    bool time_valid = false;
    bool hdop_valid = false;
    bool speed_valid = false;
    bool altitude_valid = false;
    bool course_valid = false;
};

static lv_group_t *g_group = nullptr;
static lv_timer_t *g_refresh_timer = nullptr;

static lv_obj_t *g_status_title = nullptr;
static lv_obj_t *g_status_detail = nullptr;
static lv_obj_t *g_status_chip = nullptr;
static lv_obj_t *g_status_chip_text = nullptr;
static lv_obj_t *g_sat_used_value = nullptr;
static lv_obj_t *g_sat_visible_value = nullptr;
static lv_obj_t *g_search_spinner = nullptr;
static lv_obj_t *g_lat_value = nullptr;
static lv_obj_t *g_lng_value = nullptr;
static lv_obj_t *g_date_value = nullptr;
static lv_obj_t *g_fix_value = nullptr;
static lv_obj_t *g_footer = nullptr;

static std::mutex g_snapshot_mutex;
static std::thread g_reader_thread;
static std::atomic<bool> g_reader_running{false};
static GpsSnapshot g_snapshot;

struct HeldLine {
    pid_t pid = -1;
    std::string chip;
    int line = -1;
    bool value = false;
};

static HeldLine g_ext_power_hold;
static HeldLine g_ext_route_hold;

static void ensure_group()
{
    if (g_group == nullptr) {
        g_group = lv_group_create();
    }
}

static uint64_t monotonic_ms()
{
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
}

static int getenv_default_int(const char *name, int dflt)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return dflt;
    }

    char *end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || parsed <= 0 || parsed > 4000000) {
        return dflt;
    }
    return (int)parsed;
}

static bool getenv_default_bool(const char *name, bool dflt)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return dflt;
    }

    if (std::strcmp(value, "1") == 0 || value[0] == 'y' || value[0] == 'Y' || value[0] == 't' || value[0] == 'T') {
        return true;
    }
    if (std::strcmp(value, "0") == 0 || value[0] == 'n' || value[0] == 'N' || value[0] == 'f' || value[0] == 'F') {
        return false;
    }
    return dflt;
}

static std::string trim_copy(const std::string &text)
{
    size_t start = 0;
    while (start < text.size() && (text[start] == '\r' || text[start] == '\n' || text[start] == ' ' || text[start] == '\t')) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && (text[end - 1] == '\r' || text[end - 1] == '\n' || text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }

    return text.substr(start, end - start);
}

static std::vector<std::string> split_csv(std::string sentence)
{
    std::vector<std::string> fields;
    const size_t checksum_pos = sentence.find('*');
    if (checksum_pos != std::string::npos) {
        sentence.erase(checksum_pos);
    }

    size_t start = 0;
    while (start <= sentence.size()) {
        const size_t pos = sentence.find(',', start);
        if (pos == std::string::npos) {
            fields.push_back(sentence.substr(start));
            break;
        }
        fields.push_back(sentence.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

static std::string suffix_sentence_name(const std::string &sentence)
{
    if (sentence.size() < 6 || sentence[0] != '$') {
        return "";
    }
    return sentence.substr(sentence.size() >= 6 ? 3 : 1, 3);
}

static std::string talker_id(const std::string &sentence)
{
    if (sentence.size() < 6 || sentence[0] != '$') {
        return "";
    }
    return sentence.substr(1, 2);
}

static int parse_positive_int(const std::string &text, int fallback = 0)
{
    if (text.empty()) {
        return fallback;
    }
    char *end = nullptr;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || parsed < 0 || parsed > 10000) {
        return fallback;
    }
    return (int)parsed;
}

static std::string trunc_text(const std::string &text, size_t max_len)
{
    if (text.size() <= max_len) {
        return text;
    }
    if (max_len <= 3) {
        return text.substr(0, max_len);
    }
    return text.substr(0, max_len - 3) + "...";
}

static std::string describe_bytes(const char *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return "hex:<empty> ascii:<empty>";
    }

    std::string hex;
    std::string ascii;
    hex.reserve(len * 3);
    ascii.reserve(len * 2);

    char buf[4];
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)data[i];
        if (!hex.empty()) {
            hex.push_back(' ');
        }
        std::snprintf(buf, sizeof(buf), "%02X", c);
        hex += buf;

        switch (c) {
        case '\r':
            ascii += "\\r";
            break;
        case '\n':
            ascii += "\\n";
            break;
        case '\t':
            ascii += "\\t";
            break;
        default:
            ascii.push_back((c >= 32 && c <= 126) ? (char)c : '.');
            break;
        }
    }

    return "hex:" + hex + " ascii:" + ascii;
}

static std::string elapsed_tag(uint64_t now_ms)
{
    char buf[24];
    const uint64_t sec = now_ms / 1000ULL;
    const uint64_t tenth = (now_ms % 1000ULL) / 100ULL;
    std::snprintf(buf, sizeof(buf), "T+%llu.%llus",
                  (unsigned long long)sec,
                  (unsigned long long)tenth);
    return buf;
}

static std::string nmea_with_checksum(const std::string &body)
{
    unsigned char checksum = 0;
    for (unsigned char c : body) {
        checksum ^= c;
    }

    char buf[8];
    std::snprintf(buf, sizeof(buf), "*%02X\r\n", checksum);
    return "$" + body + buf;
}

static std::string trim_text_copy(std::string text)
{
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }

    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t')) {
        ++start;
    }

    return text.substr(start);
}

static void push_log(GpsSnapshot *snapshot, const std::string &message)
{
    if (snapshot == nullptr || message.empty()) {
        return;
    }

    const uint64_t now_ms = monotonic_ms();
    const std::string log_line = "LOG | " + elapsed_tag(now_ms) + " | " + trunc_text(message, kLogTextMaxLen);
    snapshot->activity_log = log_line;

    std::fprintf(stderr, "[CapGPS][%s] %s\n", elapsed_tag(now_ms).c_str(), message.c_str());
    std::fflush(stderr);
}

static bool try_start_held_line(HeldLine *hold, const char *chip, int line, bool value, std::string *error)
{
    if (hold == nullptr || chip == nullptr || chip[0] == '\0' || line < 0) {
        if (error) *error = "invalid gpio request";
        return false;
    }

    if (hold->pid > 0 && hold->chip == chip && hold->line == line && hold->value == value) {
        return true;
    }

    if (hold->pid > 0) {
        kill(hold->pid, SIGTERM);
        waitpid(hold->pid, nullptr, 0);
        hold->pid = -1;
        hold->chip.clear();
        hold->line = -1;
        hold->value = false;
    }

    int err_pipe[2] = {-1, -1};
    if (pipe(err_pipe) != 0) {
        if (error) *error = "pipe failed";
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        if (error) *error = "fork failed";
        return false;
    }

    if (pid == 0) {
        close(err_pipe[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[1]);

        const std::string assignment = std::to_string(line) + "=" + (value ? "1" : "0");
        char *const argv[] = {
            const_cast<char *>("gpioset"),
            const_cast<char *>("-c"),
            const_cast<char *>(chip),
            const_cast<char *>(assignment.c_str()),
            nullptr,
        };
        execvp("gpioset", argv);
        std::fprintf(stderr, "exec gpioset failed: %s\n", std::strerror(errno));
        _exit(127);
    }

    close(err_pipe[1]);
    int flags = fcntl(err_pipe[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(err_pipe[0], F_SETFL, flags | O_NONBLOCK);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    std::array<char, 160> buf{};
    ssize_t got = read(err_pipe[0], buf.data(), buf.size() - 1);
    close(err_pipe[0]);

    int status = 0;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
        if (error) {
            if (got > 0) {
                *error = trim_text_copy(std::string(buf.data(), (size_t)got));
            } else {
                *error = "gpioset exited early";
            }
        }
        return false;
    }

    hold->pid = pid;
    hold->chip = chip;
    hold->line = line;
    hold->value = value;
    if (error) {
        error->clear();
    }
    return true;
}

static void stop_held_line(HeldLine *hold)
{
    if (hold == nullptr || hold->pid <= 0) {
        return;
    }

    kill(hold->pid, SIGTERM);
    waitpid(hold->pid, nullptr, 0);
    hold->pid = -1;
    hold->chip.clear();
    hold->line = -1;
    hold->value = false;
}

static void ensure_ext_uart_path(GpsSnapshot *snapshot)
{
    if (snapshot == nullptr) {
        return;
    }

    std::string error;
    if (try_start_held_line(&g_ext_power_hold, kExtPowerChip, kExtPowerLine, true, &error)) {
        push_log(snapshot, std::string("EXT power hold ok: ") + kExtPowerChip + "[" + std::to_string(kExtPowerLine) + "]=1");
    } else {
        push_log(snapshot, std::string("EXT power hold failed: ") + error);
    }

    if (try_start_held_line(&g_ext_route_hold, kExtRouteChip, kExtRouteLine, kExtRouteHigh, &error)) {
        push_log(snapshot, std::string("EXT route hold ok: ") + kExtRouteChip + "[" + std::to_string(kExtRouteLine) +
                           "]=" + (kExtRouteHigh ? "1" : "0"));
    } else {
        push_log(snapshot, std::string("EXT route hold failed: ") + error);
    }
}

static std::string format_lat(double latitude, bool valid)
{
    if (!valid) {
        return "LAT --.------";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "LAT %c %.6f", latitude >= 0.0 ? 'N' : 'S', std::fabs(latitude));
    return buf;
}

static std::string format_lng(double longitude, bool valid)
{
    if (!valid) {
        return "LNG --.------";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "LNG %c %.6f", longitude >= 0.0 ? 'E' : 'W', std::fabs(longitude));
    return buf;
}

static std::string format_coordinate_compact(double value, bool valid, char positive, char negative)
{
    if (!valid) {
        return "--.------";
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%c %.6f", value >= 0.0 ? positive : negative, std::fabs(value));
    return buf;
}

static std::string format_metric_value(double value, bool valid, const char *unit, int decimals)
{
    if (!valid) {
        return "--";
    }
    char fmt[16];
    char buf[64];
    std::snprintf(fmt, sizeof(fmt), "%%.%df%%s", decimals);
    std::snprintf(buf, sizeof(buf), fmt, value, unit);
    return buf;
}

static std::string compact_connection_summary(const GpsSnapshot &snapshot)
{
    if (!snapshot.serial_ok) {
        return "LINK offline  |  TX --  |  RX --";
    }

    std::string summary = std::string("LINK ok  |  TX ");
    summary += std::to_string(snapshot.tx_bytes);
    summary += "  |  RX ";
    summary += snapshot.module_seen ? "yes" : "no";
    summary += snapshot.nmea_seen ? "/NMEA" : "/raw";

    if (snapshot.nmea_seen && (snapshot.date_valid || snapshot.time_valid)) {
        summary += "  |  UTC ";
        summary += snapshot.utc_time;
    } else if (snapshot.nmea_seen) {
        summary += "  |  SAT ";
        summary += std::to_string(snapshot.satellites_visible);
        summary += " vis";
    }

    return summary;
}

static std::string activity_heartbeat_text(const GpsSnapshot &snapshot)
{
    if (!snapshot.module_seen) {
        return "Waiting for module data";
    }

    if (!snapshot.nmea_seen) {
        return "Module active | RX " + std::to_string(snapshot.rx_bytes) + "B | parsing";
    }

    if (snapshot.has_fix) {
        std::string text = "Fix active | SAT ";
        text += std::to_string(snapshot.satellites_used);
        text += "/";
        text += std::to_string(snapshot.satellites_visible);
        if (snapshot.time_valid) {
            text += " | UTC ";
            text += snapshot.utc_time;
        }
        return text;
    }

    std::string text = "Searching | SAT ";
    text += std::to_string(snapshot.satellites_used);
    text += "/";
    text += std::to_string(snapshot.satellites_visible);
    text += " | RX ";
    text += std::to_string(snapshot.rx_bytes);
    text += "B";
    return text;
}

static std::string quality_to_string(TinyGPSLocation::Quality quality)
{
    switch (quality) {
    case TinyGPSLocation::GPS: return "GPS";
    case TinyGPSLocation::DGPS: return "DGPS";
    case TinyGPSLocation::PPS: return "PPS";
    case TinyGPSLocation::RTK: return "RTK";
    case TinyGPSLocation::FloatRTK: return "Float RTK";
    case TinyGPSLocation::Estimated: return "Estimated";
    case TinyGPSLocation::Manual: return "Manual";
    case TinyGPSLocation::Simulated: return "Simulated";
    case TinyGPSLocation::Invalid:
    default: return "Invalid";
    }
}

static std::string mode_to_string(TinyGPSLocation::Mode mode)
{
    switch (mode) {
    case TinyGPSLocation::A: return "Autonomous";
    case TinyGPSLocation::D: return "Differential";
    case TinyGPSLocation::E: return "Estimated";
    case TinyGPSLocation::N:
    default: return "No fix";
    }
}

static speed_t baud_to_termios(int baud_rate)
{
    switch (baud_rate) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default: return 0;
    }
}

static bool configure_serial(int fd, int baud_rate, std::string *error)
{
    struct termios tty;
    std::memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        if (error) *error = "tcgetattr failed";
        return false;
    }

    const speed_t speed = baud_to_termios(baud_rate);
    if (speed == 0) {
        if (error) *error = "unsupported baud";
        return false;
    }

    cfmakeraw(&tty);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        if (error) *error = "tcsetattr failed";
        return false;
    }

    return true;
}

static bool write_all_nonblocking(int fd, const uint8_t *data, size_t len, std::string *error)
{
    if (data == nullptr || len == 0) {
        return true;
    }

    size_t offset = 0;
    while (offset < len) {
        const ssize_t written = write(fd, data + offset, len - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            std::memset(&pfd, 0, sizeof(pfd));
            pfd.fd = fd;
            pfd.events = POLLOUT;
            if (poll(&pfd, 1, 250) > 0) {
                continue;
            }
            if (error) *error = "write poll timeout";
            return false;
        }
        if (error) *error = std::strerror(errno);
        return false;
    }

    return true;
}

static void send_probe_sequence(int fd, GpsSnapshot *snapshot)
{
    if (snapshot == nullptr) {
        return;
    }

    const std::string pmtk_query = nmea_with_checksum("PMTK605");
    const std::array<uint8_t, 8> ubx_mon_ver = {0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34};
    const std::array<uint8_t, 2> wake_bytes = {'\r', '\n'};

    struct ProbeFrame {
        const char *name;
        const uint8_t *data;
        size_t len;
    };

    const ProbeFrame frames[] = {
        {"wake-crlf", wake_bytes.data(), wake_bytes.size()},
        {"pmtk-fw-query", reinterpret_cast<const uint8_t *>(pmtk_query.data()), pmtk_query.size()},
        {"ubx-mon-ver", ubx_mon_ver.data(), ubx_mon_ver.size()},
    };

    for (const ProbeFrame &frame : frames) {
        std::string write_error;
        if (write_all_nonblocking(fd, frame.data, frame.len, &write_error)) {
            snapshot->tx_bytes += frame.len;
            push_log(snapshot, std::string("Probe TX ok: ") + frame.name + " (" + std::to_string(frame.len) + " bytes)");
        } else {
            push_log(snapshot, std::string("Probe TX failed: ") + frame.name + " | " + write_error);
        }
        tcdrain(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

static void publish_snapshot(const GpsSnapshot &snapshot)
{
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    g_snapshot = snapshot;
}

static GpsSnapshot read_snapshot()
{
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    return g_snapshot;
}

static int recompute_visible_satellites(const std::unordered_map<std::string, int> &gsv_counts)
{
    auto combined = gsv_counts.find("GN");
    if (combined != gsv_counts.end()) {
        return combined->second;
    }

    int total = 0;
    for (const auto &entry : gsv_counts) {
        total += entry.second;
    }
    return total;
}

static void process_nmea_sentence(const std::string &line, GpsSnapshot *snapshot,
                                  std::unordered_map<std::string, int> *gsv_counts)
{
    if (snapshot == nullptr || gsv_counts == nullptr) {
        return;
    }

    const std::string trimmed = trim_copy(line);
    if (trimmed.empty()) {
        return;
    }

    snapshot->last_sentence = trunc_text(trimmed, 72);
    snapshot->last_sentence_ms = monotonic_ms();
    snapshot->module_seen = true;
    snapshot->nmea_seen = true;

    if (suffix_sentence_name(trimmed) == "GSV") {
        const auto fields = split_csv(trimmed);
        if (fields.size() > 3) {
            (*gsv_counts)[talker_id(trimmed)] = parse_positive_int(fields[3], 0);
            snapshot->satellites_visible = recompute_visible_satellites(*gsv_counts);
        }
    }
}

static void refresh_parser_state(TinyGPSPlus *gps, GpsSnapshot *snapshot)
{
    if (gps == nullptr || snapshot == nullptr) {
        return;
    }

    snapshot->chars_processed = gps->charsProcessed();
    snapshot->checksum_passed = gps->passedChecksum();
    snapshot->checksum_failed = gps->failedChecksum();

    snapshot->has_location = gps->location.isValid();
    snapshot->has_fix = gps->location.isValid();
    if (snapshot->has_location) {
        snapshot->latitude = gps->location.lat();
        snapshot->longitude = gps->location.lng();
        snapshot->last_fix_ms = monotonic_ms();
    }

    snapshot->fix_quality = quality_to_string(gps->location.FixQuality());
    snapshot->fix_mode = mode_to_string(gps->location.FixMode());

    snapshot->satellites_used = gps->satellites.isValid() ? (int)gps->satellites.value() : 0;
    snapshot->altitude_valid = gps->altitude.isValid();
    snapshot->speed_valid = gps->speed.isValid();
    snapshot->hdop_valid = gps->hdop.isValid();
    snapshot->course_valid = gps->course.isValid();
    snapshot->altitude_m = snapshot->altitude_valid ? gps->altitude.meters() : 0.0;
    snapshot->speed_kmh = snapshot->speed_valid ? gps->speed.kmph() : 0.0;
    snapshot->hdop = snapshot->hdop_valid ? gps->hdop.hdop() : 0.0;
    snapshot->course_deg = snapshot->course_valid ? gps->course.deg() : 0.0;

    snapshot->date_valid = gps->date.isValid();
    snapshot->time_valid = gps->time.isValid();
    if (snapshot->date_valid) {
        char date_buf[32];
        std::snprintf(date_buf, sizeof(date_buf), "%02u/%02u/%04u",
                      gps->date.month(), gps->date.day(), gps->date.year());
        snapshot->utc_date = date_buf;
    }
    if (snapshot->time_valid) {
        char time_buf[32];
        std::snprintf(time_buf, sizeof(time_buf), "%02u:%02u:%02u",
                      gps->time.hour(), gps->time.minute(), gps->time.second());
        snapshot->utc_time = time_buf;
    }
}

static void update_status_text(GpsSnapshot *snapshot)
{
    if (snapshot == nullptr) {
        return;
    }

    const uint64_t now = monotonic_ms();
    if (!snapshot->serial_ok) {
        snapshot->status_title = "Serial offline";
        if (!snapshot->error_message.empty()) {
            snapshot->status_detail = snapshot->error_message;
        } else {
            snapshot->status_detail = std::string("Unable to open ") + snapshot->serial_device;
        }
        snapshot->fix_badge = "ERROR";
        return;
    }

    if (!snapshot->module_seen) {
        snapshot->status_title = "UART linked";
        snapshot->status_detail = "UART ready, no bytes from module yet";
        snapshot->fix_badge = "LINK";
        return;
    }

    if (!snapshot->nmea_seen) {
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "RX %llu bytes | waiting for valid NMEA @ %dbps",
                      (unsigned long long)snapshot->rx_bytes,
                      snapshot->baud_rate);
        snapshot->status_title = "Module responding";
        snapshot->status_detail = detail;
        snapshot->fix_badge = "RX";
        return;
    }

    if (snapshot->has_fix) {
        char detail[160];
        const uint64_t age_sec = (now > snapshot->last_fix_ms) ? ((now - snapshot->last_fix_ms) / 1000ULL) : 0ULL;
        std::snprintf(detail, sizeof(detail),
                      "%s fix | %d used / %d visible | HDOP %s",
                      snapshot->fix_mode.c_str(),
                      snapshot->satellites_used,
                      snapshot->satellites_visible,
                      snapshot->hdop_valid ? format_metric_value(snapshot->hdop, true, "", 1).c_str() : "--");
        snapshot->status_title = (age_sec <= 2) ? "Locked on position" : "Position held";
        snapshot->status_detail = detail;
        snapshot->fix_badge = "FIX";
        return;
    }

    if (snapshot->chars_processed > 0) {
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "Searching sky | %d used / %d visible | %s",
                      snapshot->satellites_used,
                      snapshot->satellites_visible,
                      snapshot->serial_device.c_str());
        snapshot->status_title = "Searching satellites";
        snapshot->status_detail = detail;
        snapshot->fix_badge = "SCAN";
        return;
    }

    snapshot->status_title = "Waiting for NMEA";
    snapshot->status_detail = std::string("Listening on ") + snapshot->serial_device;
    snapshot->fix_badge = "PORT";
}

static void gps_reader_loop()
{
    const std::string serial_device = kDefaultSerialDevice;
    const int baud_rate = getenv_default_int("CAPGPS_BAUD", kDefaultBaudRate);
    const bool verbose_rx_logging = getenv_default_bool("CAPGPS_DEBUG_RX", false);

    TinyGPSPlus gps;
    GpsSnapshot snapshot;
    snapshot.serial_device = serial_device;
    snapshot.baud_rate = baud_rate;
    snapshot.status_title = "Booting GPS";
    snapshot.status_detail = std::string("Opening ") + serial_device;
    push_log(&snapshot, "Boot sequence started on " + serial_device + " @ " + std::to_string(baud_rate) + "bps");
    ensure_ext_uart_path(&snapshot);
    publish_snapshot(snapshot);

    std::unordered_map<std::string, int> gsv_counts;
    std::string line_buffer;
    bool nmea_logged = false;
    bool last_fix_state = false;
    std::string last_error_message;
    uint64_t last_silence_log_ms = 0;
    uint64_t last_activity_log_ms = 0;

    while (g_reader_running.load()) {
        int fd = open(serial_device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            snapshot.serial_ok = false;
            snapshot.error_message = std::string("open failed: ") + std::strerror(errno);
            if (snapshot.error_message != last_error_message) {
                push_log(&snapshot, "Serial open failed: " + snapshot.error_message);
                last_error_message = snapshot.error_message;
            }
            update_status_text(&snapshot);
            publish_snapshot(snapshot);
            for (int i = 0; i < kSerialReconnectDelayMs / 50 && g_reader_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        std::string serial_error;
        if (!configure_serial(fd, baud_rate, &serial_error)) {
            snapshot.serial_ok = false;
            snapshot.error_message = serial_error;
            if (snapshot.error_message != last_error_message) {
                push_log(&snapshot, "Serial setup failed: " + snapshot.error_message);
                last_error_message = snapshot.error_message;
            }
            update_status_text(&snapshot);
            publish_snapshot(snapshot);
            close(fd);
            for (int i = 0; i < kSerialReconnectDelayMs / 50 && g_reader_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        gps = TinyGPSPlus();
        gsv_counts.clear();
        line_buffer.clear();
        snapshot.has_location = false;
        snapshot.has_fix = false;
        snapshot.module_seen = false;
        snapshot.nmea_seen = false;
        snapshot.date_valid = false;
        snapshot.time_valid = false;
        snapshot.hdop_valid = false;
        snapshot.speed_valid = false;
        snapshot.altitude_valid = false;
        snapshot.course_valid = false;
        snapshot.satellites_used = 0;
        snapshot.satellites_visible = 0;
        snapshot.tx_bytes = 0;
        snapshot.rx_bytes = 0;
        snapshot.chars_processed = 0;
        snapshot.checksum_passed = 0;
        snapshot.checksum_failed = 0;
        snapshot.last_rx_ms = 0;
        snapshot.last_sentence = "waiting for NMEA";
        snapshot.serial_ok = true;
        snapshot.error_message.clear();
        snapshot.status_title = "GPS online";
        snapshot.status_detail = std::string("Streaming from ") + serial_device;
        push_log(&snapshot, "UART link established, starting active probe");
        send_probe_sequence(fd, &snapshot);
        last_error_message.clear();
        nmea_logged = false;
        last_fix_state = false;
        last_silence_log_ms = 0;
        last_activity_log_ms = 0;
        update_status_text(&snapshot);
        publish_snapshot(snapshot);

        while (g_reader_running.load()) {
            struct pollfd pfd;
            std::memset(&pfd, 0, sizeof(pfd));
            pfd.fd = fd;
            pfd.events = POLLIN;
            const int poll_result = poll(&pfd, 1, kSerialPollTimeoutMs);
            if (poll_result < 0) {
                snapshot.serial_ok = false;
                snapshot.error_message = std::string("poll failed: ") + std::strerror(errno);
                if (snapshot.error_message != last_error_message) {
                    push_log(&snapshot, "Serial poll failed: " + snapshot.error_message);
                    last_error_message = snapshot.error_message;
                }
                break;
            }

            if (poll_result == 0) {
                const uint64_t now_ms = monotonic_ms();
                if (!snapshot.module_seen) {
                    if (now_ms >= 3000 && now_ms - last_silence_log_ms >= 5000) {
                        push_log(&snapshot, "No UART data received from module yet");
                        last_silence_log_ms = now_ms;
                    }
                } else if (now_ms - last_activity_log_ms >= kActivityHeartbeatMs) {
                    push_log(&snapshot, activity_heartbeat_text(snapshot));
                    last_activity_log_ms = now_ms;
                }
                update_status_text(&snapshot);
                publish_snapshot(snapshot);
                continue;
            }

            if ((pfd.revents & POLLIN) == 0) {
                continue;
            }

            char buffer[256];
            const ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
            if (bytes_read < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                snapshot.serial_ok = false;
                snapshot.error_message = std::string("read failed: ") + std::strerror(errno);
                if (snapshot.error_message != last_error_message) {
                    push_log(&snapshot, "Serial read failed: " + snapshot.error_message);
                    last_error_message = snapshot.error_message;
                }
                break;
            }

            if (bytes_read == 0) {
                continue;
            }

            snapshot.rx_bytes += (uint64_t)bytes_read;
            snapshot.last_rx_ms = monotonic_ms();
            if (!snapshot.module_seen) {
                snapshot.module_seen = true;
                push_log(&snapshot, "Module data stream detected");
                last_activity_log_ms = snapshot.last_rx_ms;
            }
            if (verbose_rx_logging) {
                push_log(&snapshot,
                         "RX chunk: " + std::to_string((int)bytes_read) +
                         " bytes | " + describe_bytes(buffer, (size_t)bytes_read));
            }

            for (ssize_t i = 0; i < bytes_read; ++i) {
                const char c = buffer[i];
                gps.encode(c);
                if (c == '\r') {
                    continue;
                }

                line_buffer.push_back(c);
                if (c == '\n') {
                    process_nmea_sentence(line_buffer, &snapshot, &gsv_counts);
                    line_buffer.clear();
                } else if (line_buffer.size() > 256) {
                    line_buffer.clear();
                }
            }

            refresh_parser_state(&gps, &snapshot);

            if (!nmea_logged && snapshot.nmea_seen) {
                push_log(&snapshot, "NMEA detected, searching satellites");
                nmea_logged = true;
                last_activity_log_ms = monotonic_ms();
            }
            if (!last_fix_state && snapshot.has_fix) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "Position fix acquired: %.6f, %.6f",
                              snapshot.latitude, snapshot.longitude);
                push_log(&snapshot, buf);
                last_activity_log_ms = monotonic_ms();
            } else if (last_fix_state && !snapshot.has_fix) {
                push_log(&snapshot, "Fix lost, returning to satellite search");
                last_activity_log_ms = monotonic_ms();
            }
            last_fix_state = snapshot.has_fix;
            update_status_text(&snapshot);
            publish_snapshot(snapshot);
        }

        close(fd);
        if (!g_reader_running.load()) {
            break;
        }

        snapshot.serial_ok = false;
        push_log(&snapshot, "Serial stream disconnected, retrying");
        update_status_text(&snapshot);
        publish_snapshot(snapshot);

        for (int i = 0; i < kSerialReconnectDelayMs / 50 && g_reader_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

static lv_obj_t *create_card(lv_obj_t *parent, int x, int y, int w, int h,
                             lv_color_t bg0, lv_color_t bg1, lv_opa_t opa, int radius)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_bg_color(card, bg0, 0);
    lv_obj_set_style_bg_grad_color(card, bg1, 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(card, opa, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 18, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x050A12), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_color_t state_color(const GpsSnapshot &snapshot)
{
    if (!snapshot.serial_ok) {
        return lv_color_hex(0xEF4444);
    }
    if (snapshot.has_fix) {
        return lv_color_hex(0x19C37D);
    }
    return lv_color_hex(0xF59E0B);
}

static std::string compact_fix_summary(const GpsSnapshot &snapshot)
{
    std::string text = snapshot.fix_quality + " / " + snapshot.fix_mode;
    if (snapshot.time_valid) {
        text += " / UTC ";
        text += snapshot.utc_time;
    } else if (snapshot.hdop_valid) {
        text += " / HDOP ";
        text += format_metric_value(snapshot.hdop, true, "", 1);
    }
    return trunc_text(text, 42);
}

static std::string compact_meta_summary(const GpsSnapshot &snapshot)
{
    if (!snapshot.serial_ok) {
        return "UART offline";
    }

    if (snapshot.has_fix) {
        std::string text = "SAT ";
        text += std::to_string(snapshot.satellites_used);
        text += "/";
        text += std::to_string(snapshot.satellites_visible);
        if (snapshot.hdop_valid) {
            text += " | HDOP ";
            text += format_metric_value(snapshot.hdop, true, "", 1);
        }
        if (snapshot.time_valid) {
            text += " | UTC ";
            text += snapshot.utc_time;
        }
        return trunc_text(text, 44);
    }

    if (snapshot.nmea_seen) {
        std::string text = "SEARCH | SAT ";
        text += std::to_string(snapshot.satellites_used);
        text += "/";
        text += std::to_string(snapshot.satellites_visible);
        text += " | RX ";
        text += std::to_string(snapshot.rx_bytes);
        text += "B";
        return trunc_text(text, 44);
    }

    if (snapshot.module_seen) {
        return trunc_text("RX " + std::to_string(snapshot.rx_bytes) + "B | waiting for NMEA", 44);
    }

    return trunc_text(compact_connection_summary(snapshot), 44);
}

static std::string compact_bottom_summary(const GpsSnapshot &snapshot)
{
    std::string text = compact_fix_summary(snapshot);
    const std::string meta = compact_meta_summary(snapshot);
    if (!meta.empty()) {
        text += " | ";
        text += meta;
    }
    return trunc_text(text, 72);
}

static void refresh_ui(lv_timer_t *)
{
    const GpsSnapshot snapshot = read_snapshot();
    const lv_color_t accent = state_color(snapshot);
    const bool show_spinner = snapshot.serial_ok && !snapshot.has_fix;

    lv_label_set_text(g_status_title, snapshot.status_title.c_str());
    lv_label_set_text(g_status_detail, snapshot.status_detail.c_str());
    lv_label_set_text(g_status_chip_text, snapshot.fix_badge.c_str());
    lv_obj_set_style_bg_color(g_status_chip, accent, 0);
    lv_obj_set_style_bg_opa(g_status_chip, kOpaChipFill, 0);
    lv_obj_set_style_border_color(g_status_chip, accent, 0);
    lv_obj_set_style_border_width(g_status_chip, 1, 0);
    lv_obj_set_style_text_color(g_status_chip_text, accent, 0);

    char sat_buf[32];
    std::snprintf(sat_buf, sizeof(sat_buf), "USED %02d", snapshot.satellites_used);
    lv_label_set_text(g_sat_used_value, sat_buf);
    std::snprintf(sat_buf, sizeof(sat_buf), "VIS %02d", snapshot.satellites_visible);
    lv_label_set_text(g_sat_visible_value, sat_buf);
    if (show_spinner) {
        lv_obj_clear_flag(g_search_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_search_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_arc_color(g_search_spinner, accent, LV_PART_INDICATOR);

    lv_label_set_text(g_lat_value, format_coordinate_compact(snapshot.latitude, snapshot.has_location, 'N', 'S').c_str());
    lv_label_set_text(g_lng_value, format_coordinate_compact(snapshot.longitude, snapshot.has_location, 'E', 'W').c_str());

    lv_label_set_text(g_date_value, "");
    lv_label_set_text(g_fix_value, compact_bottom_summary(snapshot).c_str());
    lv_obj_set_style_text_color(g_fix_value, accent, 0);

    lv_label_set_text(g_footer, snapshot.activity_log.c_str());
    lv_obj_set_style_text_color(g_footer, accent, 0);
}

static void start_reader_thread()
{
    if (g_reader_running.load()) {
        return;
    }

    g_reader_running = true;
    g_reader_thread = std::thread(gps_reader_loop);
}

static void stop_reader_thread()
{
    if (!g_reader_running.load()) {
        return;
    }

    g_reader_running = false;
    if (g_reader_thread.joinable()) {
        g_reader_thread.join();
    }
}

static void init_screen_style(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x061019), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x10283A), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *halo_a = lv_obj_create(screen);
    lv_obj_set_size(halo_a, 132, 132);
    lv_obj_set_pos(halo_a, -34, -36);
    lv_obj_set_style_radius(halo_a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(halo_a, lv_color_hex(0x1C4F7A), 0);
    lv_obj_set_style_bg_opa(halo_a, (lv_opa_t)90, 0);
    lv_obj_set_style_border_width(halo_a, 0, 0);
    lv_obj_clear_flag(halo_a, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

void ui_init()
{
    ensure_group();
    start_reader_thread();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    init_screen_style(screen);

    const int screen_w = (int)lv_display_get_horizontal_resolution(lv_display_get_default());
    const int screen_h = (int)lv_display_get_vertical_resolution(lv_display_get_default());
    const int hero_w = screen_w - 24;
    const int coord_w = screen_w - 24;

    lv_obj_t *hero = create_card(screen, 12, 10, hero_w, 66,
                                 lv_color_hex(0x0B1A26), lv_color_hex(0x164866), kOpaMediumCard, 22);
    lv_obj_t *coord = create_card(screen, 12, 84, coord_w, 56,
                                  lv_color_hex(0xF4F7F9), lv_color_hex(0xDAE7EF), kOpaLightCard, 18);
    lv_obj_t *log_card = create_card(screen, 12, screen_h - 24, screen_w - 24, 16,
                                     lv_color_hex(0x0A1620), lv_color_hex(0x102331), kOpaMediumCard, 12);

    lv_obj_t *eyebrow = lv_label_create(hero);
    lv_label_set_text(eyebrow, "CAPGPS / GNSS");
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(0x8DA8BE), 0);
    lv_obj_set_style_text_font(eyebrow, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(eyebrow, 16, 9);

    g_status_title = lv_label_create(hero);
    lv_label_set_text(g_status_title, "Searching satellites");
    lv_obj_set_style_text_color(g_status_title, lv_color_hex(0xF4F8FB), 0);
    lv_obj_set_style_text_font(g_status_title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(g_status_title, 16, 22);

    g_status_detail = lv_label_create(hero);
    lv_label_set_text(g_status_detail, "Listening on /dev/ttyS0");
    lv_obj_set_width(g_status_detail, 168);
    lv_obj_set_style_text_color(g_status_detail, lv_color_hex(0xA5B9C9), 0);
    lv_obj_set_style_text_font(g_status_detail, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(g_status_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_status_detail, 16, 46);

    g_status_chip = lv_obj_create(hero);
    lv_obj_set_size(g_status_chip, 58, 22);
    lv_obj_set_pos(g_status_chip, hero_w - 78, 12);
    lv_obj_set_style_radius(g_status_chip, 12, 0);
    lv_obj_set_style_bg_color(g_status_chip, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_bg_opa(g_status_chip, kOpaChipFill, 0);
    lv_obj_set_style_shadow_width(g_status_chip, 0, 0);
    lv_obj_set_style_outline_width(g_status_chip, 0, 0);
    lv_obj_set_style_border_width(g_status_chip, 1, 0);
    lv_obj_set_style_pad_all(g_status_chip, 0, 0);
    lv_obj_clear_flag(g_status_chip, LV_OBJ_FLAG_SCROLLABLE);

    g_status_chip_text = lv_label_create(g_status_chip);
    lv_label_set_text(g_status_chip_text, "SCAN");
    lv_obj_set_style_text_color(g_status_chip_text, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_text_font(g_status_chip_text, &lv_font_montserrat_12, 0);
    lv_obj_center(g_status_chip_text);

    g_search_spinner = lv_spinner_create(hero);
    lv_obj_set_size(g_search_spinner, 18, 18);
    lv_obj_set_pos(g_search_spinner, hero_w - 70, 41);
    lv_spinner_set_anim_params(g_search_spinner, 850, 70);
    lv_obj_set_style_arc_width(g_search_spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_search_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_search_spinner, lv_color_hex(0x24455F), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_search_spinner, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_search_spinner, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_search_spinner, LV_OBJ_FLAG_CLICKABLE);

    g_sat_used_value = lv_label_create(hero);
    lv_label_set_text(g_sat_used_value, "USED 00");
    lv_obj_set_width(g_sat_used_value, 56);
    lv_obj_set_style_text_color(g_sat_used_value, lv_color_hex(0xDCE6EE), 0);
    lv_obj_set_style_text_font(g_sat_used_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(g_sat_used_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_sat_used_value, hero_w - 60, 38);

    g_sat_visible_value = lv_label_create(hero);
    lv_label_set_text(g_sat_visible_value, "VIS 00");
    lv_obj_set_width(g_sat_visible_value, 56);
    lv_obj_set_style_text_color(g_sat_visible_value, lv_color_hex(0x90A8BC), 0);
    lv_obj_set_style_text_font(g_sat_visible_value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(g_sat_visible_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_sat_visible_value, hero_w - 60, 52);

    lv_obj_t *lat_title = lv_label_create(coord);
    lv_label_set_text(lat_title, "LATITUDE");
    lv_obj_set_style_text_color(lat_title, lv_color_hex(0x5A7487), 0);
    lv_obj_set_style_text_font(lat_title, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lat_title, 14, 8);

    lv_obj_t *lng_title = lv_label_create(coord);
    lv_label_set_text(lng_title, "LONGITUDE");
    lv_obj_set_style_text_color(lng_title, lv_color_hex(0x5A7487), 0);
    lv_obj_set_style_text_font(lng_title, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(lng_title, 166, 8);

    g_fix_value = lv_label_create(coord);
    lv_label_set_text(g_fix_value, "Invalid / No fix");
    lv_obj_set_width(g_fix_value, screen_w - 48);
    lv_obj_set_style_text_color(g_fix_value, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_text_font(g_fix_value, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(g_fix_value, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(g_fix_value, 14, 40);

    g_lat_value = lv_label_create(coord);
    lv_label_set_text(g_lat_value, "--.------");
    lv_obj_set_width(g_lat_value, 140);
    lv_obj_set_style_text_color(g_lat_value, lv_color_hex(0x10283A), 0);
    lv_obj_set_style_text_font(g_lat_value, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(g_lat_value, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(g_lat_value, 14, 18);

    g_lng_value = lv_label_create(coord);
    lv_label_set_text(g_lng_value, "--.------");
    lv_obj_set_width(g_lng_value, 140);
    lv_obj_set_style_text_color(g_lng_value, lv_color_hex(0x10283A), 0);
    lv_obj_set_style_text_font(g_lng_value, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(g_lng_value, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(g_lng_value, 166, 18);

    g_date_value = lv_label_create(coord);
    lv_label_set_text(g_date_value, "");
    lv_obj_add_flag(g_date_value, LV_OBJ_FLAG_HIDDEN);

    g_footer = lv_label_create(log_card);
    lv_label_set_text(g_footer, "LOG | waiting for GPS stream");
    lv_obj_set_width(g_footer, screen_w - 44);
    lv_obj_set_style_text_color(g_footer, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_text_font(g_footer, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(g_footer, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_pos(g_footer, 10, 1);

    if (g_refresh_timer != nullptr) {
        lv_timer_del(g_refresh_timer);
    }
    g_refresh_timer = lv_timer_create(refresh_ui, kRefreshPeriodMs, nullptr);
    refresh_ui(nullptr);
}

lv_group_t *ui_get_input_group()
{
    ensure_group();
    return g_group;
}

void ui_shutdown()
{
    if (g_refresh_timer != nullptr) {
        lv_timer_del(g_refresh_timer);
        g_refresh_timer = nullptr;
    }

    stop_reader_thread();
    stop_held_line(&g_ext_route_hold);
    stop_held_line(&g_ext_power_hold);

    if (g_group != nullptr) {
        lv_group_delete(g_group);
        g_group = nullptr;
    }
}
