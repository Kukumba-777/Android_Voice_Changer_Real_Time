#include "avcrt_agm.h"
#include "avcrt_agm_path.h"
#include "avcrt_cellular_backend.h"
#ifdef AVCRT_AGM_LAB
#include "avcrt_agm_loopback.h"
#include "avcrt_agm_overlap.h"
#include "avcrt_live_pcm.h"
#endif
#include "avcrt_agm_stock_tx.h"
#include "avcrt_agm_tx_lease.h"
#include "avcrt_agm_uplink.h"
#include "avcrt_agm_usb_acdb.h"
#include "avcrt_engine.h"
#include "avcrt_mic_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <exception>
#include <fcntl.h>
#include <glob.h>
#include <mutex>
#include <poll.h>
#include <sound/asound.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <tinyalsa/asoundlib.h>

extern char** environ;

#if defined(AVCRT_VA_TEST_PRIVATE_PCM)
#if !defined(AVCRT_VA_TEST_PCM) || !defined(AVCRT_VA_TEST_SPEAKER) || !defined(AVCRT_VA_TEST_INSTANCE)
#error "Build the private VA lab probe with the verified PCM graph profile"
#endif
#endif

namespace avcrt::agm {
namespace {
using Clock = std::chrono::steady_clock;
constexpr char kVoiceState[] = "/sys/module/audio_pkt_dlkm/parameters/avcrt_voice_state";
constexpr uint32_t kNonTunnel = 2, kRx = 1, kTx = 2, kS16 = 2, kFixedPoint = 1, kNonBlocking = 2;
#ifdef AVCRT_AGM_LAB
constexpr uint32_t kCompress = 4; // AGM session mode, not the requested media format
#endif
constexpr uint32_t kShmemEndpoint = 0xc0000001;
constexpr uint32_t kWriteShmemModule = 0x07001000;
constexpr uint32_t kReadShmemModule = 0x07001001;
constexpr uint32_t kRatRender = 0xc0000022, kRatModule = 0x07001041;
constexpr uint32_t kMuxDemux = 0xc0000027, kMuxModule = 0x07001098;
constexpr uint32_t kDataPathEvent = 1;
constexpr unsigned kRate = 48000, kChannels = 2, kFrames = kRate / 100, kPeriods = 4;
using Period = std::array<int16_t, kFrames * kChannels>;
using MonoPeriod = std::array<int16_t, kFrames>;
#ifdef AVCRT_AGM_LAB
static_assert(overlap::kFrames == kFrames);
#endif
volatile sig_atomic_t interrupted;
std::atomic<bool> agm_service_died{false};
void stop_signal(int) { interrupted = 1; }

int64_t monotonic_ms(Clock::time_point now = Clock::now()) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

struct WorkerProgress {
    std::atomic<int64_t> updated{monotonic_ms()};
    std::atomic<const char*> stage{"read call state"};
    void at(const char* label) {
        stage.store(label);
        updated.store(monotonic_ms());
    }
};

struct Voice {
    unsigned long long epoch = 0;
    unsigned vsid = 0;
    std::string node;
    std::string generation() const { return std::to_string(epoch) + ":" + std::to_string(vsid); }
    bool read(const char* path = kVoiceState) {
        unsigned version = 0;
        char extra;
        FILE* f = fopen(path, "re");
        if (!f) return false;
        const int n = fscanf(f, "%u %llu %u %c", &version, &epoch, &vsid, &extra);
        fclose(f);
        return n == 3 && version == 1;
    }
    bool same() const {
        return vsid && unchanged();
    }
    bool unchanged() const {
        Voice now;
        return !interrupted && now.read(node.empty() ? kVoiceState : node.c_str()) &&
               now.vsid == vsid && now.epoch == epoch;
    }
};

struct GraphKey { uint32_t id = 0, value = 0; };
bool valid(GraphKey key) { return key.id && key.value; }
using XmlStart = void (*)(void*, const char*, const char**);
using XmlEnd = void (*)(void*, const char*);
using XmlText = void (*)(void*, const char*, int);
template <class T> bool symbol(void* lib, const char* name, T& fn) {
    fn = reinterpret_cast<T>(dlsym(lib, name));
    if (fn) return true;
    fprintf(stderr, "[avcrtd:agm] missing ABI symbol: %s\n", name);
    return false;
}

std::string_view attr(const char** a, std::string_view name) {
    for (; *a; a += 2) if (*a == name) return a[1];
    return {};
}
uint32_t hex(std::string_view val) {
    if (val.substr(0, 2) != "0x") return 0;
    val.remove_prefix(2);
    uint32_t out = 0;
    const auto r = std::from_chars(val.data(), val.data() + val.size(), out, 16);
    return r.ec == std::errc{} && r.ptr == val.data() + val.size() ? out : 0;
}
struct XmlKeys {
    std::string_view key_name, value_name;
    unsigned depth = 0, keys = 0, values = 0;
    bool list = false, selected = false;
    GraphKey match;
    std::vector<GraphKey>* all = nullptr;
    static void start(void* data, const char* name, const char** attrs) {
        auto& s = *static_cast<XmlKeys*>(data);
        const std::string_view tag(name);
        ++s.depth;
        if (s.depth == 2) s.list = tag == "KEY_LIST";
        if (s.depth == 3 && s.list) {
            s.selected = tag == "KEY" && attr(attrs, "name") == s.key_name;
            if (s.selected) { ++s.keys; s.match.id = hex(attr(attrs, "id")); }
        }
        if (s.depth == 4 && s.selected && tag == "VALUE") {
            if (s.all) s.all->push_back({s.match.id, hex(attr(attrs, "val"))});
            if (attr(attrs, "name") == s.value_name) {
                ++s.values;
                s.match.value = hex(attr(attrs, "val"));
            }
        }
    }
    static void end(void* data, const char*) {
        auto& s = *static_cast<XmlKeys*>(data);
        if (s.depth == 3) s.selected = false;
        if (s.depth == 2) s.list = false;
        --s.depth;
    }
};

bool parse_xml(const char* path, void* state, XmlStart start, XmlEnd end, XmlText text = nullptr) {
    void* lib = dlopen("libexpat.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("/vendor/lib64/libexpat.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
    void* (*create)(const char*);
    void (*destroy)(void*);
    void (*user)(void*, void*);
    void (*handlers)(void*, XmlStart, XmlEnd);
    void (*characters)(void*, XmlText);
    int (*parse)(void*, const char*, int, int);
    bool ok = symbol(lib, "XML_ParserCreate", create) && symbol(lib, "XML_ParserFree", destroy) &&
              symbol(lib, "XML_SetUserData", user) && symbol(lib, "XML_SetElementHandler", handlers) &&
              symbol(lib, "XML_Parse", parse) && (!text || symbol(lib, "XML_SetCharacterDataHandler", characters));
    if (ok) {
        FILE* f = fopen(path, "re");
        void* p = create(nullptr);
        ok = f && p;
        if (ok) {
            char buf[4096];
            user(p, state);
            handlers(p, start, end);
            if (text) characters(p, text);
            for (size_t total = 0;;) {
                size_t n = fread(buf, 1, sizeof(buf), f);
                total += n;
                ok = total <= 1024 * 1024 && !ferror(f) && parse(p, buf, n, feof(f)) == 1;
                if (!ok || feof(f)) break;
            }
        }
        if (p) destroy(p);
        if (f) fclose(f);
    }
    dlclose(lib);
    return ok;
}

GraphKey discover_key(std::string_view key_name, std::string_view value_name) {
    XmlKeys keys{key_name, value_name, 0, 0, 0, false, false, {}};
    const bool ok = parse_xml("/vendor/etc/kvh2xml.xml", &keys, XmlKeys::start, XmlKeys::end);
    return ok && keys.keys == 1 && keys.values == 1 ? keys.match : GraphKey{};
}
GraphKey discover_music() { return discover_key("StreamRX", "Incall_Music"); }

std::vector<GraphKey> discover_instances() {
    std::vector<GraphKey> instances;
    XmlKeys keys{"Instance", {}, 0, 0, 0, false, false, {}, &instances};
    if (!parse_xml("/vendor/etc/kvh2xml.xml", &keys, XmlKeys::start, XmlKeys::end) || keys.keys != 1 ||
        instances.empty() || instances.size() > 16) return {};
    std::sort(instances.begin(), instances.end(), [](GraphKey a, GraphKey b) { return a.value < b.value; });
    for (size_t i = 0; i < instances.size(); ++i)
        if (!valid(instances[i]) || (i && instances[i-1].value == instances[i].value)) return {};
    return instances;
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == text.npos ? std::string_view{} : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

struct VaResource {
    std::string backend, path, rate, channels, bits;
    bool operator==(const VaResource& o) const {
        return backend == o.backend && path == o.path && rate == o.rate && channels == o.channels && bits == o.bits;
    }
};

struct VaXml {
    unsigned depth = 0, device = 0, matches = 0;
    std::string id;
    VaResource current, match;
    std::string* field = nullptr;
    bool invalid = false;
    std::string_view role;

    static void start(void* data, const char* name, const char**) {
        auto& s = *static_cast<VaXml*>(data);
        ++s.depth;
        const std::string_view tag(name);
        if (!s.device && tag == "in-device") {
            s.device = s.depth;
            s.id.clear();
            s.current = {};
        } else if (s.device && s.depth == s.device + 1) {
            if (tag == "id") s.field = &s.id;
            else if (tag == "back_end_name") s.field = &s.current.backend;
            else if (tag == "snd_device_name") s.field = &s.current.path;
            else if (tag == "samplerate") s.field = &s.current.rate;
            else if (tag == "channels") s.field = &s.current.channels;
            else if (tag == "bit_width") s.field = &s.current.bits;
            if (s.field && !s.field->empty()) s.invalid = true;
        }
    }
    static void text(void* data, const char* buf, int len) {
        auto& s = *static_cast<VaXml*>(data);
        if (!s.field || s.depth != s.device + 1) return;
        if (len < 0 || s.field->size() + static_cast<size_t>(len) > 128) s.invalid = true;
        else s.field->append(buf, len);
    }
    static void end(void* data, const char*) {
        auto& s = *static_cast<VaXml*>(data);
        if (s.device && s.depth == s.device + 1 && s.field) {
            *s.field = std::string(trim(*s.field));
            s.field = nullptr;
        }
        if (s.depth == s.device) {
            if (s.id == s.role) {
                ++s.matches;
                s.match = s.current;
            }
            s.device = 0;
        }
        --s.depth;
    }
};

VaResource discover_va_resource(std::string_view role = "PAL_DEVICE_IN_HANDSET_VA_MIC") {
    // Do not guess the active SKU: require every discovered handset VA declaration to agree.
    constexpr const char* roots[] = {"/odm/etc/", "/odm/etc/audio/", "/vendor/etc/", "/vendor/etc/audio/"};
    constexpr const char* paths[] = {"resourcemanager*.xml", "*/resourcemanager*.xml", "*/*/resourcemanager*.xml"};
    VaResource resource;
    for (const char* root : roots) for (const char* suffix : paths) {
        glob_t files{};
        const int rc = glob((std::string(root) + suffix).c_str(), GLOB_ERR | GLOB_NOSORT, nullptr, &files);
        bool ok = (rc == 0 && files.gl_pathc <= 64) || rc == GLOB_NOMATCH;
        for (size_t i = 0; ok && rc == 0 && i < files.gl_pathc; ++i) {
            VaXml xml;
            xml.role = role;
            ok = parse_xml(files.gl_pathv[i], &xml, VaXml::start, VaXml::end, VaXml::text) && !xml.invalid;
            if (!ok || !xml.matches) continue;
            ok = xml.matches == 1 && !xml.match.backend.empty() && !xml.match.path.empty() &&
                 !xml.match.rate.empty() && !xml.match.channels.empty() && !xml.match.bits.empty() &&
                 (resource.backend.empty() || resource == xml.match);
            if (ok) resource = std::move(xml.match);
        }
        globfree(&files);
        if (!ok) return {};
    }
    return resource;
}

std::string discover_voice_node() {
    glob_t matches{};
    const int rc = glob("/sys/module/*/parameters/avcrt_voice_state", GLOB_ERR, nullptr, &matches);
    std::string path = rc == 0 && matches.gl_pathc == 1 ? matches.gl_pathv[0] : "";
    globfree(&matches);
    return path;
}

StockRoute read_stock_route(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno != ENOENT) return {};
        // Older installed kernel (0012): only the codec ABI exists.
        const auto old = path.substr(0, path.size() - sizeof("_route") + 1);
        const int compat = open(old.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (compat < 0) return {};
        char text[128];
        const auto size = read(compat, text, sizeof(text));
        close(compat);
        if (size <= 0 || size >= static_cast<ssize_t>(sizeof(text))) return {};
        auto tx = StockTx::parse(std::string_view(text, size));
        return tx.codec ? StockRoute{StockRoute::Kind::Codec, tx} : StockRoute{};
    }
    char text[128];
    const auto size = read(fd, text, sizeof(text));
    close(fd);
    return size > 0 && size < static_cast<ssize_t>(sizeof(text)) ?
        StockRoute::parse(std::string_view(text, size)) : StockRoute{};
}

struct VoiceBinding {
    std::string node;
    StockTx tx;
    std::string generation() const {
        return std::to_string(tx.epoch) + ":" + std::to_string(tx.vsid) + ":" + std::to_string(tx.leg);
    }
    StockRoute read_route() {
        node = discover_voice_node();
        if (node.empty()) return {};
        node.replace(node.find_last_of('/') + 1, std::string::npos, "avcrt_voice_tx_route");
        const auto route = read_stock_route(node);
        tx = route.tx;
        return route;
    }
    bool read() { return read_route().kind == StockRoute::Kind::Codec; }
    bool same() const { return !interrupted && tx.codec && read_stock_route(node).tx == tx; }
};

// AudioReach AGMIPC@1.0 public PCM-only ABI; codec union is unused and zeroed.
struct SessionConfig {
    uint32_t dir, mode, start_threshold, stop_threshold;
    uint32_t codec[8];
    uint32_t data_mode, flags;
};
static_assert(sizeof(SessionConfig) == 56 && offsetof(SessionConfig, data_mode) == 48);
struct MediaConfig { uint32_t rate, channels, format, data_format; };
struct BufferConfig { uint32_t count; size_t size, max_metadata_size; };
struct TagConfig { uint32_t tag, count; GraphKey kv; };
// PARAM_ID_RAT_MEDIA_FORMAT: 18 payload bytes (one channel entry), padded to 8 bytes.
struct RatMonoConfig {
    uint32_t instance, param = 0x080010d0, size = 18, error = 0;
    uint32_t rate = kRate;
    uint16_t bits = 16, q_factor = 15;
    uint32_t format = kFixedPoint, channels = 1;
    uint16_t channel_map = 3, padding[3]{}; // PCM_CHANNEL_C
};
static_assert(sizeof(RatMonoConfig) == 40 && offsetof(RatMonoConfig, channel_map) == 32);
struct Event { uint32_t source_module_id, event_id, event_payload_size; };
using EventCallback = void (*)(uint32_t, Event*, void*);
void ignore_event(uint32_t, Event*, void*) {}

// One production uplink per worker process. Keep the callback state alive
// until process exit, including any in-flight Binder delivery during cleanup.
std::atomic<uint32_t> uplink_session{0};
std::atomic<uint64_t> uplink_done{0};
void uplink_event(uint32_t session, Event* event, void*) {
    constexpr uint32_t kGslSource = 0x2001, kWriteDone = 2;
    if (session && session == uplink_session.load() && event && event->source_module_id == kGslSource &&
        event->event_id == kWriteDone && event->event_payload_size == 0) {
        uplink_done.fetch_add(1);
    }
}
struct AifInfo { char name[32]; uint32_t dir; };
static_assert(sizeof(AifInfo) == 36);

void* load_client() {
    // Pin for the lifetime of this process: vendor Binder threads may outlive a query/session.
    void* lib = dlopen("/vendor/lib64/libagmclient.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) fprintf(stderr, "[avcrtd:agm] cannot load AGM IPC client: %s\n", dlerror());
    return lib;
}

struct Api {
    int (*crash_cb)(void (*)(uint64_t), uint64_t) = nullptr;
    int (*callback)(uint32_t, EventCallback, uint32_t, void*) = nullptr;
    int (*metadata)(uint32_t, uint32_t, uint8_t*) = nullptr;
    int (*open)(uint32_t, uint32_t, uint64_t*) = nullptr;
    int (*tags)(uint32_t, uint32_t, void*, size_t*) = nullptr;
    int (*config)(uint64_t, SessionConfig*, MediaConfig*, BufferConfig*) = nullptr;
    int (*prepare)(uint64_t) = nullptr;
    int (*start)(uint64_t) = nullptr;
    int (*write)(uint64_t, void*, size_t*) = nullptr;
    int (*read)(uint64_t, void*, size_t*) = nullptr;
    int (*params)(uint32_t, void*, size_t) = nullptr;
    int (*tag_params)(uint32_t, uint32_t, TagConfig*) = nullptr;
    int (*stop)(uint64_t) = nullptr;
    int (*close)(uint64_t) = nullptr;
    bool load_session(void* lib) {
        return lib && symbol(lib, "agm_register_service_crash_callback", crash_cb) &&
               symbol(lib, "agm_session_register_cb", callback) &&
               symbol(lib, "agm_session_set_metadata", metadata) && symbol(lib, "agm_session_open", open) &&
               symbol(lib, "agm_session_aif_get_tag_module_info", tags) && symbol(lib, "agm_session_set_config", config) &&
               symbol(lib, "agm_session_prepare", prepare) && symbol(lib, "agm_session_start", start) &&
               symbol(lib, "agm_session_stop", stop) && symbol(lib, "agm_session_close", close);
    }
    bool load_read(void* lib) {
        return symbol(lib, "agm_session_read", read) && symbol(lib, "agm_session_set_params", params);
    }
    bool load(bool capture) {
        auto* lib = load_client();
        return load_session(lib) && symbol(lib, "agm_session_write", write) &&
               (!capture || (load_read(lib) && symbol(lib, "agm_set_params_with_tag", tag_params)));
    }
};

bool result(const char* stage, int rc) {
    if (rc) fprintf(stderr, "[avcrtd:agm] %s failed: %d\n", stage, rc);
    return rc == 0;
}

struct Stream {
    Api& api;
    uint32_t id;
    uint64_t handle = 0;
    uint32_t endpoint = 0;
    bool registered = false, edited = false, started = false;
    bool clean() {
        bool ok = true;
        if (started) ok &= result("stop", api.stop(handle));
        if (registered) ok &= result("unregister callback", api.callback(id, nullptr, kDataPathEvent, nullptr));
        // Close and clear only our private session. Global AIF metadata is never modified.
        if (handle) ok &= result("close", api.close(handle));
        if (edited) {
            uint32_t empty[4]{};
            ok &= result("clear metadata", api.metadata(id, sizeof(empty), reinterpret_cast<uint8_t*>(empty)));
        }
        started = registered = edited = false;
        handle = endpoint = 0;
        return ok;
    }
    ~Stream() { clean(); }

    bool register_metadata(uint32_t* metadata, uint32_t bytes, EventCallback events) {
        if (!result("register client", api.callback(id, events, kDataPathEvent, nullptr))) return false;
        registered = true;
        edited = true;
        return result("set metadata", api.metadata(id, bytes, reinterpret_cast<uint8_t*>(metadata)));
    }

    bool open(GraphKey key, EventCallback events = ignore_event) {
        uint32_t metadata[] = {1, key.id, key.value, 0, 0x08000010, 1, 1};
        return register_metadata(metadata, sizeof(metadata), events) &&
               result("open non-tunnel", api.open(id, kNonTunnel, &handle)) && handle;
    }

    bool prepare(bool capture, GraphKey uplink = {}, uint32_t expected_endpoint = 0) {
        std::array<uint32_t, 1024> tags{};
        size_t bytes = 0;
        if (!result("size graph tags", api.tags(id, UINT32_MAX, nullptr, &bytes)) ||
            bytes < 4 || bytes > tags.size() * 4 || bytes % 4) return false;
        const size_t required = bytes;
        if (!result("get graph tags", api.tags(id, UINT32_MAX, tags.data(), &bytes)) || bytes != required) return false;
        unsigned endpoints = 0, rat_tags = 0, mux_tags = 0;
        uint32_t rat = 0;
        size_t p = 1, words = bytes / 4;
        for (uint32_t n = tags[0]; n; --n) {
            if (words - p < 2) return false;
            const uint32_t tag = tags[p++], modules = tags[p++];
            if (modules > (words - p) / 2) return false;
            if (tag == kShmemEndpoint) {
                if (modules != 1 || !tags[p] || !tags[p + 1]) return false;
                if (tags[p] != (capture ? kReadShmemModule : kWriteShmemModule)) return false;
                endpoint = tags[p + 1];
                ++endpoints;
            }
            if (capture && tag == kRatRender) {
                if (modules != 1 || tags[p] != kRatModule || !tags[p + 1]) return false;
                rat = tags[p + 1];
                ++rat_tags;
            }
            if (capture && tag == kMuxDemux) {
                if (modules != 1 || tags[p] != kMuxModule || !tags[p + 1]) return false;
                ++mux_tags;
            }
            p += 2 * modules;
        }
        if (endpoints != 1 || (expected_endpoint && endpoint != expected_endpoint) ||
            (capture && (rat_tags != 1 || mux_tags != 1 || !valid(uplink)))) {
            fprintf(stderr, "[avcrtd:agm] required unique %s graph modules not found\n",
                    capture ? "Incall_Record" : "Incall_Music");
            return false;
        }
        const unsigned channels = capture ? 1 : kChannels;
        SessionConfig cfg{capture ? kTx : kRx, kNonTunnel, 0, 0, {}, kNonBlocking, 0};
        MediaConfig media{kRate, channels, kS16, kFixedPoint};
        BufferConfig buffer{kPeriods, kFrames * channels * sizeof(int16_t), 0};
        if (!result("configure PCM", api.config(handle, &cfg, &media, &buffer))) return false;
        if (capture) {
            TagConfig mux{kMuxDemux, 1, uplink};
            RatMonoConfig format{rat};
            // Configure UL-only (never read with the default mux) before starting.
            if (!result("set UL-only mux", api.tag_params(id, UINT32_MAX, &mux)) ||
                !result("set mono RAT format", api.params(id, &format, sizeof(format)))) return false;
        }
        return result("prepare", api.prepare(handle));
    }
};

uint32_t private_id() {
    uint32_t id = 0;
    const int random = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (random < 0) return 0;
    const ssize_t got = ::read(random, &id, sizeof(id));
    ::close(random);
    return got == sizeof(id) ? 0x40000000 | (id & 0x3fffffff) : 0;
}

// Use the ALSA control node: /proc/asound lists the links but this kernel
// does not expose the per-PCM status directories. This is a preflight, not
// an exclusive reservation against the vendor sound trigger service.
struct VaPcm {
    unsigned card = 0, device = 0, total = 0, available = 0;
};

bool va_pcm(std::string_view backend, VaPcm& pcm) {
    DIR* dir = opendir("/dev/snd");
    if (!dir) return false;
    bool ok = true;
    unsigned matches = 0;
    while (ok) {
        errno = 0;
        const dirent* ent = readdir(dir);
        if (!ent) { ok = errno == 0; break; }
        unsigned card;
        int end = 0;
        if (sscanf(ent->d_name, "controlC%u%n", &card, &end) != 1 || !end || ent->d_name[end]) continue;
        const int fd = ::open((std::string("/dev/snd/") + ent->d_name).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) { ok = false; break; }
        int device = -1;
        for (;;) {
            const int prev = device;
            if (ioctl(fd, SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE, &device) < 0) { ok = false; break; }
            if (device < 0) break;
            if (device <= prev || device > 4095) { ok = false; break; }
            snd_pcm_info info{};
            info.device = device;
            info.stream = SNDRV_PCM_STREAM_CAPTURE;
            if (ioctl(fd, SNDRV_CTL_IOCTL_PCM_INFO, &info) < 0) {
                if (errno == ENOENT) continue; // playback-only link
                ok = false;
                break;
            }
            const char* raw = reinterpret_cast<const char*>(info.id);
            const std::string_view name(raw, strnlen(raw, sizeof(info.id)));
            // ASoC can append the codec DAI and device number to the link name.
            if (name != backend && !(name.size() > backend.size() &&
                    name.substr(0, backend.size()) == backend && name[backend.size()] == ' ')) continue;
            ++matches;
            pcm = {card, static_cast<unsigned>(device), info.subdevices_count, info.subdevices_avail};
        }
        ::close(fd);
    }
    closedir(dir);
    return ok && matches == 1 && pcm.total && pcm.available <= pcm.total;
}

// The CODEC_DMA kernel control packs native-endian channel count and slot
// mask into its first two u32s. This is route state, not AIF ownership.
struct VaChannelMap {
    uint32_t channels = 0, slots = 0;
    bool operator==(const VaChannelMap& other) const {
        return channels == other.channels && slots == other.slots;
    }
};

bool va_channel_map(std::string_view backend, unsigned card, VaChannelMap& map) {
    constexpr unsigned kCodecDmaMapBytes = 20 * sizeof(uint32_t); // MAX_PORT in msm_common.c
    const auto ctl_name = std::string(backend) + " Channel Map";
    snd_ctl_elem_info info{};
    if (ctl_name.size() >= sizeof(info.id.name)) return false;
    info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    memcpy(info.id.name, ctl_name.c_str(), ctl_name.size() + 1);
    const auto path = "/dev/snd/controlC" + std::to_string(card);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    snd_ctl_elem_value value{};
    const bool ok = [&] {
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 || !info.id.numid ||
            info.type != SNDRV_CTL_ELEM_TYPE_BYTES || !(info.access & SNDRV_CTL_ELEM_ACCESS_READ) ||
            info.count != kCodecDmaMapBytes ||
            info.id.iface != SNDRV_CTL_ELEM_IFACE_MIXER || info.id.device ||
            info.id.subdevice || info.id.index ||
            memcmp(info.id.name, ctl_name.c_str(), ctl_name.size() + 1) != 0) return false;
        value.id = info.id;
        if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &value) < 0) return false;
        memcpy(&map.channels, value.value.bytes.data, sizeof(map.channels));
        memcpy(&map.slots, value.value.bytes.data + sizeof(map.channels), sizeof(map.slots));
        return map.channels == static_cast<uint32_t>(__builtin_popcount(map.slots));
    }();
    ::close(fd);
    return ok;
}

// Only private VA controls are touched; the HAL's shared ADC setup is left alone.
class VaMixerRoute {
    struct Control {
        snd_ctl_elem_info info{};
        unsigned before = 0, target = 0;
        bool mux = false, touched = false;

