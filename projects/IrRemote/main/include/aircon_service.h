#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aircon {

enum class Brand {
    Midea,
    Gree,
    Haier,
    Coolix,
};

enum class Mode {
    Auto,
    Cool,
    Heat,
    Dry,
    Fan,
};

enum class Fan {
    Auto,
    Low,
    Medium,
    High,
};

enum class Action {
    SendState,
    PowerToggle,
    TempUp,
    TempDown,
    ApplyMode,
    ApplyFan,
    ToggleSwing,
};

struct State {
    Brand brand = Brand::Midea;
    bool power = false;
    int temp_c = 24;
    Mode mode = Mode::Cool;
    Fan fan = Fan::Auto;
    bool swing = false;
};

struct BuildResult {
    bool success = false;
    std::vector<uint32_t> raw_timings;
    std::string brand_name;
    std::string message;
};

std::size_t brand_count();
const char *brand_name(std::size_t index);
Brand brand_from_index(std::size_t index);
std::size_t brand_to_index(Brand brand);

State clamp_state(State state);
State preview_state_after(const State &state, Action action);
BuildResult build_command(const State &before, const State &after, Action action);

}  // namespace aircon
