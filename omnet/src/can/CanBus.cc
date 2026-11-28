#include "CanBus.h"

namespace canlab {

Resolution CanBus::resolve(const std::vector<Drive>& drives) {
    Resolution result;
    for (const auto& drive : drives) {
        if (drive.dominant) {
            result.dominant = true;
            result.dominant_drivers.push_back(drive.driver_id);
        }
    }
    return result;
}

} // namespace canlab
