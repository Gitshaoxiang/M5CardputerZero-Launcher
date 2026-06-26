#include "ir_service.h"
#include "app_log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(__has_include)
#if __has_include(<linux/lirc.h>)
#define IRREMOTE_USE_LIRC 1
#include <linux/lirc.h>
#else
#define IRREMOTE_USE_LIRC 0
#endif
#else
#define IRREMOTE_USE_LIRC 0
#endif

namespace irremote {
namespace {

constexpr uint32_t kCarrierHz = 38000;
constexpr uint32_t kDutyCycle = 50;
constexpr uint32_t kRecTimeoutUs = 30000;
constexpr std::size_t kPacketBytes = 4;
constexpr uint32_t kNecHdrPulseUs = 9000;
constexpr uint32_t kNecHdrSpaceUs = 4500;
constexpr uint32_t kNecBitPulseUs = 560;
constexpr uint32_t kNecZeroSpaceUs = 560;
constexpr uint32_t kNecOneSpaceUs = 1690;
constexpr const char *kProtocolNec = "NEC";
constexpr const char *kProtocolNecx = "NECX";
constexpr const char *kProtocolNec32 = "NEC32";

std::string bool_text(bool value)
{
    return value ? "yes" : "no";
}

std::string trim(std::string value)
{
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
    {
        value.pop_back();
    }

    size_t pos = 0;
    while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t'))
    {
        ++pos;
    }
    if (pos > 0)
    {
        value.erase(0, pos);
    }
    return value;
}

std::string read_first_line(const std::string &path)
{
    std::ifstream stream(path);
    std::string line;
    if (std::getline(stream, line))
    {
        return trim(line);
    }
    return "";
}

std::string read_uevent_value(const std::string &path, const std::string &key)
{
    std::ifstream stream(path);
    if (!stream.is_open())
    {
        return "";
    }

    const std::string prefix = key + "=";
    std::string line;
    while (std::getline(stream, line))
    {
        if (line.rfind(prefix, 0) == 0)
        {
            return trim(line.substr(prefix.size()));
        }
    }
    return "";
}

std::vector<std::string> list_rc_names()
{
    std::vector<std::string> names;
    DIR *dir = opendir("/sys/class/rc");
    if (dir == nullptr)
    {
        return names;
    }

    dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        std::string name(entry->d_name);
        if (name.rfind("rc", 0) == 0)
        {
            names.push_back(name);
        }
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> find_lirc_paths_for_rc(const std::string &rc_name)
{
    std::vector<std::string> paths;
    std::string rc_path = "/sys/class/rc/" + rc_name;
    DIR *dir = opendir(rc_path.c_str());
    if (dir == nullptr)
    {
        return paths;
    }

    dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }
        std::string name(entry->d_name);
        if (name.rfind("lirc", 0) == 0)
        {
            paths.push_back("/dev/" + name);
        }
    }
    closedir(dir);
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::string errno_message(const char *prefix)
{
    return std::string(prefix) + ": " + std::strerror(errno);
}

std::string feature_summary(uint32_t features)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << features << std::dec
        << " send[pulse=" << bool_text((features & LIRC_CAN_SEND_PULSE) != 0)
        << " raw=" << bool_text((features & LIRC_CAN_SEND_RAW) != 0)
        << "] recv[mode2=" << bool_text((features & LIRC_CAN_REC_MODE2) != 0)
        << " raw=" << bool_text((features & LIRC_CAN_REC_RAW) != 0)
        << " scancode=" << bool_text((features & LIRC_CAN_REC_SCANCODE) != 0)
        << "]";
    return out.str();
}

std::string preview_timings(const std::vector<uint32_t> &timings, std::size_t limit = 8)
{
    if (timings.empty())
    {
        return "(empty)";
    }

    std::ostringstream out;
    const std::size_t count = std::min(limit, timings.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            out << ",";
        }
        out << timings[i];
    }
    if (timings.size() > count)
    {
        out << ",...";
    }
    return out.str();
}

std::string send_mode_name(uint32_t mode)
{
    switch (mode)
    {
    case LIRC_MODE_PULSE: return "pulse";
    case LIRC_MODE_RAW: return "raw";
    case LIRC_MODE_MODE2: return "mode2";
    case LIRC_MODE_SCANCODE: return "scancode";
    default: return "unknown";
    }
}

bool configure_send_mode(int fd, const std::string &path, uint32_t features, uint32_t *selected_mode, std::string *error)
{
    if (selected_mode != nullptr)
    {
        *selected_mode = LIRC_MODE_PULSE;
    }

    uint32_t pulse_mode = LIRC_MODE_PULSE;
    if (ioctl(fd, LIRC_SET_SEND_MODE, &pulse_mode) == 0)
    {
        if (selected_mode != nullptr)
        {
            *selected_mode = pulse_mode;
        }
        applog::debug("IR send mode configured path=" + path + " mode=pulse");
        return true;
    }

    const std::string pulse_error = std::strerror(errno);
    applog::warn("Failed to set pulse send mode path=" + path + " error=" + pulse_error);

    if ((features & LIRC_CAN_SEND_RAW) != 0)
    {
        uint32_t raw_mode = LIRC_MODE_RAW;
        if (ioctl(fd, LIRC_SET_SEND_MODE, &raw_mode) == 0)
        {
            if (selected_mode != nullptr)
            {
                *selected_mode = raw_mode;
            }
            applog::warn("IR send mode fell back to raw path=" + path);
            return true;
        }

        const std::string raw_error = std::strerror(errno);
        applog::error("Failed to set raw send mode path=" + path + " error=" + raw_error);
        if (error != nullptr)
        {
            *error = "Set send mode failed: pulse=" + pulse_error + ", raw=" + raw_error;
        }
        return false;
    }

    if (error != nullptr)
    {
        *error = "Set send mode failed: " + pulse_error;
    }
    return false;
}

