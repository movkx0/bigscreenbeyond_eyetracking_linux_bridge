#include "gaze.hpp"

#include <cmath>
#include <cstring>
#include <string_view>

namespace beyond {
namespace {

uint32_t word(const uint8_t *data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

bool string(const uint8_t *data, size_t size, size_t &offset, std::string_view &out) {
    const size_t start = offset;
    while (offset < size && data[offset]) ++offset;
    if (offset == size) return false;
    out = std::string_view(reinterpret_cast<const char *>(data + start), offset - start);
    const size_t end = (offset + 4) & ~size_t(3);
    if (end > size) return false;
    while (offset < end) if (data[offset++]) return false;
    return true;
}

float number(const uint8_t *data) {
    const uint32_t bits = word(data);
    float value;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool decode(const uint8_t *data, size_t size, Update &out, unsigned depth, unsigned &messages) {
    if (!size || size > 4096 || size % 4 || depth > 4) return false;
    if (size >= 8 && std::memcmp(data, "#bundle\0", 8) == 0) {
        if (size < 16) return false;
        // Only immediate OSC bundles are accepted. Future timetags require a
        // scheduler; publishing them on arrival would misrepresent sample age.
        if (word(data + 8) != 0 || word(data + 12) != 1) return false;
        size_t offset = 16;
        while (offset < size) {
            if (size - offset < 4) return false;
            const size_t length = word(data + offset);
            offset += 4;
            if (!length || length > size - offset || !decode(data + offset, length, out, depth + 1, messages))
                return false;
            offset += length;
        }
        return true;
    }
    if (++messages > 64) return false;
    size_t offset = 0;
    std::string_view address, types;
    if (!string(data, size, offset, address) || address.empty() || address[0] != '/' ||
        !string(data, size, offset, types) || types.empty() || types[0] != ',') return false;
    if (address == "/tracking/eye/LeftRightPitchYaw") {
        if (types != ",ffff" || size - offset != 16) return false;
        std::array<float, 4> angles{};
        for (unsigned i = 0; i < angles.size(); ++i) angles[i] = number(data + offset + 4 * i);
        std::array<float, 3> direction;
        if (!gaze_direction(angles, direction)) return false;
        out.angles = angles;
        out.has_gaze = true;
    } else if (address == "/tracking/eye/EyesClosedAmount") {
        if (types != ",f" || size - offset != 4) return false;
        const float value = number(data + offset);
        if (!std::isfinite(value) || value < 0 || value > 1) return false;
        out.eyes_closed = value;
        out.has_blink = true;
    }
    return true;
}

} // namespace

PacketResult decode_osc(const uint8_t *data, size_t size, Update &update) {
    Update candidate;
    unsigned messages = 0;
    if (!data || !decode(data, size, candidate, 0, messages)) return PacketResult::malformed;
    if (!candidate.has_gaze && !candidate.has_blink) return PacketResult::ignored;
    update = candidate;
    return PacketResult::accepted;
}

bool gaze_direction(const std::array<float, 4> &angles, std::array<float, 3> &direction) {
    for (float angle : angles) if (!std::isfinite(angle) || std::abs(angle) > 90) return false;
    constexpr double radians = 3.14159265358979323846 / 180.0;
    std::array<double, 3> sum{};
    for (unsigned eye = 0; eye < 2; ++eye) {
        const double pitch = angles[eye * 2] * radians;
        const double yaw = angles[eye * 2 + 1] * radians;
        // VRChat positive pitch looks down. OpenVR has +Y up, +X right,
        // and forward along -Z. Average the two eye rays, then normalize.
        sum[0] += std::sin(yaw) * std::cos(pitch);
        sum[1] -= std::sin(pitch);
        sum[2] -= std::cos(yaw) * std::cos(pitch);
    }
    const double length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
    if (length < 0.00001) return false;
    for (unsigned i = 0; i < 3; ++i) direction[i] = static_cast<float>(sum[i] / length);
    return true;
}

PacketResult GazeState::ingest(const uint8_t *data, size_t size, int64_t now_ns) {
    Update update;
    const auto result = decode_osc(data, size, update);
    if (result == PacketResult::malformed) ++rejected_;
    if (result != PacketResult::accepted) return result;
    if (update.has_gaze) {
        gaze_direction(update.angles, direction_);
        gaze_time_ = now_ns;
        ++accepted_;
    }
    if (update.has_blink) {
        eyes_closed_ = update.eyes_closed;
        blink_time_ = now_ns;
    }
    return result;
}

bool GazeState::sample(int64_t now_ns, std::array<float, 3> &direction) const {
    direction = {0, 0, -1};
    if (gaze_time_ < 0 || now_ns < gaze_time_ || now_ns - gaze_time_ >= timeout_ns_) return false;
    if (blink_time_ >= 0 && now_ns >= blink_time_ && now_ns - blink_time_ < timeout_ns_ && eyes_closed_ >= 0.95f)
        return false;
    direction = direction_;
    return true;
}

} // namespace beyond