        bool get(int fd, unsigned& value) const {
            snd_ctl_elem_value v{};
            v.id = info.id;
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0) return false;
            if (mux) {
                value = v.value.enumerated.item[0];
                return value < info.value.enumerated.items;
            }
            if (v.value.integer.value[0] != 0 && v.value.integer.value[0] != 1) return false;
            value = v.value.integer.value[0];
            return true;
        }
        bool put(int fd, unsigned value) const {
            snd_ctl_elem_value v{};
            v.id = info.id;
            if (mux) v.value.enumerated.item[0] = value;
            else v.value.integer.value[0] = value;
            unsigned readback;
            return ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) >= 0 && get(fd, readback) && readback == value;
        }
        bool inspect(int fd, const snd_ctl_elem_id& id, const std::string& item) {
            info.id = id;
            mux = !item.empty();
            if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 || info.count != 1 ||
                !(info.access & SNDRV_CTL_ELEM_ACCESS_READ) || !(info.access & SNDRV_CTL_ELEM_ACCESS_WRITE) ||
                ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LOCK, &info.id) < 0) return false;
            if (mux) {
                if (info.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED || info.value.enumerated.items > 64) return false;
                unsigned found = 0;
                for (unsigned i = 0; i < info.value.enumerated.items; ++i) {
                    auto query = info;
                    query.value.enumerated.item = i;
                    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &query) < 0) return false;
                    const auto* name = query.value.enumerated.name;
                    if (item == std::string_view(name, strnlen(name, sizeof(query.value.enumerated.name)))) {
                        target = i;
                        ++found;
                    }
                }
                if (found != 1) return false;
            } else {
                if (info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN &&
                    (info.type != SNDRV_CTL_ELEM_TYPE_INTEGER || info.value.integer.min != 0 ||
                     info.value.integer.max != 1)) return false;
                target = 1;
            }
            return get(fd, before);
        }
    };
    struct Edge { unsigned aif, dec; snd_ctl_elem_id id; };
    int fd_ = -1;
    std::array<Control, 3> controls_{};
    std::vector<Edge> edges_;
    unsigned aif_ = 0, dec_ = 0;

    bool gates(bool engaged) const {
        for (const auto& edge : edges_) {
            if (edge.aif != aif_ && edge.dec != dec_) continue;
            snd_ctl_elem_value v{};
            v.id = edge.id;
            if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0 ||
                v.value.integer.value[0] != (engaged && edge.aif == aif_ && edge.dec == dec_)) return false;
        }
        return true;
    }

public:
    VaMixerRoute() = default;
    VaMixerRoute(const VaMixerRoute&) = delete;
    ~VaMixerRoute() { if (fd_ >= 0) { restore(); close(fd_); } }

    template <typename Stable>
    bool setup(unsigned card, unsigned aif, const std::string& mux, const std::string& input, Stable stable) {
        fd_ = ::open(("/dev/snd/controlC" + std::to_string(card)).c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd_ < 0) return false;
        aif_ = aif;
        snd_ctl_elem_id ids[32];
        snd_ctl_elem_list list{};
        list.space = 32;
        list.pids = ids;
        do {
            if (ioctl(fd_, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0 || list.used > list.space ||
                (!list.used && list.offset < list.count) || list.count > 8192) return false;
            for (unsigned i = 0; i < list.used; ++i) {
                const auto& id = ids[i];
                if (id.iface != SNDRV_CTL_ELEM_IFACE_MIXER) continue;
                char name[sizeof(id.name) + 1]{};
                memcpy(name, id.name, sizeof(id.name));
                unsigned found_aif, found_dec;
                int n = 0;
                if (sscanf(name, "VA_AIF%u_CAP Mixer DEC%u%n", &found_aif, &found_dec, &n) != 2 ||
                    !n || name[n]) continue;
                if (!found_aif || found_dec >= 32 || id.device || id.subdevice || id.index) return false;
                for (const auto& e : edges_) if (e.aif == found_aif && e.dec == found_dec) return false;
                edges_.push_back({found_aif, found_dec, id});
            }
            list.offset += list.used;
        } while (list.offset < list.count);
        std::sort(edges_.begin(), edges_.end(), [](const auto& a, const auto& b) { return a.dec < b.dec; });
        for (const auto& edge : edges_) {
            if (edge.aif != aif_) continue;
            dec_ = edge.dec;
            if (!stable() || !gates(false)) continue;
            const auto find = [&](const std::string& name, snd_ctl_elem_id& id) {
                if (name.size() >= sizeof(id.name)) return false;
                id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
                memcpy(id.name, name.c_str(), name.size() + 1);
                return true;
            };
            const auto n = std::to_string(dec_);
            snd_ctl_elem_id source{}, select{};
            if (!find((mux == "SWR_MIC" ? "VA SMIC MUX" : "VA DMIC MUX") + n, source) ||
                !find("VA DEC" + n + " MUX", select) ||
                !controls_[0].inspect(fd_, source, input) || !controls_[1].inspect(fd_, select, mux) ||
                !controls_[2].inspect(fd_, edge.id, {}) || controls_[2].before || !gates(false)) return false;
            for (auto& ctl : controls_) {
                unsigned now;
                if (!stable() || !ctl.get(fd_, now) || now != ctl.before) return false;
                if (ctl.before == ctl.target) continue;
                ctl.touched = true;
                if (!ctl.put(fd_, ctl.target)) return false;
            }
            return stable() && unchanged();
        }
        return false;
    }

    bool unchanged() const {
        for (const auto& ctl : controls_) {
            unsigned current;
            if (!ctl.get(fd_, current) || current != ctl.target) return false;
        }
        return gates(true);
    }

    bool restore() {
        bool ok = true;
        for (auto it = controls_.rbegin(); it != controls_.rend(); ++it) {
            if (!it->touched) continue;
            it->touched = false;
            unsigned current;
            if (!it->get(fd_, current)) { ok = false; continue; }
            if (current == it->target) ok = it->put(fd_, it->before) && ok;
            else if (current != it->before) ok = false;
        }
        return ok;
    }
};

struct VaApi : Api {
    int (*list)(AifInfo*, size_t*) = nullptr;
    int (*aif_metadata)(uint32_t, uint32_t, uint32_t, uint8_t*) = nullptr;
    int (*connect)(uint32_t, uint32_t, bool) = nullptr;
    bool load(bool read_pcm) {
        void* lib = load_client();
        if (!lib || !symbol(lib, "agm_get_aif_info_list", list) ||
            !symbol(lib, "agm_session_aif_set_metadata", aif_metadata)) return false;
        if (!read_pcm) return symbol(lib, "agm_session_set_metadata", metadata) &&
                              symbol(lib, "agm_session_aif_get_tag_module_info", tags);
        if (!load_session(lib) || !load_read(lib)) return false;
#ifdef AVCRT_VA_TEST_PRIVATE_PCM
        // The AIF is queried for tags, never connected to the private graph.
        return true;
#else
        return symbol(lib, "agm_session_aif_connect", connect);
#endif
    }
};

struct VaStream : Stream {
    VaApi& va;
    uint32_t aif;
    bool edited_aif = false, staged = false;
    VaStream(VaApi& api, uint32_t id, uint32_t aif) : Stream{api, id}, va(api), aif(aif) {}
    bool clean() {
        // A successful open adopts the staged connection; AGM close releases it.
        // If open never succeeded, cancel the staged connection explicitly.
        bool ok = true;
        if (staged && !handle) ok &= result("unstage VA AIF", va.connect(id, aif, false));
        ok &= Stream::clean();
        if (edited_aif) {
            uint32_t empty[4]{};
            ok &= result("clear private VA session/AIF metadata",
                         va.aif_metadata(id, aif, sizeof(empty), reinterpret_cast<uint8_t*>(empty)));
        }
        edited_aif = staged = false;
        return ok;
    }
    ~VaStream() { clean(); }
};

bool codec_dma_interface(std::string_view backend, std::string_view prefix, unsigned& index) {
    // Vendor link names use zero-based TX suffixes; SPF uses one-based indices.
    if (backend.substr(0, prefix.size()) != prefix) return false;
    const auto suffix = backend.substr(prefix.size());
    unsigned zero_based = 0;
    const auto parsed = std::from_chars(suffix.begin(), suffix.end(), zero_based);
    if (parsed.ec != std::errc{} || parsed.ptr != suffix.end() || zero_based == UINT32_MAX) return false;
    index = zero_based + 1;
    return true;
}

bool va_dma_interface(std::string_view backend, unsigned& index) {
    return codec_dma_interface(backend, "CODEC_DMA-LPAIF_VA-TX-", index) && index <= 3;
}

// The ALSA substream owns codec hw_params; AGM owns only the
// private DSP graph. Do not connect the graph to an AGM AIF or write its format.
struct VaBackendFd {
    pcm* device = nullptr;
    ~VaBackendFd() { close(); }
    bool setup(const VaPcm& info, unsigned rate) {
        pcm_config cfg{};
        cfg.channels = 1;
        cfg.rate = rate;
        cfg.period_size = rate / 100;
        cfg.period_count = kPeriods;
        cfg.format = PCM_FORMAT_S16_LE;
        device = pcm_open(info.card, info.device, PCM_IN, &cfg);
        if (pcm_is_ready(device)) {
            // Match the actual open fd to the hardware PCM found via ALSA
            // control ioctls; do not accept a tinyalsa plugin as the VA link.
            const int fd = pcm_get_poll_fd(device);
            const auto path = "/dev/snd/pcmC" + std::to_string(info.card) + "D" + std::to_string(info.device) + "c";
            struct stat actual{}, expected{};
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && fstat(fd, &actual) == 0 &&
                stat(path.c_str(), &expected) == 0 && S_ISCHR(actual.st_mode) &&
                actual.st_rdev == expected.st_rdev && pcm_prepare(device) == 0) return true;
        }
        fprintf(stderr, "[avcrtd:agm-va] standalone ALSA prepare: %s\n", pcm_get_error(device));
        close();
        return false;
    }
    bool close() {
        if (!device) return true;
        const int rc = pcm_close(device);
        device = nullptr;
        return result("close standalone VA ALSA", rc);
    }
};

struct VaHwConfig {
    // Public AudioReach APM module parameters, each padded to eight bytes.
    uint32_t media_instance, media_param = 0x08001017, media_size = 12, media_error = 0;
    uint32_t rate;
    uint16_t bits = 16, channels = 1;
    uint32_t format = kFixedPoint, media_padding = 0;
    uint32_t dma_instance, dma_param = 0x08001063, dma_size = 12, dma_error = 0;
    uint32_t lpaif = 3, index, mask, dma_padding = 0;
};
static_assert(sizeof(VaHwConfig) == 64 && offsetof(VaHwConfig, dma_instance) == 32);

#ifdef AVCRT_AGM_LAB
template <typename Stable> bool va_levels(VaStream& stream, Stable stable, unsigned rate,
                                         bool in_call = false, TxLease* test_mute = nullptr) {
    MonoPeriod pcm{};
    const size_t period_bytes = rate / 100 * sizeof(pcm[0]);
    constexpr unsigned periods = 300, warmup = 20;
    uint64_t samples = 0, nonzero = 0, energy = 0;
    int peak = 0;
    unsigned i = 0, complete = 0, empty = 0, short_reads = 0;
#if defined(AVCRT_VA_TEST_PCM) && defined(AVCRT_VA_TEST_ACOUSTIC)
    // Lab-only check against sound from an external speaker. Keep only per-band
    // power, not audio; no signal generation or playback on the device.
    constexpr std::array<unsigned, 2> tones{1000, 1500};
    std::array<double, tones.size()> power{};
    unsigned windows = 0;
    if (!in_call) fprintf(stderr, "[va-acoustic] measure 1000/1500 Hz from external source only\n");
#else
    if (!in_call) fprintf(stderr, "[avcrtd:agm-va] speak normally for 3 s; capture levels only, no playback or mute\n");
#endif
    if (in_call) fprintf(stderr, "[avcrtd:agm-va] SIM levels: speak for 3 s; %s\n",
                         test_mute ? "brief TX mute, no playback" : "no playback or mute");
    uint64_t window_energy = 0, window_samples = 0;
    constexpr unsigned mute_begin = 100, mute_end = 200;
    unsigned muted_full = 0;
    uint64_t muted_energy = 0, muted_samples = 0;
    bool mute_cleared = false, release_failed = false;
    const auto start = Clock::now() + std::chrono::milliseconds(10);
    for (; i < periods; ++i) {
        const auto due = start + std::chrono::milliseconds(i * 10);
        std::this_thread::sleep_until(due);
        if (Clock::now() > due + std::chrono::milliseconds(25)) {
            fprintf(stderr, "[avcrtd:agm-va] capture deadline missed at period %u\n", i);
            break;
        }
        if (!stable()) { fprintf(stderr, "[avcrtd:agm-va] route or call changed before read\n"); break; }
        if (test_mute && i == mute_end) {
            mute_cleared = test_mute->release();
            release_failed = !mute_cleared;
            fprintf(stderr, "[avcrtd:agm-va] stock TX mute %s\n", mute_cleared ? "cleared" : "clear unverified");
            if (!mute_cleared || !stable()) break;
        }
        if (test_mute && i >= mute_begin && i < mute_end) {
            // Standalone consented diagnostic only; production still requires fresh uplink PCM.
            if (complete != i - warmup || Clock::now() >= start + std::chrono::milliseconds(mute_end * 10)) break;
            if (i % 5 == 0 && !test_mute->renew()) break;
            if (i == mute_begin) fprintf(stderr, "[avcrtd:agm-va] stock TX muted: mask=%#x (1 s max scheduled)\n", test_mute->mask());
        }
        size_t bytes = period_bytes;
        const int rc = stream.api.read(stream.handle, pcm.data(), &bytes);
        if (!stable()) { fprintf(stderr, "[avcrtd:agm-va] route or call changed after read\n"); break; }
        const bool muted = test_mute && i >= mute_begin && i < mute_end;
        if (muted && (rc || bytes != period_bytes)) {
            fprintf(stderr, "[avcrtd:agm-va] incomplete VA block during mute: rc=%d bytes=%zu\n", rc, bytes);
            break;
        }
        if (muted) ++muted_full;
        if (bytes == 0 && (rc == 0 || rc == -EAGAIN || rc == -ENODATA || rc == -ENOSPC)) {
            if (i >= warmup) ++empty;
            continue;
        }
        if (!result("read VA PCM", rc) || bytes > period_bytes || bytes % sizeof(int16_t)) break;
        if (i < warmup) continue;
        if (bytes == period_bytes) ++complete;
        else ++short_reads;
#if defined(AVCRT_VA_TEST_PCM) && defined(AVCRT_VA_TEST_ACOUSTIC)
        if (!in_call && bytes == period_bytes) {
            const size_t frames = bytes / sizeof(int16_t);
            for (size_t t = 0; t < tones.size(); ++t) {
                constexpr double tau = 6.283185307179586;
                const double coeff = 2 * std::cos(tau * tones[t] / rate);
                double q1 = 0, q2 = 0;
                for (size_t j = 0; j < frames; ++j) {
                    const double q0 = static_cast<double>(pcm[j]) + coeff * q1 - q2;
                    q2 = q1;
                    q1 = q0;
                }
                power[t] += (q1 * q1 + q2 * q2 - coeff * q1 * q2) * (4.0 / (frames * frames));
            }
            ++windows;
        }
        if (!in_call && (i - warmup) % 10 == 9) {
            if (windows == 10)
                fprintf(stderr, "[va-acoustic] t=%u ms amplitude 1000Hz=%.1f 1500Hz=%.1f\n",
                        (i - 9) * 10, std::sqrt(power[0] / windows), std::sqrt(power[1] / windows));
            else
                fprintf(stderr, "[va-acoustic] t=%u ms incomplete (%u/10)\n", (i - 9) * 10, windows);
            power.fill(0);
            windows = 0;
        }
#endif
        for (size_t j = 0; j < bytes / sizeof(int16_t); ++j) {
            const int s = pcm[j];
            nonzero += s != 0;
            energy += s * s;
            window_energy += s * s;
            if (muted) { muted_energy += s * s; ++muted_samples; }
            peak = std::max(peak, std::abs(s));
            ++samples;
            ++window_samples;
        }
        if (in_call && (i - warmup) % 20 == 19) {
            fprintf(stderr, "[avcrtd:agm-va] t=%u ms RMS=%.1f samples=%llu\n", (i - 19) * 10,
                    window_samples ? std::sqrt(static_cast<double>(window_energy) / window_samples) : 0.0,
                    static_cast<unsigned long long>(window_samples));
            window_energy = window_samples = 0;
        }
    }
    bool release_ok = true;
    if (test_mute) {
        // Runs on every normal/error exit, before returning to graph teardown.
        release_ok = test_mute->release() && !release_failed;
        fprintf(stderr, "[avcrtd:agm-va] muted capture: %u/%u full blocks, RMS=%.1f; %s\n",
                muted_full, mute_end - mute_begin,
                muted_samples ? std::sqrt(static_cast<double>(muted_energy) / muted_samples) : 0.0,
                release_ok ? "no outstanding mute attempt" : "kernel lease expiry is fallback");
    }
    const double rms = samples ? std::sqrt(static_cast<double>(energy) / samples) : 0.0;
    fprintf(stderr, "[avcrtd:agm-va] %u/%u periods, %u full, %u empty, %u short after warmup; "
                    "samples=%llu nonzero=%llu peak=%d RMS=%.1f; mic independence unverified\n",
            i, periods, complete, empty, short_reads, static_cast<unsigned long long>(samples),
            static_cast<unsigned long long>(nonzero), peak, rms);
    return i == periods && complete == periods - warmup && nonzero != 0 &&
           (!test_mute || (mute_cleared && release_ok && muted_full == mute_end - mute_begin && muted_energy));
}
#endif

enum class UplinkMode { Replace, UsbReplace };

