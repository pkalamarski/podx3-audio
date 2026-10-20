// PodX3Audio: AudioServerPlugIn exposing the POD X3 as a CoreAudio device.
//
// Runs sandboxed in coreaudiod's plug-in host, which cannot use libusb, so the
// USB side lives in the podx3d helper. This plug-in maps the helper's shared
// memory (common/shm.h) over XPC and does three things with it:
//   - clock: its own sample timeline, mapped onto the helper's DLL line
//     (common/pclock.h), or onto the host clock when the helper is absent;
//   - ReadInput: copy 8-channel frames from in_ring (zeros when missing);
//   - WriteMix: put stereo frames into out_ring for the helper to send.
// Structure follows Apple's NullAudio sample: one plug-in object, one device,
// one input and one output stream, no controls.
#include "../common/pclock.h"
#include "../common/shm.h"
#include "../common/shm_io.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <xpc/xpc.h>

// Tunables (frames at 48 kHz). The DLL line is the IN *completion* time, and
// completions arrive up to ~2.3 ms late plus 1 ms of transfer granularity, so
// input data for "now" is not there yet: the safety offsets cover that. Tune
// down after hardware tests (watch the underrun/late counters in the log).
#define PODX3_IN_SAFETY 144
#define PODX3_OUT_SAFETY 144
#define PODX3_IN_LATENCY 48
#define PODX3_OUT_LATENCY 288 // 6 OUT transfers of 1 ms in flight
#define PODX3_ZTS_PERIOD 4096
#define PODX3_HEARTBEAT_MS 100 // helper silent this long -> host-clock fallback
#define PODX3_CLOCK_DOMAIN 0x50583341u

enum {
    kObjPlugIn = kAudioObjectPlugInObject,
    kObjDevice = 2,
    kObjInStream = 3,
    kObjOutStream = 4,
};

#define DEVICE_UID "PodX3Audio_UID"
#define MODEL_UID "PodX3Audio_Model_UID"

static os_log_t g_log;
static AudioServerPlugInHostRef g_host;
static _Atomic UInt32 g_refcount = 1;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER; // IO start/stop and stream state
static UInt32 g_io_count;
static bool g_in_active = true, g_out_active = true;

static dispatch_queue_t g_queue;
static xpc_connection_t g_conn;
static _Atomic(podx3_shm *) g_shm;

// IO thread state (written by StartIO before IO runs, then only by the IO thread).
static podx3_pclock g_clock;
static double g_tpf_nominal, g_ticks_per_ms;

// Counters read by the log timer.
static _Atomic uint64_t g_in_underruns, g_out_late, g_rebases;
static _Atomic int g_mode; // PODX3_PCLOCK_*

// ---------------------------------------------------------------------------
// Helper connection

static void connect_helper(void);

static void request_map(xpc_connection_t c) {
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "op", "map");
    xpc_connection_send_message_with_reply(c, msg, g_queue, ^(xpc_object_t reply) {
        // Connection errors also reach the connection's event handler, which retries.
        if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY) return;
        xpc_object_t mem = xpc_dictionary_get_value(reply, "shm");
        if (!mem || xpc_get_type(mem) != XPC_TYPE_SHMEM) {
            os_log_error(g_log, "helper reply without shared memory");
            return;
        }
        void *addr = NULL;
        size_t len = xpc_shmem_map(mem, &addr);
        podx3_shm *s = addr;
        if (!len || !addr || len < sizeof(podx3_shm) || s->magic != PODX3_SHM_MAGIC ||
            s->version != PODX3_SHM_VERSION || s->ring_frames != PODX3_SHM_RING_FRAMES ||
            s->in_channels != PODX3_SHM_IN_CH || s->out_channels != PODX3_SHM_OUT_CH) {
            os_log_error(g_log, "helper shared memory rejected (len %zu, magic %x, version %u)", len,
                         addr ? s->magic : 0, addr ? s->version : 0);
            if (addr && len) munmap(addr, len);
            return;
        }
        // The previous mapping (if any) is leaked on purpose: the IO thread may
        // still be reading it.
        atomic_store(&g_shm, s);
        pthread_mutex_lock(&g_mutex);
        atomic_store(&s->plugin_io_running, g_io_count > 0);
        pthread_mutex_unlock(&g_mutex);
        os_log(g_log, "mapped helper shared memory (%zu bytes, generation %u, streaming %u)", len,
               atomic_load(&s->generation), atomic_load(&s->streaming));
    });
    xpc_release(msg);
}

