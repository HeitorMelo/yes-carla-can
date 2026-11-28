#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>

namespace canlab {

// Phase 1 supports Classical CAN standard-identifier data frames only.
struct CanFrame {
    std::uint16_t id = 0;
    bool extended = false;
    bool remote = false;
    std::uint8_t dlc = 0;
    std::array<std::uint8_t, 8> data{};

    void validate() const {
        if (extended) throw std::invalid_argument("extended CAN frames are unsupported");
        if (remote) throw std::invalid_argument("remote CAN frames are unsupported");
        if (id > 0x7ff) throw std::invalid_argument("standard CAN identifier exceeds 11 bits");
        if (dlc > 8) throw std::invalid_argument("Classical CAN DLC exceeds 8");
    }
};

} // namespace canlab
