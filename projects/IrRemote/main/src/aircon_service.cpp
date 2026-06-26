#include "aircon_service.h"
#include "app_log.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>

#include "ir_Gree.h"
#include "ir_Haier.h"
#include "ir_Midea.h"

namespace aircon {
namespace {

constexpr std::array<const char *, 4> kBrandNames = {{
    "Midea",
    "Gree",
    "Haier YR-W02",
    "Coolix Intl",
}};

constexpr uint32_t kCoolixHdrMarkUs = 4692;
constexpr uint32_t kCoolixHdrSpaceUs = 4416;
constexpr uint32_t kCoolixBitMarkUs = 552;
constexpr uint32_t kCoolixOneSpaceUs = 1656;
constexpr uint32_t kCoolixZeroSpaceUs = 552;
constexpr uint32_t kCoolixMinGapUs = 5244;
constexpr uint32_t kCoolixDefaultState = 0xB21FC8;
constexpr uint32_t kCoolixOff = 0xB27BE0;
constexpr uint32_t kCoolixSwing = 0xB26BE0;
constexpr uint32_t kCoolixModeMask = 0x00000C;
constexpr uint32_t kCoolixTempMask = 0x0000F0;
constexpr uint32_t kCoolixSensorTempMask = 0x001F00;
constexpr uint32_t kCoolixFanMask = 0x00E000;
constexpr uint32_t kCoolixZoneFollowMask = 0x080002;
constexpr uint32_t kCoolixFixedMask = 0xF00000;
constexpr uint32_t kCoolixFixedValue = 0xB00000;
constexpr uint8_t kCoolixFanAuto0 = 0;
constexpr uint8_t kCoolixFanMin = 4;
constexpr uint8_t kCoolixFanMed = 2;
constexpr uint8_t kCoolixFanAuto = 5;
constexpr uint8_t kCoolixFanMax = 1;
constexpr uint8_t kCoolixTempMin = 17;
constexpr uint8_t kCoolixTempMax = 30;
constexpr uint8_t kCoolixFanTempCode = 14;
constexpr uint8_t kCoolixModeCool = 0;
constexpr uint8_t kCoolixModeDry = 1;
constexpr uint8_t kCoolixModeAuto = 2;
constexpr uint8_t kCoolixModeHeat = 3;
constexpr uint8_t kCoolixModeFan = 4;
constexpr uint16_t kCoolixBits = 24;
constexpr uint16_t kCoolixDefaultRepeat = 1;
constexpr uint32_t kCoolixCmdFan = 0xB2BFE4;

constexpr std::array<uint8_t, 14> kCoolixTempMap = {{
    0b0000,  // 17C
    0b0001,  // 18C
    0b0011,  // 19C
    0b0010,  // 20C
    0b0110,  // 21C
    0b0111,  // 22C
    0b0101,  // 23C
    0b0100,  // 24C
    0b1100,  // 25C
    0b1101,  // 26C
    0b1001,  // 27C
    0b1000,  // 28C
    0b1010,  // 29C
    0b1011,  // 30C
}};

const char *action_name(Action action)
{
    switch (action)
    {
    case Action::SendState: return "send_state";
    case Action::PowerToggle: return "power_toggle";
    case Action::TempUp: return "temp_up";
    case Action::TempDown: return "temp_down";
    case Action::ApplyMode: return "apply_mode";
    case Action::ApplyFan: return "apply_fan";
    case Action::ToggleSwing: return "toggle_swing";
    }
    return "unknown";
}

const char *mode_name(Mode mode)
{
    switch (mode)
    {
    case Mode::Auto: return "Auto";
    case Mode::Cool: return "Cool";
    case Mode::Heat: return "Heat";
    case Mode::Dry: return "Dry";
    case Mode::Fan: return "Fan";
    }
    return "Cool";
}

const char *fan_name(Fan fan)
{
    switch (fan)
    {
    case Fan::Auto: return "Auto";
    case Fan::Low: return "Low";
    case Fan::Medium: return "Medium";
    case Fan::High: return "High";
    }
    return "Auto";
}

uint32_t set_field(uint32_t raw, uint32_t mask, uint32_t value)
{
    if (mask == 0)
    {
        return raw;
    }

    uint32_t shift = 0;
    while (((mask >> shift) & 1U) == 0U)
    {
        ++shift;
    }

    raw &= ~mask;
    raw |= (value << shift) & mask;
    return raw;
}

uint8_t coolix_mode_from_state(const State &state)
{
    switch (state.mode)
    {
    case Mode::Cool: return kCoolixModeCool;
    case Mode::Dry: return kCoolixModeDry;
    case Mode::Auto: return kCoolixModeAuto;
    case Mode::Heat: return kCoolixModeHeat;
    case Mode::Fan: return kCoolixModeFan;
    }
    return kCoolixModeCool;
}

uint8_t coolix_temp_from_state(const State &state)
{
    if (state.mode == Mode::Fan)
    {
        return kCoolixFanTempCode;
    }

    const int clamped = std::max<int>(kCoolixTempMin, std::min<int>(kCoolixTempMax, state.temp_c));
    return kCoolixTempMap[static_cast<std::size_t>(clamped - kCoolixTempMin)];
}

uint8_t coolix_fan_from_state(const State &state)
{
    if (state.mode == Mode::Dry || state.mode == Mode::Auto)
    {
        return kCoolixFanAuto0;
    }

    switch (state.fan)
    {
    case Fan::Auto: return kCoolixFanAuto;
    case Fan::Low: return kCoolixFanMin;
    case Fan::Medium: return kCoolixFanMed;
    case Fan::High: return kCoolixFanMax;
    }
    return kCoolixFanAuto;
}

uint32_t build_coolix_state_raw(const State &state)
{
    uint32_t raw = kCoolixDefaultState;
    raw &= ~kCoolixFixedMask;
    raw |= kCoolixFixedValue;
    raw &= ~kCoolixZoneFollowMask;
    const uint8_t mode = coolix_mode_from_state(state);
    raw = set_field(raw, kCoolixModeMask, mode == kCoolixModeFan ? kCoolixModeDry : mode);
    raw = set_field(raw, kCoolixTempMask, coolix_temp_from_state(state));
    raw = set_field(raw, kCoolixSensorTempMask, 0x1F);
    raw = set_field(raw, kCoolixFanMask, coolix_fan_from_state(state));
    return raw & 0xFFFFFFU;
}

void append_coolix_byte(std::vector<uint32_t> &timings, uint8_t byte)
{
    for (int bit = 7; bit >= 0; --bit)
    {
        timings.push_back(kCoolixBitMarkUs);
        timings.push_back((byte & (1U << bit)) ? kCoolixOneSpaceUs : kCoolixZeroSpaceUs);
    }
}

std::vector<uint32_t> encode_coolix_timings(uint32_t raw, uint16_t repeat = kCoolixDefaultRepeat)
{
    std::vector<uint32_t> timings;
    const uint8_t msb = static_cast<uint8_t>((raw >> 16U) & 0xFFU);
    const uint8_t mid = static_cast<uint8_t>((raw >> 8U) & 0xFFU);
    const uint8_t lsb = static_cast<uint8_t>(raw & 0xFFU);

    for (uint16_t r = 0; r <= repeat; ++r)
    {
        timings.push_back(kCoolixHdrMarkUs);
        timings.push_back(kCoolixHdrSpaceUs);
        append_coolix_byte(timings, msb);
        append_coolix_byte(timings, static_cast<uint8_t>(~msb));
        append_coolix_byte(timings, mid);
        append_coolix_byte(timings, static_cast<uint8_t>(~mid));
        append_coolix_byte(timings, lsb);
        append_coolix_byte(timings, static_cast<uint8_t>(~lsb));
        timings.push_back(kCoolixBitMarkUs);
        timings.push_back(kCoolixMinGapUs);
    }
    return timings;
}

std::string state_summary(const State &state)
{
    std::ostringstream out;
    out << brand_name(brand_to_index(state.brand))
        << " power=" << (state.power ? "on" : "off")
        << " temp=" << state.temp_c
        << " mode=" << mode_name(state.mode)
        << " fan=" << fan_name(state.fan)
        << " swing=" << (state.swing ? "on" : "off");
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

void log_build_result(const char *brand,
                      Action action,
                      const State &before,
                      const State &after,
                      const BuildResult &result)
{
    const std::string prefix = std::string("A/C build brand=") + brand +
                               " action=" + action_name(action) +
                               " before={" + state_summary(before) + "}" +
                               " after={" + state_summary(after) + "}";
    if (result.success)
    {
        applog::info(prefix + " timings=" + std::to_string(result.raw_timings.size()) +
                     " preview=" + preview_timings(result.raw_timings));
    }
    else
    {
        applog::warn(prefix + " failed=" + result.message);
    }
}

int clamp_temp(int temp_c)
{
    return std::max(16, std::min(30, temp_c));
}

uint8_t to_midea_mode(Mode mode)
{
    switch (mode)
    {
    case Mode::Auto: return kMideaACAuto;
    case Mode::Cool: return kMideaACCool;
    case Mode::Heat: return kMideaACHeat;
    case Mode::Dry: return kMideaACDry;
    case Mode::Fan: return kMideaACFan;
    }
    return kMideaACCool;
}

uint8_t to_midea_fan(Fan fan)
{
    switch (fan)
    {
    case Fan::Auto: return kMideaACFanAuto;
    case Fan::Low: return kMideaACFanLow;
    case Fan::Medium: return kMideaACFanMed;
    case Fan::High: return kMideaACFanHigh;
    }
    return kMideaACFanAuto;
}

uint8_t to_gree_mode(Mode mode)
{
    switch (mode)
    {
    case Mode::Auto: return kGreeAuto;
    case Mode::Cool: return kGreeCool;
    case Mode::Heat: return kGreeHeat;
    case Mode::Dry: return kGreeDry;
    case Mode::Fan: return kGreeFan;
    }
    return kGreeCool;
}

uint8_t to_gree_fan(Fan fan)
{
    switch (fan)
    {
    case Fan::Auto: return kGreeFanAuto;
    case Fan::Low: return kGreeFanMin;
    case Fan::Medium: return kGreeFanMed;
    case Fan::High: return kGreeFanMax;
    }
    return kGreeFanAuto;
}

uint8_t to_haier_mode(Mode mode)
{
    switch (mode)
    {
    case Mode::Auto: return kHaierAcYrw02Auto;
    case Mode::Cool: return kHaierAcYrw02Cool;
    case Mode::Heat: return kHaierAcYrw02Heat;
    case Mode::Dry: return kHaierAcYrw02Dry;
    case Mode::Fan: return kHaierAcYrw02Fan;
    }
    return kHaierAcYrw02Cool;
}

uint8_t to_haier_fan(Fan fan)
{
    switch (fan)
    {
    case Fan::Auto: return kHaierAcYrw02FanAuto;
    case Fan::Low: return kHaierAcYrw02FanLow;
    case Fan::Medium: return kHaierAcYrw02FanMed;
    case Fan::High: return kHaierAcYrw02FanHigh;
    }
    return kHaierAcYrw02FanAuto;
}

template <typename Remote>
std::vector<uint32_t> collect_timings(Remote &remote)
{
    remote.begin();
    remote.sender_test().reset();
    remote.send();

    std::vector<uint32_t> timings;
    if (remote.sender_test().last == 0 && remote.sender_test().output[0] == 0)
    {
        return timings;
    }

    timings.reserve(remote.sender_test().last + 1);
    for (uint16_t i = 0; i <= remote.sender_test().last; ++i)
    {
        if (remote.sender_test().output[i] > 0)
        {
            timings.push_back(remote.sender_test().output[i]);
        }
    }
    return timings;
}

void apply_midea_state(IRMideaAC &remote, const State &state)
{
    remote.stateReset();
    remote.setPower(state.power);
    remote.setMode(to_midea_mode(state.mode));
    remote.setTemp(static_cast<uint8_t>(state.temp_c), true);
    remote.setFan(to_midea_fan(state.fan));
}

void apply_gree_state(IRGreeAC &remote, const State &state)
{
    remote.stateReset();
    remote.setPower(state.power);
    remote.setMode(to_gree_mode(state.mode));
    remote.setTemp(static_cast<uint8_t>(state.temp_c));
    remote.setFan(to_gree_fan(state.fan));
    remote.setSwingVertical(state.swing, state.swing ? kGreeSwingAuto : kGreeSwingLastPos);
}

void apply_haier_state(IRHaierACYRW02 &remote, const State &state)
{
    remote.stateReset();
    remote.setPower(state.power);
    remote.setMode(to_haier_mode(state.mode));
    remote.setTemp(static_cast<uint8_t>(state.temp_c));
    remote.setFan(to_haier_fan(state.fan));
    remote.setSwingV(state.swing ? kHaierAcYrw02SwingVAuto : kHaierAcYrw02SwingVOff);
}

BuildResult make_failure(std::size_t brand_index, const std::string &message)
{
    BuildResult result;
    result.brand_name = brand_name(brand_index);
    result.message = message;
    return result;
}

BuildResult build_midea(const State &before, const State &after, Action action)
{
    IRMideaAC remote(0);
    apply_midea_state(remote, before);

    switch (action)
    {
    case Action::SendState:
        apply_midea_state(remote, after);
        break;
    case Action::PowerToggle:
        remote.setPower(after.power);
        break;
    case Action::TempUp:
    case Action::TempDown:
        remote.setPower(after.power);
        remote.setTemp(static_cast<uint8_t>(after.temp_c), true);
        break;
    case Action::ApplyMode:
        remote.setPower(after.power);
        remote.setMode(to_midea_mode(after.mode));
        break;
    case Action::ApplyFan:
        remote.setPower(after.power);
        remote.setFan(to_midea_fan(after.fan));
        break;
    case Action::ToggleSwing:
        remote.setPower(after.power);
        remote.setSwingVToggle(true);
        break;
    }

    BuildResult result;
    result.brand_name = kBrandNames[0];
    result.raw_timings = collect_timings(remote);
    result.success = !result.raw_timings.empty();
    result.message = result.success ? "Midea command ready" : "Midea timing generation failed";
    log_build_result(kBrandNames[0], action, before, after, result);
    return result;
}

BuildResult build_gree(const State &before, const State &after, Action action)
{
    IRGreeAC remote(0);
    apply_gree_state(remote, before);

    switch (action)
    {
    case Action::SendState:
        apply_gree_state(remote, after);
        break;
    case Action::PowerToggle:
        remote.setPower(after.power);
        break;
    case Action::TempUp:
    case Action::TempDown:
        remote.setPower(after.power);
        remote.setTemp(static_cast<uint8_t>(after.temp_c));
        break;
    case Action::ApplyMode:
        remote.setPower(after.power);
        remote.setMode(to_gree_mode(after.mode));
        break;
    case Action::ApplyFan:
        remote.setPower(after.power);
        remote.setFan(to_gree_fan(after.fan));
        break;
    case Action::ToggleSwing:
        remote.setPower(after.power);
        remote.setSwingVertical(after.swing, after.swing ? kGreeSwingAuto : kGreeSwingLastPos);
        break;
    }

    BuildResult result;
    result.brand_name = kBrandNames[1];
    result.raw_timings = collect_timings(remote);
    result.success = !result.raw_timings.empty();
    result.message = result.success ? "Gree command ready" : "Gree timing generation failed";
    log_build_result(kBrandNames[1], action, before, after, result);
    return result;
}

void apply_haier_diff(IRHaierACYRW02 &remote, const State &before, const State &after)
{
    apply_haier_state(remote, before);
    bool changed = false;

    if (before.power != after.power)
    {
        remote.setPower(after.power);
        changed = true;
    }
    if (before.mode != after.mode)
    {
        remote.setMode(to_haier_mode(after.mode));
        changed = true;
    }
    if (before.temp_c != after.temp_c)
    {
        remote.setTemp(static_cast<uint8_t>(after.temp_c));
        changed = true;
    }
    if (before.fan != after.fan)
    {
        remote.setFan(to_haier_fan(after.fan));
        changed = true;
    }
    if (before.swing != after.swing)
    {
        remote.setSwingV(after.swing ? kHaierAcYrw02SwingVAuto : kHaierAcYrw02SwingVOff);
        changed = true;
    }

    if (!changed)
    {
        remote.setPower(after.power);
    }
}

BuildResult build_haier(const State &before, const State &after, Action action)
{
    IRHaierACYRW02 remote(0);

    switch (action)
    {
    case Action::SendState:
        apply_haier_diff(remote, before, after);
        break;
    case Action::PowerToggle:
        apply_haier_state(remote, before);
        remote.setPower(after.power);
        break;
    case Action::TempUp:
    case Action::TempDown:
        apply_haier_state(remote, before);
        remote.setPower(after.power);
        remote.setTemp(static_cast<uint8_t>(after.temp_c));
        break;
    case Action::ApplyMode:
        apply_haier_state(remote, before);
        remote.setPower(after.power);
        remote.setMode(to_haier_mode(after.mode));
        break;
    case Action::ApplyFan:
        apply_haier_state(remote, before);
        remote.setPower(after.power);
        remote.setFan(to_haier_fan(after.fan));
        break;
    case Action::ToggleSwing:
        apply_haier_state(remote, before);
        remote.setPower(after.power);
        remote.setSwingV(after.swing ? kHaierAcYrw02SwingVAuto : kHaierAcYrw02SwingVOff);
        break;
    }

    BuildResult result;
    result.brand_name = kBrandNames[2];
    result.raw_timings = collect_timings(remote);
    result.success = !result.raw_timings.empty();
    result.message = result.success ? "Haier command ready" : "Haier timing generation failed";
    log_build_result(kBrandNames[2], action, before, after, result);
    return result;
}

BuildResult build_coolix(const State &before, const State &after, Action action)
{
    (void)before;
    BuildResult result;
    result.brand_name = kBrandNames[3];

    uint32_t raw = kCoolixOff;
    switch (action)
    {
    case Action::ToggleSwing:
        raw = kCoolixSwing;
        break;
    case Action::PowerToggle:
        raw = after.power ? build_coolix_state_raw(after) : kCoolixOff;
        break;
    case Action::SendState:
        raw = after.power ? build_coolix_state_raw(after) : kCoolixOff;
        break;
    case Action::TempUp:
    case Action::TempDown:
    case Action::ApplyMode:
    case Action::ApplyFan:
        raw = after.mode == Mode::Fan ? kCoolixCmdFan : build_coolix_state_raw(after);
        break;
    }

    result.raw_timings = encode_coolix_timings(raw);
    result.success = !result.raw_timings.empty();
    result.message = result.success ? "Coolix command ready" : "Coolix timing generation failed";
    log_build_result(kBrandNames[3], action, before, after, result);
    return result;
}

}  // namespace

std::size_t brand_count()
{
    return kBrandNames.size();
}

const char *brand_name(std::size_t index)
{
    return kBrandNames[std::min(index, kBrandNames.size() - 1)];
}

Brand brand_from_index(std::size_t index)
{
    switch (std::min(index, kBrandNames.size() - 1))
    {
    case 0: return Brand::Midea;
    case 1: return Brand::Gree;
    case 2: return Brand::Haier;
    default: return Brand::Coolix;
    }
}

std::size_t brand_to_index(Brand brand)
{
    switch (brand)
    {
    case Brand::Midea: return 0;
    case Brand::Gree: return 1;
    case Brand::Haier: return 2;
    case Brand::Coolix: return 3;
    }
    return 0;
}

State clamp_state(State state)
{
    state.temp_c = clamp_temp(state.temp_c);
    return state;
}

State preview_state_after(const State &state, Action action)
{
    State next = clamp_state(state);
    switch (action)
    {
    case Action::SendState:
        break;
    case Action::PowerToggle:
        next.power = !next.power;
        break;
    case Action::TempUp:
        next.power = true;
        next.temp_c = clamp_temp(next.temp_c + 1);
        break;
    case Action::TempDown:
        next.power = true;
        next.temp_c = clamp_temp(next.temp_c - 1);
        break;
    case Action::ApplyMode:
    case Action::ApplyFan:
    case Action::ToggleSwing:
        next.power = true;
        break;
    }
    return next;
}

BuildResult build_command(const State &before, const State &after, Action action)
{
    const State safe_before = clamp_state(before);
    const State safe_after = clamp_state(after);
    const std::size_t index = brand_to_index(safe_after.brand);

    switch (safe_after.brand)
    {
    case Brand::Midea:
        return build_midea(safe_before, safe_after, action);
    case Brand::Gree:
        return build_gree(safe_before, safe_after, action);
    case Brand::Haier:
        return build_haier(safe_before, safe_after, action);
    case Brand::Coolix:
        return build_coolix(safe_before, safe_after, action);
    }
    return make_failure(index, "Unsupported air-conditioner brand");
}

}  // namespace aircon