template <typename Stable, typename Tx>
bool replace_uplink(Stream& capture, Stream& music, Stable stable, unsigned rate, Tx& tx,
                bool trial = true, bool* rollback_ok = nullptr, WorkerProgress* progress = nullptr,
                cellular::AgmRecovery* recovery = nullptr, UplinkMode mode = UplinkMode::Replace,
                void (*usb_evidence)(void*, uint64_t, Clock::time_point) = nullptr, void* usb_tx = nullptr);

#ifdef AVCRT_AGM_LAB
int probe_va(const Voice* voice = nullptr, TxLease* tx = nullptr, unsigned tx_card = 0,
             bool test_mute = false, bool replace = false) {
    if (tx && (!voice || !voice->vsid || !tx->stable())) return 1;
    const auto voice_stable = [&] { return voice && voice->unchanged() && (!tx || tx->stable()); };
    const VaResource va = discover_va_resource();
    if (va.backend.empty()) {
        fprintf(stderr, "[avcrtd:agm-va] no unambiguous handset VA resource declaration\n");
        return 1;
    }
    fprintf(stderr, "[avcrtd:agm-va] declared backend=%s path=%s rate=%s channels=%s bits=%s\n",
            va.backend.c_str(), va.path.c_str(), va.rate.c_str(), va.channels.c_str(), va.bits.c_str());

    VaPcm before;
    const bool has_pcm = va_pcm(va.backend, before);
    if (has_pcm) fprintf(stderr, "[avcrtd:agm-va] ALSA capture C%uD%u: %u/%u substreams free\n",
                         before.card, before.device, before.available, before.total);
    else fprintf(stderr, "[avcrtd:agm-va] no unique readable ALSA capture link for declared backend\n");
    VaChannelMap initial_map;
    const bool has_map = has_pcm && va_channel_map(va.backend, before.card, initial_map);
    if (has_map) {
        fprintf(stderr, "[avcrtd:agm-va] ALSA channel map: channels=%u slots=%#x (not a reservation)\n",
                initial_map.channels, initial_map.slots);
    } else {
        fprintf(stderr, "[avcrtd:agm-va] ALSA channel map missing, unreadable or incompatible\n");
    }
#ifdef AVCRT_VA_TEST_PRIVATE_PCM
    unsigned dma_index = 0;
    if (!va_dma_interface(va.backend, dma_index)) {
        fprintf(stderr, "[avcrtd:agm-va] no private-graph DMA adapter for backend %s\n", va.backend.c_str());
        return 1;
    }
    // An empty map is normal before selecting a route; a missing or
    // incompatible map is not a match even if the graph tags exist.
    if (!has_pcm || !has_map) return 1;
#endif
    if (voice && (!voice_stable() || !has_pcm || (tx && before.card != tx_card) ||
                  before.total != 1 || before.available != 1 || va.rate != "48000" ||
                  va.channels != "1" || va.bits != "16")) {
        fprintf(stderr, "[avcrtd:agm-va] refused: need stable voice state and one free VA capture link declared 48k/S16/mono on the expected card\n");
        return 1;
    }
    if (voice && (!has_map || initial_map.channels != 1)) {
        fprintf(stderr, "[avcrtd:agm-va] refused before opening AGM: no active mono VA channel map\n");
        return 1;
    }

    VaApi api;
    if (!api.load(voice != nullptr)) return 1;

    std::array<AifInfo, 512> aifs{};
    if (va.backend.size() >= sizeof(aifs[0].name) - 1) {
        fprintf(stderr, "[avcrtd:agm-va] backend name cannot be matched without truncation\n");
        return 1;
    }
    size_t count = 0;
    if (!result("size AIF list", api.list(nullptr, &count)) || !count || count > aifs.size()) return 1;
    const size_t expected = count;
    if (!result("list AIFs", api.list(aifs.data(), &count)) || count != expected) return 1;
    uint32_t aif = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        const std::string_view name(aifs[i].name, strnlen(aifs[i].name, sizeof(aifs[i].name)));
        if (name != va.backend) continue;
        if (aif != UINT32_MAX || aifs[i].dir != kTx) {
            fprintf(stderr, "[avcrtd:agm-va] ambiguous or non-TX AIF\n");
            return 1;
        }
        aif = i;
    }
    if (aif == UINT32_MAX) {
        fprintf(stderr, "[avcrtd:agm-va] declared backend not exposed by AGM (%zu AIFs)\n", count);
        return 1;
    }
    fprintf(stderr, "[avcrtd:agm-va] exact AGM TX AIF match: %u\n", aif);

#ifdef AVCRT_VA_GRAPH_SCAN
    // Standalone lab build only. Metadata and tag queries, no graph open.
    if (!voice) {
        const auto stream = discover_key("StreamTX", "PCM_Record");
        for (const char* name : {"Handset_Mic", "Speaker_Mic", "VoiceActivation_Mic", "VoiceActivation_Headset_Mic"}) {
            const auto device = discover_key("DeviceTX", name);
            if (!valid(device)) continue;
            if (!valid(stream)) continue;
            for (const char* profile : {"Instance_1", "Instance_2", "Instance_3", "Instance_4"}) {
                const auto instance = discover_key("Instance", profile);
                if (!valid(instance)) continue;
                const auto id = private_id();
                if (!id) return 1;
                VaStream query(api, id, aif);
                uint32_t session_md[]{2, stream.id, stream.value, instance.id, instance.value, 0, 0x08000010, 1, 1};
                uint32_t plain_md[]{1, device.id, device.value, 0, 0x08000010, 1, 3};
                query.edited = true;
                if (api.metadata(id, sizeof(session_md), reinterpret_cast<uint8_t*>(session_md))) return 1;
                query.edited_aif = true;
                if (api.aif_metadata(id, aif, sizeof(plain_md), reinterpret_cast<uint8_t*>(plain_md))) return 1;
                std::array<uint32_t, 1024> tags{};
                size_t size = 0;
                const int rc = api.tags(id, aif, nullptr, &size);
                fprintf(stderr, "[va-scan] %s/%s: rc=%d bytes=%zu", name, profile, rc, size);
                if (!rc && size && size <= sizeof(tags) && size % sizeof(uint32_t) == 0 &&
                    !api.tags(id, aif, tags.data(), &size) && size <= sizeof(tags) && size % sizeof(uint32_t) == 0) {
                    const size_t words = size / sizeof(uint32_t);
                    size_t offset = 1;
                    for (uint32_t n = tags[0]; n && offset + 2 <= words; --n) {
                        const auto tag = tags[offset++], count = tags[offset++];
                        if (count > (words - offset) / 2) break;
                        fprintf(stderr, " %#x", tag);
                        for (uint32_t j = 0; j < count; ++j) {
                            fprintf(stderr, "/%#x:%#x", tags[offset], tags[offset + 1]);
                            offset += 2;
                        }
                    }
                }
                fputc('\n', stderr);
                if (!query.clean()) return 1;
            }
        }
        return 0;
    }
#endif

#ifdef AVCRT_VA_TEST_PCM
    const GraphKey stream = discover_key("StreamTX", "PCM_Record");
#ifdef AVCRT_VA_TEST_SPEAKER
    const GraphKey device = discover_key("DeviceTX", "Speaker_Mic");
    const GraphKey pp = discover_key("DevicePP_Tx", "Audio_Fluence_Endfire");
#else
    const GraphKey device = discover_key("DeviceTX", "Handset_Mic");
    const GraphKey pp = discover_key("DevicePP_Tx", "Audio_Fluence_SMECNS");
#endif
#else
    const GraphKey stream = discover_key("StreamTX", "SENSOR_PCM_DATA");
    const GraphKey device = discover_key("DeviceTX", "VoiceActivation_Mic");
    const GraphKey pp = discover_key("DevicePP_Tx", "Fluence_FFECNS");
#endif
#if defined(AVCRT_VA_TEST_PCM) && defined(AVCRT_VA_TEST_INSTANCE)
    const GraphKey instance = discover_key("Instance", AVCRT_VA_TEST_INSTANCE);
#else
    const GraphKey instance = discover_key("Instance", "Instance_1");
#endif
    const uint32_t id = private_id();
    if (!id || !valid(stream) || !valid(device) || !valid(pp) || !valid(instance)) {
        fprintf(stderr, "[avcrtd:agm-va] VA graph metadata not available\n");
        return 1;
    }

    // Only private metadata; never set a global BE format, mixer path or device params.
    VaStream capture(api, id, aif);
    uint32_t session_md[]{2, stream.id, stream.value, instance.id, instance.value, 0, 0x08000010, 1, 1};
    uint32_t aif_md[]{2, device.id, device.value, pp.id, pp.value, 0, 0x08000010, 1, 3};
    const auto bytes = [](uint32_t* data) { return reinterpret_cast<uint8_t*>(data); };
    if (voice) {
        if (!result("connect IPC", api.crash_cb([](uint64_t) { _exit(1); }, 0)) || !voice_stable() ||
            !result("register VA client", api.callback(id, ignore_event, kDataPathEvent, nullptr))) return 1;
        capture.registered = true;
    }
    capture.edited = true;
    if (!result("set private VA session metadata", api.metadata(id, sizeof(session_md), bytes(session_md)))) return 1;
    capture.edited_aif = true;
    if (!result("set private VA session/AIF metadata", api.aif_metadata(id, aif, sizeof(aif_md), bytes(aif_md)))) return 1;
    std::array<uint32_t, 1024> payload{};
    size_t size = 0;
    if (!result("size VA graph tags", api.tags(id, aif, nullptr, &size)) ||
        size < sizeof(uint32_t) || size > sizeof(payload) || size % sizeof(uint32_t)) return 1;
    const size_t required = size;
    if (!result("get VA graph tags", api.tags(id, aif, payload.data(), &size)) || size != required) return 1;

    constexpr uint32_t hw_tx_tag = 0xc0000005;
#ifdef AVCRT_VA_TEST_PCM
    constexpr uint32_t read_tag = 0xc0000001, read_module = 0x07001001;
#else
    constexpr uint32_t read_tag = 0xc000002e, read_module = 0x070010d5;
#endif
    const size_t words = size / sizeof(uint32_t);
    unsigned reads = 0, inputs = 0;
#ifdef AVCRT_VA_TEST_PRIVATE_PCM
    uint32_t codec_source = 0;
#endif
    size_t p = 1;
    for (uint32_t n = payload[0]; n; --n) {
        if (words - p < 2) return 1;
        const uint32_t tag = payload[p++], modules = payload[p++];
        if (modules > (words - p) / 2) return 1;
        if (tag == read_tag || tag == hw_tx_tag) {
            if (modules != 1 || !payload[p] || !payload[p + 1]) return 1;
            if (tag == read_tag) {
                if (payload[p] != read_module) return 1;
                ++reads;
            } else {
#ifdef AVCRT_VA_TEST_PRIVATE_PCM
                if (payload[p] != 0x07001024u) return 1; // MODULE_ID_CODEC_DMA_SOURCE
                codec_source = payload[p + 1];
#endif
                ++inputs;
            }
        }
        for (uint32_t j = 0; j < modules; ++j) {
            const uint32_t module = payload[p++], miid = payload[p++];
            fprintf(stderr, "[avcrtd:agm-va] tag=%#x module=%#x instance=%#x\n", tag, module, miid);
        }
    }
    if (p != words || reads != 1 || inputs != 1) {
        fprintf(stderr, "[avcrtd:agm-va] expected one hardware TX and one SHMEM read endpoint in the ACDB query\n");
        return 1;
    }
    if (!voice) {
        fprintf(stderr, "[avcrtd:agm-va] ACDB read endpoint found; stream not opened, host PCM and mic independence unverified\n");
        return capture.clean() ? 0 : 1;
    }

    const auto map_unchanged = [&] {
        VaChannelMap map;
        return va_channel_map(va.backend, before.card, map) && map == initial_map;
    };
    const auto stable = [&] { return voice_stable() && map_unchanged(); };
    VaPcm now;
    if (!va_pcm(va.backend, now) || now.card != before.card || now.device != before.device ||
        now.total != before.total || now.available != before.available ||
        !stable()) return 1;
#ifdef AVCRT_VA_TEST_PRIVATE_PCM
    VaBackendFd backend_fd;
    const bool observed_pcm = [&] {
        constexpr unsigned rate = 16000; // lab profile previously read at 16k
        const std::string_view backend = va.backend;
        const auto owned = [&] {
            VaPcm pcm;
            return stable() && va_pcm(backend, pcm) && pcm.card == before.card &&
                   pcm.device == before.device && pcm.total == 1 && pcm.available == 0;
        };
        if (!stable() || !backend_fd.setup(before, rate) || !owned()) return false;
        fprintf(stderr, "[avcrtd:agm-va] standalone ALSA prepared %u Hz mono; testing private graph\n", rate);
        uint32_t md[]{4, stream.id, stream.value, instance.id, instance.value,
                      device.id, device.value, pp.id, pp.value, 0, 0x08000010, 4, 1, 3, 2, 5};
        // The AIF query above checked the candidate endpoints. AGM only
        // accepts the no-AIF query after open sets the session mode. Recheck
        // the private graph before configuring or starting it.
        if (!result("set private combined VA metadata", api.metadata(id, sizeof(md),
                     reinterpret_cast<uint8_t*>(md)))) return false;
        if (!owned() || !result("open private VA graph", api.open(id, kNonTunnel, &capture.handle)) ||
            !capture.handle) return false;
        std::array<uint32_t, 1024> direct{};
        size_t direct_size = 0;
        if (!result("size private VA tags", api.tags(id, UINT32_MAX, nullptr, &direct_size)) ||
            direct_size != size || !result("query private VA tags", api.tags(id, UINT32_MAX, direct.data(), &direct_size)) ||
            direct_size != size || memcmp(direct.data(), payload.data(), size) != 0) {
            fprintf(stderr, "[avcrtd:agm-va] private graph tags differ from the AIF graph\n");
            return false;
        }
        SessionConfig cfg{kTx, kNonTunnel, 0, 0, {}, kNonBlocking, 0};
        MediaConfig media{rate, 1, kS16, kFixedPoint};
        BufferConfig buffer{kPeriods, rate / 100 * sizeof(int16_t), 0};
        VaHwConfig hw{codec_source, 0x08001017, 12, 0, rate, 16, 1, kFixedPoint, 0,
                      codec_source, 0x08001063, 12, 0, 3, dma_index, initial_map.slots, 0};
        if (!owned() || !result("set private VA graph format", api.config(capture.handle, &cfg, &media, &buffer)) ||
            !result("set private VA hardware endpoint params", api.params(id, &hw, sizeof(hw))) ||
            !owned() || !result("prepare private VA graph", api.prepare(capture.handle))) return false;
        Stream music{api, replace ? private_id() : 0};
        if (replace) {
            const auto key = discover_music();
            auto* lib = load_client();
            if (!tx || !music.id || music.id == id || !valid(key) || !lib ||
                !symbol(lib, "agm_session_write", api.write) || !owned() ||
                !music.open(key) || !music.prepare(false)) return false;
        }
        if (!owned() || !result("start private VA graph", api.start(capture.handle))) return false;
        capture.started = true;
        if (replace) return replace_uplink(capture, music, owned, rate, *tx);
        return va_levels(capture, owned, rate, tx != nullptr, test_mute ? tx : nullptr);
    }();
    const bool graph_clean = capture.clean();
    const bool clean = backend_fd.close() && graph_clean;
#else
    (void)test_mute;
    (void)replace;
    const bool observed_pcm = [&] {
        // For a closed private session connect only stages the device. Tunnel open
        // adopts that association and may open the ALSA link. No stock FE is used.
        if (!result("stage VA AIF", api.connect(id, aif, true))) return false;
        capture.staged = true;
        constexpr uint32_t mode =
#if defined(AVCRT_VA_TEST_TUNNEL) || defined(AVCRT_VA_TEST_PCM)
            0; // AGM_SESSION_DEFAULT; avoids COMPRESS's mandatory SHMEM config.
#else
            kCompress;
#endif
        if (!stable() || !result("open VA capture", api.open(id, mode, &capture.handle)) ||
            !capture.handle) return false;
        SessionConfig cfg{kTx, mode, 0, 0, {}, kNonBlocking, 0};
        constexpr unsigned capture_rate =
#ifdef AVCRT_VA_TEST_PCM
            16000;
#else
            kRate;
#endif
        MediaConfig media{capture_rate, 1, kS16, kFixedPoint};
        BufferConfig buffer{kPeriods, capture_rate / 100 * sizeof(int16_t), 0};
        if (!stable() || !result("configure VA PCM16 mono", api.config(capture.handle, &cfg, &media, &buffer)) ||
            !stable() || !result("prepare VA capture", api.prepare(capture.handle)) ||
            !stable() || !result("start VA capture", api.start(capture.handle))) return false;
        capture.started = true;
        return va_levels(capture, stable, capture_rate);
    }();
    const bool clean = capture.clean();
#endif
    VaPcm after;
    const bool restored = va_pcm(va.backend, after) && after.card == before.card && after.device == before.device &&
                           after.total == before.total && after.available == before.available;
    const bool map_same = map_unchanged();
    fprintf(stderr, "[avcrtd:agm-va] cleanup %s, ALSA availability %s, channel map %s; %s\n",
             clean ? "OK" : "failed", restored ? "restored" : "changed or unreadable",
             map_same ? "unchanged" : "changed or unreadable", replace ? "replacement trial ended" :
             test_mute ? "no playback" : "no playback or mute");
    return observed_pcm && clean && restored && map_same && voice_stable() ? 0 : 1;
}

constexpr size_t kClipSamples = 4 * kRate * kChannels;
constexpr size_t kStereoFrameBytes = kChannels * sizeof(int16_t);
constexpr unsigned kCapturePeriods = 300;

bool read_speech(std::vector<int16_t>& speech) {
    constexpr size_t max_samples = kClipSamples;
    speech.resize(max_samples + 1);
    // Read and process everything before touching AGM. An incomplete frame or oversized clip
    // must not leave a partially submitted stream. Android ARM uses little endian PCM.
    const size_t bytes = fread(speech.data(), 1, max_samples * sizeof(int16_t) + 1, stdin);
    if (!feof(stdin) || ferror(stdin) || !bytes || bytes > max_samples * sizeof(int16_t) ||
        bytes % kStereoFrameBytes != 0) {
        fprintf(stderr, "[avcrtd:agm] expected up to 4 s of raw S16LE 48 kHz stereo PCM on stdin\n");
        return false;
    }
    if (bytes >= 12 && memcmp(speech.data(), "RIFF", 4) == 0 &&
        memcmp(reinterpret_cast<const char*>(speech.data()) + 8, "WAVE", 4) == 0) {
        fprintf(stderr, "[avcrtd:agm] WAV is not raw PCM; convert it before sending\n");
        return false;
    }
    speech.resize(bytes / sizeof(int16_t));
    return true;
}

DspConfig test_pitch() {
    DspConfig cfg;
    cfg.enabled = true;
    cfg.pitch_semitones = 7.0;
    cfg.preserve_formants = false;
    return cfg;
}
#endif

struct PcmLevels {
    int peak = 0;
    uint64_t energy = 0, count = 0, clipped = 0;

    void add(const int16_t* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            const int s = data[i];
            peak = std::max(peak, std::abs(s));
            energy += s * s;
            clipped += s == INT16_MIN || s == INT16_MAX;
        }
        count += size;
    }

    template <size_t N> void add(const std::array<int16_t, N>& block) {
        add(block.data(), N);
    }
    double rms() const { return count ? std::sqrt(static_cast<double>(energy) / count) : 0.0; }
};

