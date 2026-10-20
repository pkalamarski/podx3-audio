// Loads PodX3Audio.driver through CFPlugIn (like coreaudiod does, minus the
// HAL) and checks the property implementation: for every (object, selector,
// scope, element) the plug-in claims to have, GetPropertyDataSize must equal
// the bytes GetPropertyData returns. Then runs a few IO cycles on the
// host-clock fallback (no helper): zero timestamps advance, input is silence.
//   test_plugin path/to/PodX3Audio.driver
#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checked;
#define CHECK(cond, ...)                                                                                     \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            failures++;                                                                                      \
            printf("FAIL: ");                                                                                \
            printf(__VA_ARGS__);                                                                             \
            printf("\n");                                                                                    \
        }                                                                                                    \
    } while (0)

static OSStatus host_props_changed(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n,
                                   const AudioObjectPropertyAddress *a) {
    (void)h, (void)o, (void)n, (void)a;
    return noErr;
}
static OSStatus host_copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *d) {
    (void)h, (void)k;
    *d = NULL;
    return kAudioHardwareUnknownPropertyError;
}
static OSStatus host_write(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) {
    (void)h, (void)k, (void)d;
    return noErr;
}
static OSStatus host_delete(AudioServerPlugInHostRef h, CFStringRef k) {
    (void)h, (void)k;
    return noErr;
}
static OSStatus host_req(AudioServerPlugInHostRef h, AudioObjectID o, UInt64 a, void *i) {
    (void)h, (void)o, (void)a, (void)i;
    return noErr;
}
static AudioServerPlugInHostInterface g_host = {host_props_changed, host_copy, host_write, host_delete, host_req};