static void connect_helper(void) {
    static bool reported_invalid;
    xpc_connection_t c =
        xpc_connection_create_mach_service(PODX3_MACH_SERVICE, g_queue, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
    g_conn = c;
    xpc_connection_set_event_handler(c, ^(xpc_object_t ev) {
        if (ev == XPC_ERROR_CONNECTION_INTERRUPTED) {
            // Helper exited; launchd restarts it. Ask again (the message waits for it).
            os_log(g_log, "helper connection interrupted, remapping");
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), g_queue, ^{
                if (g_conn == c) request_map(c);
            });
        } else if (ev == XPC_ERROR_CONNECTION_INVALID) {
            if (!reported_invalid)
                os_log_error(g_log, "helper %s not reachable (not installed/loaded?), retrying every second",
                             PODX3_MACH_SERVICE);
            reported_invalid = true;
            if (g_conn == c) g_conn = NULL;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), g_queue, ^{
                xpc_release(c);
                if (!g_conn) connect_helper();
            });
        } else if (xpc_get_type(ev) == XPC_TYPE_DICTIONARY) {
            reported_invalid = false;
        }
    });
    xpc_connection_resume(c);
    request_map(c);
}

static void start_log_timer(void) {
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_queue);
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC,
                              NSEC_PER_SEC);
    __block uint64_t last_u = 0, last_l = 0, last_r = 0;
    __block int last_mode = 0;
    dispatch_source_set_event_handler(t, ^{
        uint64_t u = atomic_load(&g_in_underruns), l = atomic_load(&g_out_late), r = atomic_load(&g_rebases);
        int mode = atomic_load(&g_mode);
        if (u != last_u || l != last_l || r != last_r || mode != last_mode) {
            podx3_shm *s = atomic_load(&g_shm);
            os_log(g_log, "clock %{public}s, re-anchors %llu, input underrun cycles %llu (+%llu), output late "
                          "cycles %llu (+%llu), helper generation %u",
                   mode == PODX3_PCLOCK_USB ? "USB" : mode == PODX3_PCLOCK_FALLBACK ? "host fallback" : "idle",
                   r, u, u - last_u, l, l - last_l, s ? atomic_load(&s->generation) : 0);
            last_u = u, last_l = l, last_r = r, last_mode = mode;
        }
    });
    dispatch_resume(t);
}

// Snapshot of the helper's clock; valid only if usable right now.
static void clock_source(podx3_clock_src *src, uint64_t now) {
    podx3_shm *s = atomic_load(&g_shm);
    *src = (podx3_clock_src){.src = s};
    if (!s) return;
    uint32_t g1 = atomic_load(&s->generation);
    if (!atomic_load(&s->streaming)) return;
    if (podx3_clock_read(s, &src->clk)) return;
    int64_t age = (int64_t)(now - atomic_load_explicit(&s->heartbeat_host, memory_order_acquire));
    if (age > PODX3_HEARTBEAT_MS * g_ticks_per_ms) return;
    if (atomic_load(&s->generation) != g1 || src->clk.host_ticks_per_frame <= 0) return;
    src->gen = g1;
    src->valid = 1;
}

// The shm the IO thread may use for frame F: the one the clock is mapped onto,
// in the generation the clock is mapped onto.
static podx3_shm *io_shm(void) {
    if (g_clock.mode != PODX3_PCLOCK_USB) return NULL;
    podx3_shm *s = atomic_load(&g_shm);
    if (s != g_clock.src || atomic_load(&s->generation) != g_clock.gen) return NULL;
    return s;
}

// ---------------------------------------------------------------------------
// Property helpers. One function both sizes and fetches every property, so
// GetPropertyDataSize and GetPropertyData cannot disagree.

typedef struct {
    void *out;  // NULL: size query only
    UInt32 cap; // bytes available at out
    UInt32 size;
} pbuf;