template <typename Stable, typename Tx>
bool replace_uplink(Stream& capture, Stream& music, Stable stable, unsigned rate, Tx& tx,
                bool trial, bool* rollback_ok, WorkerProgress* progress, cellular::AgmRecovery* recovery,
                UplinkMode mode, void (*usb_evidence)(void*, uint64_t, Clock::time_point), void* usb_tx) {
    using namespace std::chrono_literals;
    const bool usb_replace = mode == UplinkMode::UsbReplace;
    if (!rate || rate % 100 || kRate % rate) return false;
    const unsigned ratio = kRate / rate;
    const size_t input_frames = rate / 100, input_bytes = input_frames * sizeof(int16_t);
    MonoPeriod input{};
    MonoPeriod mono{};
    Period stereo{};
    // Reuse the application's real preset and shared soundboard path. Keep the
    // time base fixed for live audio; all selected voice/effect parameters and
    // soundboard state otherwise follow the same Engine used by Nord/Zygisk.
    Engine engine(kRate, 1, true);
    ConfigStore preset;
    DspConfig processing = preset.snapshot();
    if (!processing.enabled || (usb_replace && (!usb_evidence || !usb_tx))) {
        fprintf(stderr, "[avcrtd:agm-va-replace] enable AVCRT in the app before running this probe\n");
        return false;
    }
    processing.time_ratio = 1.0;
    engine.update(processing);
    auto& api = capture.api;
    const auto at = [&](const char* label) { if (progress) progress->at(label); };
    if (!stable()) return false;
    at("start Incall_Music");
    if (!result("start VA replacement uplink", api.start(music.handle))) return false;
    music.started = true;
    const auto start = Clock::now() + 10ms;
    constexpr unsigned replace_limit = 500;
    const unsigned prime_limit = trial ? 200 : 1000;
    constexpr auto period = 10ms, slack = 35ms;
    UplinkWindow uplink(kPeriods);
    UplinkPriming<kPeriods> priming;
    UplinkSilence silence;
    UplinkTiming timing;
    unsigned good_input = 0;
    uint64_t silent_blocks = 0, capture_gap_blocks = 0, late_blocks = 0;
    const auto done = [&] { return trial ? uint64_t{0} : uplink_done.load(); };
    struct Lease {
        Tx& tx;
        const decltype(at)& stage;
        Clock::time_point began{};
        bool renew() {
            began = Clock::now();
            stage("renew TX mute");
            return tx.renew();
        }
        bool release() {
            stage("release TX mute");
            return tx.release();
        }
    } lease{tx, at};
    cellular::ReplacementSession transaction(lease);
    int previous = 0;
    bool reanchor = true, switching = false, drain_capture = false;
    auto due = start;
    PcmLevels input_levels, engine_levels, sent_levels;
    constexpr unsigned kLevelBlocks = 200;
    unsigned measured = 0;
    const auto measured_revision = processing.revision;
    const auto report_levels = [&] {
        fprintf(stderr, "[avcrtd:agm] levels (preset revision=%lld, %u full periods while replacing, PCM16): "
                        "VA rms=%.1f peak=%d rail=%llu; Engine rms=%.1f peak=%d rail=%llu; "
                        "uplink rms=%.1f peak=%d rail=%llu\n",
                static_cast<long long>(measured_revision), measured, input_levels.rms(), input_levels.peak,
                static_cast<unsigned long long>(input_levels.clipped),
                engine_levels.rms(), engine_levels.peak, static_cast<unsigned long long>(engine_levels.clipped),
                sent_levels.rms(), sent_levels.peak, static_cast<unsigned long long>(sent_levels.clipped));
    };
    int64_t first_capture = -1, first_full = -1, first_write = -1, first_done = -1;
    unsigned zero_prime_blocks = 0;
    bool prime_exhausted = true;
    const char* reason = "no usable PCM before priming deadline";
    for (int64_t i = 0; transaction.periods() ? (!trial || transaction.periods() < replace_limit) : i < prime_limit;
         ++i, due += period) {
        using cellular::Block;
        using cellular::Step;
        using cellular::PeriodCheck;
        using cellular::AgmRecovery;
        const auto renewals = transaction.periods();
        std::this_thread::sleep_until(due);
        at("read app config");
        auto latest = preset.snapshot();
        if (!latest.enabled) { reason = "disabled in app"; prime_exhausted = false; break; }
        if (latest.revision != processing.revision) {
            latest.time_ratio = 1.0;
            processing = latest;
            engine.update(processing);
            switching = !trial && renewals != 0;
            if (switching) fprintf(stderr, "[avcrtd:agm] updating preset %lld in place\n",
                                   static_cast<long long>(processing.revision));
        }
        // Check the live binding even on a late period, before the transaction
        // releases the lease. A revoked or rerouted lease is not a timing retry.
        bool missed_deadline = false;
        const auto check_deadline = [&] {
            const auto now = Clock::now();
            if (!trial && timing.grace_elapsed(now)) {
                missed_deadline = true;
                return PeriodCheck::Invalid;
            }
            missed_deadline = now >= due + slack;
            if (!missed_deadline) return PeriodCheck::Ready;
            return !trial && timing.can_skip(now) ? PeriodCheck::SkipLate : PeriodCheck::Invalid;
        };
        bool committing_first = false;
        const auto valid_period = [&] {
            missed_deadline = false;
            if (!stable()) {
                reason = "call or route changed";
                return PeriodCheck::Invalid;
            }
            const auto now = preset.snapshot();
            if (!now.enabled || (trial && now.revision != processing.revision)) {
                reason = "app state changed";
                return PeriodCheck::Invalid;
            }
            if (committing_first && !priming.ready(done(), Clock::now(), now.revision)) {
                reason = "startup completion expired or belongs to an older preset";
                return PeriodCheck::Invalid;
            }
            return check_deadline();
        };
        // A full cold ring must also be diagnosed when subsequent capture is
        // quiet or the Engine has no full block to offer the write path.
        if (!trial && !renewals && priming.no_progress(done(), Clock::now())) {
            prime_exhausted = false;
            if (stable() && preset.snapshot().enabled && uplink.stalled_at_start(done())) {
                reason = "full startup ring without WRITE_DONE";
                if (recovery) *recovery = AgmRecovery::ColdUplink;
                break;
            }
            if (valid_period() == PeriodCheck::Invalid) break;
        }
        bool capture_gap = false, full_voice = false;
        const auto state = transaction.step(
            valid_period,
            [&] {
                at("read VA PCM (AGM)");
                size_t bytes;
                int rc;
                const auto deadline = std::min(due + slack, Clock::now() + 5ms);
                if (drain_capture) {
                    // Don't replay queued capture from the missed window. Drain
                    // at most the configured input queue per period; a full
                    // drain without an empty read is not a freshness proof.
                    for (unsigned n = 0; n < kPeriods && Clock::now() < deadline; ++n) {
                        bytes = input_bytes;
                        rc = api.read(capture.handle, input.data(), &bytes);
                        if (empty_va_read(rc, bytes)) { drain_capture = false; break; }
                        if (rc || bytes != input_bytes) return Block::Failed;
                    }
                    capture_gap = true;
                    return Block::Full;
                }
                for (;;) {
                    bytes = input_bytes;
                    rc = api.read(capture.handle, input.data(), &bytes);
                    if (!empty_va_read(rc, bytes)) break;
                    if (!good_input && i < 20) return Block::Pending;
                    if (trial || Clock::now() >= deadline) break;
                    std::this_thread::sleep_until(std::min(deadline, Clock::now() + 1ms));
                    if (Clock::now() >= deadline || valid_period() != PeriodCheck::Ready || Clock::now() >= deadline) break;
                }
                if (rc || bytes != input_bytes) {
                    if (!trial && renewals && empty_va_read(rc, bytes)) {
                        capture_gap = true;
                        reanchor = true;
                        return Block::Full; // synthesize silence below, never process stale input
                    }
                    fprintf(stderr, "[avcrtd:agm-va-replace] VA read rc=%d bytes=%zu/%zu\n", rc, bytes, input_bytes);
                    return Block::Failed;
                }
                if (!trial && first_capture < 0) first_capture = monotonic_ms();
                return Block::Full;
            },
            [&] {
                at("process PCM (Engine)");
                if (capture_gap) return Block::Full;
                if (trial) input_levels.add(input.data(), input_frames);
                // Causal interpolation carries the previous sample across block boundaries.
                ++good_input;
                if (reanchor) { previous = input.front(); reanchor = false; }
                for (size_t j = 0; j < input_frames; ++j) {
                    const int next = input[j];
                    for (int phase = 1; phase <= static_cast<int>(ratio); ++phase)
                        mono[ratio * j + phase - 1] = static_cast<int16_t>(
                                ((static_cast<int>(ratio) - phase) * previous + phase * next) / static_cast<int>(ratio));
                    previous = next;
                }
                // With stock TX muted, Original voice uses the same Engine
                // uplink as the effects and soundboard; don't zero its input.
                if (!engine.process(mono.data(), mono.size(), &full_voice)) return Block::Failed;
                // Never transmit a warm-up block from the Engine (which can
                // include tails or soundboard). An active lease can send zeros.
                if (!full_voice) return !trial && renewals ? Block::Full : Block::Pending;
                if (!trial && first_full < 0) first_full = monotonic_ms();
                if (trial && !uplink.sent()) {
                    PcmLevels candidate;
                    candidate.add(mono);
                    if (candidate.rms() < 64.0) return Block::Pending;
                }
                const int fade = trial ? std::min<uint64_t>(10, replace_limit - renewals) : 10;
                for (size_t j = 0; j < mono.size(); ++j) {
                    const int v = trial ? std::clamp(static_cast<int>(mono[j]) / 2, -3000, 3000) * fade / 10
                                        : static_cast<int>(mono[j]) / 2;
                    stereo[2 * j] = stereo[2 * j + 1] = static_cast<int16_t>(v);
                }
                if (!trial && !renewals && !nonzero_pcm(stereo)) {
                    ++zero_prime_blocks;
                    return Block::Pending;
                }
                return Block::Full;
            },
            [&] {
                if (!trial && uplink.credit(done()) != UplinkWindow::Credit::Ready) {
                    // Do not burst into a full DSP ring when the host wakes
                    // late. Allow at most one period for a real completion,
                    // and never extend the freshness budget or renew to wait.
                    at("wait Incall_Music WRITE_DONE (AGM)");
                    const auto deadline = std::min(due + slack, Clock::now() + period);
                    if (!uplink.wait_writable(done, [&] { return interrupted || Clock::now() >= deadline; },
                                                [&] { std::this_thread::sleep_until(std::min(deadline, Clock::now() + 1ms)); })) {
                        // If credit returned just as the PCM expired, discard
                        // the block; don't mistake this for a stalled ring.
                        if (uplink.credit(done()) == UplinkWindow::Credit::Ready &&
                            valid_period() == PeriodCheck::SkipLate) return Block::Late;
                        reason = "no usable WRITE_DONE credit before deadline";
                        if (recovery && stable()) {
                            const auto completed = done();
                            if (!renewals && uplink.stalled_at_start(completed)) *recovery = AgmRecovery::ColdUplink;
                            else if (renewals && uplink.credit(completed) == UplinkWindow::Credit::Full)
                                *recovery = AgmRecovery::Transient;
                        }
                        return Block::Failed;
                    }
                    const auto check = valid_period();
                    if (check == PeriodCheck::SkipLate) return Block::Late;
                    if (check != PeriodCheck::Ready) {
                        reason = missed_deadline ? "PCM deadline after waiting for uplink" : "binding invalid while waiting for uplink";
                        if (recovery && renewals && missed_deadline) *recovery = AgmRecovery::Transient;
                        return Block::Failed;
                    }
                }
                if (!trial) {
                    const auto current = preset.snapshot();
                    if (!current.enabled || check_deadline() != PeriodCheck::Ready) {
                        if (current.enabled && valid_period() == PeriodCheck::SkipLate) return Block::Late;
                        reason = current.enabled ? "PCM deadline before write" : "disabled in app before write";
                        if (recovery && renewals && current.enabled && missed_deadline) *recovery = AgmRecovery::Transient;
                        return Block::Failed;
                    }
                    // A newer revision observed after processing must not let
                    // through the old block (notably a previous Original voice).
                    full_voice &= current.revision == processing.revision;
                    if (!full_voice && !renewals) return Block::Pending;
                    if (!silence.prepare(renewals != 0, full_voice, stereo)) {
                        reason = capture_gap ? "VA remained empty past silence budget" : "DSP remained unready past silence budget";
                        if (recovery && capture_gap) *recovery = AgmRecovery::Transient;
                        return Block::Failed;
                    }
                }
                at("write Incall_Music PCM (AGM)");
                size_t bytes = sizeof(stereo);
                const int rc = api.write(music.handle, stereo.data(), &bytes);
                if (!uplink.accept(rc, bytes, sizeof(stereo))) {
                    fprintf(stderr, "[avcrtd:agm-va-replace] uplink write rc=%d reported_bytes=%zu/%zu\n",
                            rc, bytes, sizeof(stereo));
                    reason = "failed or short uplink write";
                    return Block::Failed;
                }
                if (trial) sent_levels.add(stereo);
                else {
                    if (first_write < 0) first_write = monotonic_ms();
                    if (!renewals || usb_replace) priming.accepted(uplink.sent(), due + slack, processing.revision);
                }
                const auto completed = done();
                if (completed > uplink.sent()) {
                    reason = "unexpected WRITE_DONE count";
                    return Block::Failed;
                }
                if (!trial && !renewals && uplink.stalled_at_start(completed))
                    priming.ring_filled(Clock::now());
                if (completed && first_done < 0) first_done = monotonic_ms();
                if (trial) return Block::Full;
                const bool ready = renewals ? uplink.draining(completed) :
                    priming.ready(completed, Clock::now(), processing.revision);
                if (usb_replace) usb_evidence(usb_tx, completed, priming.deadline(completed, processing.revision));
                committing_first = !renewals && ready;
                return ready ? Block::Full : Block::Pending;
            });
        if (state == Step::SkippedLate) {
            timing.skipped();
            ++late_blocks;
            if (late_blocks == 1)
                fprintf(stderr, "[avcrtd:agm] skipped late PCM period; keeping active TX lease and graph\n");
            engine.discardPendingVoice();
            drain_capture = reanchor = true;
            due = Clock::now(); // Next iteration starts a fresh 10-ms cadence.
            continue;
        }
        if (state == Step::Priming) continue;
        if (state != Step::Replaced) {
            prime_exhausted = false;
            switch (state) {
            case Step::Invalidated:
                if (missed_deadline) reason = "PCM freshness deadline missed";
                if (recovery && renewals && missed_deadline) *recovery = AgmRecovery::Transient;
                break;
            case Step::CaptureFailed: reason = "failed or short VA read"; break;
            case Step::DspFailed: reason = "DSP processing failed or underrun"; break;
            case Step::UplinkFailed: break;  // The write path already describes the failure.
            case Step::LeaseFailed: reason = "TX lease refused or lost"; break;
            default: reason = "replacement closed"; break;
            }
            break;
        }
        timing.replaced(lease.began);
        if (!renewals) {
            at("log first TX lease");
            if (usb_replace) fprintf(stderr, "[avcrtd:agm-usb] fresh WRITE_DONE; stock USB TX cutoff confirmed\n");
            else if (trial) fprintf(stderr, "[avcrtd:agm-va-replace] TX lease active (mask %#x)\n", tx.mask());
            else {
                const auto since_start = [&](int64_t value) {
                    return static_cast<long long>(value - monotonic_ms(start - period));
                };
                fprintf(stderr, "[avcrtd:agm-va-replace] TX lease active (mask %#x) at mono_ms=%lld; "
                                "first_capture_ms=%lld first_full_ms=%lld first_write_ms=%lld first_done_seen_ms=%lld "
                                "since uplink start; zero_prime=%u\n", tx.mask(),
                        static_cast<long long>(monotonic_ms()), since_start(first_capture), since_start(first_full),
                        since_start(first_write), since_start(first_done), zero_prime_blocks);
            }
        }
        if (!trial && full_voice && processing.revision == measured_revision && measured < kLevelBlocks) {
            input_levels.add(input.data(), input_frames);
            engine_levels.add(mono);
            sent_levels.add(stereo);
            if (++measured == kLevelBlocks) report_levels();
        }
        if (!full_voice) ++silent_blocks;
        if (capture_gap) ++capture_gap_blocks;
        if (switching && full_voice) {
            switching = false;
            fprintf(stderr, "[avcrtd:agm] new preset PCM accepted with TX lease held\n");
        }
    }
    if (!trial && prime_exhausted && !transaction.periods() && uplink.stalled_at_start(done()) &&
        stable() && preset.snapshot().enabled) {
        reason = "full startup ring without WRITE_DONE";
        if (recovery) *recovery = cellular::AgmRecovery::ColdUplink;
    }
    const bool unmuted = transaction.finish(); // Before either graph is torn down.
    const auto renewals = static_cast<unsigned>(transaction.periods());
    if (renewals && unmuted) fprintf(stderr, "[avcrtd:agm] stock TX mute cleared at mono_ms=%lld\n",
                                   static_cast<long long>(monotonic_ms()));
    if (!trial && measured && measured < kLevelBlocks) report_levels();
    if (rollback_ok) *rollback_ok = unmuted;
    const bool complete = trial && renewals == replace_limit;
    if (trial) fprintf(stderr, "[avcrtd:agm-va-replace] %u VA periods, %llu writes, %u/%u renewed; "
                              "input RMS=%.1f uplink RMS=%.1f; %s\n",
                       good_input, static_cast<unsigned long long>(uplink.sent()), renewals, replace_limit,
                       input_levels.rms(), sent_levels.rms(),
                       complete ? "time limit" : reason);
    else fprintf(stderr, "[avcrtd:agm] %s stopped: submitted=%llu WRITE_DONE=%llu active_blocks=%u "
                         "silent=%llu va_gaps=%llu late=%llu elapsed_ms=%lld; %s\n",
                 usb_replace ? "USB replacement" : "replacement",
                 static_cast<unsigned long long>(uplink.sent()), static_cast<unsigned long long>(done()), renewals,
                  static_cast<unsigned long long>(silent_blocks), static_cast<unsigned long long>(capture_gap_blocks),
                  static_cast<unsigned long long>(late_blocks),
                 static_cast<long long>(monotonic_ms() - monotonic_ms(start - period)), reason);
    if (!trial && !renewals) {
        const auto offset = [&](int64_t mark) -> long long { return mark < 0 ? -1 : mark - monotonic_ms(start - period); };
        fprintf(stderr, "[avcrtd:agm] priming: capture_ms=%lld engine_ms=%lld write_ms=%lld done_seen_ms=%lld zero_prime=%u\n",
                offset(first_capture), offset(first_full), offset(first_write), offset(first_done), zero_prime_blocks);
    }
    if (!trial) return renewals != 0 && unmuted;
    return music.clean() && complete && unmuted;
}

#ifdef AVCRT_AGM_LAB
bool verify_tx(TxLease& lease, const Voice& voice, const char* backend, unsigned aif, bool write,
               unsigned* card = nullptr) {
    VaPcm pcm;
    if (!backend || !voice.same() || !va_pcm(backend, pcm) || pcm.available != 0 ||
        !lease.open_card(pcm.card, pcm.device, pcm.total, backend, aif, write) || !voice.same()) {
        fprintf(stderr, "[avcrtd:agm-tx] refused: need a single active call, its busy codec TX PCM, an unshared "
                        "DEC mask matching the selected AIF/codec channel map, zero mute and a 250 ms lease\n");
        return false;
    }
    fprintf(stderr, "[avcrtd:agm-tx] epoch=%llu VSID=%#x %s C%uD%u TX_AIF%u DEC mask=%#x; "
                    "ALSA checks passed (operator must verify modem route)\n",
            voice.epoch, voice.vsid, backend, pcm.card, pcm.device, aif, lease.mask());
    if (card) *card = pcm.card;
    return true;
}
#endif

// Audio policy and Sound Trigger are preflight gates, not audio transports.
bool idle_audio_service(const char* command, bool sound_trigger) {
    FILE* pipe = popen(command, "r");
    if (!pipe) return false;
    char* line = nullptr;
    size_t size = 0;
    unsigned markers = 0, inputs = 0;
    bool ok = true, models = false;
    while (getline(&line, &size, pipe) >= 0) {
        auto text = trim(line);
        if (sound_trigger) {
            if (text.substr(0, 7) == "Module " && text.size() > 7 && text[7] >= '0' && text[7] <= '9') ++markers;
            if (text == "Loaded models (handle, active, description):") {
                if (models) ok = false;
                models = true;
            } else if (text.size() > 1 && text.find_first_not_of("-=") == text.npos) {
                models = false;
            } else if (models && !text.empty()) ok = false;
        } else {
            if (text.substr(0, 12) == "Phone state:") {
                ++markers;
                if (trim(text.substr(12)) != "AUDIO_MODE_IN_CALL") ok = false;
            }
            if (text.substr(0, 8) == "Inputs (") {
                ++inputs;
                if (text != "Inputs (0):") ok = false;
            }
        }
    }
    ok = ok && !ferror(pipe);
    free(line);
    const int rc = pclose(pipe);
    return rc == 0 && ok && !models && (sound_trigger ? markers > 0 : markers == 1 && inputs == 1);
}

template <typename Report>
MicStatus dump_mic_state(const char* service, Clock::time_point deadline, pid_t& retired) {
    using namespace std::chrono_literals;
    if (Clock::now() >= deadline) return MicStatus::Unavailable;
    int pipe[2];
    if (pipe2(pipe, O_CLOEXEC)) return MicStatus::Unavailable;
    pid_t child = -1;
    posix_spawn_file_actions_t files;
    if (!posix_spawn_file_actions_init(&files)) {
        const int rc = posix_spawn_file_actions_adddup2(&files, pipe[1], STDOUT_FILENO) ||
            posix_spawn_file_actions_addopen(&files, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        if (!rc) {
            std::vector<char*> env;
            for (auto p = environ; *p; ++p) {
                if (std::string_view(*p).substr(0, 16) != "LD_LIBRARY_PATH=") env.push_back(*p);
            }
            env.push_back(nullptr);
            char path[] = "/system/bin/dumpsys", timeout[] = "-t", seconds[] = "1";
            char* args[]{path, timeout, seconds, const_cast<char*>(service), nullptr};
            if (posix_spawn(&child, path, &files, nullptr, args, env.data())) child = -1;
        }
        posix_spawn_file_actions_destroy(&files);
    }
    close(pipe[1]);
    Report report;
    bool eof = false;
    if (child > 0 && !fcntl(pipe[0], F_SETFL, O_NONBLOCK)) {
        // Bound every stage ourselves: dumpsys's -t does not cover all Binder
        // and process-exit stalls. Never join a monitor waiting in pclose().
        std::string output;
        while (Clock::now() < deadline) {
            constexpr size_t kMaxDumpBytes = 1024 * 1024;
            char buf[4096];
            const auto n = read(pipe[0], buf, sizeof(buf));
            if (!n) { eof = true; break; }
            if (n > 0) {
                if (output.size() + n > kMaxDumpBytes) break;
                output.append(buf, n);
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd wait{pipe[0], POLLIN, 0};
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                if (left <= 0ms || (poll(&wait, 1, static_cast<int>(left.count())) < 0 && errno != EINTR)) break;
            } else if (errno != EINTR) break;
        }
        if (eof) {
            std::string_view lines(output);
            while (!lines.empty()) {
                const auto n = lines.find('\n');
                report.line(trim(lines.substr(0, n)));
                if (n == lines.npos) break;
                lines.remove_prefix(n + 1);
            }
        }
    }
    close(pipe[0]);
    if (child <= 0) return MicStatus::Unavailable;
    int status;
    while (eof && Clock::now() < deadline) {
        const auto rc = waitpid(child, &status, WNOHANG);
        if (rc == child) return WIFEXITED(status) && !WEXITSTATUS(status) ? report.status() : MicStatus::Unavailable;
        if (rc < 0 && errno != EINTR) return MicStatus::Unavailable;
        std::this_thread::sleep_for(1ms);
    }
    kill(child, SIGKILL);
    // Do not block on an uninterruptible child. The probe will reap it before
    // spawning again, so persistent service stalls do not accumulate children.
    retired = child;
    return MicStatus::Unavailable;
}

class MicGate {
    MicProbe probe_;
    std::atomic<bool> stop_{false};
    std::atomic<int64_t> sample_{MicPoll::kUnavailable};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread monitor_;
    bool sample(Clock::time_point started) {
        const auto status = probe_.sample(true);
        sample_.store(status == MicStatus::Clear ? monotonic_ms(started) :
                      status == MicStatus::Blocked ? MicPoll::kBlocked : MicPoll::kUnavailable);
        return status == MicStatus::Clear;
    }
public:
    template<class Continue>
    bool start(Continue keep_waiting, unsigned tries) {
        // Measure the freshness from the beginning of the query, not the end.
        auto started = Clock::now();
        while (true) {
            if (!tries || !keep_waiting()) return false;
            --tries;
            started = Clock::now();
            if (sample(started)) break;
            // A transient failure at call setup can be resampled before any
            // graph is open. A definite mute/privacy veto is never retried.
            if (status() != MicStatus::Unavailable || !tries || !keep_waiting()) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(MicPoll::interval_ms));
        }
        monitor_ = std::thread([this, started] {
            auto next = started;
            while (!stop_.load()) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    next += std::chrono::milliseconds(MicPoll::interval_ms);
                    if (wake_.wait_until(lock, next, [&] { return stop_.load(); })) return;
                }
                next = Clock::now();
                if (!sample(next)) return;
            }
        });
        return allowed();
    }
    bool start() { return start([] { return true; }, 1); }
    MicStatus status() const { return MicPoll::status(sample_.load(), monotonic_ms()); }
    bool allowed() const { return status() == MicStatus::Clear; }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_.store(true);
        }
        wake_.notify_one();
        if (monitor_.joinable()) monitor_.join();
        probe_.reap();
    }
    ~MicGate() { stop(); }
};