static const char *fourcc(UInt32 v, char *b) {
    for (int i = 0; i < 4; i++) b[i] = (char)(v >> (24 - 8 * i));
    b[4] = 0;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], (CFIndex)strlen(argv[1]), true);
    CFPlugInRef plugin = CFPlugInCreate(NULL, url);
    CFArrayRef factories = plugin ? CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID, plugin) : NULL;
    if (!factories || CFArrayGetCount(factories) != 1) {
        printf("FAIL: no AudioServerPlugIn factory in %s\n", argv[1]);
        return 1;
    }
    CFUUIDRef factory = CFArrayGetValueAtIndex(factories, 0);
    void *iunknown = CFPlugInInstanceCreate(NULL, factory, kAudioServerPlugInTypeUUID);
    CHECK(iunknown, "factory returned NULL");
    if (!iunknown) return 1;
    AudioServerPlugInDriverRef drv = NULL;
    (*(AudioServerPlugInDriverRef)iunknown)->QueryInterface(iunknown, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID),
                                                           (LPVOID *)&drv);
    CHECK(drv, "QueryInterface failed");
    if (!drv) return 1;
    setenv("PODX3AUDIO_OFFLINE", "1", 1); // never talk to an installed helper
    CHECK((*drv)->Initialize(drv, &g_host) == noErr, "Initialize");

    static const AudioObjectPropertySelector sels[] = {
        kAudioObjectPropertyBaseClass, kAudioObjectPropertyClass, kAudioObjectPropertyOwner,
        kAudioObjectPropertyName, kAudioObjectPropertyManufacturer, kAudioObjectPropertyOwnedObjects,
        kAudioObjectPropertyControlList, kAudioObjectPropertyElementName, kAudioPlugInPropertyDeviceList,
        kAudioPlugInPropertyTranslateUIDToDevice, kAudioPlugInPropertyResourceBundle, kAudioDevicePropertyDeviceUID,
        kAudioDevicePropertyModelUID, kAudioDevicePropertyTransportType, kAudioDevicePropertyRelatedDevices,
        kAudioDevicePropertyClockDomain, kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyDeviceIsRunning,
        kAudioDevicePropertyDeviceCanBeDefaultDevice, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice,
        kAudioDevicePropertyLatency, kAudioDevicePropertyStreams, kAudioDevicePropertySafetyOffset,
        kAudioDevicePropertyNominalSampleRate, kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioDevicePropertyIsHidden, kAudioDevicePropertyPreferredChannelsForStereo,
        kAudioDevicePropertyPreferredChannelLayout, kAudioDevicePropertyZeroTimeStampPeriod,
        kAudioDevicePropertyClockIsStable, kAudioStreamPropertyIsActive, kAudioStreamPropertyDirection,
        kAudioStreamPropertyTerminalType, kAudioStreamPropertyStartingChannel, kAudioStreamPropertyLatency,
        kAudioStreamPropertyVirtualFormat, kAudioStreamPropertyPhysicalFormat,
        kAudioStreamPropertyAvailableVirtualFormats, kAudioStreamPropertyAvailablePhysicalFormats,
        'fsiz', kAudioDevicePropertyIcon, // expected absent (buffer size is the HAL's)
    };
    static const AudioObjectPropertyScope scopes[] = {kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyScopeInput,
                                                      kAudioObjectPropertyScopeOutput};
    CFStringRef uid = CFSTR("PodX3Audio_UID");
    for (AudioObjectID o = 1; o <= 5; o++)
        for (size_t s = 0; s < sizeof sels / sizeof *sels; s++)
            for (int sc = 0; sc < 3; sc++)
                for (UInt32 el = 0; el <= 9; el++) {
                    AudioObjectPropertyAddress a = {sels[s], scopes[sc], el};
                    if (!(*drv)->HasProperty(drv, o, 0, &a)) continue;
                    CHECK(o <= 4, "object %u claims properties", o);
                    const void *q = NULL;
                    UInt32 qs = 0;
                    if (sels[s] == kAudioPlugInPropertyTranslateUIDToDevice) q = &uid, qs = sizeof uid;
                    UInt32 size = 0, got = 0;
                    char fc[5];
                    OSStatus e = (*drv)->GetPropertyDataSize(drv, o, 0, &a, qs, q, &size);
                    CHECK(!e, "obj %u %s scope %d el %u: size err %d", o, fourcc(sels[s], fc), sc, el, (int)e);
                    uint8_t buf[1024];
                    memset(buf, 0xAA, sizeof buf);
                    e = (*drv)->GetPropertyData(drv, o, 0, &a, qs, q, sizeof buf, &got, buf);
                    CHECK(!e && got == size, "obj %u %s scope %d el %u: size %u, data %u (err %d)", o,
                          fourcc(sels[s], fc), sc, el, size, got, (int)e);
                    if (size >= 4) {
                        e = (*drv)->GetPropertyData(drv, o, 0, &a, qs, q, size - 1, &got, buf);
                        // Arrays may truncate; scalars must refuse a short buffer.
                        CHECK(e || got <= size - 1, "obj %u %s: overflowed a short buffer", o, fourcc(sels[s], fc));
                    }
                    checked++;
                }
    // Spot checks.
    AudioObjectPropertyAddress a = {kAudioPlugInPropertyTranslateUIDToDevice, 0, 0};
    AudioObjectID dev = 0;
    UInt32 sz = 0;
    (*drv)->GetPropertyData(drv, 1, 0, &a, sizeof uid, &uid, sizeof dev, &sz, &dev);
    CHECK(dev == 2, "UID -> device %u", dev);
    AudioObjectID ids[4];
    AudioClassID streamclass = kAudioStreamClassID;
    a = (AudioObjectPropertyAddress){kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeInput, 0};
    (*drv)->GetPropertyData(drv, 2, 0, &a, sizeof streamclass, &streamclass, sizeof ids, &sz, ids);
    CHECK(sz == 4 && ids[0] == 3, "device input owned streams");
    AudioClassID devclass = kAudioDeviceClassID;
    (*drv)->GetPropertyData(drv, 2, 0, &a, sizeof devclass, &devclass, sizeof ids, &sz, ids);
    CHECK(sz == 0, "class filter");
    a = (AudioObjectPropertyAddress){kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, 7};
    CFStringRef name = NULL;
    (*drv)->GetPropertyData(drv, 2, 0, &a, 0, NULL, sizeof name, &sz, &name);
    CHECK(name && CFEqual(name, CFSTR("DI L")), "element 7 name");
    a = (AudioObjectPropertyAddress){kAudioDevicePropertyNominalSampleRate, 0, 0};
    Float64 r = 44100;
    CHECK((*drv)->SetPropertyData(drv, 2, 0, &a, 0, NULL, sizeof r, &r) != noErr, "accepted 44100");
    r = 48000;
    CHECK((*drv)->SetPropertyData(drv, 2, 0, &a, 0, NULL, sizeof r, &r) == noErr, "refused 48000");

    // IO on the host-clock fallback (no helper reachable from a test process).
    CHECK((*drv)->StartIO(drv, 2, 1) == noErr, "StartIO");
    Float64 st, st_prev = -1;
    UInt64 ht, ht_prev = 0, seed;
    int zts = 0;
    float in[512 * 8];
    for (int i = 0; i < 120; i++) { // 1.2 s
        (*drv)->GetZeroTimeStamp(drv, 2, 1, &st, &ht, &seed);
        if (st != st_prev) {
            CHECK(st > st_prev && ht > ht_prev && ht <= mach_absolute_time(), "zero timestamp went backwards");
            if (st_prev >= 0) {
                double tpf = (double)(ht - ht_prev) / (st - st_prev);
                CHECK(tpf > 499 && tpf < 501, "zero timestamp rate %.3f ticks/frame", tpf);
            }
            zts++, st_prev = st, ht_prev = ht;
        }
        AudioServerPlugInIOCycleInfo ci = {0};
        ci.mInputTime.mSampleTime = st;
        memset(in, 0x7f, sizeof in);
        Boolean will = 0, inplace = 0;
        (*drv)->WillDoIOOperation(drv, 2, 1, kAudioServerPlugInIOOperationReadInput, &will, &inplace);
        CHECK(will && inplace, "ReadInput not done in place");
        (*drv)->DoIOOperation(drv, 2, 3, 1, kAudioServerPlugInIOOperationReadInput, 512, &ci, in, NULL);
        CHECK(in[0] == 0 && in[512 * 8 - 1] == 0, "fallback input not silent");
        (*drv)->DoIOOperation(drv, 2, 4, 1, kAudioServerPlugInIOOperationWriteMix, 512, &ci, in, NULL);
        usleep(10000);
    }
    CHECK((*drv)->StopIO(drv, 2, 1) == noErr, "StopIO");
    printf("plugin: %d properties checked, %d zero timestamps in fallback mode\n", checked, zts);
    CHECK(checked > 60, "too few properties (%d)", checked);
    CHECK(zts >= 13, "zero timestamps did not advance (%d)", zts);
    printf(failures ? "%d FAILURES\n" : "plugin tests passed\n", failures);
    return failures != 0;
}
