// smc — минимальный CLI для чтения/записи SMC-ключей (AppleSMC user client).
// Build: cc -O2 -o smc smc.c -framework IOKit
//
// Usage:
//   smc r KEY             прочитать и декодировать значение
//   smc i KEY             только info: тип, размер, сырые байты
//   smc w KEY VALUE       записать (VALUE: число → кодируется по типу ключа,
//                         или hex:0100 → сырые байты; нужен root)
//   smc l [PREFIX]        перечислить все ключи (или начинающиеся с PREFIX)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>

enum { kSMCUCClose = 1, kSMCHandleYPCEvent = 2 };
enum { kSMCReadKey = 5, kSMCWriteKey = 6, kSMCGetKeyFromIndex = 8, kSMCGetKeyInfo = 9 };

typedef unsigned char SMCBytes_t[32];
typedef struct { char major, minor, build, reserved[1]; uint16_t release; } SMCKeyDescriptor_vers_t;
typedef struct { uint16_t version, length; uint32_t cpuPLimit, gpuPLimit, memPLimit; } SMCKeyDescriptor_pLimit_t;
typedef struct { uint32_t dataSize; uint32_t dataType; uint8_t dataAttributes; } SMCKeyDescriptor_keyInfo_t;
typedef struct {
    uint32_t key;
    SMCKeyDescriptor_vers_t vers;
    SMCKeyDescriptor_pLimit_t pLimitData;
    SMCKeyDescriptor_keyInfo_t keyInfo;
    char result;
    char status;
    uint8_t data8;
    uint32_t data32;
    SMCBytes_t bytes;
} SMCParamStruct;

static io_connect_t g_conn = IO_OBJECT_NULL;
static kern_return_t g_lastKr = KERN_SUCCESS;

static uint32_t fourcc(const char *s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
}
static void unfourcc(uint32_t k, char out[5]) {
    out[0] = (k >> 24) & 0xff; out[1] = (k >> 16) & 0xff;
    out[2] = (k >> 8) & 0xff;  out[3] = k & 0xff; out[4] = 0;
    for (int i = 0; i < 4; i++) if (out[i] < 32 || out[i] > 126) out[i] = '?';
}

static int openSMC(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) return -1;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    return kr == KERN_SUCCESS ? 0 : -1;
}

static SMCParamStruct smcCall(SMCParamStruct *in) {
    SMCParamStruct out; memset(&out, 0, sizeof(out));
    size_t is = sizeof(SMCParamStruct), os = sizeof(SMCParamStruct);
    g_lastKr = IOConnectCallStructMethod(g_conn, kSMCHandleYPCEvent, in, is, &out, &os);
    return out;
}

static SMCParamStruct getInfo(uint32_t key) {
    SMCParamStruct in; memset(&in, 0, sizeof(in));
    in.key = key; in.data8 = kSMCGetKeyInfo;
    return smcCall(&in);
}

static int readBytes(uint32_t key, uint32_t typeHint, uint8_t *bytes, uint32_t *size, uint32_t *typeOut) {
    SMCParamStruct info = getInfo(key);
    if (info.result) return -(int)info.result;
    uint32_t sz = info.keyInfo.dataSize;
    if (sz == 0 || sz > 32) return -100;
    SMCParamStruct in; memset(&in, 0, sizeof(in));
    in.key = key; in.data8 = kSMCReadKey; in.keyInfo.dataSize = sz;
    SMCParamStruct out = smcCall(&in);
    if (g_lastKr != KERN_SUCCESS) return -1000;
    if (out.result) return -(int)out.result;
    memcpy(bytes, out.bytes, sz);
    if (size) *size = sz;
    if (typeOut) *typeOut = info.keyInfo.dataType;
    (void)typeHint;
    return 0;
}

// Returns 0, or -result for an SMC-level error, or -1000 when the IOKit call itself
// failed (g_lastKr holds the reason; writes need root -> kIOReturnNotPrivileged).
static int writeBytes(uint32_t key, uint32_t type, const uint8_t *bytes, uint32_t size) {
    SMCParamStruct in; memset(&in, 0, sizeof(in));
    in.key = key;
    in.keyInfo.dataSize = size;
    in.keyInfo.dataType = type;
    in.data8 = kSMCWriteKey;
    memcpy(in.bytes, bytes, size);
    SMCParamStruct out = smcCall(&in);
    if (g_lastKr != KERN_SUCCESS) return -1000;
    return out.result ? -(int)out.result : 0;
}

