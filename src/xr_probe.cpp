#include <openxr/openxr.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
bool check(XrResult result, const char *operation) {
    if (XR_FAILED(result)) {
        std::fprintf(stderr, "%s failed: OpenXR result %d\n", operation, result);
        return false;
    }
    return true;
}
void string(const char *value) {
    std::putchar('"');
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
        if (*p < 32 || *p == '"' || *p == '\\') std::printf("\\u%04x", *p);
        else std::putchar(*p);
    }
    std::putchar('"');
}
}

int main() {
    uint32_t count = 0;
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "Enumerate extensions")) return 1;
    std::vector<XrExtensionProperties> extensions(count);
    for (auto &extension : extensions) extension.type = XR_TYPE_EXTENSION_PROPERTIES;
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()), "Enumerate extensions")) return 1;
    bool found = false;
    for (const auto &extension : extensions)
        if (!std::strcmp(extension.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) found = true;
    if (!found) {
        std::puts("{\"extension_available\":false,\"supports_eye_gaze\":false}");
        return 1;
    }
    const char *enabled[] = {XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
    XrInstanceCreateInfo create{};
    create.type = XR_TYPE_INSTANCE_CREATE_INFO;
    std::strcpy(create.applicationInfo.applicationName, "Beyond gaze capability check");
    create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    create.enabledExtensionCount = 1;
    create.enabledExtensionNames = enabled;
    XrInstance instance;
    if (!check(xrCreateInstance(&create, &instance), "Create OpenXR instance")) return 1;
    XrInstanceProperties runtime{};
    runtime.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (!check(xrGetInstanceProperties(instance, &runtime), "Get runtime properties")) {
        xrDestroyInstance(instance); return 1;
    }
    XrSystemGetInfo get{};
    get.type = XR_TYPE_SYSTEM_GET_INFO;
    get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system;
    if (!check(xrGetSystem(instance, &get, &system), "Get HMD system")) {
        xrDestroyInstance(instance); return 1;
    }
    XrSystemEyeGazeInteractionPropertiesEXT gaze{};
    gaze.type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT;
    XrSystemProperties properties{};
    properties.type = XR_TYPE_SYSTEM_PROPERTIES;
    properties.next = &gaze;
    if (!check(xrGetSystemProperties(instance, system, &properties), "Get gaze support")) {
        xrDestroyInstance(instance); return 1;
    }
    std::printf("{\"runtime\":"); string(runtime.runtimeName);
    std::printf(",\"system\":"); string(properties.systemName);
    std::printf(",\"extension_available\":true,\"supports_eye_gaze\":%s}\n",
                gaze.supportsEyeGazeInteraction ? "true" : "false");
    xrDestroyInstance(instance);
    return gaze.supportsEyeGazeInteraction ? 0 : 1;
}
