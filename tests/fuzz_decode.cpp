#include "gaze.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    beyond::GazeState state;
    state.ingest(data, size, 1000000000);
    std::array<float, 3> direction;
    state.sample(1000000001, direction);
    state.sample(2000000000, direction);
    return 0;
}