static OSStatus put(pbuf *b, const void *v, UInt32 n) {
    if (b->out) {
        if (b->cap < n) return kAudioHardwareBadPropertySizeError;
        memcpy(b->out, v, n);
    }
    b->size = n;
    return noErr;
}
static OSStatus put_u32(pbuf *b, UInt32 v) { return put(b, &v, sizeof v); }
static OSStatus put_f64(pbuf *b, Float64 v) { return put(b, &v, sizeof v); }
static OSStatus put_str(pbuf *b, CFStringRef v) { return put(b, &v, sizeof v); } // CFSTR: immortal, HAL releases
// Arrays are truncated to what fits (the HAL may ask for fewer items).
static OSStatus put_ids(pbuf *b, const AudioObjectID *ids, UInt32 n) {
    if (b->out && n > b->cap / sizeof(AudioObjectID)) n = b->cap / sizeof(AudioObjectID);
    return put(b, ids, n * (UInt32)sizeof(AudioObjectID));
}

static AudioClassID class_of(AudioObjectID o) {
    switch (o) {
    case kObjPlugIn: return kAudioPlugInClassID;
    case kObjDevice: return kAudioDeviceClassID;
    default: return kAudioStreamClassID;
    }
}

// OwnedObjects with the class-ID qualifier honoured.
static OSStatus put_owned(pbuf *b, const AudioObjectID *ids, UInt32 n, UInt32 qsize, const void *qdata) {
    AudioObjectID keep[4];
    UInt32 m = 0;
    UInt32 nq = qdata ? qsize / sizeof(AudioClassID) : 0;
    for (UInt32 i = 0; i < n; i++) {
        bool ok = nq == 0;
        for (UInt32 q = 0; q < nq && !ok; q++) {
            AudioClassID c = ((const AudioClassID *)qdata)[q];
            ok = c == class_of(ids[i]) || c == kAudioObjectClassID;
        }
        if (ok) keep[m++] = ids[i];
    }
    return put_ids(b, keep, m);
}

static AudioStreamBasicDescription stream_format(AudioObjectID stream) {
    UInt32 ch = stream == kObjInStream ? PODX3_SHM_IN_CH : PODX3_SHM_OUT_CH;
    return (AudioStreamBasicDescription){
        .mSampleRate = 48000,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked,
        .mBytesPerPacket = 4 * ch,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 4 * ch,
        .mChannelsPerFrame = ch,
        .mBitsPerChannel = 32,
    };
}

static CFStringRef element_name(AudioObjectPropertyScope scope, AudioObjectPropertyElement el) {
    static const CFStringRef in[8] = {CFSTR("Main L"),   CFSTR("Main R"),   CFSTR("Tone 1 L"), CFSTR("Tone 1 R"),
                                      CFSTR("Tone 2 L"), CFSTR("Tone 2 R"), CFSTR("DI L"),     CFSTR("DI R")};
    static const CFStringRef out[2] = {CFSTR("Out L"), CFSTR("Out R")};
    if (scope == kAudioObjectPropertyScopeInput && el >= 1 && el <= 8) return in[el - 1];
    if (scope == kAudioObjectPropertyScopeOutput && el >= 1 && el <= 2) return out[el - 1];
    return NULL;
}

