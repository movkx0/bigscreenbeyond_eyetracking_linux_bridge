#include "gaze.hpp"
#include <openvr_driver.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *section = "driver_beyond_gaze";

int64_t now_ns() {
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return int64_t(time.tv_sec) * 1000000000 + time.tv_nsec;
}

void log(const char *format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (vr::VRDriverLog()) vr::VRDriverLog()->Log(message);
}

class Driver final : public vr::IServerTrackedDeviceProvider {
public:
    vr::EVRInitError Init(vr::IVRDriverContext *context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        vr::EVRSettingsError setting_error;
        int port = vr::VRSettings()->GetInt32(section, "port", &setting_error);
        if (setting_error != vr::VRSettingsError_None) port = 9020;
        if (port < 1024 || port > 65535) return vr::VRInitError_Driver_Failed;
        int timeout = vr::VRSettings()->GetInt32(section, "timeout_ms", &setting_error);
        if (setting_error != vr::VRSettingsError_None) timeout = 250;
        if (timeout < 50 || timeout > 1000) return vr::VRInitError_Driver_Failed;
        state_ = beyond::GazeState(int64_t(timeout) * 1000000);
        vr::VRSettings()->GetString(section, "serial", serial_filter_, sizeof(serial_filter_), &setting_error);
        if (setting_error != vr::VRSettingsError_None) serial_filter_[0] = 0;
        socket_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<uint16_t>(port));
        if (socket_ < 0 || bind(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            log("Cannot bind OSC receiver to 127.0.0.1:%d: %s", port, std::strerror(errno));
            if (socket_ >= 0) close(socket_);
            socket_ = -1;
            return vr::VRInitError_Driver_Failed;
        }
        log("Linux OSC gaze bridge listening on 127.0.0.1:%d; timeout=%d ms; waiting for Beyond HMD", port, timeout);
        return vr::VRInitError_None;
    }

    void Cleanup() override {
        if (component_) {
            vr::VREyeTrackingData_t inactive{};
            inactive.vGazeTarget.v[2] = -1;
            vr::VRDriverInput()->UpdateEyeTrackingComponent(component_, &inactive, 0);
        }
        if (socket_ >= 0) close(socket_);
        socket_ = -1;
        component_ = 0;
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

    void RunFrame() override {
        const auto now = now_ns();
        if (!component_ && !attach_failed_ && now >= next_attach_) {
            next_attach_ = now + 500000000;
            attach();
        }
        // Bound work per frame so a busy local sender cannot starve tracking.
        for (unsigned count = 0; count < 64 && socket_ >= 0; ++count) {
            uint8_t packet[4096];
            const ssize_t size = recv(socket_, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
            if (size < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (size > static_cast<ssize_t>(sizeof(packet))) continue;
            state_.ingest(packet, static_cast<size_t>(size), now);
        }
        if (!component_) return;
        std::array<float, 3> direction;
        const bool valid = state_.sample(now, direction);
        vr::VREyeTrackingData_t data{};
        data.bActive = data.bValid = data.bTracked = valid;
        for (unsigned i = 0; i < 3; ++i) data.vGazeTarget.v[i] = direction[i];
        const auto error = vr::VRDriverInput()->UpdateEyeTrackingComponent(component_, &data, 0);
        if (error != last_error_) {
            log("UpdateEyeTrackingComponent result=%d", error);
            last_error_ = error;
        }
        if (valid != last_valid_) {
            log("Gaze %s; accepted=%llu rejected=%llu", valid ? "active" : "inactive (timeout or closed eyes)",
                static_cast<unsigned long long>(state_.accepted()), static_cast<unsigned long long>(state_.rejected()));
            last_valid_ = valid;
        }
    }

private:
    void attach() {
        const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(vr::k_unTrackedDeviceIndex_Hmd);
        if (!container) return;
        char model[256]{}, serial[256]{};
        vr::ETrackedPropertyError error;
        vr::VRProperties()->GetStringProperty(container, vr::Prop_ModelNumber_String, model, sizeof(model), &error);
        if (error != vr::TrackedProp_Success || !model[0]) return;
        vr::VRProperties()->GetStringProperty(container, vr::Prop_SerialNumber_String, serial, sizeof(serial), &error);
        if (error != vr::TrackedProp_Success || !serial[0]) return;
        if (!std::strstr(model, "Beyond") || (serial_filter_[0] && std::strcmp(serial_filter_, serial))) {
            log("Not attaching to HMD model=%s serial=%s: it does not match the configured Beyond", model, serial);
            attach_failed_ = true;
            return;
        }
        if (vr::VRProperties()->GetBoolProperty(container, vr::Prop_SupportsXrEyeGazeInteraction_Bool, &error) &&
            error == vr::TrackedProp_Success) {
            log("HMD already advertises eye tracking; refusing to replace its gaze source");
            attach_failed_ = true;
            return;
        }
        const auto created = vr::VRDriverInput()->CreateEyeTrackingComponent(container, "/eyetracking", &component_);
        log("Attach model=%s serial=%s container=%llu: CreateEyeTrackingComponent=%d component=%llu", model, serial,
            static_cast<unsigned long long>(container), created, static_cast<unsigned long long>(component_));
        if (created != vr::VRInputError_None || !component_) {
            component_ = 0;
            attach_failed_ = true;
            return;
        }
        error = vr::VRProperties()->SetBoolProperty(container, vr::Prop_SupportsXrEyeGazeInteraction_Bool, true);
        log("Advertise eye gaze support result=%d", error);
        if (error != vr::TrackedProp_Success) {
            component_ = 0;
            attach_failed_ = true;
        }
    }

    int socket_ = -1;
    vr::VRInputComponentHandle_t component_ = 0;
    beyond::GazeState state_;
    char serial_filter_[128]{};
    int64_t next_attach_ = 0;
    bool attach_failed_ = false;
    bool last_valid_ = false;
    vr::EVRInputError last_error_ = vr::VRInputError_None;
};

Driver driver;

} // namespace

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name, int *error) {
    if (name && std::strcmp(name, vr::IServerTrackedDeviceProvider_Version) == 0) return &driver;
    if (error) *error = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
