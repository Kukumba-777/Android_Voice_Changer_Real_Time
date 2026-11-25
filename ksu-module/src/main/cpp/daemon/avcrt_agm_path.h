#pragma once

#include <cstdint>
#include <limits>

namespace avcrt::agm {

// AudioReach APM_PARAM_ID_PATH_DELAY. A successful query establishes a live
// structural path, not a VSID binding, a mux selection or a route reservation.
struct PathQuery {
    static constexpr uint32_t kApm = 1, kPathDelay = 0x08001119;
    static constexpr uint32_t kUnset = std::numeric_limits<uint32_t>::max();
    uint32_t instance = kApm, param = kPathDelay, size = 24, error = 0;
    uint32_t count = 1;
    uint32_t source = 0, source_port = 0, sink = 0, sink_port = 0;
    uint32_t delay = kUnset;

    static PathQuery between(uint32_t source, uint32_t sink) {
        PathQuery request;
        request.source = source;
        request.sink = sink;
        return request;
    }

    bool accepts(int transport_status, const PathQuery& reply) const {
        return !transport_status && source && sink && source != sink && reply.instance == kApm &&
               reply.param == kPathDelay && reply.size == 24 && !reply.error &&
               reply.count == 1 && reply.source == source && reply.sink == sink &&
               reply.source_port && reply.sink_port &&
               (!source_port || source_port == reply.source_port) &&
               (!sink_port || sink_port == reply.sink_port) && reply.delay != kUnset;
    }
};
static_assert(sizeof(PathQuery) == 40, "APM in-band payload ABI");

// AudioReach soft-volume GET, exactly one 4-byte parameter with 8-byte
// payload alignment. The transport can return success with an unchanged
// buffer. A reply is evidence of readback only, never of write ownership.
struct SoftVolumeQuery {
    static constexpr uint32_t kMasterGain = 0x08001035, kMasterMute = 0x08001036;
    uint32_t instance, param, size = 4, error = 0;
    uint32_t value = UINT32_MAX, padding = 0;

    bool accepts(int transport_status, const SoftVolumeQuery& reply) const {
        if (transport_status || !instance || reply.instance != instance ||
            reply.param != param || reply.size != size || reply.error) return false;
        if (param == kMasterMute) return reply.value <= 1;
        return param == kMasterGain && reply.value <= UINT16_MAX; // Upper 16 bits reserved.
    }
};
static_assert(sizeof(SoftVolumeQuery) == 24, "soft-volume in-band payload ABI");

}  // namespace avcrt::agm