static OSStatus plugin_prop(const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, pbuf *b) {
    static const AudioObjectID dev = kObjDevice;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: return put_u32(b, kAudioObjectClassID);
    case kAudioObjectPropertyClass: return put_u32(b, kAudioPlugInClassID);
    case kAudioObjectPropertyOwner: return put_u32(b, kAudioObjectUnknown);
    case kAudioObjectPropertyManufacturer: return put_str(b, CFSTR("Line 6 (podx3-audio)"));
    case kAudioObjectPropertyOwnedObjects: return put_owned(b, &dev, 1, qsize, qdata);
    case kAudioPlugInPropertyDeviceList: return put_ids(b, &dev, 1);
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        AudioObjectID r = kAudioObjectUnknown;
        if (b->out) { // size queries come without the qualifier
            if (qsize != sizeof(CFStringRef) || !qdata) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(const CFStringRef *)qdata;
            if (uid && CFStringCompare(uid, CFSTR(DEVICE_UID), 0) == kCFCompareEqualTo) r = kObjDevice;
        }
        return put_u32(b, r);
    }
    case kAudioPlugInPropertyResourceBundle: return put_str(b, CFSTR(""));
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus device_prop(const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, pbuf *b) {
    AudioObjectPropertyScope sc = a->mScope;
    bool in = sc == kAudioObjectPropertyScopeInput, out = sc == kAudioObjectPropertyScopeOutput;
    static const AudioObjectID both[2] = {kObjInStream, kObjOutStream};
    const AudioObjectID *streams = in ? &both[0] : out ? &both[1] : both;
    UInt32 nstreams = in || out ? 1 : 2;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: return put_u32(b, kAudioObjectClassID);
    case kAudioObjectPropertyClass: return put_u32(b, kAudioDeviceClassID);
    case kAudioObjectPropertyOwner: return put_u32(b, kObjPlugIn);
    case kAudioObjectPropertyName: return put_str(b, CFSTR("POD X3"));
    case kAudioObjectPropertyManufacturer: return put_str(b, CFSTR("Line 6 (podx3-audio)"));
    case kAudioObjectPropertyOwnedObjects: return put_owned(b, streams, nstreams, qsize, qdata);
    case kAudioObjectPropertyControlList: return put_ids(b, NULL, 0);
    case kAudioObjectPropertyElementName: {
        CFStringRef n = element_name(sc, a->mElement);
        return n ? put_str(b, n) : kAudioHardwareUnknownPropertyError;
    }
    case kAudioDevicePropertyDeviceUID: return put_str(b, CFSTR(DEVICE_UID));
    case kAudioDevicePropertyModelUID: return put_str(b, CFSTR(MODEL_UID));
    case kAudioDevicePropertyTransportType: return put_u32(b, kAudioDeviceTransportTypeUSB);
    case kAudioDevicePropertyRelatedDevices: {
        static const AudioObjectID dev = kObjDevice;
        return put_ids(b, &dev, 1);
    }
    case kAudioDevicePropertyClockDomain: return put_u32(b, PODX3_CLOCK_DOMAIN);
    case kAudioDevicePropertyDeviceIsAlive: return put_u32(b, 1);
    case kAudioDevicePropertyDeviceIsRunning: {
        pthread_mutex_lock(&g_mutex);
        UInt32 r = g_io_count > 0;
        pthread_mutex_unlock(&g_mutex);
        return put_u32(b, r);
    }
    case kAudioDevicePropertyDeviceCanBeDefaultDevice: return put_u32(b, 1);
    // Output to the POD is unverified; keep alerts/system sounds off it.
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: return put_u32(b, 0);
    case kAudioDevicePropertyLatency: return put_u32(b, in ? PODX3_IN_LATENCY : out ? PODX3_OUT_LATENCY : 0);
    case kAudioDevicePropertySafetyOffset: return put_u32(b, in ? PODX3_IN_SAFETY : out ? PODX3_OUT_SAFETY : 0);
    case kAudioDevicePropertyStreams: return put_ids(b, streams, nstreams);
    case kAudioDevicePropertyNominalSampleRate: return put_f64(b, 48000);
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        AudioValueRange r = {48000, 48000};
        if (b->out && b->cap < sizeof r) return put(b, &r, 0); // array: truncate
        return put(b, &r, sizeof r);
    }
    case kAudioDevicePropertyIsHidden: return put_u32(b, 0);
    case kAudioDevicePropertyZeroTimeStampPeriod: return put_u32(b, PODX3_ZTS_PERIOD);
    case kAudioDevicePropertyClockIsStable: return put_u32(b, 1);
    case kAudioDevicePropertyPreferredChannelsForStereo: {
        UInt32 ch[2] = {1, 2};
        return put(b, ch, sizeof ch);
    }
    case kAudioDevicePropertyPreferredChannelLayout: {
        UInt32 n = in ? PODX3_SHM_IN_CH : PODX3_SHM_OUT_CH;
        struct {
            AudioChannelLayout l;
            AudioChannelDescription more[PODX3_SHM_IN_CH - 1];
        } lay;
        memset(&lay, 0, sizeof lay);
        lay.l.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
        lay.l.mNumberChannelDescriptions = n;
        AudioChannelDescription *d = lay.l.mChannelDescriptions;
        for (UInt32 i = 0; i < n; i++)
            d[i].mChannelLabel = i == 0 ? kAudioChannelLabel_Left
                                 : i == 1 ? kAudioChannelLabel_Right
                                          : kAudioChannelLabel_Discrete_0 + i;
        return put(b, &lay, (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) +
                                     n * sizeof(AudioChannelDescription)));
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus stream_prop(AudioObjectID o, const AudioObjectPropertyAddress *a, pbuf *b) {
    bool in = o == kObjInStream;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass: return put_u32(b, kAudioObjectClassID);
    case kAudioObjectPropertyClass: return put_u32(b, kAudioStreamClassID);
    case kAudioObjectPropertyOwner: return put_u32(b, kObjDevice);
    case kAudioObjectPropertyOwnedObjects: return put_ids(b, NULL, 0);
    case kAudioObjectPropertyName: return put_str(b, in ? CFSTR("POD X3 Input") : CFSTR("POD X3 Output"));
    case kAudioStreamPropertyIsActive: {
        pthread_mutex_lock(&g_mutex);
        UInt32 r = in ? g_in_active : g_out_active;
        pthread_mutex_unlock(&g_mutex);
        return put_u32(b, r);
    }
    case kAudioStreamPropertyDirection: return put_u32(b, in ? 1 : 0);
    case kAudioStreamPropertyTerminalType:
        return put_u32(b, in ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine);
    case kAudioStreamPropertyStartingChannel: return put_u32(b, 1);
    case kAudioStreamPropertyLatency: return put_u32(b, 0);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        AudioStreamBasicDescription f = stream_format(o);
        return put(b, &f, sizeof f);
    }
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        AudioStreamRangedDescription r = {.mFormat = stream_format(o), .mSampleRateRange = {48000, 48000}};
        if (b->out && b->cap < sizeof r) return put(b, &r, 0); // array: truncate
        return put(b, &r, sizeof r);
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus get_prop(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata,
                         pbuf *b) {
    if (!a) return kAudioHardwareIllegalOperationError;
    switch (o) {
    case kObjPlugIn: return plugin_prop(a, qsize, qdata, b);
    case kObjDevice: return device_prop(a, qsize, qdata, b);
    case kObjInStream:
    case kObjOutStream: return stream_prop(o, a, b);
    default: return kAudioHardwareBadObjectError;
    }
}

