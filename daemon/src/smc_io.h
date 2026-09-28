// smc_io.h — minimal AppleSMC user-client access (read/write SMC keys).
// Header-only; used by gpufand.
#pragma once
#include <stdint.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

enum { kSMCHandleYPCEvent = 2 };
enum { kSMCReadKey = 5, kSMCWriteKey = 6, kSMCGetKeyInfo = 9 };

typedef struct { char major, minor, build, reserved[1]; uint16_t release; } SMCVers;
typedef struct { uint16_t version, length; uint32_t cpuPLimit, gpuPLimit, memPLimit; } SMCPLimit;
typedef struct { uint32_t dataSize; uint32_t dataType; uint8_t dataAttributes; } SMCKeyInfo;
typedef struct {
    uint32_t key;
    SMCVers vers;
    SMCPLimit pLimitData;
    SMCKeyInfo keyInfo;
    char result;
    char status;
    uint8_t data8;
    uint32_t data32;
    uint8_t bytes[32];
} SMCParam;

static io_connect_t smc_conn = IO_OBJECT_NULL;

static inline uint32_t smc_fourcc(const char *s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
}

static inline int smc_open(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) return -1;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &smc_conn);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS ? 0 : -1;
}

static inline int smc_call(SMCParam *in, SMCParam *out) {
    size_t os = sizeof(SMCParam);
    memset(out, 0, sizeof(*out));
    kern_return_t kr = IOConnectCallStructMethod(smc_conn, kSMCHandleYPCEvent, in, sizeof(SMCParam), out, &os);
    if (kr != KERN_SUCCESS) return -1000;
    return out->result ? -(int)(uint8_t)out->result : 0;
}

// Returns 0 on success; fills size/type.
static inline int smc_info(const char *key, uint32_t *size, uint32_t *type) {
    SMCParam in, out; memset(&in, 0, sizeof(in));
    in.key = smc_fourcc(key); in.data8 = kSMCGetKeyInfo;
    int rc = smc_call(&in, &out);
    if (rc) return rc;
    if (size) *size = out.keyInfo.dataSize;
    if (type) *type = out.keyInfo.dataType;
    return 0;
}

static inline int smc_read_raw(const char *key, uint8_t *buf, uint32_t *size, uint32_t *type) {
    uint32_t sz, t;
    int rc = smc_info(key, &sz, &t);
    if (rc) return rc;
    if (sz == 0 || sz > 32) return -100;
    SMCParam in, out; memset(&in, 0, sizeof(in));
    in.key = smc_fourcc(key); in.data8 = kSMCReadKey; in.keyInfo.dataSize = sz;
    rc = smc_call(&in, &out);
    if (rc) return rc;
    memcpy(buf, out.bytes, sz);
    if (size) *size = sz;
    if (type) *type = t;
    return 0;
}

static inline int smc_write_raw(const char *key, const uint8_t *buf, uint32_t size) {
    SMCParam in, out; memset(&in, 0, sizeof(in));
    in.key = smc_fourcc(key); in.data8 = kSMCWriteKey; in.keyInfo.dataSize = size;
    memcpy(in.bytes, buf, size);
    return smc_call(&in, &out);
}

// Decode common numeric SMC types. Returns 0 on success.
static inline int smc_read_num(const char *key, double *val) {
    uint8_t b[32]; uint32_t s, t;
    int rc = smc_read_raw(key, b, &s, &t);
    if (rc) return rc;
    if (t == smc_fourcc("sp78") && s == 2) *val = (int16_t)((b[0] << 8) | b[1]) / 256.0;
    else if (t == smc_fourcc("fpe2") && s == 2) *val = ((b[0] << 8) | b[1]) / 4.0;
    else if (t == smc_fourcc("flt ") && s == 4) { float f; memcpy(&f, b, 4); *val = f; } // VirtualSMC flt: host order
    else if (t == smc_fourcc("ui8 ") && s == 1) *val = b[0];
    else if (t == smc_fourcc("ui16") && s == 2) *val = (b[0] << 8) | b[1];
    else return -101;
    return 0;
}

static inline int smc_write_fpe2(const char *key, double v) {
    if (v < 0) v = 0;
    if (v > 16383) v = 16383;
    uint16_t r = (uint16_t)(v * 4.0 + 0.5);
    uint8_t b[2] = { (uint8_t)(r >> 8), (uint8_t)r };
    return smc_write_raw(key, b, 2);
}

static inline int smc_write_ui8(const char *key, uint8_t v) {
    return smc_write_raw(key, &v, 1);
}
