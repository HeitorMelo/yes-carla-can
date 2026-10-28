#pragma once

#include "CanCodec.h"
#include <vector>

namespace canlab {

struct Drive {
    unsigned driver_id = 0;
    bool dominant = false;
};

struct Resolution {
    bool dominant = false;
    std::vector<unsigned> dominant_drivers;
};

class CanBus {
public:
    // Ideal wired-AND: any dominant driver makes the sampled bus dominant.
    static Resolution resolve(const std::vector<Drive>& drives);
};

} // namespace canlab