static void notify(AudioObjectID o, AudioObjectPropertySelector sel) {
    AudioServerPlugInHostRef host = g_host;
    if (!host) return;
    dispatch_async(g_queue, ^{
        AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        host->PropertiesChanged(host, o, 1, &a);
    });
}

// ---------------------------------------------------------------------------
// AudioServerPlugInDriverInterface

static HRESULT QueryInterface(void *drv, REFIID iid, LPVOID *out);
static ULONG AddRef(void *drv);
static ULONG Release(void *drv);
static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host);
static OSStatus CreateDevice(AudioServerPlugInDriverRef drv, CFDictionaryRef desc,
                             const AudioServerPlugInClientInfo *client, AudioObjectID *out);
static OSStatus DestroyDevice(AudioServerPlugInDriverRef drv, AudioObjectID dev);
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef drv, AudioObjectID dev,
                                const AudioServerPlugInClientInfo *client);
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef drv, AudioObjectID dev,
                                   const AudioServerPlugInClientInfo *client);
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt64 action,
                                                 void *info);
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt64 action,
                                               void *info);
static Boolean HasProperty(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                           const AudioObjectPropertyAddress *a);
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                                   const AudioObjectPropertyAddress *a, Boolean *out);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata,
                                    UInt32 *out);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 size,
                                UInt32 *outsize, void *out);
static OSStatus SetPropertyData(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 size,
                                const void *data);
static OSStatus StartIO(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client);
static OSStatus StopIO(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client);
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client,
                                 Float64 *sample, UInt64 *host, UInt64 *seed);
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client, UInt32 op,
                                  Boolean *will, Boolean *inplace);
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client, UInt32 op,
                                 UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle);