struct VaTags {
    std::array<uint32_t, 1024> words{};
    size_t size = 0;
    uint32_t codec = 0, shmem = 0;
    bool query(VaStream& stream, uint32_t aif) {
        auto& api = stream.api;
        if (api.tags(stream.id, aif, nullptr, &size) || size < 4 || size > sizeof(words) || size % 4) return false;
        const auto count = size;
        if (api.tags(stream.id, aif, words.data(), &size) || size != count) return false;
        size_t pos = 1, end = size / 4;
        unsigned read = 0, source = 0;
        for (uint32_t n = words[0]; n; --n) {
            if (end - pos < 2) return false;
            const auto tag = words[pos++], modules = words[pos++];
            if (modules > (end - pos) / 2) return false;
            if (tag == kShmemEndpoint || tag == 0xc0000005) {
                if (modules != 1 || !words[pos + 1]) return false;
                if (tag == kShmemEndpoint) {
                    if (words[pos] != kReadShmemModule) return false;
                    shmem = words[pos + 1];
                    ++read;
                } else {
                    if (words[pos] != 0x07001024) return false; // Codec DMA source
                    codec = words[pos + 1];
                    ++source;
                }
            }
            pos += modules * 2;
        }
        return pos == end && read == 1 && source == 1;
    }
};

WorkerExit auto_va(const VoiceBinding& voice, int64_t revision, bool (*running)(), WorkerProgress& progress) {
    using cellular::AgmRecovery;
    const auto started = monotonic_ms();
    const auto refused = [](const char* reason) {
        fprintf(stderr, "[avcrtd:agm] stock TX: %s\n", reason);
        return WorkerExit::Refused;
    };
    ConfigStore config;
    if (config.snapshot().revision != revision) return refused("preset changed before worker launch");
    const char* call_veto = nullptr;
    const auto call = [&] {
        progress.at("check call and route");
        if (!running()) call_veto = "worker stop requested";
        else if (agm_service_died.load(std::memory_order_relaxed)) call_veto = "AGM service disconnected";
        else if (!config.snapshot().enabled) call_veto = "disabled in app";
        else if (!voice.same()) call_veto = "sole call identity changed or became unavailable";
        else return true;
        return false;
    };
    MicGate mic;
    if (!call()) return refused("call changed before microphone check");
    progress.at("check Android microphone state");
    if (!mic.start()) return call() ? WorkerExit::MicWait : WorkerExit::Stopped;
    if (!call() || !idle_audio_service("unset LD_LIBRARY_PATH; exec /system/bin/dumpsys -t 1 media.audio_policy 2>/dev/null", false) ||
        !call() || !idle_audio_service("unset LD_LIBRARY_PATH; exec /system/bin/dumpsys -t 1 soundtrigger_middleware 2>/dev/null", true) || !call())
        return refused("call or audio resources not available");

    // Resolve the vendor's handset and VA resources, not the sound card order
    // or the first powered DEC (which may be an unrelated capture stream).
    const auto va = discover_va_resource();
    const auto handset = discover_va_resource("PAL_DEVICE_IN_HANDSET_MIC");
    VaPcm va_info, tx_info;
    unsigned dma_index, stock_index;
    if (!va_dma_interface(va.backend, dma_index) ||
        !codec_dma_interface(handset.backend, "CODEC_DMA-LPAIF_RXTX-TX-", stock_index) ||
        !va_pcm(va.backend, va_info) || va_info.total != 1 || va_info.available != 1 ||
        !va_pcm(handset.backend, tx_info) || tx_info.card != va_info.card || tx_info.available)
        return refused("no unique free handset VA link and busy handset TX link");
    TxLease tx;
    std::string mux, input;
    if (!tx.open_card(tx_info.card, tx_info.device, tx_info.total, handset.backend, 0, true) ||
        !tx.microphone(mux, input)) return refused("kernel TX lease or live microphone route unavailable");
    if (!voice.tx.matches(stock_index, tx.mask())) return refused("SPF TX DMA does not match the live ALSA link");
    MicStatus mic_veto = MicStatus::Clear;
    const auto mic_clear = [&] {
        const auto status = mic.status();
        if (status == MicStatus::Clear) return true;
        mic_veto = status;
        return false;
    };
    const auto stock = [&] {
        if (!call()) return false;
        progress.at("check live TX (ALSA)");
        return tx.stable();
    };
    VaChannelMap map;
    if (!stock() || !va_channel_map(va.backend, va_info.card, map) || map.channels)
        return refused("VA already routed");
    const VaChannelMap initial_map = map;
    if (!mic_clear()) return call() ? WorkerExit::MicWait : WorkerExit::Stopped;

    VaApi api;
    progress.at("load vendor AGM IPC");
    auto* lib = load_client();
    if (!lib || !api.load_session(lib) || !api.load_read(lib) ||
        !symbol(lib, "agm_session_write", api.write) || !symbol(lib, "agm_get_aif_info_list", api.list) ||
        !symbol(lib, "agm_session_aif_set_metadata", api.aif_metadata) ||
        !result("connect IPC", api.crash_cb([](uint64_t) { agm_service_died.store(true); }, 0)))
        return refused("AGM IPC unavailable");
    std::array<AifInfo, 512> aifs{};
    size_t count = 0;
    if (api.list(nullptr, &count) || !count || count > aifs.size()) return refused("AGM AIF list unavailable");
    const auto expected = count;
    progress.at("get AGM AIF list");
    if (api.list(aifs.data(), &count) || count != expected) return refused("AGM AIF list changed");
    const auto aif_id = [&](std::string_view name) {
        uint32_t id = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& a = aifs[i];
            if (name != std::string_view(a.name, strnlen(a.name, sizeof(a.name)))) continue;
            if (id != UINT32_MAX || a.dir != kTx) return UINT32_MAX;
            id = i;
        }
        return id;
    };
    const auto aif = aif_id(va.backend), stock_aif = aif_id(handset.backend);
    if (aif == UINT32_MAX || stock_aif == UINT32_MAX || aif == stock_aif) return refused("AGM endpoints ambiguous");

    const auto stream = discover_key("StreamTX", "PCM_Record"), music_key = discover_music();
    const auto instances = discover_instances();
    struct Profile { GraphKey device, pp, instance; VaTags tags; };
    const auto bytes = [](uint32_t* data) { return reinterpret_cast<uint8_t*>(data); };
    const auto stage = [&](VaStream& query, const Profile& p) {
        progress.at("query VA graph metadata and tags");
        uint32_t sm[]{2, stream.id, stream.value, p.instance.id, p.instance.value, 0, 0x08000010, 1, 1};
        uint32_t am[]{2, p.device.id, p.device.value, p.pp.id, p.pp.value, 0, 0x08000010, 1, 3};
        query.edited = true;
        if (api.metadata(query.id, sizeof(sm), bytes(sm))) return false;
        query.edited_aif = true;
        return api.aif_metadata(query.id, query.aif, sizeof(am), bytes(am)) == 0;
    };
    if (!valid(stream) || !valid(music_key) || instances.empty()) return refused("AGM graph keys unavailable");
    // Keep the existing guard against the conflicting default read graph;
    // the codec exclusion below comes from live SPF/ALSA, not this ACDB profile.
    const auto handset_device = discover_key("DeviceTX", "Handset_Mic");
    const auto handset_pp = discover_key("DevicePP_Tx", "Audio_Fluence_SMECNS");
    Profile stock_graph{handset_device, handset_pp, instances.front(), {}};
    const auto id = private_id();
    if (!id || !valid(handset_device) || !valid(handset_pp)) return refused("handset graph unavailable");
    VaStream query(api, id, aif);
    const bool stock_tags = mic_clear() && stock() && stage(query, stock_graph) && stock_graph.tags.query(query, aif);
    if (!query.clean()) return WorkerExit::Unclean;
    if (!mic_clear()) return call() ? WorkerExit::MicWait : WorkerExit::Stopped;
    if (!stock_tags) return refused("could not identify the default handset graph");
    Profile selected{};
    std::vector<Profile> profiles;
    for (const auto& role : {std::pair{"Speaker_Mic", "Audio_Fluence_Endfire"},
                             std::pair{"VoiceActivation_Mic", "Fluence_FFECNS"},
                             std::pair{"Handset_Mic", "Audio_Fluence_SMECNS"}}) {
        const auto device = discover_key("DeviceTX", role.first), pp = discover_key("DevicePP_Tx", role.second);
        if (valid(device) && valid(pp)) profiles.push_back({device, pp, {}, {}});
    }
    for (auto instance : instances) {
        for (auto p : profiles) {
            if (!stock()) return refused("call route changed");
            if (!mic_clear()) return WorkerExit::MicWait;
            VaStream lookup(api, id, aif);
            p.instance = instance;
            const bool match = stage(lookup, p) && p.tags.query(lookup, aif) &&
                               voice.tx.separate(p.tags.codec, p.tags.shmem, stock_graph.tags.shmem);
            if (!lookup.clean()) return WorkerExit::Unclean;
            if (match) { selected = p; break; }
        }
        if (selected.tags.codec) break;
    }
    if (!selected.tags.codec) return refused("no separate VA capture graph");

    if (!call()) return refused("call changed before VA setup");
    if (!mic_clear()) return WorkerExit::MicWait;
    VaMixerRoute route;
    VaBackendFd pcm;
    const auto write_id = private_id();
    if (!write_id || write_id == id) return WorkerExit::Refused;
    VaStream capture(api, id, aif);
    Stream music{api, write_id};
    bool replacement_clean = true;
    auto recovery = AgmRecovery::None;
    bool route_touched = false;
    bool lease_lost = false;
    const char* route_veto = nullptr;
    int64_t route_veto_ms = -1;
    const auto available = [&] {
        if (!call()) {
            route_veto = call_veto;
            route_veto_ms = monotonic_ms();
            return false;
        }
        if (!mic_clear()) return false;
        progress.at("check live TX (ALSA)");
        if (tx.stable(&route_veto, &lease_lost)) return true;
        route_veto_ms = monotonic_ms();
        return false;
    };
    const auto cleanup = [&] {
        const bool pcm_closed = pcm.close();
        const bool restored = route.restore();
        VaPcm after;
        VaChannelMap after_map;
        const bool verified = !route_touched || (va_pcm(va.backend, after) && after.card == va_info.card &&
            after.device == va_info.device && after.total == 1 && after.available == 1 &&
            va_channel_map(va.backend, va_info.card, after_map) && after_map == initial_map);
        if (!pcm_closed || !restored || !verified) fprintf(stderr, "[avcrtd:agm] VA route cleanup not verified\n");
        return pcm_closed && restored && verified;
    };
    const bool ran = [&] {
        const auto fail = [&](const char* reason) { refused(reason); return false; };
        VaPcm free;
        if (!available() || !va_pcm(va.backend, free) || free.card != va_info.card ||
            free.device != va_info.device || free.total != 1 || free.available != 1)
            return fail("call, route or VA availability changed");
        route_touched = true;
        if (!route.setup(va_info.card, dma_index, mux, input, available) ||
            !va_channel_map(va.backend, va_info.card, map) || map.channels != 1)
            return fail("VA mixer route unavailable");
        const auto stable = [&] {
            VaChannelMap current;
            if (!available()) return false;
            progress.at("check VA route (ALSA)");
            if (route.unchanged() && va_channel_map(va.backend, va_info.card, current) && current == map) return true;
            route_veto = "VA mixer route or channel map changed";
            route_veto_ms = monotonic_ms();
            return false;
        };
        unsigned declared_rate = 0;
        const auto* end = va.rate.data() + va.rate.size();
        const auto parsed = std::from_chars(va.rate.data(), end, declared_rate);
        if (parsed.ec != std::errc{} || parsed.ptr != end || va.bits != "16" || va.channels != "1")
            return fail("unsupported VA media format");
        unsigned rate = 0;
        // 16 kHz is a preferred speech format, not a board-specific setting.
        // The actual codec hw_params must accept it before advertising it to AGM.
        for (unsigned candidate : {16000u, declared_rate}) {
            if (candidate && candidate % 100 == 0 && kRate % candidate == 0 && stable()) {
                progress.at("open and prepare VA PCM (ALSA)");
                if (pcm.setup(va_info, candidate)) {
                    rate = candidate;
                    break;
                }
            }
            if (candidate == declared_rate) break;
        }
        if (!rate || !stable()) return fail("VA PCM format negotiation failed");
        progress.at("register VA callback (AGM)");
        if (api.callback(id, ignore_event, kDataPathEvent, nullptr)) return fail("VA callback unavailable");
        capture.registered = true;
        const auto& p = selected;
        uint32_t md[]{4, stream.id, stream.value, p.instance.id, p.instance.value,
                      p.device.id, p.device.value, p.pp.id, p.pp.value, 0, 0x08000010, 4, 1, 3, 2, 5};
        capture.edited = true;
        if (!stable()) return fail("route changed before opening VA graph");
        progress.at("open VA graph (AGM)");
        if (!result("set VA metadata", api.metadata(id, sizeof(md), bytes(md))) ||
            !result("open VA graph", api.open(id, kNonTunnel, &capture.handle)) || !capture.handle)
            return fail("private VA open failed");
        VaTags direct;
        if (!direct.query(capture, UINT32_MAX) || direct.size != p.tags.size ||
            memcmp(direct.words.data(), p.tags.words.data(), direct.size) != 0) return fail("private VA tags changed");
        SessionConfig cfg{kTx, kNonTunnel, 0, 0, {}, kNonBlocking, 0};
        MediaConfig media{rate, 1, kS16, kFixedPoint};
        BufferConfig buffer{kPeriods, rate / 100 * sizeof(int16_t), 0};
        VaHwConfig hw{direct.codec, 0x08001017, 12, 0, rate, 16, 1, kFixedPoint, 0,
                      direct.codec, 0x08001063, 12, 0, 3, dma_index, map.slots, 0};
        if (!stable()) return fail("route changed before preparing VA graph");
        progress.at("configure and prepare VA graph (AGM)");
        if (!result("configure VA format", api.config(capture.handle, &cfg, &media, &buffer)) ||
            !result("configure VA endpoint", api.params(id, &hw, sizeof(hw))) ||
            !result("prepare VA", api.prepare(capture.handle)) || !stable()) return fail("VA graph preparation failed");
        progress.at("open and prepare Incall_Music (AGM)");
        uplink_session.store(music.id);
        if (!music.open(music_key, uplink_event) || !music.prepare(false)) return fail("Incall_Music graph preparation failed");
        if (!stable()) return fail("route changed before starting VA graph");
        progress.at("start VA graph (AGM)");
        if (!result("start VA", api.start(capture.handle))) return false;
        capture.started = true;
        fprintf(stderr, "[avcrtd:agm] VA/AGM call %s C%uD%u %u Hz codec=%#x read=%#x -> "
                        "Incall_Music write=%#x; stock=%s codec=%#x AIF=%u DEC=%#x VA input=%s/%s setup_ms=%lld (best effort)\n",
                voice.generation().c_str(), va_info.card, va_info.device, rate, direct.codec, direct.shmem, music.endpoint,
                handset.backend.c_str(), voice.tx.codec, tx.aif(), tx.mask(), mux.c_str(), input.c_str(),
                static_cast<long long>(monotonic_ms() - started));
        return replace_uplink(capture, music, stable, rate, tx, false, &replacement_clean, &progress, &recovery);
    }();
    // replace_uplink releases the lease before returning (including any failed
    // renewal). Run every remaining cleanup even if one fails.
    // A cleared lease with the same call and ALSA binding is not evidence of
    // a different microphone. Still discard the old graph and lease, even if
    // the driver just revoked mute during an unrelated codec route update.
    progress.at("check VA route before lease-loss cleanup");
    const auto cleanup_started = monotonic_ms();
    VaChannelMap before_cleanup;
    const bool lease_recheck = ran && (lease_lost || tx.lost_on_renewal()) && mic.status() == MicStatus::Clear &&
        call() && route.unchanged() && va_channel_map(va.backend, va_info.card, before_cleanup) &&
        before_cleanup == map;
    progress.at("close Incall_Music (AGM)");
    if (mic_veto != MicStatus::Clear) fprintf(stderr, "[avcrtd:agm] microphone guard: %s\n", mic_status_name(mic_veto));
    if (route_veto) fprintf(stderr, "[avcrtd:agm] route guard at mono_ms=%lld: %s\n",
                            static_cast<long long>(route_veto_ms), route_veto);
    const bool music_clean = music.clean();
    progress.at("close capture graph (AGM)");
    const bool capture_clean = capture.clean();
    // Wait out an in-flight dumpsys before validating PCM close: tinyalsa
    // opens before we can set CLOEXEC, so a just-spawned command may share it.
    progress.at("stop Android microphone monitor");
    const auto mic_before_stop = mic.status();
    mic.stop();
    const auto mic_after_stop = mic.status();
    progress.at("close VA PCM and restore mixer");
    const bool route_clean = cleanup();
    const auto cleaned = monotonic_ms();
    fprintf(stderr, "[avcrtd:agm] cleanup call %s at mono_ms=%lld; cleanup_ms=%lld\n", voice.generation().c_str(),
            static_cast<long long>(cleaned), static_cast<long long>(cleaned - cleanup_started));
    if (!replacement_clean || !music_clean || !capture_clean || !route_clean) return WorkerExit::Unclean;
    if (mic_veto != MicStatus::Clear || mic_before_stop != MicStatus::Clear ||
        mic_after_stop == MicStatus::Blocked || mic_after_stop == MicStatus::Unavailable)
        return call() ? WorkerExit::MicWait : WorkerExit::Stopped;
    if (lease_recheck && stock()) {
        fprintf(stderr, "[avcrtd:agm] same call and ALSA routes revalidated after TX lease loss\n");
        return WorkerExit::LeaseLost;
    }
    // Retry a stalled startup or a transient failure of an established path
    // only after clean rollback. Do not retry a changed route.
    if (!route_veto && recovery != AgmRecovery::None && stock()) {
        return recovery == AgmRecovery::ColdUplink ? WorkerExit::ColdUplink : WorkerExit::Recoverable;
    }
    return ran ? WorkerExit::Stopped : WorkerExit::Refused;
}

#ifdef AVCRT_AGM_LAB
int live_stdin(Api& api, GraphKey key, const Voice& voice, bool (*stock_mic)(), TxLease* lease) {
    using namespace std::chrono_literals;
    using live_pcm::Status;
    live_pcm::Input input(STDIN_FILENO);
    if (!input.valid()) {
        fprintf(stderr, "[avcrtd:agm-pcm] pipe live raw S16LE 48 kHz mono PCM to stdin (no TTY/WAV)\n");
        return 1;
    }
    const uint32_t id = private_id();
    if (!id) return 1;
    Engine engine(kRate, 1);
    engine.update(test_pitch());
    Stream music{api, id};
    const auto state_ok = [&] { return voice.same(); };
    const auto stable = [&] { return state_ok() && (lease ? lease->stable() : stock_mic()); };

    MonoPeriod in{};
    Period out{};
    constexpr unsigned limit = 800;
    constexpr auto period = 10ms;
    const auto slack = lease ? 50ms : 80ms;
    fprintf(stderr, "[avcrtd:agm-pcm] speak into the external source: +7 semitones, up to 8 s; %s\n",
            lease ? "stock mic stays on until processed PCM is flowing" : "stock mic remains on");
    auto status = input.period(in.data(), sizeof(in), Clock::now() + 2s, state_ok);
    if (status == Status::Full) {
        const auto* header = reinterpret_cast<const char*>(in.data());
        if (memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0) {
            fprintf(stderr, "[avcrtd:agm-pcm] WAV is not raw PCM; nothing sent\n");
            return 1;
        }
        if (!stable() || !result("connect IPC", api.crash_cb([](uint64_t) { _exit(1); }, 0)) ||
            !stable() || !music.open(key) || !music.prepare(false) || !stable() ||
            !result("start live uplink", api.start(music.handle))) return 1;
        music.started = true;
    }
    const auto start = Clock::now();
    unsigned sent = 0;
    bool replaced = false;
    PcmLevels input_levels, sent_levels;
    const char* reason = "time limit";
    for (; sent < limit; ++sent) {
        const auto due = start + sent * period;
        if (sent) status = input.period(in.data(), sizeof(in), due + slack, state_ok);
        if (status != Status::Full) {
            reason = status == Status::End ? "source EOF" : status == Status::Short ? "partial input period" :
                     status == Status::Timeout ? "input timeout" : status == Status::Stopped ? "call changed or stopped" :
                     "input I/O error";
            break;
        }
        std::this_thread::sleep_until(due);
        if (!state_ok() || (sent % 10 == 0 && !stable())) { reason = "call, route or mute state changed"; break; }
        input_levels.add(in);
        bool full_voice = false;
        if (!engine.process(in.data(), in.size(), &full_voice) ||
            (lease && lease->active() && !full_voice)) { reason = "DSP processing failed or underrun"; break; }
        const int fade = std::min({10u, sent, limit - 1 - sent});
        for (unsigned i = 0; i < kFrames; ++i) {
            const int s = std::clamp(static_cast<int>(in[i]) / 2, -3000, 3000) * fade / 10;
            out[2*i] = out[2*i+1] = static_cast<int16_t>(s);
        }
        if (!state_ok() || Clock::now() >= due + slack) { reason = "call state or period timing changed"; break; }
        size_t bytes = sizeof(out);
        if (!result("write live PCM", api.write(music.handle, out.data(), &bytes)) || bytes != sizeof(out)) {
            reason = "failed or short uplink write";
            break;
        }
        sent_levels.add(out);
        // Warm the real speech path, not a calibration signal. Keep a bounded
        // lease through ordinary pauses once armed, but never on failed/stale I/O.
        if (lease && full_voice && (lease->active() || sent >= 15)) {
            if (!lease->active()) {
                PcmLevels current;
                current.add(out);
                if (current.rms() < 32.0) continue;
            }
            if (!stable() || !state_ok() || Clock::now() >= due + slack || !lease->renew()) {
                reason = "lease, call, route or uplink deadline changed";
                ++sent; // this PCM write succeeded, but mute could not be maintained
                break;
            }
            if (!replaced) {
                replaced = true;
                fprintf(stderr, "[avcrtd:agm-pcm] stock TX muted (DEC mask %#x); testing processed-only audio\n", lease->mask());
            }
        }
    }
    // Never wait on AGM teardown with our mute still active.
    const bool unmuted = !lease || lease->release();
    if (lease && replaced && unmuted) fprintf(stderr, "[avcrtd:agm-pcm] stock TX mute cleared\n");
    const bool clean = music.clean();
    fprintf(stderr, "[avcrtd:agm-pcm] %u periods accepted; input peak=%d RMS=%.1f; "
                    "sent peak=%d RMS=%.1f; %s; %s\n",
            sent, input_levels.peak, input_levels.rms(), sent_levels.peak, sent_levels.rms(), reason,
            replaced ? "mute lease was active" : "stock mic was not replaced");
    // Sanity floor, not a detector for speech or remote audibility.
    constexpr int min_sent_peak = 64;
    if (sent && sent_levels.peak < min_sent_peak)
        fprintf(stderr, "[avcrtd:agm-pcm] sent signal near silence; verify the external microphone and its level\n");
    return clean && unmuted && (!lease || replaced) && sent_levels.peak >= min_sent_peak &&
           (sent == limit || status == Status::End) ? 0 : 1;
}

