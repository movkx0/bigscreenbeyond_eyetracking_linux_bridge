#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace beyond {

enum class PacketResult { ignored, accepted, malformed };

struct Update {
    bool has_gaze = false;
    bool has_blink = false;
    std::array<float, 4> angles{}; // left pitch/yaw, right pitch/yaw, degrees
    float eyes_closed = 0;
};

// A malformed bundle never applies a partial update. Unknown OSC addresses are
// ignored, including the other expression messages Baballonia sends.
PacketResult decode_osc(const uint8_t *data, size_t size, Update &update);
bool gaze_direction(const std::array<float, 4> &angles, std::array<float, 3> &direction);

class GazeState {
public:
    explicit GazeState(int64_t timeout_ns = 250000000) : timeout_ns_(timeout_ns) {}
    PacketResult ingest(const uint8_t *data, size_t size, int64_t now_ns);
    bool sample(int64_t now_ns, std::array<float, 3> &direction) const;
    uint64_t accepted() const { return accepted_; }
    uint64_t rejected() const { return rejected_; }

private:
    int64_t timeout_ns_;
    int64_t gaze_time_ = -1;
    int64_t blink_time_ = -1;
    std::array<float, 3> direction_{0, 0, -1};
    float eyes_closed_ = 0;
    uint64_t accepted_ = 0;
    uint64_t rejected_ = 0;
};

} // namespace beyond