std::string flag_name(int flags)
{
    switch (flags)
    {
    case O_WRONLY: return "O_WRONLY";
    case O_RDWR: return "O_RDWR";
    case (O_WRONLY | O_NONBLOCK): return "O_WRONLY|O_NONBLOCK";
    case (O_RDWR | O_NONBLOCK): return "O_RDWR|O_NONBLOCK";
    default: return "flags=" + std::to_string(flags);
    }
}

std::string forced_device_env_name(bool sender)
{
    return sender ? "IRREMOTE_LIRC_SEND_DEVICE" : "IRREMOTE_LIRC_RECV_DEVICE";
}

std::string forced_lirc_path(bool sender)
{
    const std::string specific_name = forced_device_env_name(sender);
    const char *specific = getenv(specific_name.c_str());
    if (specific != nullptr && specific[0] != '\0')
    {
        return specific;
    }

    const char *generic = getenv("IRREMOTE_LIRC_DEVICE");
    if (generic != nullptr && generic[0] != '\0')
    {
        return generic;
    }
    return "";
}

#if IRREMOTE_USE_LIRC
int open_lirc_for_features(const std::string &path)
{
    for (int flags : {O_RDWR | O_NONBLOCK, O_RDONLY | O_NONBLOCK, O_WRONLY | O_NONBLOCK})
    {
        int fd = open(path.c_str(), flags);
        if (fd >= 0)
        {
            return fd;
        }
    }
    return -1;
}

int open_sender_device(const std::string &path, int *used_flags)
{
    for (int flags : {O_WRONLY, O_RDWR, O_WRONLY | O_NONBLOCK, O_RDWR | O_NONBLOCK})
    {
        int fd = open(path.c_str(), flags);
        if (fd >= 0)
        {
            if (used_flags != nullptr)
            {
                *used_flags = flags;
            }
            return fd;
        }
    }
    return -1;
}

bool supports_sender(uint32_t features)
{
    return (features & (LIRC_CAN_SEND_PULSE | LIRC_CAN_SEND_RAW)) != 0;
}

bool supports_receiver(uint32_t features)
{
    return (features & (LIRC_CAN_REC_MODE2 | LIRC_CAN_REC_RAW | LIRC_CAN_REC_SCANCODE)) != 0;
}
#endif

DeviceInfo make_device_info(const std::string &rc_name, const std::string &lirc_path, bool sender)
{
    DeviceInfo info;
    info.rc_name = rc_name;
    info.lirc_path = lirc_path;

    const std::string rc_path = "/sys/class/rc/" + rc_name;
    info.driver_name = read_uevent_value(rc_path + "/uevent", "DRV_NAME");
    info.device_name = read_uevent_value(rc_path + "/uevent", "DEV_NAME");
    if (info.driver_name.empty())
    {
        info.driver_name = read_uevent_value(rc_path + "/device/uevent", "DRIVER");
    }
    if (info.device_name.empty())
    {
        info.device_name = read_first_line(rc_path + "/name");
    }

    if (info.lirc_path.empty())
    {
        info.error_message = "No LIRC node found for " + rc_name;
        applog::warn("IR " + std::string(sender ? "sender" : "receiver") +
                     " missing LIRC node for rc=" + rc_name);
        return info;
    }

#if IRREMOTE_USE_LIRC
    int fd = open_lirc_for_features(info.lirc_path);
    if (fd < 0)
    {
        info.error_message = "Open failed: " + std::string(std::strerror(errno));
        applog::warn("IR " + std::string(sender ? "sender" : "receiver") +
                     " open failed rc=" + rc_name + " path=" + info.lirc_path +
                     " error=" + info.error_message);
        return info;
    }

    uint32_t features = 0;
    if (ioctl(fd, LIRC_GET_FEATURES, &features) != 0)
    {
        info.error_message = "Read LIRC features failed: " + std::string(std::strerror(errno));
        applog::warn("IR " + std::string(sender ? "sender" : "receiver") +
                     " feature probe failed rc=" + rc_name + " path=" + info.lirc_path +
                     " error=" + info.error_message);
        close(fd);
        return info;
    }
    close(fd);

    info.features = features;
    const bool ok = sender ? supports_sender(features) : supports_receiver(features);
    if (!ok)
    {
        info.error_message = "LIRC device does not advertise required capability";
        applog::warn("IR " + std::string(sender ? "sender" : "receiver") +
                     " capability mismatch rc=" + rc_name + " path=" + info.lirc_path +
                     " features=" + feature_summary(features));
        return info;
    }

    info.available = true;
    applog::info("IR " + std::string(sender ? "sender" : "receiver") +
                 " ready rc=" + rc_name + " path=" + info.lirc_path +
                 " driver=" + (info.driver_name.empty() ? "-" : info.driver_name) +
                 " name=" + (info.device_name.empty() ? "-" : info.device_name) +
                 " features=" + feature_summary(features));
#else
    (void)sender;
    info.error_message = "LIRC support not available at build time";
    applog::warn("IR backend disabled at build time");
#endif

    return info;
}

