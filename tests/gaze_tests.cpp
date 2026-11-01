#include "gaze.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (false)
using Bytes = std::vector<uint8_t>;

void string(Bytes &out, const std::string &value) {
    out.insert(out.end(), value.begin(), value.end());
    out.push_back(0);
    while (out.size() % 4) out.push_back(0);
}
void word(Bytes &out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(value >> shift));
}
Bytes message(const std::string &address, const std::vector<float> &values) {
    Bytes out;
    string(out, address);
    string(out, "," + std::string(values.size(), 'f'));
    for (float value : values) { uint32_t bits; std::memcpy(&bits, &value, 4); word(out, bits); }
    return out;
}
Bytes gaze(float pitch, float yaw) { return message("/tracking/eye/LeftRightPitchYaw", {pitch, yaw, pitch, yaw}); }
Bytes bundle(const std::vector<Bytes> &packets) {
    Bytes out{'#','b','u','n','d','l','e',0};
    word(out, 0); word(out, 1);
    for (const auto &packet : packets) { word(out, packet.size()); out.insert(out.end(), packet.begin(), packet.end()); }
    return out;
}

int main() {
    using beyond::PacketResult;
    std::array<float, 3> direction;
    beyond::GazeState state;
    CHECK(!state.sample(0, direction));
    auto front = gaze(0, 0);
    CHECK(state.ingest(front.data(), front.size(), 0) == PacketResult::accepted);
    CHECK(state.sample(1, direction) && direction[0] == 0 && direction[1] == 0 && direction[2] == -1);
    CHECK(state.sample(249999999, direction));
    CHECK(!state.sample(250000000, direction));
    auto right = gaze(0, 30);
    state.ingest(right.data(), right.size(), 300000000);
    CHECK(state.sample(300000001, direction) && std::abs(direction[0] - 0.5f) < 0.00001f && direction[2] < 0);
    auto up = gaze(-30, 0);
    state.ingest(up.data(), up.size(), 400000000);
    CHECK(state.sample(400000001, direction) && std::abs(direction[1] - 0.5f) < 0.00001f);

    auto unrelated = message("/avatar/parameters/cheekPuffLeft", {1});
    CHECK(state.ingest(unrelated.data(), unrelated.size(), 500000000) == PacketResult::ignored);
    CHECK(!state.sample(650000000, direction)); // unrelated OSC cannot keep old gaze alive
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 91.0f}) {
        auto packet = gaze(invalid, 0);
        CHECK(state.ingest(packet.data(), packet.size(), 700000000) == PacketResult::malformed);
        CHECK(!state.sample(700000000, direction));
    }
    for (size_t size = 0; size < front.size(); ++size) {
        beyond::Update update;
        CHECK(beyond::decode_osc(front.data(), size, update) == PacketResult::malformed);
    }
    auto wrong_types = message("/tracking/eye/LeftRightPitchYaw", {1, 2, 3});
    CHECK(state.ingest(wrong_types.data(), wrong_types.size(), 700000000) == PacketResult::malformed);
    auto malformed_bundle = bundle({front, Bytes{1,2,3,4}});
    CHECK(state.ingest(malformed_bundle.data(), malformed_bundle.size(), 800000000) == PacketResult::malformed);
    CHECK(!state.sample(800000000, direction)); // no partial application of the first element

    auto closed = message("/tracking/eye/EyesClosedAmount", {1});
    auto open = message("/tracking/eye/EyesClosedAmount", {0});
    auto packet = bundle({closed, front});
    CHECK(state.ingest(packet.data(), packet.size(), 900000000) == PacketResult::accepted);
    CHECK(!state.sample(900000001, direction));
    packet = bundle({open, front, unrelated});
    CHECK(state.ingest(packet.data(), packet.size(), 1000000000) == PacketResult::accepted);
    CHECK(state.sample(1000000001, direction));
    CHECK(!state.sample(999999999, direction)); // monotonic clock rollback must not extend validity
    for (int i = 0; i < 6; ++i) packet = bundle({packet});
    CHECK(state.ingest(packet.data(), packet.size(), 1200000000) == PacketResult::malformed);
    packet = bundle({front}); packet[15] = 2; // a future OSC timetag isn't an immediate sample
    CHECK(state.ingest(packet.data(), packet.size(), 1300000000) == PacketResult::malformed);
    CHECK(!state.sample(1300000000, direction));
    CHECK(!beyond::gaze_direction({0, -90, 0, 90}, direction));
    CHECK(beyond::gaze_direction({0, -10, 0, 10}, direction) && std::abs(direction[0]) < 0.00001f && direction[2] < -0.999f);
    std::puts("OSC parsing, gaze axes, bundles, invalid samples, blinking, and timeout checks passed.");
}