bool render_speech(std::vector<int16_t>& speech) {
    const size_t input = speech.size();
    if (!input || input > kClipSamples || input % kChannels != 0) return false;
    const size_t period = Period{}.size();
    speech.resize(((input + period - 1) / period + 30) * period, 0); // SoundTouch latency/tail

    Engine engine(kRate, kChannels);
    engine.update(test_pitch());
    int peak = 0;
    for (size_t i = 0; i < speech.size(); i += period) {
        if (!engine.process(speech.data() + i, kFrames)) return false;
        for (size_t j = 0; j < period; ++j)
            peak = std::max(peak, std::abs(static_cast<int>(speech[i + j])));
    }
    if (peak < 32) {
        fprintf(stderr, "[avcrtd:agm] processed clip is silent or too quiet\n");
        return false;
    }
    constexpr int max_peak = 6000;
    if (peak > max_peak)
        for (auto& s : speech) s = static_cast<int16_t>(s * max_peak / peak);
    fprintf(stderr, "[avcrtd:agm] rendered %zu ms of input at +7 semitones (PCM peak %d)\n",
             1000 * input / (kRate * kChannels), std::min(peak, max_peak));
    return true;
}

int play(Api& api, GraphKey key, const Voice& voice, bool (*stock_mic)(),
         const std::vector<int16_t>& speech) {
    // A private synthetic session ID, not a PCM/FE number from card-defs.
    // AGM's audited IPC backend keys non-tunnel sessions by uint32_t and creates them on demand.
    // Leave clear of all PAL/ALSA FE IDs and use a random ID in that private range.
    const uint32_t id = private_id();
    if (!id) return 1;
    Stream stream{api, id};
    if (!result("connect IPC", api.crash_cb([](uint64_t) { _exit(1); }, 0)) ||
        !voice.same() || !stock_mic() || !stream.open(key) || !stream.prepare(false) ||
        !voice.same() || !stock_mic() ||
        !result("start", api.start(stream.handle))) return 1;
    stream.started = true;
    fprintf(stderr, "[avcrtd:agm] Incall_Music started for observed VSID %#x; stock mic left on\n", voice.vsid);
    const unsigned periods = speech.empty() ? 200 : speech.size() / Period{}.size();
    const auto start = Clock::now();
    Period buffer{};
    size_t transferred = 0;
    for (unsigned i = 0; i < periods; ++i) {
        std::this_thread::sleep_until(start + std::chrono::milliseconds(i * 10));
        if (!voice.same() || (i % 10 == 0 && !stock_mic()) ||
            Clock::now() > start + std::chrono::milliseconds(i * 10 + 80)) break;
        if (speech.empty()) {
            const double fade = std::min({1.0, i / 4.0, (periods - 1 - i) / 4.0});
            for (unsigned j = 0; j < kFrames; ++j) {
                const double phase = (i * kFrames + j) * (2.0 * 3.141592653589793 * 660 / kRate);
                buffer[2 * j] = buffer[2 * j + 1] = std::lround(1400 * fade * std::sin(phase));
            }
        } else {
            std::copy_n(speech.data() + i * buffer.size(), buffer.size(), buffer.data());
        }
        size_t bytes = sizeof(buffer);
        const int rc = api.write(stream.handle, buffer.data(), &bytes);
        if (!result("write", rc) || bytes != sizeof(buffer)) {
            fprintf(stderr, "[avcrtd:agm] incomplete PCM period: %zu/%zu\n", bytes, sizeof(buffer));
            break;
        }
        transferred += bytes;
    }
    const bool clean = stream.clean();
    fprintf(stderr, "[avcrtd:agm] %zu/%zu PCM bytes accepted; remote audibility not verified\n",
            transferred, periods * sizeof(buffer));
    return clean && transferred == periods * sizeof(buffer) ? 0 : 1;
}

int capture_ul(Api& api, GraphKey key, GraphKey uplink, const Voice& voice, bool (*stock_mic)(),
               std::vector<int16_t>* clip = nullptr) {
    if (clip) {
        clip->clear();
        clip->reserve(kCapturePeriods * kFrames * kChannels);
    }
    const uint32_t id = private_id();
    if (!id) return 1;
    Stream capture{api, id};
    if (!result("connect IPC", api.crash_cb([](uint64_t) { _exit(1); }, 0)) ||
        !voice.same() || !stock_mic() || !capture.open(key) || !capture.prepare(true, uplink) ||
        !voice.same() || !stock_mic() || !result("start capture", api.start(capture.handle))) return 1;
    capture.started = true;
    if (clip) {
        fprintf(stderr, "[avcrtd:agm] say the test phrase now (3 s); no playback or mute during capture\n");
    } else {
        fprintf(stderr, "[avcrtd:agm] measuring Incall_Record UL levels for 3 s; no playback or mute\n");
    }

    MonoPeriod buffer{};
    const auto start = Clock::now() + std::chrono::milliseconds(30);
    size_t samples = 0, nonzero = 0;
    uint64_t energy = 0;
    int peak = 0;
    unsigned empty = 0;
    unsigned i = 0;
    for (; i < kCapturePeriods; ++i) {
        const auto due = start + std::chrono::milliseconds(i * 10);
        std::this_thread::sleep_until(due);
        if (!voice.same() || (i % 10 == 0 && !stock_mic()) ||
            Clock::now() > due + std::chrono::milliseconds(80)) break;
        size_t bytes = sizeof(buffer);
        const int rc = api.read(capture.handle, buffer.data(), &bytes);
        // GSL's nonblocking read can return AR_ENEEDMORE, mapped to ENOSPC,
        // while the completed-buffer queue is empty. Only retry empty reads,
        // at the period cadence and within this probe's fixed window.
        if (bytes == 0 && (rc == 0 || rc == -EAGAIN || rc == -ENODATA || rc == -ENOSPC)) {
            ++empty;
            if (clip) {
                fprintf(stderr, "[avcrtd:agm] incomplete capture period; test phrase will not be sent\n");
                break;
            }
            continue;
        }
        if (!result("read capture", rc) || bytes > sizeof(buffer) || bytes % sizeof(buffer[0])) break;
        if (clip && bytes != sizeof(buffer)) {
            fprintf(stderr, "[avcrtd:agm] short capture period; test phrase will not be sent\n");
            break;
        }
        if (!voice.same()) break;
        const size_t n = bytes / sizeof(buffer[0]);
        for (size_t j = 0; j < n; ++j) {
            const int sample = buffer[j];
            nonzero += sample != 0;
            energy += sample * sample;
            peak = std::max(peak, std::abs(sample));
            if (clip) {
                clip->push_back(buffer[j]);
                clip->push_back(buffer[j]);
            }
        }
        samples += n;
    }
    const bool stable = voice.same() && stock_mic();
    const bool clean = capture.clean();
    const double rms = samples ? std::sqrt(static_cast<double>(energy) / samples) : 0.0;
    fprintf(stderr, "[avcrtd:agm] UL capture: %zu samples, %zu nonzero, peak=%d RMS=%.1f, empty=%u/%u; "
                    "mic independence not established\n", samples, nonzero, peak, rms, empty, i);
    return clean && stable && i == kCapturePeriods && samples && nonzero ? 0 : 1;
}

int live_ul(Api& api, GraphKey record, GraphKey uplink, GraphKey music_key,
            const Voice& voice, bool (*stock_mic)(), Probe mode) {
    const uint32_t read_id = private_id(), write_id = private_id();
    if (!read_id || !write_id || read_id == write_id) return 1;
    overlap::Probe marker;
    const bool marker_only = mode == Probe::Overlap;
    const bool calibrate = !marker_only;
    const auto reference = calibrate ? loopback::stimulus() : std::vector<int16_t>{};
    std::vector<int16_t> measured(reference.size());
    loopback::Fit fit;
    std::atomic<bool> fit_ready{false};
    std::thread worker;
    loopback::BandLimit live_band;
    // Cancellation uses exactly the samples sent to AGM, not Engine input/output before gain.
    constexpr unsigned history_size = loopback::kTail + kFrames;
    std::vector<int16_t> sent(calibrate ? history_size : 0);
    Engine engine(kRate, 1);
    engine.update(test_pitch());
    Stream capture{api, read_id}, music{api, write_id}; // close music first on any early exit
    const auto stable = [&] { return voice.same() && stock_mic(); };
    if (!result("connect IPC", api.crash_cb([](uint64_t) { _exit(1); }, 0)) || !stable() ||
        !capture.open(record) || !capture.prepare(true, uplink) || !stable() ||
        !music.open(music_key) || !music.prepare(false) || !stable() ||
        !result("start simultaneous capture", api.start(capture.handle))) return 1;
    capture.started = true;
    if (!stable() || !result("start simultaneous playback", api.start(music.handle))) return 1;
    music.started = true;

    constexpr unsigned live_periods = 800;
    static_assert(loopback::kRate == kRate && loopback::kSamples % kFrames == 0);
    const unsigned probe_periods = marker_only ? overlap::kTotal : loopback::kSamples / kFrames;
    constexpr unsigned fit_wait_periods = 30;
    unsigned limit = probe_periods + (mode == Probe::Realtime ? fit_wait_periods : 0);
    unsigned live_start = 0;
    fprintf(stderr, "[avcrtd:agm-live] stay quiet for calibration; stock mic stays on\n");
    const auto start = Clock::now() + std::chrono::milliseconds(30);
    MonoPeriod in{};
    Period out{};
    const auto report_marker = [&] {
        const auto r = marker.result();
        fprintf(stderr, "[avcrtd:agm-live] overlap: %s (marker=%.1f contrast=%.2f lag=%u ms, "
                        "narrow peak=%.1f period RMS peak=%.1f)\n",
                r.coupled() ? "playback detected in capture" : r.clear() ? "no marker detected in quiet window" : "inconclusive",
                r.aligned, r.contrast, r.lag * 10, r.narrow_peak, r.rms_peak);
        return r.clear();
    };
    const auto report_fit = [&] {
        fprintf(stderr, "[avcrtd:agm-live] loop fit: %u frames (%.2f ms), gain=%.4f corr=%.4f tap norm=%.3f "
                        "worst residual=%.2f%% RMS=%.1f; %s\n",
                fit.delay, 1000.0 * fit.delay / kRate, fit.gain, fit.correlation, fit.tap_norm,
                100 * fit.worst_residual_ratio, fit.worst_residual_rms,
                fit.usable() ? "validated" : "not validated");
        return fit.usable();
    };
    bool clear = false;
    unsigned processed = 0, periods = 0;
    int live_peak = 0;
    for (; periods < limit; ++periods) {
        const auto due = start + std::chrono::milliseconds(periods * 10);
        std::this_thread::sleep_until(due);
        if (!voice.same() || (periods % 10 == 0 && !stable()) ||
            Clock::now() > due + std::chrono::milliseconds(25)) {
            fprintf(stderr, "[avcrtd:agm-live] call state, mute state or period timing changed; stopping\n");
            break;
        }
        size_t bytes = sizeof(in);
        if (!result("read simultaneous capture", api.read(capture.handle, in.data(), &bytes)) ||
            bytes != sizeof(in)) {
            fprintf(stderr, "[avcrtd:agm-live] incomplete capture period (%zu/%zu); stopping\n", bytes, sizeof(in));
            break;
        }
        const size_t base = static_cast<size_t>(periods) * kFrames;
        if (mode == Probe::Realtime && periods >= probe_periods && !live_start &&
            fit_ready.load(std::memory_order_acquire)) {
            clear = report_fit();
            // A synchronous read cannot predict samples from the period about to be written.
            if (!clear || fit.delay < kFrames + loopback::kEarly || !stable()) break;
            live_start = periods;
            limit = live_start + live_periods;
            fprintf(stderr, "[avcrtd:agm-live] speak now: up to 8 s at +7 semitones (original also audible)\n");
        }
        if (marker_only) {
            marker.observe(periods, in.data());
            for (unsigned i = 0; i < kFrames; ++i) out[2*i] = out[2*i+1] = marker.sample(periods, i);
        } else if (periods < probe_periods) {
            std::copy(in.begin(), in.end(), measured.begin() + base);
            for (unsigned i = 0; i < kFrames; ++i) out[2*i] = out[2*i+1] = reference[base + i];
        } else if (live_start) {
            for (unsigned i = 0; i < kFrames; ++i) {
                const long residual = in[i] - std::lround(fit.predict(base + i, [&](size_t n) {
                    return sent[n % history_size];
                }));
                in[i] = static_cast<int16_t>(std::clamp(residual, -32768L, 32767L));
            }
            if (!engine.process(in.data(), in.size())) break;
            const auto n = periods - live_start;
            const int fade = std::min({10u, n, live_periods - 1 - n});
            for (unsigned i = 0; i < kFrames; ++i) {
                const auto limited = static_cast<int16_t>(std::clamp(static_cast<int>(in[i]) / loopback::kLiveAttenuation,
                                                  -overlap::kLivePeak, overlap::kLivePeak));
                const int s = live_band.process(limited) * fade / 10;
                live_peak = std::max(live_peak, std::abs(s));
                out[2*i] = out[2*i+1] = static_cast<int16_t>(s);
            }
        } else {
            out.fill(0);
        }
        if (!voice.same() || Clock::now() > due + std::chrono::milliseconds(25)) break;
        bytes = sizeof(out);
        if (!result("write simultaneous playback", api.write(music.handle, out.data(), &bytes)) ||
            bytes != sizeof(out)) {
            fprintf(stderr, "[avcrtd:agm-live] incomplete playback period (%zu/%zu); stopping\n", bytes, sizeof(out));
            break;
        }
        if (calibrate) for (unsigned i = 0; i < kFrames; ++i) sent[(base + i) % history_size] = out[2*i];
        if (live_start) ++processed;
        if (mode == Probe::Realtime && periods + 1 == probe_periods) {
            // Keep the IPC cadence while fitting. This worker never touches either graph.
            try {
                worker = std::thread([&] {
                    fit = loopback::analyze(reference, measured);
                    fit_ready.store(true, std::memory_order_release);
                });
            } catch (const std::exception& e) {
                fprintf(stderr, "[avcrtd:agm-live] could not start calibration worker: %s\n", e.what());
                break;
            }
        }
    }
    const bool music_clean = music.clean();
    const bool capture_clean = capture.clean();
    if (worker.joinable()) worker.join();
    if (mode == Probe::Realtime && !live_start && periods == limit)
        fprintf(stderr, "[avcrtd:agm-live] calibration exceeded its timing budget; no speech sent\n");
    if (marker_only && periods == limit) clear = report_marker();
    if (mode == Probe::Calibrate && periods == limit) {
        fit = loopback::analyze(reference, measured);
        clear = report_fit();
        const auto profile = loopback::delay_profile(reference, measured);
        for (unsigned i = 0; i < profile.size(); i += 3) {
            const auto& a = profile[i];
            const auto& b = profile[i + 1];
            const auto& c = profile[i + 2];
            fprintf(stderr, "[avcrtd:agm-live] fit blocks %u–%u: %u/%.3f %u/%.3f %u/%.3f (frames/corr)\n",
                    i, i + 2, a.delay, a.correlation, b.delay, b.correlation, c.delay, c.correlation);
        }
    }
    fprintf(stderr, "[avcrtd:agm-live] %u/%u periods, %u processed, output peak=%d; no mic mute\n",
            periods, limit, processed, live_peak);
    return music_clean && capture_clean && periods == limit && clear && stable() &&
           (mode != Probe::Realtime || live_peak) ? 0 : 1;
}
#endif
} // namespace

bool MicProbe::reap() {
    if (retired_ <= 0) return true;
    const auto rc = waitpid(retired_, nullptr, WNOHANG);
    if (rc != retired_ && (rc >= 0 || errno != ECHILD)) return false;
    retired_ = -1;
    return true;
}

MicStatus MicProbe::sample(bool log_veto, bool* privacy_clear) {
    if (privacy_clear) *privacy_clear = false;
    if (!reap()) return MicStatus::Unavailable;
    const auto deadline = Clock::now() + std::chrono::milliseconds(MicPoll::query_ms);
    // AudioService's cached mute does not separately report sensor privacy.
    const auto privacy = dump_mic_state<MicPrivacyReport>("sensor_privacy", deadline, retired_);
    if (privacy != MicStatus::Clear) {
        if (log_veto) fprintf(stderr, "[avcrtd:agm-mic] sensor_privacy: %s\n", mic_status_name(privacy));
        return privacy;
    }
    if (privacy_clear) *privacy_clear = true;
    const auto audio = dump_mic_state<MicMuteReport>("audio", deadline, retired_);
    if (log_veto && audio != MicStatus::Clear) fprintf(stderr, "[avcrtd:agm-mic] audio: %s\n", mic_status_name(audio));
    return audio;
}

bool candidate() {
    Voice voice;
    const auto path = discover_voice_node();
    return !path.empty() && voice.read(path.c_str());
}

bool started_call(std::string& generation, bool* usb_capture) {
    VoiceBinding voice;
    const auto kind = voice.read_route().kind;
    if (usb_capture) *usb_capture = kind == StockRoute::Kind::UsbCapture;
    if (kind == StockRoute::Kind::Unknown) return false;
    generation = voice.generation();
    return true;
}

template<typename Run>
static int guarded_worker(bool (*running)(), Run run) {
    using namespace std::chrono_literals;
    const int fd = open("/data/adb/avcrt_agm.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return static_cast<int>(WorkerExit::Refused);
    const auto status = [&] {
        if (flock(fd, LOCK_EX | LOCK_NB)) return WorkerExit::Refused;
        if (getpgrp() != getpid()) return WorkerExit::Refused;
        // Journal an in-flight attempt under the same lock. SIGKILL closes the
        // ALSA fds and the lease expires, but mixer values can survive a crash.
        // Preserve the veto even if the normal service supervisor respawns us.
        std::array<char, 37> boot{}, last{};
        struct stat st{};
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022)) return WorkerExit::Unclean;
        const int id = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
        if (id < 0) return WorkerExit::Refused;
        const auto got = read(id, boot.data(), boot.size());
        close(id);
        if (got != static_cast<ssize_t>(boot.size()) || boot.back() != '\n') return WorkerExit::Refused;
        const auto saved = pread(fd, last.data(), last.size(), 0);
        if (saved < 0 || (saved != 0 && saved != got) || last == boot) {
            fprintf(stderr, "[avcrtd:agm] refusing after unverified cleanup (reboot required)\n");
            return WorkerExit::Unclean;
        }
        if (!running()) return WorkerExit::Refused;
        if (pwrite(fd, boot.data(), boot.size(), 0) != got) return WorkerExit::Unclean;

        // This runs independently of vendor IPC and also covers teardown and
        // orphaned workers. Mute is not renewed by the watchdog.
        WorkerProgress progress;
        std::mutex mutex;
        std::condition_variable finished;
        bool done = false;
        std::thread watchdog([&] {
            std::unique_lock<std::mutex> lock(mutex);
            while (!finished.wait_for(lock, 100ms, [&] { return done; })) {
                if (monotonic_ms() - progress.updated.load() < 2000) continue;
                char message[192];
                const int n = snprintf(message, sizeof(message), "[avcrtd:agm] watchdog: stalled at %s; cleanup unverified\n",
                                       progress.stage.load());
                if (n > 0 && n < static_cast<int>(sizeof(message))) write(STDERR_FILENO, message, n);
                kill(0, SIGKILL); // This is the private worker group, including blocked dumpsys children.
                _exit(static_cast<int>(WorkerExit::Unclean));
            }
        });
        const auto outcome = run(progress);
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        finished.notify_one();
        watchdog.join();
        if (outcome == WorkerExit::Unclean || ftruncate(fd, 0)) return WorkerExit::Unclean;
        return outcome;
    }();
    close(fd);
    return static_cast<int>(status);
}

namespace {
WorkerExit usb_uplink_once(const VoiceBinding& voice, const StockRoute& usb, int64_t revision,
                           bool (*running)(), WorkerProgress& progress);
}

int run_cellular(const char* generation, int64_t revision, bool (*running)()) {
    return guarded_worker(running, [&](WorkerProgress& progress) {
        VoiceBinding voice;
        const auto route = voice.read_route();
        if (route.kind == StockRoute::Kind::Unknown || voice.generation() != generation) return WorkerExit::Refused;
        if (route.kind == StockRoute::Kind::UsbCapture)
            return usb_uplink_once(voice, route, revision, running, progress);
        return auto_va(voice, revision, running, progress);
    });
}

