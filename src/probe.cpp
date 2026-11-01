#include <openvr.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char **argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 1;
    if (seconds < 1 || seconds > 60) return 2;
    vr::EVRInitError error;
    auto *system = vr::VR_Init(&error, vr::VRApplication_Background);
    if (error != vr::VRInitError_None) {
        std::fprintf(stderr, "SteamVR initialization failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        return 1;
    }
    char serial[256]{};
    system->GetStringTrackedDeviceProperty(0, vr::Prop_SerialNumber_String, serial, sizeof(serial));
    const bool supported = system->GetBoolTrackedDeviceProperty(0, vr::Prop_SupportsXrEyeGazeInteraction_Bool);
    const uint32_t scene = vr::VRApplications()->GetCurrentSceneProcessId();
    std::printf("{\"hmd\":\"");
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(serial); *p; ++p) {
        if (*p < 32 || *p == '"' || *p == '\\') std::printf("\\u%04x", *p);
        else std::putchar(*p);
    }
    std::printf("\",\"supports_eye_gaze\":%s,\"scene_pid\":%u}\n", supported ? "true" : "false", scene);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < seconds * 20; ++i) {
        vr::HmdVector2_t left{}, right{};
        const bool valid = system->GetEyeTrackedFoveationCenter(&left, &right);
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("{\"time\":%.3f,\"valid\":%s,\"left\":[%.6f,%.6f],\"right\":[%.6f,%.6f]}\n",
                    elapsed, valid ? "true" : "false", left.v[0], left.v[1], right.v[0], right.v[1]);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    vr::VR_Shutdown();
}
