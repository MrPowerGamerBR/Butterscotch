#include <stdio.h>
#include <string.h>
#include <loadfile.h>
#include "../utils.h"
#include "ps2_utils.h"

PS2DeviceKey deviceKey;
bool deviceKeyLoaded = false;

void PS2Utils_extractDeviceKey(const char* path) {
    require(!deviceKeyLoaded);

    char* pos = strchr(path, ':');
    requireNotNull(pos);

    size_t length = pos - path;
    char* result = safeMalloc((length + 1) * sizeof(char));
    strncpy(result, path, length);
    result[length] = '\0';

    // The "result" is the device key as a string (example: "mass" or "host")
    deviceKey = (PS2DeviceKey) {
        .key = result,
        .usesISO9660 = strncmp(result, "cdrom", strlen("cdrom")) == 0,
        .usesHost = strncmp(result, "host", strlen("host")) == 0,
    };

    deviceKeyLoaded = true;
}

// Creates a path with the device key + path for the loaded device key
// You need to free after using the path!
char* PS2Utils_createDevicePath(const char* path) {
    require(deviceKeyLoaded);

    if (deviceKey.usesISO9660) {
        size_t len = strlen(deviceKey.key) + 3 + strlen(path) + 2 + 1;
        char* devicePath = safeMalloc(len);
        snprintf(devicePath, len, "%s:\\%s;1", deviceKey.key, path);
        return devicePath;
    } else if (deviceKey.usesHost) {
        // Paths are naturally handled relative to the parent directory
        // of the original ELF.
        size_t len = strlen(path) + 1;
        char* devicePath = safeMalloc(len);
        snprintf(devicePath, len, "%s", path);
        return devicePath;
    } else {
        size_t len = strlen(deviceKey.key) + 1 + strlen(path) + 1;
        char* devicePath = safeMalloc(len);
        snprintf(devicePath, len, "%s:%s", deviceKey.key, path);
        return devicePath;
    }
}