namespace {
#ifdef AVCRT_USB_CAPTURE_LAB
bool usb_lab_signals() {
    struct sigaction sa{};
    sa.sa_handler = stop_signal;
    return !sigaction(SIGINT, &sa, nullptr) && !sigaction(SIGTERM, &sa, nullptr);
}

#endif

bool read_usb_start(const VoiceBinding& voice, std::optional<UsbStartEvidence>& evidence) {
    const auto path = voice.node.substr(0, voice.node.find_last_of('/') + 1) + "avcrt_voice_usb_subgraphs";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno != ENOENT) return false;
        evidence.reset();
        return true;
    }
    char buf[256];
    const auto n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0 || n >= static_cast<ssize_t>(sizeof(buf))) return false;
    evidence = UsbStartEvidence::parse(std::string_view(buf, n));
    return evidence.has_value();
}

bool usb_alsa_present(uint32_t token) {
    // QMI token: card, device, direction. Use only the read-only control node.
    const unsigned card = token >> 16, device = (token >> 8) & 0xff;
    if (card > 255 || (token & 0xff) != SNDRV_PCM_STREAM_CAPTURE) return false;
    const auto path = "/dev/snd/controlC" + std::to_string(card);
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    snd_ctl_card_info info{};
    snd_pcm_info pcm{};
    pcm.device = device;
    pcm.stream = SNDRV_PCM_STREAM_CAPTURE;
    const bool ok = !ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &info) && info.card == static_cast<int>(card) &&
                    !std::strncmp(reinterpret_cast<const char*>(info.driver), "USB-Audio", sizeof(info.driver)) &&
                    !ioctl(fd, SNDRV_CTL_IOCTL_PCM_INFO, &pcm) && pcm.subdevices_count == 1;
    close(fd);
    return ok;
}

std::optional<usb_acdb::Candidate> usb_topology(const usb_acdb::Keys& keys, const UsbStartEvidence& start) {
    std::set<std::string> paths;
    for (const char* root : {"/odm/etc/acdbdata/", "/vendor/etc/acdbdata/"}) {
        for (const char* pattern : {"*.acdb", "*/*.acdb", "*/*/*.acdb", "*/*/*/*.acdb"}) {
            glob_t list{};
            const auto rc = glob((std::string(root) + pattern).c_str(), GLOB_NOSORT, nullptr, &list);
            const bool ok = rc == GLOB_NOMATCH || (rc == 0 && list.gl_pathc <= 32);
            if (ok && !rc) for (size_t i = 0; i < list.gl_pathc; ++i) paths.insert(list.gl_pathv[i]);
            globfree(&list);
            if (!ok || paths.size() > 32) return {};
        }
    }
    std::optional<usb_acdb::Candidate> answer;
    for (const auto& path : paths) {
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) return {};
        struct stat st{};
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024) {
            close(fd);
            return {};
        }
        std::string bytes;
        try { bytes.resize(static_cast<size_t>(st.st_size)); }
        catch (...) { close(fd); return {}; }
        size_t pos = 0;
        while (pos < bytes.size()) {
            const auto n = read(fd, bytes.data() + pos, bytes.size() - pos);
            if (n <= 0) break;
            pos += static_cast<size_t>(n);
        }
        close(fd);
        if (pos != bytes.size()) return {};
        const auto x = usb_acdb::resolve(bytes, keys, start);
        if (!x || (answer && !(*x == *answer))) return {};
        answer = x;
    }
    return answer;
}