static double decodeVal(uint32_t t, const uint8_t *b, uint32_t s) {
    if (t == fourcc("sp78") && s >= 2) return (double)(int16_t)((b[0] << 8) | b[1]) / 256.0;
    if (t == fourcc("fpe2") && s >= 2) return (double)(int16_t)((b[0] << 8) | b[1]) / 4.0;
    if (t == fourcc("flt ") && s >= 4) { float f; uint8_t x[4] = {b[3], b[2], b[1], b[0]}; memcpy(&f, x, 4); return (double)f; }
    if (t == fourcc("ui8 ") && s >= 1) return (double)b[0];
    if (t == fourcc("ui16") && s >= 2) return (double)((b[0] << 8) | b[1]);
    if (t == fourcc("ui32") && s >= 4) return (double)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]);
    if (t == fourcc("si8 ") && s >= 1) return (double)(int8_t)b[0];
    if (t == fourcc("si16") && s >= 2) return (double)(int16_t)((b[0] << 8) | b[1]);
    return NAN;
}

// encode numeric value by key type → bytes; returns size or -1
static int encodeVal(uint32_t t, double v, uint8_t *out) {
    if (t == fourcc("sp78")) { int16_t r = (int16_t)(v * 256.0); out[0] = (r >> 8) & 0xff; out[1] = r & 0xff; return 2; }
    if (t == fourcc("fpe2")) { int16_t r = (int16_t)(v * 4.0);   out[0] = (r >> 8) & 0xff; out[1] = r & 0xff; return 2; }
    if (t == fourcc("ui8 ")) { out[0] = (uint8_t)v; return 1; }
    if (t == fourcc("ui16")) { out[0] = ((uint16_t)v >> 8) & 0xff; out[1] = (uint16_t)v & 0xff; return 2; }
    if (t == fourcc("ui32")) { uint32_t u = (uint32_t)v; out[0] = (u >> 24) & 0xff; out[1] = (u >> 16) & 0xff; out[2] = (u >> 8) & 0xff; out[3] = u & 0xff; return 4; }
    if (t == fourcc("flt ")) { float f = (float)v; uint8_t *p = (uint8_t *)&f; out[0] = p[3]; out[1] = p[2]; out[2] = p[1]; out[3] = p[0]; return 4; }
    if (t == fourcc("si8 ")) { out[0] = (uint8_t)(int8_t)v; return 1; }
    if (t == fourcc("si16")) { int16_t r = (int16_t)v; out[0] = (r >> 8) & 0xff; out[1] = r & 0xff; return 2; }
    return -1;
}

static int parseHexBytes(const char *s, uint8_t *out, int max) {
    int n = 0;
    while (*s && n < max) {
        if (s[0] == 0 || !s[1]) return -1;
        char c[3] = {s[0], s[1], 0};
        out[n++] = (uint8_t)strtol(c, NULL, 16);
        s += 2;
    }
    return n;
}

// Перечисление: #KEY -> количество, затем ключ по индексу.
static int listKeys(const char *prefix) {
    uint8_t b[32]; uint32_t sz = 0, ty = 0;
    if (readBytes(fourcc("#KEY"), 0, b, &sz, &ty) || sz < 4) {
        fprintf(stderr, "cannot read #KEY\n");
        return 1;
    }
    uint32_t count = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    size_t plen = prefix ? strlen(prefix) : 0;
    printf("# %u keys\n", count);
    for (uint32_t i = 0; i < count; i++) {
        SMCParamStruct in; memset(&in, 0, sizeof(in));
        in.data8 = kSMCGetKeyFromIndex; in.data32 = i;
        SMCParamStruct out = smcCall(&in);
        if (g_lastKr != KERN_SUCCESS || out.result) continue;
        char name[5]; unfourcc(out.key, name);
        if (plen && strncmp(name, prefix, plen)) continue;
        SMCParamStruct info = getInfo(out.key);
        char t[5]; unfourcc(info.keyInfo.dataType, t);
        printf("%-4s %-4s %2u 0x%02x", name, t, info.keyInfo.dataSize, info.keyInfo.dataAttributes);
        uint32_t rs = 0, rt = 0;
        if (readBytes(out.key, 0, b, &rs, &rt) == 0) {
            double v = decodeVal(rt, b, rs);
            if (!isnan(v)) printf("  %.2f", v);
            else { printf("  "); for (uint32_t j = 0; j < rs && j < 16; j++) printf("%02x", b[j]); }
        }
        printf("\n");
    }
    return 0;
}