DeviceInfo find_ir_device(bool sender)
{
    DeviceInfo probe_error;
    bool has_probe_error = false;
    std::size_t candidate_count = 0;

    const std::string forced = forced_lirc_path(sender);
    if (!forced.empty())
    {
        applog::info("IR " + std::string(sender ? "sender" : "receiver") +
                     " using forced device from " + forced_device_env_name(sender) +
                     " or IRREMOTE_LIRC_DEVICE: " + forced);
        DeviceInfo info = make_device_info(sender ? "forced-send" : "forced-recv", forced, sender);
        if (!info.available)
        {
            applog::warn("Forced IR device probe failed path=" + forced +
                         " error=" + info.error_message);
        }
        return info;
    }

    for (const std::string &rc_name : list_rc_names())
    {
        for (const std::string &lirc_path : find_lirc_paths_for_rc(rc_name))
        {
            ++candidate_count;
            applog::debug("Probing IR " + std::string(sender ? "sender" : "receiver") +
                          " candidate rc=" + rc_name + " path=" + lirc_path);
            DeviceInfo info = make_device_info(rc_name, lirc_path, sender);
            if (info.available)
            {
                return info;
            }
            if (!has_probe_error)
            {
                probe_error = info;
                has_probe_error = true;
            }
        }
    }

    if (has_probe_error)
    {
        return probe_error;
    }

    DeviceInfo info;
    info.error_message = candidate_count == 0 ? "No LIRC nodes found under /sys/class/rc"
                                              : std::string("No LIRC device advertises IR ") +
                                                    (sender ? "sender" : "receiver") + " capability";
    applog::error("IR " + std::string(sender ? "sender" : "receiver") +
                  " discovery failed: " + info.error_message);
    return info;
}

uint16_t make_nec_command(uint8_t command_byte)
{
    return static_cast<uint16_t>(command_byte) |
           static_cast<uint16_t>(static_cast<uint8_t>(~command_byte) << 8U);
}

std::vector<uint8_t> make_nec_bytes(uint16_t address, uint16_t command)
{
    return {
        static_cast<uint8_t>(address & 0xffU),
        static_cast<uint8_t>((address >> 8U) & 0xffU),
        static_cast<uint8_t>(command & 0xffU),
        static_cast<uint8_t>((command >> 8U) & 0xffU),
    };
}

std::vector<uint32_t> encode_nec_packet(const std::vector<uint8_t> &bytes)
{
    std::vector<uint32_t> pulses;
    pulses.reserve(2 + bytes.size() * 16 + 1);
    pulses.push_back(kNecHdrPulseUs);
    pulses.push_back(kNecHdrSpaceUs);
    for (uint8_t byte : bytes)
    {
        for (int bit = 0; bit < 8; ++bit)
        {
            pulses.push_back(kNecBitPulseUs);
            pulses.push_back((byte & (1U << bit)) ? kNecOneSpaceUs : kNecZeroSpaceUs);
        }
    }
    pulses.push_back(kNecBitPulseUs);
    return pulses;
}

std::vector<uint32_t> sanitize_raw_timings(const std::vector<uint32_t> &timings)
{
    std::vector<uint32_t> sanitized;
    sanitized.reserve(timings.size());
    for (uint32_t value : timings)
    {
        if (value > 0)
        {
            sanitized.push_back(value);
        }
    }
    return sanitized;
}

std::vector<uint32_t> prepare_pulse_timings(const std::vector<uint32_t> &timings)
{
    std::vector<uint32_t> prepared = sanitize_raw_timings(timings);
    if (!prepared.empty() && (prepared.size() % 2) == 0)
    {
        applog::warn("Pulse send requires odd timing count; trimming trailing space len=" +
                     std::to_string(prepared.back()) +
                     " original_count=" + std::to_string(prepared.size()));
        prepared.pop_back();
    }
    return prepared;
}

bool approx(uint32_t value, uint32_t target, uint32_t tolerance)
{
    return value + tolerance >= target && value <= target + tolerance;
}

