#include "avcrt_agm_path.h"

#include <cassert>
#include <cerrno>
#include <initializer_list>

using avcrt::agm::PathQuery;
using avcrt::agm::SoftVolumeQuery;

int main() {
    const auto request = PathQuery::between(0x4100, 0x4200);
    assert(!request.accepts(0, request));
    auto reply = request;
    reply.source_port = 1;
    reply.sink_port = 2;
    assert(!request.accepts(0, reply));  // header/ports copied, no delay returned
    reply.delay = 0;
    assert(request.accepts(0, reply));  // a valid delay can be zero
    assert(!request.accepts(-ENOSYS, reply));
    auto exact = request;
    exact.source_port = reply.source_port;
    exact.sink_port = reply.sink_port;
    assert(exact.accepts(0, reply));

    for (auto field : {&PathQuery::instance, &PathQuery::param, &PathQuery::size,
                       &PathQuery::error, &PathQuery::count, &PathQuery::source, &PathQuery::sink,
                       &PathQuery::source_port, &PathQuery::sink_port}) {
        auto bad = reply;
        ++(bad.*field);
        assert(!exact.accepts(0, bad));
    }
    for (auto field : {&PathQuery::source_port, &PathQuery::sink_port}) {
        auto bad = reply;
        bad.*field = 0;
        assert(!request.accepts(0, bad));
    }
    assert(!PathQuery::between(0, request.sink).accepts(0, reply));
    assert(!PathQuery::between(request.sink, request.sink).accepts(0, reply));

    for (auto param : {SoftVolumeQuery::kMasterMute, SoftVolumeQuery::kMasterGain}) {
        const SoftVolumeQuery get{0x4101, param};
        assert(!get.accepts(0, get));
        auto returned = get;
        returned.value = 0;
        assert(get.accepts(0, returned));
        returned.value = param == SoftVolumeQuery::kMasterMute ? 1 : 0x2000;
        assert(get.accepts(0, returned));
        assert(!get.accepts(-EINVAL, returned));
        for (auto field : {&SoftVolumeQuery::instance, &SoftVolumeQuery::param,
                           &SoftVolumeQuery::size, &SoftVolumeQuery::error}) {
            auto bad = returned;
            ++(bad.*field);
            assert(!get.accepts(0, bad));
        }
        returned.value = param == SoftVolumeQuery::kMasterMute ? 2 : 0x10000;
        assert(!get.accepts(0, returned));
    }
    auto unknown = SoftVolumeQuery{0x4101, 0};
    unknown.value = 0;
    assert(!unknown.accepts(0, unknown));
}