static OSStatus DoIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, AudioObjectID stream,
                              UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle,
                              void *main, void *secondary);
static OSStatus EndIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client, UInt32 op,
                               UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle);

static AudioServerPlugInDriverInterface g_interface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient,
    RemoveDeviceClient, PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty,
    IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp,
    WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation,
};
static AudioServerPlugInDriverInterface *g_interface_ptr = &g_interface;
static AudioServerPlugInDriverRef g_driver = &g_interface_ptr;

// Factory named in Info.plist (CFPlugInFactories).
void *PodX3Audio_Create(CFAllocatorRef alloc, CFUUIDRef type);
void *PodX3Audio_Create(CFAllocatorRef alloc, CFUUIDRef type) {
    (void)alloc;
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? g_driver : NULL;
}

static HRESULT QueryInterface(void *drv, REFIID iid, LPVOID *out) {
    if (drv != g_driver || !out) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&g_refcount, 1);
        *out = g_driver;
        r = S_OK;
    }
    CFRelease(req);
    return r;
}

static ULONG AddRef(void *drv) {
    if (drv != g_driver) return 0;
    return atomic_fetch_add(&g_refcount, 1) + 1;
}

static ULONG Release(void *drv) {
    if (drv != g_driver) return 0;
    UInt32 n = atomic_load(&g_refcount);
    while (n > 0 && !atomic_compare_exchange_weak(&g_refcount, &n, n - 1)) {
    }
    return n ? n - 1 : 0; // the driver is static; nothing to free
}

static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host) {
    if (drv != g_driver) return kAudioHardwareBadObjectError;
    g_host = host;
    g_log = os_log_create("com.pkalamarski.podx3audio", "plugin");
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_ticks_per_ms = 1e6 * tb.denom / tb.numer;
    g_tpf_nominal = g_ticks_per_ms / 48.0;
    g_queue = dispatch_queue_create("com.pkalamarski.podx3audio", DISPATCH_QUEUE_SERIAL);
    // Tests load the plug-in outside coreaudiod; the helper is a system-wide
    // service, so they must not reach a real one (WriteMix would play on the POD).
    if (getenv("PODX3AUDIO_OFFLINE")) return noErr;
    os_log(g_log, "PodX3Audio loaded, connecting to %{public}s", PODX3_MACH_SERVICE);
    dispatch_async(g_queue, ^{
        connect_helper();
        start_log_timer();
    });
    return noErr;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef drv, CFDictionaryRef desc,
                             const AudioServerPlugInClientInfo *client, AudioObjectID *out) {
    (void)drv, (void)desc, (void)client, (void)out;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus DestroyDevice(AudioServerPlugInDriverRef drv, AudioObjectID dev) {
    (void)drv, (void)dev;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus AddDeviceClient(AudioServerPlugInDriverRef drv, AudioObjectID dev,
                                const AudioServerPlugInClientInfo *client) {
    (void)client;
    return drv == g_driver && dev == kObjDevice ? noErr : kAudioHardwareBadObjectError;
}

static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef drv, AudioObjectID dev,
                                   const AudioServerPlugInClientInfo *client) {
    (void)client;
    return drv == g_driver && dev == kObjDevice ? noErr : kAudioHardwareBadObjectError;
}

static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt64 action,
                                                 void *info) {
    (void)action, (void)info;
    return drv == g_driver && dev == kObjDevice ? noErr : kAudioHardwareBadObjectError;
}

static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt64 action,
                                               void *info) {
    (void)action, (void)info;
    return drv == g_driver && dev == kObjDevice ? noErr : kAudioHardwareBadObjectError;
}