bool decode_nec_packet_from(const std::vector<uint32_t> &durations,
                            std::size_t start,
                            std::vector<uint8_t> *output)
{
    constexpr std::size_t required = 2 + kPacketBytes * 16 + 1;
    if (durations.size() < start + required)
    {
        return false;
    }
    if (!approx(durations[start], kNecHdrPulseUs, 2500) ||
        !approx(durations[start + 1], kNecHdrSpaceUs, 1800))
    {
        return false;
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(kPacketBytes);
    std::size_t index = start + 2;
    for (std::size_t byte_index = 0; byte_index < kPacketBytes; ++byte_index)
    {
        uint8_t byte = 0;
        for (int bit = 0; bit < 8; ++bit)
        {
            uint32_t pulse = durations[index++];
            uint32_t space = durations[index++];
            if (!approx(pulse, kNecBitPulseUs, 450))
            {
                return false;
            }
            if (approx(space, kNecOneSpaceUs, 850))
            {
                byte |= static_cast<uint8_t>(1U << bit);
            }
            else if (!approx(space, kNecZeroSpaceUs, 500))
            {
                return false;
            }
        }
        bytes.push_back(byte);
    }

    if (output != nullptr)
    {
        *output = std::move(bytes);
    }
    return true;
}

bool decode_nec_packet(const std::vector<uint32_t> &durations, std::vector<uint8_t> *output)
{
    for (std::size_t i = 0; i + 2 < durations.size(); ++i)
    {
        if (decode_nec_packet_from(durations, i, output))
        {
            return true;
        }
    }
    return false;
}

std::string summarize_raw(const std::vector<uint32_t> &durations)
{
    if (durations.empty())
    {
        return "";
    }

    std::ostringstream out;
    const std::size_t count = std::min<std::size_t>(durations.size(), 12);
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            out << ", ";
        }
        out << durations[i] << "us";
    }
    if (durations.size() > count)
    {
        out << " ...";
    }
    return out.str();
}

void finalize_receive(ReceiveSnapshot *snapshot, const std::vector<uint32_t> &durations)
{
    if (snapshot == nullptr)
    {
        return;
    }

    const bool raw_changed = snapshot->raw_timings != durations;
    snapshot->signal_seen = true;
    snapshot->raw_timings = durations;
    snapshot->raw_summary = summarize_raw(durations);
    snapshot->data_changed = raw_changed;

    std::vector<uint8_t> decoded;
    if (!decode_nec_packet(durations, &decoded))
    {
        const bool metadata_cleared = !snapshot->data.empty() || !snapshot->protocol.empty() ||
                                      snapshot->address != 0 || snapshot->command != 0;
        snapshot->data_changed = raw_changed || metadata_cleared;
        snapshot->data.clear();
        snapshot->address = 0;
        snapshot->command = 0;
        snapshot->protocol = "RAW";
        snapshot->address_matched = false;
        snapshot->message = "IR signal received (raw copy)";
        return;
    }

    uint16_t address = 0;
    uint16_t command = 0;
    std::string protocol;
    if (!parse_nec_bytes(decoded, &address, &command, &protocol))
    {
        snapshot->message = "IR signal received (raw)";
        return;
    }

    const bool matched = !snapshot->address_filter_enabled || address == snapshot->expected_address;
    snapshot->data_changed = raw_changed || decoded != snapshot->data || address != snapshot->address ||
                             command != snapshot->command || protocol != snapshot->protocol ||
                             matched != snapshot->address_matched;
    snapshot->data = std::move(decoded);
    snapshot->address = address;
    snapshot->command = command;
    snapshot->protocol = std::move(protocol);
    snapshot->address_matched = matched;
    snapshot->message = matched ? snapshot->protocol + " packet received" : "NEC address mismatch";
}

#if IRREMOTE_USE_LIRC
bool decode_scancode(const lirc_scancode &scancode,
                     uint16_t *address,
                     uint16_t *command,
                     std::vector<uint8_t> *data,
                     std::string *protocol)
{
    if (address == nullptr || command == nullptr || data == nullptr || protocol == nullptr)
    {
        return false;
    }

    uint8_t command_byte = static_cast<uint8_t>(scancode.scancode & 0xffU);
    *command = make_nec_command(command_byte);

    switch (scancode.rc_proto)
    {
    case RC_PROTO_NEC: {
        uint8_t address_byte = static_cast<uint8_t>((scancode.scancode >> 8U) & 0xffU);
        *address = static_cast<uint16_t>(address_byte) |
                   static_cast<uint16_t>(static_cast<uint8_t>(~address_byte) << 8U);
        *data = make_standard_nec_bytes(address_byte, command_byte);
        *protocol = kProtocolNec;
        return true;
    }
    case RC_PROTO_NECX:
        *address = static_cast<uint16_t>((scancode.scancode >> 8U) & 0xffffU);
        *data = make_nec_bytes(*address, *command);
        *protocol = kProtocolNecx;
        return true;
    case RC_PROTO_NEC32: {
        uint32_t value = static_cast<uint32_t>(scancode.scancode & 0xffffffffULL);
        *address = static_cast<uint16_t>((value >> 16U) & 0xffffU);
        *command = static_cast<uint16_t>(value & 0xffffU);
        *data = make_nec_bytes(*address, *command);
        *protocol = kProtocolNec32;
        return true;
    }
    default:
        return false;
    }
}

