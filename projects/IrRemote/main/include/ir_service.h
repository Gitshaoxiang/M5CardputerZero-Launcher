#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace irremote {

struct DeviceInfo {
    bool available = false;
    std::string rc_name;
    std::string lirc_path;
    std::string driver_name;
    std::string device_name;
    std::string error_message;
    uint32_t features = 0;
};

struct SendResult {
    bool success = false;
    uint16_t address = 0;
    uint16_t command = 0;
    std::vector<uint8_t> data;
    std::vector<uint32_t> raw_timings;
    std::string protocol;
    std::string device_path;
    std::string message;
};

struct ReceiveSnapshot {
    bool opened = false;
    bool signal_seen = false;
    bool data_changed = false;
    bool address_matched = false;
    bool address_filter_enabled = false;
    uint16_t expected_address = 0;
    uint16_t address = 0;
    uint16_t command = 0;
    std::vector<uint8_t> data;
    std::vector<uint32_t> raw_timings;
    std::string protocol;
    std::string device_path;
    std::string raw_summary;
    std::string message;
};

DeviceInfo probe_receiver();
DeviceInfo probe_sender();

std::vector<uint8_t> make_standard_nec_bytes(uint8_t address_byte, uint8_t command_byte);
bool parse_nec_bytes(const std::vector<uint8_t> &bytes,
                     uint16_t *address,
                     uint16_t *command,
                     std::string *protocol);

SendResult send_nec_bytes(const std::vector<uint8_t> &data);
SendResult send_standard_nec(uint8_t address_byte, uint8_t command_byte);
SendResult send_raw_timings(const std::vector<uint32_t> &timings);

std::string format_hex_bytes(const std::vector<uint8_t> &data);
std::string format_nec_address(uint16_t address);
std::string format_nec_command(uint16_t command);

class ReceiverSession {
public:
    ReceiverSession();
    ~ReceiverSession();

    ReceiverSession(const ReceiverSession &) = delete;
    ReceiverSession &operator=(const ReceiverSession &) = delete;

    bool start(uint16_t expected_address = 0, bool filter_address = false, bool raw_only = false);
    void stop();
    ReceiveSnapshot poll();
    const ReceiveSnapshot &snapshot() const;

private:
    int fd_ = -1;
    bool scancode_mode_ = false;
    bool mode2_ = true;
    bool raw_only_ = false;
    std::vector<uint32_t> pulse_space_buffer_;
    ReceiveSnapshot snapshot_;
};

}  // namespace irremote