class UsbCutLease {
    const StockRoute& route_;
    int fd_ = -1;
    uint64_t owner_ = 0, done_ = 0, confirmed_done_ = 0;
    bool attempted_ = false, active_ = false;
    int error_ = 0;
    Clock::time_point last_{}, fresh_until_{};
    std::string arm_;
    struct Status {
        unsigned state, vsid, miid, reason;
        unsigned long long owner, epoch, generation;
        int result;
    };
    std::optional<Status> status() const {
        char buf[192], extra;
        const auto n = pread(fd_, buf, sizeof(buf) - 1, 0);
        if (n <= 0 || n >= static_cast<ssize_t>(sizeof(buf) - 1)) return {};
        buf[n] = '\0';
        Status s{};
        unsigned version;
        if (sscanf(buf, "%u %u %llu %llu %u %llu %u %u %d %c", &version, &s.state, &s.owner,
                   &s.epoch, &s.vsid, &s.generation, &s.miid, &s.reason, &s.result, &extra) != 9 ||
            version != 1 || s.state > 3) return {};
        return s;
    }
    bool cmd(const std::string& line) {
        const auto n = pwrite(fd_, line.data(), line.size(), 0);
        error_ = n < 0 ? errno : n == static_cast<ssize_t>(line.size()) ? 0 : EIO;
        if (error_) fprintf(stderr, "[avcrtd:agm-usb] cutoff operation not confirmed: errno=%d\n", error_);
        return !error_;
    }
    std::string command(unsigned op) const { return "1 " + std::to_string(op) + ' ' + std::to_string(owner_); }
public:
    explicit UsbCutLease(const StockRoute& route) : route_(route) {}
    ~UsbCutLease() { if (fd_ >= 0) close(fd_); }
    unsigned mask() const { return 0; } // no LPASS mask on USB
    bool open_node(const std::string& node) {
        const int random = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (random < 0) return false;
        const auto n = read(random, &owner_, sizeof(owner_));
        close(random);
        if (n != sizeof(owner_) || !owner_) return false;
        fd_ = open(node.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        return fd_ >= 0;
    }
    bool available() const {
        const auto s = status();
        return s && (s->state == 0 || (s->state == 2 && !s->result));
    }
    cellular::AgmRecovery recovery() const { return recovery_; }
    void target(const usb_acdb::Candidate& c) {
        arm_ = command(1) + ' ' + std::to_string(route_.tx.epoch) + ' ' + std::to_string(route_.tx.vsid) +
               ' ' + std::to_string(route_.tx.leg);
        for (const auto v : {route_.usb_miid, route_.usb_token, route_.usb_interval, c.source_sg, c.cutoff_sg, c.cutoff})
            arm_ += ' ' + std::to_string(v);
        target_ = c.cutoff;
    }
    static void evidence(void* ptr, uint64_t done, Clock::time_point fresh_until) {
        auto& self = *static_cast<UsbCutLease*>(ptr);
        self.done_ = done;
        self.fresh_until_ = fresh_until;
    }
    bool renew() {
        using namespace std::chrono_literals;
        const auto now = Clock::now();
        if (active_ && now - last_ < 30ms) return true;
        if (now >= fresh_until_ || done_ <= confirmed_done_) return active_ && now - last_ < 100ms;
        if (!usb_alsa_present(route_.usb_token)) return false;
        // Android steady_clock and kernel ktime_get share CLOCK_MONOTONIC.
        const auto deadline = std::chrono::duration_cast<std::chrono::nanoseconds>(fresh_until_.time_since_epoch()).count();
        const bool first = !attempted_;
        if (!first && !active_) return false;
        attempted_ = true;
        if (!cmd((first ? arm_ : command(2)) + ' ' + std::to_string(deadline) + '\n')) {
            active_ = false;
            return false;
        }
        active_ = true;
        confirmed_done_ = done_;
        last_ = now;
        return true;
    }
    bool release() {
        if (!attempted_) return true;
        const int prior_error = error_;
        const bool released = cmd(command(3) + '\n');
        const int release_error = error_;
        const auto s = status();
        active_ = false;
        if (!s || s->state == 1 || s->state == 3) return false;
        if (s->owner != owner_) {
            // An arm rejected before taking ownership did not write a SET.
            const bool untouched = !confirmed_done_ && !released && release_error == ESTALE &&
                (prior_error == ESTALE || prior_error == EBUSY || prior_error == EACCES || prior_error == EPERM ||
                 prior_error == ETIME);
            if (untouched) recovery_ = cellular::usb_cut_recovery(false, prior_error, 0);
            return untouched;
        }
        if (s->miid != target_ || s->epoch != route_.tx.epoch || s->vsid != route_.tx.vsid ||
            s->generation != route_.tx.leg) return false;
        if ((s->state == 2 && s->result == 0) || (!confirmed_done_ && s->state == 0 && s->result != 0))
            recovery_ = cellular::usb_cut_recovery(confirmed_done_ != 0, prior_error, s->reason);
        return (s->state == 0 && s->result != 0) || (s->state == 2 && s->result == 0);
    }
private:
    uint32_t target_ = 0;
    cellular::AgmRecovery recovery_ = cellular::AgmRecovery::None;
};

template<class Stable>
bool usb_live_paths(Stream& music, int (*get)(uint32_t, void*, size_t), uint32_t source,
                    const usb_acdb::Candidate& x, const Stable& stable) {
    if (music.endpoint != x.music) return false;
    const auto path = [&](uint32_t from, uint32_t to) {
        if (!stable()) return false;
        const auto query = PathQuery::between(from, to);
        auto reply = query;
        const int rc = get(music.id, &reply, sizeof(reply));
        return stable() && query.accepts(rc, reply);
    };
    SoftVolumeQuery q{x.stream_mute, SoftVolumeQuery::kMasterMute};
    auto reply = q;
    if (!stable()) return false;
    const int rc = get(music.id, &reply, sizeof(reply));
    return stable() && q.accepts(rc, reply) && reply.value == 0 &&
        path(source, x.cutoff) && path(x.cutoff, x.stream_mute) && path(x.stream_mute, x.encoder) &&
        path(x.music, x.encoder);
}

class UsbCaptureCleanup {
    int fd_ = -1;
    uint32_t endpoint_ = 0;
public:
    ~UsbCaptureCleanup() { if (fd_ >= 0) close(fd_); }
    bool open_node(const std::string& node) {
        fd_ = open(node.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        return fd_ >= 0;
    }
    bool watch(uint32_t miid) {
        const auto line = "1 " + std::to_string(miid) + '\n';
        if (write(fd_, line.data(), line.size()) != static_cast<ssize_t>(line.size())) return false;
        endpoint_ = miid;
        return true;
    }
    bool stopped_and_closed() const {
        char text[80], extra;
        const auto n = pread(fd_, text, sizeof(text) - 1, 0);
        if (n <= 0 || n >= static_cast<ssize_t>(sizeof(text) - 1)) return false;
        text[n] = '\0';
        unsigned version, miid, state;
        return sscanf(text, "%u %u %u %c", &version, &miid, &state, &extra) == 3 &&
            version == 1 && miid == endpoint_ && state == 2;
    }
};

bool usb_capture_tags(Stream& stream, uint32_t source) {
    constexpr uint32_t source_tag = 0xc0000005, usb_module = 0x07001050;
    std::array<uint32_t, 1024> tags{};
    size_t size = 0;
    int rc = stream.api.tags(stream.id, UINT32_MAX, nullptr, &size);
    if (rc || size < 4 || size % 4 || size > sizeof(tags)) {
        fprintf(stderr, "[avcrt-usb-lab] tag size query: rc=%d bytes=%zu\n", rc, size);
        return false;
    }
    const auto expected = size;
    rc = stream.api.tags(stream.id, UINT32_MAX, tags.data(), &size);
    if (rc || size != expected) {
        fprintf(stderr, "[avcrt-usb-lab] tag reply: rc=%d bytes=%zu expected=%zu\n", rc, size, expected);
        return false;
    }
    size_t p = 1, words = size / 4;
    unsigned sources = 0, reads = 0;
    uint32_t endpoint = 0;
    for (uint32_t n = tags[0]; n; --n) {
        if (words - p < 2) return false;
        const uint32_t tag = tags[p++], modules = tags[p++];
        if (modules > (words - p) / 2) return false;
        if (tag == source_tag || tag == kShmemEndpoint) {
            const auto module = modules == 1 ? tags[p] : 0;
            const auto miid = modules == 1 ? tags[p + 1] : 0;
            fprintf(stderr, "[avcrt-usb-lab] tag=%#x count=%u module=%#x miid=%#x\n", tag, modules, module, miid);
            if (tag == source_tag) {
                if (++sources != 1 || module != usb_module || miid != source) return false;
            } else if (++reads != 1 || module != kReadShmemModule || !miid || miid == source) {
                return false;
            } else {
                endpoint = miid;
            }
        }
        p += modules * 2;
    }
    if (p != words || sources != 1 || reads != 1) {
        fprintf(stderr, "[avcrt-usb-lab] incomplete tag reply: words=%zu/%zu sources=%u reads=%u\n",
                p, words, sources, reads);
        return false;
    }
    stream.endpoint = endpoint;
    return true;
}

WorkerExit usb_capture_once(const VoiceBinding& voice, const StockRoute& usb, WorkerProgress& progress,
                            bool (*running)(), std::optional<int64_t> revision) {
    using namespace std::chrono_literals;
    const bool live = revision.has_value();
    UsbCutLease tx(usb);
    UsbCaptureCleanup capture_cleanup;
    std::optional<UsbStartEvidence> start;
    if (live) {
        const auto root = voice.node.substr(0, voice.node.find_last_of('/') + 1);
        if (!tx.open_node(root + "avcrt_voice_usb_cut_lease") || !tx.available() ||
            !capture_cleanup.open_node(root + "avcrt_usb_capture_cleanup") ||
            !read_usb_start(voice, start) || !start || !(start->route == usb)) {
            fprintf(stderr, "[avcrtd:agm-usb] no eligible USB cutoff lease or ACKed START; not mixing with stock\n");
            return WorkerExit::Refused;
        }
    }
    const auto same_route = [&] {
        return running() && !interrupted && !agm_service_died.load() && read_stock_route(voice.node) == usb;
    };
    MicGate mic;
    ConfigStore config;
    const auto allowed = [&] { return same_route() && config.snapshot().enabled; };
    const auto stable = [&] { return allowed() && mic.allowed(); };
    const auto preflight = [&] { return allowed() && usb_alsa_present(usb.usb_token); };
    progress.at("USB preflight");
    if ((live && *revision != config.snapshot().revision) || !preflight()) return WorkerExit::Refused;
    if (!mic.start(preflight, live ? 3 : 1))
        return live && allowed() ? WorkerExit::MicWait : WorkerExit::Refused;
    progress.at("USB lab audio policy");
    if (!stable() || !idle_audio_service("unset LD_LIBRARY_PATH; exec /system/bin/dumpsys -t 1 media.audio_policy 2>/dev/null", false)) {
        fprintf(stderr, "[avcrt-usb-lab] refused: SIM audio policy or input resources unavailable\n");
        return WorkerExit::Refused;
    }
    progress.at("USB lab sound trigger");
    if (!stable() || !idle_audio_service("unset LD_LIBRARY_PATH; exec /system/bin/dumpsys -t 1 soundtrigger_middleware 2>/dev/null", true)) {
        fprintf(stderr, "[avcrt-usb-lab] refused: sound trigger state or microphone guard unavailable\n");
        return WorkerExit::Refused;
    }
    progress.at("USB lab metadata discovery");
    const GraphKey keys[] = {discover_key("DeviceTX", "USB_Tx"), discover_key("Instance", "Instance_3"),
                             discover_key("DevicePP_Tx", "Audio_Fluence_SMECNS"), discover_key("StreamTX", "PCM_Record")};
    // SG types: stream, device and device PP, as declared by this ACDB profile.
    uint32_t metadata[] = {4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x08000010, 3, 1, 2, 5};
    for (size_t i = 0; i < std::size(keys); ++i) {
        if (!valid(keys[i])) return WorkerExit::Refused;
        metadata[1 + 2 * i] = keys[i].id;
        metadata[2 + 2 * i] = keys[i].value;
    }
    Api api;
    const auto id = private_id();
    const auto music_id = live ? private_id() : 0;
    const auto music_key = live ? discover_music() : GraphKey{};
    std::optional<usb_acdb::Candidate> cutoff;
    if (live) {
        progress.at("resolve USB TX topology");
        const auto v = discover_key("StreamTX", "Voice_Call_Tx");
        if (!valid(v) || !valid(music_key)) return WorkerExit::Refused;
        usb_acdb::Keys topology{{keys[0].id, keys[0].value}, {v.id, v.value}, {}, {{music_key.id, music_key.value}}};
        for (const auto k : keys) topology.capture.emplace(k.id, k.value);
        cutoff = usb_topology(topology, *start);
        if (!cutoff || !stable()) {
            fprintf(stderr, "[avcrtd:agm-usb] installed ACDB variants did not confirm one cutoff for this START\n");
            return WorkerExit::Refused;
        }
        tx.target(*cutoff);
    }
    if (!stable()) return WorkerExit::Refused;
    progress.at("USB lab IPC connect");
    auto* lib = load_client();
    int (*get_params)(uint32_t, void*, size_t) = nullptr;
    if (!id || !api.load_session(lib) || !symbol(lib, "agm_session_read", api.read) ||
        (live && (!music_id || music_id == id || !symbol(lib, "agm_session_write", api.write) ||
                  !symbol(lib, "agm_session_get_params", get_params))) ||
        !result("connect IPC", api.crash_cb([](uint64_t) { agm_service_died.store(true); }, 0)))
        return WorkerExit::Refused;
    Stream capture{api, id};
    Stream music{api, music_id};
    auto recovery = cellular::AgmRecovery::None;
    bool rollback_ok = true, capture_start_attempted = false;
    const auto step = [&](const char* stage, const auto& fn) {
        progress.at(stage);
        if (stable() && fn() && stable()) return true;
        fprintf(stderr, "[avcrt-usb-lab] stopped at %s route=%s mic=%s mono_ms=%lld\n", stage,
                same_route() ? "same" : "withdrawn", mic_status_name(mic.status()),
                static_cast<long long>(monotonic_ms()));
        return false;
    };
    const auto outcome = [&] {
        fprintf(stderr, "[avcrt-usb-lab] bind=%s source=%#x token=%#x private=%#x mono_ms=%lld\n",
                voice.generation().c_str(), usb.usb_miid, usb.usb_token, id, static_cast<long long>(monotonic_ms()));
        if (!step("USB lab register", [&] { return capture.register_metadata(metadata, sizeof(metadata), ignore_event); }) ||
            !step("USB lab open", [&] {
                return usb_alsa_present(usb.usb_token) && same_route() &&
                       result("open capture", api.open(id, kNonTunnel, &capture.handle)) && capture.handle;
             }) || !step("USB capture tags", [&] {
                 return usb_capture_tags(capture, usb.usb_miid) && (!live || capture.endpoint == cutoff->capture);
             }))
            return WorkerExit::Refused;
        SessionConfig cfg{kTx, kNonTunnel, 0, 0, {}, kNonBlocking, 0};
        MediaConfig media{kRate, 1, kS16, kFixedPoint};
        BufferConfig buffer{kPeriods, sizeof(MonoPeriod), 0};
        if (!step("USB capture config", [&] { return result("configure private PCM", api.config(capture.handle, &cfg, &media, &buffer)); }) ||
            !step("USB capture prepare", [&] { return result("prepare private capture", api.prepare(capture.handle)); }))
            return WorkerExit::Refused;
        if (live && !step("USB prepare Incall_Music", [&] {
                uplink_session.store(music.id);
                return music.open(music_key, uplink_event) && music.prepare(false, {}, cutoff->music) && music.endpoint != capture.endpoint;
            })) return WorkerExit::Refused;
        if (!step("USB capture start", [&] {
                if (live && !capture_cleanup.watch(capture.endpoint)) return false;
                capture_start_attempted = true;
                capture.started = result("start private capture", api.start(capture.handle));
                return capture.started;
            })) return WorkerExit::Refused;
        if (live) {
            if (!step("verify live USB TX paths", [&] { return usb_live_paths(music, get_params, usb.usb_miid, *cutoff, stable); }))
                return WorkerExit::Refused;
            std::optional<UsbStartEvidence> now;
            if (!usb_alsa_present(usb.usb_token) || !read_usb_start(voice, now) || !(now == start) || !stable())
                return WorkerExit::Refused;
            fprintf(stderr, "[avcrtd:agm-usb] %s: private capture -> Engine -> Incall_Music; priming replacement\n",
                    voice.generation().c_str());
            const auto ok = replace_uplink(capture, music, stable, kRate, tx, false, &rollback_ok, &progress,
                                            &recovery, UplinkMode::UsbReplace, UsbCutLease::evidence, &tx);
            return ok ? WorkerExit::Stopped : WorkerExit::Refused;
        }
#ifdef AVCRT_USB_CAPTURE_LAB
        fprintf(stderr, "[avcrt-usb-lab] started read=%#x mono_ms=%lld (8 seconds max)\n",
                capture.endpoint, static_cast<long long>(monotonic_ms()));
        unsigned full = 0, nonzero = 0, partial = 0, empty = 0;
        uint64_t power = 0;
        int peak = 0;
        bool failed = false;
        auto due = Clock::now();
        const auto deadline = due + 8s;
        while (Clock::now() < deadline) {
            progress.at("USB lab read");
            if (!stable()) break;
            MonoPeriod pcm{};
            size_t bytes = sizeof(pcm);
            const int rc = api.read(capture.handle, pcm.data(), &bytes);
            const bool still_allowed = stable() && Clock::now() < deadline;
            if (still_allowed && !rc && bytes == sizeof(pcm)) {
                ++full;
                bool signal = false;
                for (int s : pcm) { power += s * s; peak = std::max(peak, std::abs(s)); signal |= s != 0; }
                nonzero += signal;
            } else if (still_allowed && !bytes && (!rc || rc == -EAGAIN || rc == -ENODATA || rc == -ENOSPC)) {
                ++empty;
            } else if (still_allowed && !rc && bytes < sizeof(pcm) && bytes % sizeof(int16_t) == 0) {
                ++partial;
            } else if (still_allowed) {
                fprintf(stderr, "[avcrt-usb-lab] read error: rc=%d bytes=%zu\n", rc, bytes);
                failed = true;
            }
            volatile int16_t* wipe = pcm.data();
            for (size_t i = 0; i < pcm.size(); ++i) wipe[i] = 0;
            if (!still_allowed || failed) break;
            due = std::max(due + 10ms, Clock::now() + 1ms);
            std::this_thread::sleep_until(due);
        }
        fprintf(stderr, "[avcrt-usb-lab] read stopped mono_ms=%lld route=%s mic=%s full=%u nonzero=%u partial=%u empty=%u rms=%.1f peak=%d\n",
                static_cast<long long>(monotonic_ms()), same_route() ? "same" : "withdrawn", mic_status_name(mic.status()),
                full, nonzero, partial, empty, full ? std::sqrt(double(power) / (full * kFrames)) : 0.0, peak);
        return !failed && nonzero ? WorkerExit::Stopped : WorkerExit::Refused;
#else
        return WorkerExit::Refused;
#endif
    }();
    const auto mic_status = mic.status();
    progress.at("USB stop/close Incall_Music");
    const bool music_clean = music.clean();
    progress.at("USB stop/close capture");
    const bool capture_clean = capture.clean();
    const bool dsp_clean = !live || !capture_start_attempted || capture_cleanup.stopped_and_closed();
    const bool clean = capture_clean && music_clean && dsp_clean;
    fprintf(stderr, "[avcrtd:agm-usb] cleanup=%s (AGM=%d DSP=%d cutoff=%d) mono_ms=%lld\n",
            clean && rollback_ok ? "ok" : "unverified", capture_clean && music_clean, dsp_clean, rollback_ok,
            static_cast<long long>(monotonic_ms()));
    if (!clean || !rollback_ok || agm_service_died.load()) return WorkerExit::Unclean;
    if (live && allowed()) {
        if (mic_status != MicStatus::Clear || !mic.allowed()) return WorkerExit::MicWait;
        if (tx.recovery() == cellular::AgmRecovery::LeaseLost) return WorkerExit::LeaseLost;
        if (recovery == cellular::AgmRecovery::ColdUplink) return WorkerExit::ColdUplink;
        if (recovery == cellular::AgmRecovery::Transient || tx.recovery() == cellular::AgmRecovery::Transient)
            return WorkerExit::Recoverable;
    }
    return outcome;
}

WorkerExit usb_uplink_once(const VoiceBinding& voice, const StockRoute& usb, int64_t revision,
                           bool (*running)(), WorkerProgress& progress) {
    return usb_capture_once(voice, usb, progress, running, revision);
}
} // namespace

#ifdef AVCRT_USB_CAPTURE_LAB
namespace {
WorkerExit usb_cutoff_once(const std::string& node, const StockRoute& usb, const UsbPathCandidate& candidate) {
    using namespace std::chrono_literals;
    const int fd = open(node.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return WorkerExit::Refused;
    const auto read_status = [&](uint64_t& state, uint64_t& reason, int& result) {
        char buf[192];
        const auto n = pread(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0 || n >= static_cast<ssize_t>(sizeof(buf) - 1)) return false;
        buf[n] = '\0';
        unsigned version, parsed_state, vsid, miid, parsed_reason;
        unsigned long long epoch, generation;
        char extra;
        if (std::sscanf(buf, "%u %u %llu %u %llu %u %u %d %c", &version, &parsed_state, &epoch,
                        &vsid, &generation, &miid, &parsed_reason, &result, &extra) != 8 || version != 1)
            return false;
        state = parsed_state;
        reason = parsed_reason;
        return epoch == usb.tx.epoch && vsid == usb.tx.vsid && generation == usb.tx.leg && miid == candidate.cutoff;
    };
    auto line = std::to_string(1) + ' ' + std::to_string(usb.tx.epoch) + ' ' + std::to_string(usb.tx.vsid) +
                ' ' + std::to_string(usb.tx.leg);
    for (const auto value : {usb.usb_miid, usb.usb_token, usb.usb_interval,
                             candidate.source_sg, candidate.cutoff_sg, candidate.cutoff})
        line += ' ' + std::to_string(value);
    line += '\n';
    const auto n = write(fd, line.data(), line.size());
    const int error = n < 0 ? errno : 0;
    fprintf(stderr, "[avcrt-usb-cutoff] apply=%s errno=%d mono_ms=%lld\n",
            n == static_cast<ssize_t>(line.size()) ? "SET+GET confirmed" : "not confirmed", error,
            static_cast<long long>(monotonic_ms()));
    const auto until = Clock::now() + 700ms;
    auto outcome = WorkerExit::Unclean;
    do {
        uint64_t state = 0, reason = 0;
        int result = 0;
        if (!read_status(state, reason, result)) {
            // The request did not take ownership; do not infer restoration.
            if (error == ESTALE || error == EBUSY || error == EACCES || error == EPERM)
                outcome = WorkerExit::Refused;
            break;
        }
        if (state != 1) {
            fprintf(stderr, "[avcrt-usb-cutoff] state=%llu reason=%llu result=%d mono_ms=%lld\n",
                    static_cast<unsigned long long>(state), static_cast<unsigned long long>(reason), result,
                    static_cast<long long>(monotonic_ms()));
            if (state == 0) outcome = WorkerExit::Refused;
            if (state == 2 && !result)
                outcome = n == static_cast<ssize_t>(line.size()) ? WorkerExit::Stopped : WorkerExit::Refused;
            break;
        }
        std::this_thread::sleep_for(10ms);
    } while (Clock::now() < until);
    close(fd);
    return outcome;
}

WorkerExit usb_readback_once(const VoiceBinding& voice, const StockRoute& usb,
                             const UsbPathCandidate& candidate, WorkerProgress& progress, UsbReadbackMode mode) {
    const bool compare_mute = mode == UsbReadbackMode::AndroidMute;
    const bool cutoff = mode == UsbReadbackMode::Cutoff;
    const auto cut_node = voice.node.substr(0, voice.node.find_last_of('/') + 1) + "avcrt_voice_usb_cut_test";
    std::optional<UsbStartEvidence> initial;
    if (!read_usb_start(voice, initial) ||
        (cutoff && (!initial || access(cut_node.c_str(), R_OK | W_OK))) ||
        (initial && (!(initial->route == usb) || !initial->contains(candidate.source_sg) ||
                     !initial->contains(candidate.cutoff_sg)))) {
        fprintf(stderr, "[avcrt-usb-readback] refused: mismatched or invalid live START evidence\n");
        return WorkerExit::Refused;
    }
    fprintf(stderr, "[avcrt-usb-readback] bind=%s source=%#x START=%s; candidates are not write authority\n",
            voice.generation().c_str(), usb.usb_miid, initial ? "matched" : "recorded hypothesis only (no live ABI)");
    MicGate mic;
    ConfigStore config;
    const auto same = [&] {
        std::optional<UsbStartEvidence> now;
        return !interrupted && !agm_service_died.load() && read_stock_route(voice.node) == usb &&
               read_usb_start(voice, now) && initial == now && read_stock_route(voice.node) == usb;
    };
    const auto allowed = [&] { return same() && config.snapshot().enabled; };
    const auto stable = [&] { return allowed() && (compare_mute || mic.allowed()); };
    progress.at("USB readback preflight");
    const auto preflight = [&] { return allowed() && usb_alsa_present(usb.usb_token); };
    if (!(compare_mute ? preflight() : mic.start(preflight, 3))) {
        fprintf(stderr, "[avcrt-usb-readback] preflight refused; no private graph opened\n");
        return WorkerExit::Refused;
    }
    const auto key = discover_music();
    if (!stable() || !valid(key)) return WorkerExit::Refused;

    progress.at("USB readback IPC connect");
    Api api;
    int (*get_params)(uint32_t, void*, size_t) = nullptr;
    auto* lib = load_client();
    const auto id = private_id();
    if (!id || !api.load_session(lib) || !symbol(lib, "agm_session_get_params", get_params) ||
        !result("connect IPC", api.crash_cb([](uint64_t) { agm_service_died.store(true); }, 0)))
        return WorkerExit::Refused;

    Stream music{api, id};
    const bool ok = [&] {
        MicProbe mute_probe;
        const auto sample_mute = [&]() -> std::optional<MicStatus> {
            if (!allowed()) return {};
            progress.at("USB mute comparison Android state");
            bool privacy_clear = false;
            const auto status = mute_probe.sample(false, &privacy_clear);
            if (!privacy_clear || !allowed()) return {};
            return status;
        };
        if (compare_mute) {
            // This mode never configures, prepares or starts audio. Permit
            // GET during an observed Android mute only, not sensor privacy.
            bool clear = false;
            for (unsigned i = 0; i < 3; ++i) {
                const auto status = sample_mute();
                if (!status || *status == MicStatus::Blocked) break;
                if (*status == MicStatus::Clear) { clear = true; break; }
                if (i != 2) std::this_thread::sleep_for(std::chrono::milliseconds(MicPoll::interval_ms));
            }
            if (!clear) return false;
        }
        progress.at("USB readback open Incall_Music");
        if (!stable() || !music.open(key) || !stable()) return false;
        const auto volume = [&](uint32_t miid, uint32_t param) -> std::optional<uint32_t> {
            progress.at("USB readback GET soft volume");
            if (!stable()) return {};
            const SoftVolumeQuery request{miid, param};
            auto reply = request;
            const int rc = get_params(music.id, &reply, sizeof(reply));
            if (!stable() || !request.accepts(rc, reply)) {
                fprintf(stderr, "[avcrt-usb-readback] invalid GET miid=%#x param=%#x rc=%d\n", miid, param, rc);
                return {};
            }
            if (!compare_mute)
                fprintf(stderr, "[avcrt-usb-readback] GET miid=%#x param=%#x value=%#x\n", miid, param, reply.value);
            return reply.value;
        };
        const auto read_volumes = [&] {
            return std::array{volume(candidate.cutoff, SoftVolumeQuery::kMasterMute),
                              volume(candidate.cutoff, SoftVolumeQuery::kMasterGain),
                              volume(candidate.stream_mute, SoftVolumeQuery::kMasterMute),
                              volume(candidate.stream_mute, SoftVolumeQuery::kMasterGain)};
        };
        if (compare_mute) {
            using namespace std::chrono_literals;
            using Values = std::array<std::optional<uint32_t>, 4>;
            const auto phase = [&](const char* label, MicStatus expected, auto window) -> std::optional<Values> {
                const auto until = Clock::now() + window;
                std::optional<Values> previous;
                while (Clock::now() < until) {
                    const auto before = sample_mute();
                    if (!before) break;
                    if (*before == expected) {
                        const auto values = read_volumes();
                        if (!std::all_of(values.begin(), values.end(), [](const auto& value) { return value.has_value(); }))
                            break;
                        const auto after = sample_mute();
                        if (!after) break;
                        if (*after == expected && previous == values) {
                            fprintf(stderr, "[avcrt-usb-mute] %s mono_ms=%lld cutoff mute=%u gain=%#x; stream mute=%u gain=%#x\n",
                                    label, static_cast<long long>(monotonic_ms()), *values[0], *values[1], *values[2], *values[3]);
                            return values;
                        }
                        previous = *after == expected ? std::optional<Values>{values} : std::nullopt;
                    } else previous.reset();
                    std::this_thread::sleep_for(200ms);
                }
                fprintf(stderr, "[avcrt-usb-mute] %s: no stable sample\n", label);
                return {};
            };
            const auto clear = phase("unmuted", MicStatus::Clear, 2s);
            const auto muted = clear ? phase("muted", MicStatus::Blocked, 22s) : std::nullopt;
            const auto restored = muted ? phase("unmuted again", MicStatus::Clear, 18s) : std::nullopt;
            if (!restored) return false;
            const bool cutoff_same = (*clear)[0] == (*muted)[0] && (*clear)[1] == (*muted)[1] &&
                                     (*clear)[0] == (*restored)[0] && (*clear)[1] == (*restored)[1];
            const bool stream_follows = (*clear)[2] == 0 && (*muted)[2] == 1 && (*restored)[2] == 0;
            fprintf(stderr, "[avcrt-usb-mute] cutoff=%s stream_mute=%s; no mute written by probe\n",
                    cutoff_same ? "unchanged" : "changed", stream_follows ? "0->1->0" : "0->1->0 not observed");
            return true;
        }
        const auto values = read_volumes();
        bool all = std::all_of(values.begin(), values.end(), [](const auto& value) { return value.has_value(); });
        if (cutoff && (!all || values[0] != 0 || values[2] != 0)) return false;

        progress.at("USB readback prepare unstarted Incall_Music");
        // Configure only the isolated playback graph. Do not start it; no
        // capture graph, AIF connection, PCM, or stock SET parameters here.
        if (!stable() || !music.prepare(false, {}, candidate.music) || !stable()) return false;
        const auto path = [&](uint32_t from, uint32_t to) {
            progress.at("USB readback GET path");
            if (!stable()) return false;
            const auto request = PathQuery::between(from, to);
            auto reply = request;
            const int rc = get_params(music.id, &reply, sizeof(reply));
            if (!stable() || !request.accepts(rc, reply)) {
                fprintf(stderr, "[avcrt-usb-readback] invalid path %#x -> %#x rc=%d\n", from, to, rc);
                return false;
            }
            fprintf(stderr, "[avcrt-usb-readback] path %#x:%#x -> %#x:%#x delay=%u us\n",
                    from, reply.source_port, to, reply.sink_port, reply.delay);
            return true;
        };
        all &= path(usb.usb_miid, candidate.cutoff);
        all &= path(candidate.cutoff, candidate.stream_mute);
        all &= path(candidate.stream_mute, candidate.encoder);
        all &= path(music.endpoint, candidate.encoder);
        return all;
    }();
    progress.at("USB readback close Incall_Music");
    const bool clean = music.clean() && !agm_service_died.load();
    fprintf(stderr, "[avcrt-usb-readback] queries=%s cleanup=%s; no write authority inferred\n",
            ok ? "validated" : "incomplete", clean ? "ok" : "unverified");
    if (cutoff && clean && ok && stable()) {
        progress.at("USB cutoff final call and privacy check");
        MicProbe probe;
        const auto sampled = Clock::now();
        if (probe.sample(true) != MicStatus::Clear || !preflight() || !stable() ||
            Clock::now() - sampled >= std::chrono::milliseconds(MicPoll::max_age_ms)) return WorkerExit::Refused;
        progress.at("USB kernel cutoff and rollback status");
        return usb_cutoff_once(cut_node, usb, candidate);
    }
    return !clean ? WorkerExit::Unclean : ok && stable() ? WorkerExit::Stopped : WorkerExit::Refused;
}
} // namespace

int usb_capture_levels() {
    if (!usb_lab_signals()) return 2;
    VoiceBinding voice;
    const auto usb = voice.read_route();
    if (usb.kind != StockRoute::Kind::UsbCapture) {
        fprintf(stderr, "[avcrt-usb-lab] refused: no active, uniquely bound USB SIM call\n");
        return 1;
    }
    return guarded_worker([] { return !interrupted; }, [&](WorkerProgress& progress) {
        return usb_capture_once(voice, usb, progress, [] { return !interrupted; }, std::nullopt);
    });
}

int usb_readback(const UsbPathCandidate& candidate, UsbReadbackMode mode) {
    if (!usb_lab_signals()) return 2;
    std::array sorted{candidate.source, candidate.cutoff, candidate.stream_mute, candidate.encoder, candidate.music};
    std::sort(sorted.begin(), sorted.end());
    if (!candidate.source_sg || !candidate.cutoff_sg || candidate.source_sg == candidate.cutoff_sg ||
        !sorted.front() || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) return 2;
    VoiceBinding voice;
    const auto usb = voice.read_route();
    if (usb.kind != StockRoute::Kind::UsbCapture || usb.usb_miid != candidate.source) {
        fprintf(stderr, "[avcrt-usb-readback] refused: no matching, uniquely bound USB SIM call\n");
        return 1;
    }
    return guarded_worker([] { return !interrupted; }, [&](WorkerProgress& progress) {
        return usb_readback_once(voice, usb, candidate, progress, mode);
    });
}
#endif

#ifdef AVCRT_AGM_LAB
int probe_paths(bool (*stock_mic)(), unsigned codec_tx, unsigned encoder) {
    // These MIIDs are operator-supplied hypotheses, never proof of identity.
    Voice voice;
    const auto key = discover_music();
    if (!codec_tx || !encoder || codec_tx == encoder || !valid(key) || !voice.read()) return 1;
    const auto stable = [&] { return voice.same() && stock_mic(); };
    if (!stable()) return 1;

    struct sigaction sa{};
    sa.sa_handler = stop_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    const int lock = open("/data/adb/avcrt_agm.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) return 1;
    const bool ok = flock(lock, LOCK_EX | LOCK_NB) == 0 && [&] {
        Api api;
        int (*get_params)(uint32_t, void*, size_t) = nullptr;
        auto* lib = load_client();
        const auto id = private_id();
        if (!id || !api.load_session(lib) || !symbol(lib, "agm_session_get_params", get_params) ||
            !result("connect IPC", api.crash_cb([](uint64_t) { _exit(1); }, 0))) return false;
        Stream music{api, id};
        if (!stable() || !music.open(key) || !stable() || !music.prepare(false) || !stable()) return false;
        if (!music.endpoint || codec_tx == music.endpoint || encoder == music.endpoint) return false;
        const auto query = [&](uint32_t from) {
            const auto request = PathQuery::between(from, encoder);
            auto reply = request;
            if (!stable()) return false;
            const int rc = get_params(id, &reply, sizeof(reply));
            if (!stable() || !request.accepts(rc, reply)) {
                fprintf(stderr, "[avcrtd:agm-path] no validated path %#x -> %#x (AGM rc=%d)\n", from, encoder, rc);
                return false;
            }
            fprintf(stderr, "[avcrtd:agm-path] %#x:%#x -> %#x:%#x delay=%u us\n",
                    from, reply.source_port, encoder, reply.sink_port, reply.delay);
            return true;
        };
        const bool music_path = query(music.endpoint);
        const bool codec_path = query(codec_tx);
        const bool clean = music.clean();
        if (music_path && codec_path && clean && stable()) {
            fprintf(stderr, "[avcrtd:agm-path] common live encoder candidate; VSID/route ownership NOT verified\n");
            return true;
        }
        return false;
    }();
    close(lock);
    return ok ? 0 : 1;
}

#ifdef AVCRT_VA_TEST_PRIVATE_PCM
int va_call_levels(const char* backend, unsigned aif, unsigned long long epoch, unsigned vsid, VaCallMode mode) {
    const Voice voice{epoch, vsid, {}};
    struct sigaction sa{};
    sa.sa_handler = stop_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    TxLease tx;
    unsigned card = 0;
    const int lock = open("/data/adb/avcrt_agm.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) { perror("[avcrtd:agm-va] lock"); return 1; }
    const int rc = flock(lock, LOCK_EX | LOCK_NB) == 0 &&
                   verify_tx(tx, voice, backend, aif, mode != VaCallMode::Levels, &card)
        ? probe_va(&voice, &tx, card, mode == VaCallMode::BriefMute, mode == VaCallMode::Replace) : 1;
    close(lock);
    return rc;
}
#endif

int run(Probe probe, bool (*stock_mic)(), const char* tx_backend, unsigned tx_aif) {
    if (probe == Probe::VaTags) return probe_va();
    Voice voice;
    const bool has_state = voice.read();
    if (probe == Probe::TxRoute) {
        TxLease lease;
        return has_state && verify_tx(lease, voice, tx_backend, tx_aif, false) ? 0 : 1;
    }
    const bool va_capture = probe == Probe::VaLevels;
    // ABI v1 also reports VSID=0 for ambiguous state. The operator must confirm
    // telephony is idle before consenting; this rejects known calls/epoch changes.
    if (va_capture && (!has_state || voice.vsid || !voice.unchanged())) {
        fprintf(stderr, "[avcrtd:agm-va] refused: DSP call active or state unreadable; test only outside calls\n");
        return 1;
    }
    std::vector<int16_t> speech;
    if (probe == Probe::Speech && (!read_speech(speech) || !render_speech(speech))) return 1;
    const bool replay = probe == Probe::CaptureReplay;
    const bool live = probe == Probe::Realtime || probe == Probe::Overlap || probe == Probe::Calibrate;
    const bool capture = live || replay || probe == Probe::CaptureLevels;
    const GraphKey music = !va_capture && (!capture || replay || live) ? discover_music() : GraphKey{};
    const GraphKey key = capture ? discover_key("StreamTX", "Incall_Record") : music;
    const GraphKey uplink = capture ? discover_key("Stream_MuxDemux", "UL_Mono") : GraphKey{};
    Api api;
    if (!va_capture) {
        fprintf(stderr, "[avcrtd:agm] voice ABI=%s epoch=%llu VSID=%#x %s=%#x:%#x\n",
                has_state ? "v1" : "unavailable", voice.epoch, voice.vsid,
                capture ? "Incall_Record" : "Incall_Music", key.id, key.value);
        if (!valid(key) || (capture && !valid(uplink)) || ((replay || live) && !valid(music))) return 1;
        if (probe != Probe::Capabilities && (!has_state || !voice.same() || !stock_mic())) {
            fprintf(stderr, "[avcrtd:agm] refused: need one unchanged started DSP call and both LPASS mute controls at zero\n");
            return 1;
        }
        if (!api.load(capture)) return 1;
    }
    if (probe == Probe::Capabilities) return has_state ? 0 : 1;

    // Serialize the one-shot experiment; never tries to lock or mutate a PAL FE.
    const int lock = open("/data/adb/avcrt_agm.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) { perror("[avcrtd:agm] lock"); return 1; }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) { close(lock); return 1; }
    struct sigaction sa{};
    sa.sa_handler = stop_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    int rc;
    if (va_capture) {
        rc = probe_va(&voice);
    } else if (probe == Probe::LivePcm || probe == Probe::ReplacePcm) {
        TxLease lease;
        const bool replace = probe == Probe::ReplacePcm;
        rc = !replace || verify_tx(lease, voice, tx_backend, tx_aif, true)
            ? live_stdin(api, music, voice, stock_mic, replace ? &lease : nullptr) : 1;
    } else if (live) {
        rc = live_ul(api, key, uplink, music, voice, stock_mic, probe);
    } else if (capture) {
        rc = capture_ul(api, key, uplink, voice, stock_mic, replay ? &speech : nullptr);
        if (replay) {
            if (rc == 0 && voice.same() && stock_mic() && render_speech(speech)) {
                rc = play(api, music, voice, stock_mic, speech);
            } else {
                fprintf(stderr, "[avcrtd:agm] replay skipped; no audio sent\n");
                rc = 1;
            }
        }
    } else {
        rc = play(api, key, voice, stock_mic, speech);
    }
    close(lock);
    return rc;
}
#endif
} // namespace avcrt::agm