void finalize_receive_scancode(ReceiveSnapshot *snapshot, const lirc_scancode &scancode)
{
    if (snapshot == nullptr)
    {
        return;
    }

    if (scancode.flags & LIRC_SCANCODE_FLAG_REPEAT)
    {
        snapshot->signal_seen = true;
        snapshot->message = "NEC repeat received";
        return;
    }

    uint16_t address = 0;
    uint16_t command = 0;
    std::vector<uint8_t> data;
    std::string protocol;
    if (!decode_scancode(scancode, &address, &command, &data, &protocol))
    {
        snapshot->signal_seen = true;
        snapshot->message = "IR scancode received";
        return;
    }

    const bool matched = !snapshot->address_filter_enabled || address == snapshot->expected_address;
    snapshot->signal_seen = true;
    const bool raw_changed = !snapshot->raw_timings.empty();
    snapshot->raw_timings.clear();
    snapshot->data_changed = raw_changed || data != snapshot->data || address != snapshot->address ||
                             command != snapshot->command || protocol != snapshot->protocol ||
                             matched != snapshot->address_matched;
    snapshot->address = address;
    snapshot->command = command;
    snapshot->data = std::move(data);
    snapshot->protocol = std::move(protocol);
    snapshot->address_matched = matched;
    snapshot->raw_summary.clear();
    snapshot->message = matched ? snapshot->protocol + " packet received" : "NEC address mismatch";
}

bool protocols_have_nec_enabled(const std::string &path)
{
    std::ifstream stream(path);
    std::string content;
    std::getline(stream, content);
    return content.find("[nec]") != std::string::npos ||
           content.find("[necx]") != std::string::npos ||
           content.find("[nec32]") != std::string::npos;
}

void try_enable_nec_protocols(const std::string &rc_name)
{
    const std::string protocols_path = "/sys/class/rc/" + rc_name + "/protocols";
    if (protocols_have_nec_enabled(protocols_path))
    {
        return;
    }

    for (const char *protocol : {"+nec", "+necx", "+nec32"})
    {
        std::ofstream stream(protocols_path);
        if (!stream.is_open())
        {
            return;
        }
        stream << protocol;
    }
}
#endif

bool close_fd(int *fd)
{
    if (fd != nullptr && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
        return true;
    }
    return false;
}

}  // namespace

DeviceInfo probe_receiver()
{
    return find_ir_device(false);
}

DeviceInfo probe_sender()
{
    return find_ir_device(true);
}

std::vector<uint8_t> make_standard_nec_bytes(uint8_t address_byte, uint8_t command_byte)
{
    return {
        address_byte,
        static_cast<uint8_t>(~address_byte),
        command_byte,
        static_cast<uint8_t>(~command_byte),
    };
}

bool parse_nec_bytes(const std::vector<uint8_t> &bytes,
                     uint16_t *address,
                     uint16_t *command,
                     std::string *protocol)
{
    if (bytes.size() != kPacketBytes)
    {
        return false;
    }

    uint16_t parsed_address =
        static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1] << 8U);
    uint16_t parsed_command =
        static_cast<uint16_t>(bytes[2]) | static_cast<uint16_t>(bytes[3] << 8U);

    if (address != nullptr)
    {
        *address = parsed_address;
    }
    if (command != nullptr)
    {
        *command = parsed_command;
    }
    if (protocol != nullptr)
    {
        if (static_cast<uint8_t>(bytes[0] ^ bytes[1]) == 0xffU)
        {
            *protocol = kProtocolNec;
        }
        else
        {
            *protocol = static_cast<uint8_t>(bytes[2] ^ bytes[3]) == 0xffU ? kProtocolNecx : kProtocolNec32;
        }
    }
    return true;
}