static int checkKeyArg(const char *k) {
    if (strlen(k) != 4) { fprintf(stderr, "key must be 4 chars: '%s'\n", k); return -1; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s r|w|i KEY [VALUE] | %s l [PREFIX]\n", argv[0], argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "l")) {
        if (openSMC()) { fprintf(stderr, "cannot open AppleSMC\n"); return 1; }
        return listKeys(argc > 2 ? argv[2] : NULL);
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s r|w|i KEY [VALUE] | %s l [PREFIX]\n", argv[0], argv[0]);
        return 2;
    }
    const char *cmd = argv[1], *keyName = argv[2];
    if (checkKeyArg(keyName)) return 2;
    if (openSMC()) { fprintf(stderr, "cannot open AppleSMC\n"); return 1; }

    uint32_t key = fourcc(keyName);
    SMCParamStruct info = getInfo(key);
    if (info.result) { fprintf(stderr, "%s: key not found (result=%d)\n", keyName, info.result); return 1; }
    uint32_t sz = info.keyInfo.dataSize, type = info.keyInfo.dataType;
    char t[5]; unfourcc(type, t);

    if (!strcmp(cmd, "i")) {
        printf("%s type='%s' size=%u\n", keyName, t, sz);
        return 0;
    }

    if (!strcmp(cmd, "r")) {
        uint8_t b[32]; uint32_t rs = 0, rt = 0;
        int rc = readBytes(key, 0, b, &rs, &rt);
        if (rc) { fprintf(stderr, "%s: read failed (%d)\n", keyName, rc); return 1; }
        printf("%s type='%s' size=%u bytes=", keyName, t, rs);
        for (uint32_t i = 0; i < rs; i++) printf("%02x ", b[i]);
        double v = decodeVal(type, b, rs);
        if (!isnan(v)) printf(" value=%.2f", v);
        printf("\n");
        return 0;
    }

    if (!strcmp(cmd, "w")) {
        if (argc < 4) { fprintf(stderr, "write needs VALUE\n"); return 2; }
        const char *val = argv[3];
        uint8_t b[32]; int n;
        if (!strncmp(val, "hex:", 4)) {
            n = parseHexBytes(val + 4, b, 32);
            if (n < 0 || (uint32_t)n != sz) {
                fprintf(stderr, "hex value must be exactly %u bytes\n", sz);
                return 2;
            }
        } else {
            double v = strtod(val, NULL);
            n = encodeVal(type, v, b);
            if (n < 0) { fprintf(stderr, "cannot encode value for type '%s', use hex:\n", t); return 2; }
            if ((uint32_t)n != sz) {
                fprintf(stderr, "encoded size %d != key size %u for type '%s', use hex:\n", n, sz, t);
                return 2;
            }
        }
        int rc = writeBytes(key, type, b, (uint32_t)sz);
        if (rc == -1000) {
            fprintf(stderr, "%s: write rejected by IOKit (0x%x%s)\n", keyName, g_lastKr,
                    g_lastKr == kIOReturnNotPrivileged ? ", kIOReturnNotPrivileged: run as root" : "");
            return 1;
        }
        if (rc) { fprintf(stderr, "%s: write failed (SMC result %d)\n", keyName, -rc); return 1; }
        printf("%s: written %u bytes\n", keyName, sz);
        return 0;
    }

    fprintf(stderr, "unknown command '%s' (r|w|i)\n", cmd);
    return 2;
}