static Boolean HasProperty(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                           const AudioObjectPropertyAddress *a) {
    (void)pid;
    if (drv != g_driver) return false;
    pbuf b = {0};
    return get_prop(o, a, 0, NULL, &b) == noErr;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                                   const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)pid;
    if (drv != g_driver) return kAudioHardwareBadObjectError;
    if (!out) return kAudioHardwareIllegalOperationError;
    pbuf b = {0};
    OSStatus e = get_prop(o, a, 0, NULL, &b);
    if (e) return e;
    switch (a->mSelector) {
    case kAudioDevicePropertyNominalSampleRate: *out = o == kObjDevice; break;
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: *out = o == kObjInStream || o == kObjOutStream; break;
    default: *out = false;
    }
    return noErr;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata,
                                    UInt32 *out) {
    (void)pid;
    if (drv != g_driver) return kAudioHardwareBadObjectError;
    if (!out) return kAudioHardwareIllegalOperationError;
    pbuf b = {0};
    OSStatus e = get_prop(o, a, qsize, qdata, &b);
    if (!e) *out = b.size;
    return e;
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 size,
                                UInt32 *outsize, void *out) {
    (void)pid;
    if (drv != g_driver) return kAudioHardwareBadObjectError;
    if (!out || !outsize) return kAudioHardwareIllegalOperationError;
    pbuf b = {.out = out, .cap = size};
    OSStatus e = get_prop(o, a, qsize, qdata, &b);
    if (!e) *outsize = b.size;
    return e;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef drv, AudioObjectID o, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsize, const void *qdata, UInt32 size,
                                const void *data) {
    (void)pid, (void)qsize, (void)qdata;
    if (drv != g_driver) return kAudioHardwareBadObjectError;
    if (!a || !data) return kAudioHardwareIllegalOperationError;
    Boolean settable = false;
    OSStatus e = IsPropertySettable(drv, o, pid, a, &settable);
    if (e) return e;
    if (!settable) return kAudioHardwareUnsupportedOperationError;
    switch (a->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
        if (size != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return *(const Float64 *)data == 48000 ? noErr : kAudioHardwareIllegalOperationError;
    case kAudioStreamPropertyIsActive: {
        if (size != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        bool v = *(const UInt32 *)data != 0;
        pthread_mutex_lock(&g_mutex);
        bool *p = o == kObjInStream ? &g_in_active : &g_out_active;
        bool changed = *p != v;
        *p = v;
        pthread_mutex_unlock(&g_mutex);
        if (changed) notify(o, kAudioStreamPropertyIsActive);
        return noErr;
    }
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        if (size != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = data;
        AudioStreamBasicDescription ours = stream_format(o);
        // Only one format exists; accept it (sample rate 0 = "don't care").
        if (f->mFormatID != ours.mFormatID || f->mChannelsPerFrame != ours.mChannelsPerFrame ||
            f->mBitsPerChannel != ours.mBitsPerChannel || f->mFormatFlags != ours.mFormatFlags ||
            (f->mSampleRate != 0 && f->mSampleRate != 48000))
            return kAudioDeviceUnsupportedFormatError;
        return noErr;
    }
    default: return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus StartIO(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (drv != g_driver || dev != kObjDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&g_mutex);
    bool first = g_io_count++ == 0;
    if (first) {
        uint64_t now = mach_absolute_time();
        podx3_clock_src src;
        clock_source(&src, now);
        podx3_pclock_start(&g_clock, g_tpf_nominal, PODX3_ZTS_PERIOD, &src, now);
        atomic_store(&g_mode, g_clock.mode);
        podx3_shm *s = atomic_load(&g_shm);
        if (s) atomic_store(&s->plugin_io_running, 1);
    }
    pthread_mutex_unlock(&g_mutex);
    if (first) {
        os_log(g_log, "IO started on %{public}s clock", g_clock.mode == PODX3_PCLOCK_USB ? "USB" : "host");
        notify(kObjDevice, kAudioDevicePropertyDeviceIsRunning);
    }
    return noErr;
}

static OSStatus StopIO(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client) {
    (void)client;
    if (drv != g_driver || dev != kObjDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&g_mutex);
    bool last = g_io_count > 0 && --g_io_count == 0;
    if (last) {
        podx3_shm *s = atomic_load(&g_shm);
        if (s) atomic_store(&s->plugin_io_running, 0);
        atomic_store(&g_mode, 0);
    }
    pthread_mutex_unlock(&g_mutex);
    if (last) {
        os_log(g_log, "IO stopped");
        notify(kObjDevice, kAudioDevicePropertyDeviceIsRunning);
    }
    return noErr;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client,
                                 Float64 *sample, UInt64 *host, UInt64 *seed) {
    (void)client;
    if (drv != g_driver || dev != kObjDevice) return kAudioHardwareBadObjectError;
    uint64_t now = mach_absolute_time();
    podx3_clock_src src;
    clock_source(&src, now);
    if (podx3_pclock_update(&g_clock, &src, now)) {
        atomic_fetch_add(&g_rebases, 1);
        atomic_store(&g_mode, g_clock.mode);
    }
    podx3_pclock_zero_ts(&g_clock, now, sample, host);
    *seed = 1; // the timeline never jumps (pclock keeps it continuous)
    return noErr;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client, UInt32 op,
                                  Boolean *will, Boolean *inplace) {
    (void)client;
    if (drv != g_driver || dev != kObjDevice) return kAudioHardwareBadObjectError;
    bool w = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
    if (will) *will = w;
    if (inplace) *inplace = true;
    return noErr;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client, UInt32 op,
                                 UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle) {
    (void)client, (void)op, (void)frames, (void)cycle;
    return drv == g_driver && dev == kObjDevice ? noErr : kAudioHardwareBadObjectError;
}

static void read_input(float *dst, UInt32 n, Float64 t) {
    podx3_shm *s = io_shm();
    if (!s) {
        memset(dst, 0, (size_t)n * PODX3_SHM_IN_CH * sizeof(float));
        return;
    }
    uint32_t gen = g_clock.gen;
    int64_t f = podx3_pclock_frame(&g_clock, t);
    uint32_t skip = 0;
    if (f < 0) { // before this generation's first frame
        skip = (uint32_t)(-f < (int64_t)n ? -f : (int64_t)n);
        memset(dst, 0, (size_t)skip * PODX3_SHM_IN_CH * sizeof(float));
    }
    uint64_t w = atomic_load_explicit(&s->in_write_frame, memory_order_acquire);
    uint32_t want = n - skip;
    uint32_t got = want ? podx3_in_get(s, (uint64_t)(f + skip), want, w, dst + (size_t)skip * PODX3_SHM_IN_CH) : 0;
    if (atomic_load(&s->generation) != gen) { // helper restarted the stream mid-copy
        memset(dst, 0, (size_t)n * PODX3_SHM_IN_CH * sizeof(float));
        got = 0;
    }
    if (got < n) {
        atomic_fetch_add(&g_in_underruns, 1);
        atomic_fetch_add_explicit(&s->plugin_in_underruns, 1, memory_order_relaxed);
    }
}

static void write_mix(const float *src, UInt32 n, Float64 t) {
    podx3_shm *s = io_shm();
    if (!s) return; // nowhere to send it
    int64_t f = podx3_pclock_frame(&g_clock, t);
    if (f < 0) {
        if (-f >= (int64_t)n) return;
        src += (size_t)(-f) * PODX3_SHM_OUT_CH, n -= (UInt32)(-f), f = 0;
    }
    uint64_t r = atomic_load_explicit(&s->out_read_frame, memory_order_acquire);
    if (podx3_out_put(s, (uint64_t)f, src, n, r)) {
        atomic_fetch_add(&g_out_late, 1);
        atomic_fetch_add_explicit(&s->plugin_out_late, 1, memory_order_relaxed);
    }
}

static OSStatus DoIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, AudioObjectID stream,
                              UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle,
                              void *main, void *secondary) {
    (void)client, (void)secondary;
    if (drv != g_driver || dev != kObjDevice) return kAudioHardwareBadObjectError;
    if (!main || !cycle) return noErr;
    if (op == kAudioServerPlugInIOOperationReadInput && stream == kObjInStream)
        read_input(main, frames, cycle->mInputTime.mSampleTime);
    else if (op == kAudioServerPlugInIOOperationWriteMix && stream == kObjOutStream)
        write_mix(main, frames, cycle->mOutputTime.mSampleTime);
    return noErr;
}

static OSStatus EndIOOperation(AudioServerPlugInDriverRef drv, AudioObjectID dev, UInt32 client, UInt32 op,
                               UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle) {
    (void)client, (void)op, (void)frames, (void)cycle;
    return drv == g_driver && dev == kObjDevice ? noErr : kAudioHardwareBadObjectError;
}