SendResult send_nec_bytes(const std::vector<uint8_t> &data)
{
    SendResult result;
    result.data = data;
    result.raw_timings = prepare_pulse_timings(encode_nec_packet(data));
    parse_nec_bytes(data, &result.address, &result.command, &result.protocol);
    const std::vector<uint32_t> encoded = result.raw_timings;
    DeviceInfo info = probe_sender();
    result.device_path = info.lirc_path;
    if (!info.available)
    {
        result.message = info.error_message.empty() ? "IR sender unavailable" : info.error_message;
        applog::warn("NEC send skipped: " + result.message);
        return result;
    }

#if IRREMOTE_USE_LIRC
    applog::info("NEC send request path=" + info.lirc_path +
                 " protocol=" + result.protocol +
                 " address=" + format_nec_address(result.address) +
                 " command=" + format_nec_command(result.command) +
                 " data=" + format_hex_bytes(result.data) +
                 " timings=" + std::to_string(encoded.size()) +
                 " preview=" + preview_timings(encoded));
    int open_flags = 0;
    int fd = open_sender_device(info.lirc_path, &open_flags);
    if (fd < 0)
    {
        result.message = errno_message("Open sender failed");
        applog::error("NEC sender open failed path=" + info.lirc_path +
                      " error=" + result.message);
        return result;
    }
    applog::debug("NEC sender opened path=" + info.lirc_path + " flags=" + flag_name(open_flags));

    if (info.features & LIRC_CAN_SET_SEND_CARRIER)
    {
        uint32_t carrier = kCarrierHz;
        if (ioctl(fd, LIRC_SET_SEND_CARRIER, &carrier) != 0)
        {
            applog::warn("Failed to set IR send carrier path=" + info.lirc_path +
                         " carrier=" + std::to_string(carrier) +
                         " error=" + std::strerror(errno));
        }
    }
    if (info.features & LIRC_CAN_SET_SEND_DUTY_CYCLE)
    {
        uint32_t duty_cycle = kDutyCycle;
        if (ioctl(fd, LIRC_SET_SEND_DUTY_CYCLE, &duty_cycle) != 0)
        {
            applog::warn("Failed to set IR duty cycle path=" + info.lirc_path +
                         " duty=" + std::to_string(duty_cycle) +
                         " error=" + std::strerror(errno));
        }
    }
    if (info.features & LIRC_CAN_SET_TRANSMITTER_MASK)
    {
        uint32_t mask = 1;
        if (ioctl(fd, LIRC_SET_TRANSMITTER_MASK, &mask) != 0)
        {
            applog::warn("Failed to set IR transmitter mask path=" + info.lirc_path +
                         " mask=1 error=" + std::strerror(errno));
        }
    }

    uint32_t send_mode = LIRC_MODE_PULSE;
    std::string mode_error;
    if (!configure_send_mode(fd, info.lirc_path, info.features, &send_mode, &mode_error))
    {
        result.message = mode_error.empty() ? errno_message("Set send mode failed") : mode_error;
        applog::error("NEC send mode setup failed path=" + info.lirc_path +
                      " mode=" + send_mode_name(send_mode) +
                      " error=" + result.message);
        close(fd);
        return result;
    }

    ssize_t bytes_written = write(fd, encoded.data(), encoded.size() * sizeof(encoded[0]));
    int write_errno = errno;
    close(fd);

    if (bytes_written < 0)
    {
        errno = write_errno;
        result.message = errno_message("Send failed");
        applog::error("NEC send write failed path=" + info.lirc_path +
                      " mode=" + send_mode_name(send_mode) +
                      " error=" + result.message);
        return result;
    }
    if (static_cast<std::size_t>(bytes_written) != encoded.size() * sizeof(encoded[0]))
    {
        result.message = "Short write while sending NEC packet";
        applog::error("NEC send short write path=" + info.lirc_path +
                      " expected=" + std::to_string(encoded.size() * sizeof(encoded[0])) +
                      " actual=" + std::to_string(bytes_written));
        return result;
    }

    result.success = true;
    result.message = "NEC packet sent";
    applog::info("NEC send ok path=" + info.lirc_path +
                 " mode=" + send_mode_name(send_mode) +
                 " bytes=" + std::to_string(bytes_written));
#else
    result.message = "LIRC support not available at build time";
    applog::error(result.message);
#endif
    return result;
}

SendResult send_standard_nec(uint8_t address_byte, uint8_t command_byte)
{
    return send_nec_bytes(make_standard_nec_bytes(address_byte, command_byte));
}

SendResult send_raw_timings(const std::vector<uint32_t> &timings)
{
    SendResult result;
    result.raw_timings = prepare_pulse_timings(timings);
    if (result.raw_timings.size() < 2)
    {
        result.message = "Raw timing buffer is empty";
        applog::warn("Raw send skipped: " + result.message);
        return result;
    }

    if (decode_nec_packet(result.raw_timings, &result.data))
    {
        parse_nec_bytes(result.data, &result.address, &result.command, &result.protocol);
    }
    else
    {
        result.protocol = "RAW";
    }

    DeviceInfo info = probe_sender();
    result.device_path = info.lirc_path;
    if (!info.available)
    {
        result.message = info.error_message.empty() ? "IR sender unavailable" : info.error_message;
        applog::warn("Raw send skipped: " + result.message);
        return result;
    }

#if IRREMOTE_USE_LIRC
    applog::info("Raw send request path=" + info.lirc_path +
                 " protocol=" + result.protocol +
                 " timings=" + std::to_string(result.raw_timings.size()) +
                 " preview=" + preview_timings(result.raw_timings));
    int open_flags = 0;
    int fd = open_sender_device(info.lirc_path, &open_flags);
    if (fd < 0)
    {
        result.message = errno_message("Open sender failed");
        applog::error("Raw sender open failed path=" + info.lirc_path +
                      " error=" + result.message);
        return result;
    }
    applog::debug("Raw sender opened path=" + info.lirc_path + " flags=" + flag_name(open_flags));

    if (info.features & LIRC_CAN_SET_SEND_CARRIER)
    {
        uint32_t carrier = kCarrierHz;
        if (ioctl(fd, LIRC_SET_SEND_CARRIER, &carrier) != 0)
        {
            applog::warn("Failed to set IR send carrier path=" + info.lirc_path +
                         " carrier=" + std::to_string(carrier) +
                         " error=" + std::strerror(errno));
        }
    }
    if (info.features & LIRC_CAN_SET_SEND_DUTY_CYCLE)
    {
        uint32_t duty_cycle = kDutyCycle;
        if (ioctl(fd, LIRC_SET_SEND_DUTY_CYCLE, &duty_cycle) != 0)
        {
            applog::warn("Failed to set IR duty cycle path=" + info.lirc_path +
                         " duty=" + std::to_string(duty_cycle) +
                         " error=" + std::strerror(errno));
        }
    }
    if (info.features & LIRC_CAN_SET_TRANSMITTER_MASK)
    {
        uint32_t mask = 1;
        if (ioctl(fd, LIRC_SET_TRANSMITTER_MASK, &mask) != 0)
        {
            applog::warn("Failed to set IR transmitter mask path=" + info.lirc_path +
                         " mask=1 error=" + std::strerror(errno));
        }
    }

    uint32_t send_mode = LIRC_MODE_PULSE;
    std::string mode_error;
    if (!configure_send_mode(fd, info.lirc_path, info.features, &send_mode, &mode_error))
    {
        result.message = mode_error.empty() ? errno_message("Set send mode failed") : mode_error;
        applog::error("Raw send mode setup failed path=" + info.lirc_path +
                      " mode=" + send_mode_name(send_mode) +
                      " error=" + result.message);
        close(fd);
        return result;
    }

    ssize_t bytes_written =
        write(fd, result.raw_timings.data(), result.raw_timings.size() * sizeof(result.raw_timings[0]));
    int write_errno = errno;
    close(fd);

    if (bytes_written < 0)
    {
        errno = write_errno;
        result.message = errno_message("Send failed");
        applog::error("Raw send write failed path=" + info.lirc_path +
                      " mode=" + send_mode_name(send_mode) +
                      " error=" + result.message);
        return result;
    }
    if (static_cast<std::size_t>(bytes_written) != result.raw_timings.size() * sizeof(result.raw_timings[0]))
    {
        result.message = "Short write while sending raw IR";
        applog::error("Raw send short write path=" + info.lirc_path +
                      " expected=" + std::to_string(result.raw_timings.size() * sizeof(result.raw_timings[0])) +
                      " actual=" + std::to_string(bytes_written));
        return result;
    }

    result.success = true;
    result.message = "Raw IR sent";
    applog::info("Raw send ok path=" + info.lirc_path +
                 " mode=" + send_mode_name(send_mode) +
                 " bytes=" + std::to_string(bytes_written));
#else
    result.message = "LIRC support not available at build time";
    applog::error(result.message);
#endif
    return result;
}

std::string format_hex_bytes(const std::vector<uint8_t> &data)
{
    if (data.empty())
    {
        return "--";
    }

    std::ostringstream out;
    out << std::uppercase << std::hex;
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        if (i > 0)
        {
            out << " ";
        }
        char buffer[8] = {};
        std::snprintf(buffer, sizeof(buffer), "%02X", data[i]);
        out << buffer;
    }
    return out.str();
}

std::string format_nec_address(uint16_t address)
{
    char buffer[12] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%04X", address);
    return buffer;
}

std::string format_nec_command(uint16_t command)
{
    char buffer[12] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%04X", command);
    return buffer;
}

ReceiverSession::ReceiverSession() = default;

ReceiverSession::~ReceiverSession()
{
    stop();
}

bool ReceiverSession::start(uint16_t expected_address, bool filter_address, bool raw_only)
{
    if (fd_ >= 0)
    {
        snapshot_.expected_address = expected_address;
        snapshot_.address_filter_enabled = filter_address;
        raw_only_ = raw_only;
        return true;
    }

    snapshot_ = {};
    snapshot_.expected_address = expected_address;
    snapshot_.address_filter_enabled = filter_address;
    raw_only_ = raw_only;

    DeviceInfo info = probe_receiver();
    snapshot_.device_path = info.lirc_path;
    if (!info.available)
    {
        snapshot_.message = info.error_message.empty() ? "IR receiver unavailable" : info.error_message;
        applog::warn("IR receiver start failed: " + snapshot_.message);
        return false;
    }

#if IRREMOTE_USE_LIRC
    fd_ = open(info.lirc_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd_ < 0)
    {
        snapshot_.message = errno_message("Open receiver failed");
        applog::error("IR receiver open failed path=" + info.lirc_path +
                      " error=" + snapshot_.message);
        return false;
    }

    scancode_mode_ = false;
    mode2_ = (info.features & LIRC_CAN_REC_MODE2) != 0;

    uint32_t rec_mode = LIRC_MODE_RAW;
    const bool has_raw_receive = (info.features & (LIRC_CAN_REC_MODE2 | LIRC_CAN_REC_RAW)) != 0;
    if (raw_only_ && !has_raw_receive)
    {
        snapshot_.message = "Receiver does not support raw capture";
        applog::warn(snapshot_.message + " path=" + info.lirc_path);
        close_fd(&fd_);
        return false;
    }

    if (!raw_only_)
    {
        try_enable_nec_protocols(info.rc_name);
    }

    if (!raw_only_ && (info.features & LIRC_CAN_REC_SCANCODE) && !has_raw_receive)
    {
        rec_mode = LIRC_MODE_SCANCODE;
        if (ioctl(fd_, LIRC_SET_REC_MODE, &rec_mode) == 0)
        {
            scancode_mode_ = true;
        }
        else
        {
            applog::warn("IR receiver scancode mode failed path=" + info.lirc_path +
                         " error=" + std::strerror(errno));
        }
    }
    if (!scancode_mode_)
    {
        rec_mode = mode2_ ? LIRC_MODE_MODE2 : LIRC_MODE_RAW;
        if (ioctl(fd_, LIRC_SET_REC_MODE, &rec_mode) != 0)
        {
            applog::warn("IR receiver raw mode setup failed path=" + info.lirc_path +
                         " mode=" + send_mode_name(rec_mode) +
                         " error=" + std::strerror(errno));
        }
    }

    if (!scancode_mode_ && (info.features & LIRC_CAN_SET_REC_TIMEOUT))
    {
        uint32_t timeout = kRecTimeoutUs;
        if (ioctl(fd_, LIRC_SET_REC_TIMEOUT, &timeout) != 0)
        {
            applog::warn("IR receiver timeout setup failed path=" + info.lirc_path +
                         " error=" + std::strerror(errno));
        }
        uint32_t enable_reports = 1;
        if (ioctl(fd_, LIRC_SET_REC_TIMEOUT_REPORTS, &enable_reports) != 0)
        {
            applog::warn("IR receiver timeout-report setup failed path=" + info.lirc_path +
                         " error=" + std::strerror(errno));
        }
    }

    pulse_space_buffer_.clear();
    snapshot_.opened = true;
    snapshot_.message = raw_only_ ? "Waiting for raw IR data" : "Waiting for IR data";
    applog::info("IR receiver opened path=" + info.lirc_path +
                 " raw_only=" + bool_text(raw_only_) +
                 " mode=" + std::string(scancode_mode_ ? "scancode" : (mode2_ ? "mode2" : "raw")));
    return true;
#else
    snapshot_.message = "LIRC support not available at build time";
    applog::error(snapshot_.message);
    return false;
#endif
}

void ReceiverSession::stop()
{
    if (fd_ >= 0)
    {
        applog::debug("Closing IR receiver path=" + snapshot_.device_path);
    }
    close_fd(&fd_);
    scancode_mode_ = false;
    pulse_space_buffer_.clear();
    snapshot_.opened = false;
}

ReceiveSnapshot ReceiverSession::poll()
{
    snapshot_.data_changed = false;
    if (fd_ < 0)
    {
        return snapshot_;
    }

#if IRREMOTE_USE_LIRC
    if (scancode_mode_)
    {
        std::array<lirc_scancode, 8> scancodes {};
        while (true)
        {
            ssize_t bytes_read = read(fd_, scancodes.data(), scancodes.size() * sizeof(scancodes[0]));
            if (bytes_read < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                {
                    break;
                }
                snapshot_.message = errno_message("Receive failed");
                stop();
                return snapshot_;
            }
            if (bytes_read == 0)
            {
                break;
            }

            std::size_t count = static_cast<std::size_t>(bytes_read) / sizeof(scancodes[0]);
            for (std::size_t i = 0; i < count; ++i)
            {
                finalize_receive_scancode(&snapshot_, scancodes[i]);
            }
        }
        return snapshot_;
    }

    std::array<uint32_t, 96> words {};
    while (true)
    {
        ssize_t bytes_read = read(fd_, words.data(), words.size() * sizeof(words[0]));
        if (bytes_read < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                break;
            }
            snapshot_.message = errno_message("Receive failed");
            stop();
            return snapshot_;
        }
        if (bytes_read == 0)
        {
            break;
        }

        std::size_t count = static_cast<std::size_t>(bytes_read) / sizeof(words[0]);
        for (std::size_t i = 0; i < count; ++i)
        {
            const uint32_t word = words[i];
            if (mode2_)
            {
                if (LIRC_IS_TIMEOUT(word)
#ifdef LIRC_IS_OVERFLOW
                    || LIRC_IS_OVERFLOW(word)
#endif
                )
                {
                    if (!pulse_space_buffer_.empty())
                    {
                        finalize_receive(&snapshot_, pulse_space_buffer_);
                        pulse_space_buffer_.clear();
                    }
                    continue;
                }
                if (LIRC_IS_SPACE(word) || LIRC_IS_PULSE(word))
                {
                    uint32_t value = LIRC_VALUE(word);
                    if (LIRC_IS_SPACE(word) && value > 12000 && !pulse_space_buffer_.empty())
                    {
                        finalize_receive(&snapshot_, pulse_space_buffer_);
                        pulse_space_buffer_.clear();
                    }
                    pulse_space_buffer_.push_back(value);
                }
            }
            else
            {
                pulse_space_buffer_.push_back(word);
            }

            if (pulse_space_buffer_.size() >= 2 + kPacketBytes * 16 + 1)
            {
                std::vector<uint8_t> decoded;
                if (decode_nec_packet(pulse_space_buffer_, &decoded))
                {
                    finalize_receive(&snapshot_, pulse_space_buffer_);
                    pulse_space_buffer_.clear();
                }
            }
            if (pulse_space_buffer_.size() > 160)
            {
                finalize_receive(&snapshot_, pulse_space_buffer_);
                pulse_space_buffer_.clear();
            }
        }
    }
#endif

    return snapshot_;
}

const ReceiveSnapshot &ReceiverSession::snapshot() const
{
    return snapshot_;
}

}  // namespace irremote
