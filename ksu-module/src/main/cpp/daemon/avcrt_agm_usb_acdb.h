#pragma once

#include "avcrt_agm_stock_tx.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace avcrt::agm::usb_acdb {

using Key = std::pair<uint32_t, uint32_t>;
struct Keys {
    Key usb, voice;
    std::set<Key> capture, music;
};
struct Candidate {
    uint32_t source_sg, cutoff_sg, cutoff, stream_mute, encoder, capture, music;
    bool operator==(const Candidate& rhs) const {
        return std::tie(source_sg, cutoff_sg, cutoff, stream_mute, encoder, capture, music) ==
            std::tie(rhs.source_sg, rhs.cutoff_sg, rhs.cutoff, rhs.stream_mute, rhs.encoder, rhs.capture, rhs.music);
    }
};

// Static ACDB evidence only. Bounds are checked even on tables that cannot
// authorize a particular START. The caller also checks live AGM and DSP data.
namespace detail {
inline void need(bool ok) { if (!ok) throw std::runtime_error("invalid or ambiguous USB ACDB topology"); }
using Bytes = std::string_view;
inline Bytes slice(Bytes b, size_t offset, size_t size) {
    need(offset <= b.size() && size <= b.size() - offset);
    return b.substr(offset, size);
}
struct Reader {
    Bytes b;
    uint32_t word() {
        const auto w = take(4);
        return uint32_t(uint8_t(w[0])) | (uint32_t(uint8_t(w[1])) << 8) |
            (uint32_t(uint8_t(w[2])) << 16) | (uint32_t(uint8_t(w[3])) << 24);
    }
    Bytes take(size_t n) { const auto p = slice(b, 0, n); b.remove_prefix(n); return p; }
    Bytes record() { return take(word()); }
    Reader at(size_t offset) const { return {slice(b, offset, offset <= b.size() ? b.size() - offset : 0)}; }
    void end() const { need(b.empty()); }
};

constexpr uint32_t ModuleList = 0x08001002, Connections = 0x08001004;
constexpr uint32_t Usb = 0x07001050, Read = 0x07001001, Write = 0x07001000;
constexpr uint32_t Volume = 0x0700101b, Encoder = 0x07001008;
constexpr uint32_t DeviceMute = 0xc0000035, StreamMute = 0xc0000007, Shmem = 0xc0000001, MasterMute = 0x08001036;

using Edge = std::pair<uint32_t, uint32_t>;
struct Graph { std::set<uint32_t> sgs; std::set<Edge> edges; };
using Owner = std::pair<uint32_t, uint32_t>;  // SG, public MID
using Tagged = std::pair<uint32_t, uint32_t>; // SG, MIID
template<class T> const T& one(const std::set<T>& values) {
    need(values.size() == 1);
    return *values.begin();
}

class Topology {
    std::map<Bytes, Bytes> tables_;
    std::map<uint32_t, std::set<Owner>> owners_;
    std::map<uint32_t, Graph> graphs_;
    std::map<std::set<Key>, std::set<uint32_t>> profiles_;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> tags_;
    std::vector<std::tuple<uint32_t, uint32_t, std::set<Edge>>> cross_;

    Reader table(Bytes tag) const { return {tables_.at(tag)}; }
    Bytes pool(uint32_t offset) const { return table("POOL").at(offset).record(); }
    static void links(Bytes payload, std::set<Edge>& edges) {
        Reader r{payload};
        const auto n = r.word();
        need(r.b.size() / 16 == n && r.b.size() % 16 == 0);
        for (uint32_t i = 0; i < n; ++i) {
            const auto from = r.word(); r.word();
            const auto to = r.word(); r.word();
            edges.emplace(from, to);
        }
    }
    void graph(uint32_t offset) {
        auto [it, inserted] = graphs_.try_emplace(offset);
        if (!inserted) return;
        auto& g = it->second;
        Reader rows{pool(offset)};
        for (auto n = rows.word(); n; --n) {
            const auto sg = rows.word();
            need(sg && g.sgs.insert(sg).second);
            Reader props{rows.record()};
            props.record(); // driver props
            Reader spf{props.record()};
            props.end();
            while (!spf.b.empty()) {
                spf.word();
                const auto param = spf.word(), size = spf.word();
                const auto payload = spf.take(size);
                spf.take((8 - size % 8) % 8);
                if (param == Connections) links(payload, g.edges);
                if (param != ModuleList) continue;
                Reader mods{payload};
                for (auto lists = mods.word(); lists; --lists) {
                    need(mods.word() == sg);
                    mods.word();
                    for (auto m = mods.word(); m; --m) {
                        const auto mid = mods.word(), miid = mods.word();
                        need(mid && miid);
                        owners_[miid].emplace(sg, mid);
                    }
                }
                mods.end();
            }
        }
        rows.end();
    }
    void profiles() {
        auto keys = table("GKVT");
        const auto values = table("GKVL");
        for (auto sections = keys.word(); sections; --sections) {
            const auto width = keys.word(), n = keys.word();
            need(width && width <= 32);
            for (uint32_t row = 0; row < n; ++row) {
                std::vector<uint32_t> ids(width);
                for (auto& id : ids) id = keys.word();
                auto variants = values.at(keys.word());
                need(variants.word() == width);
                for (auto count = variants.word(); count; --count) {
                    std::set<Key> profile;
                    for (const auto id : ids) profile.emplace(id, variants.word());
                    variants.word(); // SG list offset; modules are in the SG properties
                    const auto offset = variants.word();
                    profiles_[std::move(profile)].insert(offset);
                    graph(offset);
                }
            }
        }
        keys.end();
    }
    template<class Row> void tag_table(Bytes table_name, Row row) {
        auto entries = table(table_name);
        std::set<Key> seen;
        for (auto n = entries.word(); n; --n) {
            const auto sg = entries.word(), tag = entries.word(), offset = entries.word();
            need(seen.emplace(sg, tag).second);
            row(sg, tag, offset);
        }
        entries.end();
    }
    static bool relevant(uint32_t tag) { return tag == DeviceMute || tag == StreamMute || tag == Shmem; }
    Tagged tagged(const std::set<uint32_t>& sgs, uint32_t tag, uint32_t mid) const {
        std::set<Tagged> all;
        for (const auto sg : sgs) {
            auto pos = tags_.find({sg, tag});
            if (pos == tags_.end()) continue;
            auto lists = table("TMDE").at(pos->second);
            for (auto n = lists.word(); n; --n) {
                need(lists.word() == mid);
                const auto miid = lists.word();
                need(one(owners_.at(miid)) == Owner(sg, mid));
                all.emplace(sg, miid);
            }
        }
        return one(all);
    }
    void mute_params(uint32_t sg, uint32_t miid) {
        std::set<uint32_t> seen;
        tag_table("MTKT", [&](uint32_t s, uint32_t tag, uint32_t offset) {
            if (s != sg || tag != DeviceMute) return;
            auto variants = table("MTLU").at(offset);
            need(variants.word() == 1 && variants.word() == 2);
            for (unsigned i = 0; i < 2; ++i) {
                const auto value = variants.word();
                need(value <= 1 && seen.insert(value).second);
                auto defs = table("MTDE").at(variants.word());
                auto offsets = table("MTDO").at(variants.word());
                auto n = defs.word();
                need(n == offsets.word());
                unsigned matches = 0;
                for (; n; --n) {
                    const auto target = defs.word(), param = defs.word(), p = offsets.word();
                    if (param != MasterMute) continue;
                    Reader v{pool(p)};
                    need(target == miid && v.word() == value && ++matches == 1);
                    v.end();
                }
                need(matches == 1);
            }
        });
        need(seen.size() == 2);
    }
    void cross_links() {
        auto lut = table("SCLU");
        auto defs = table("SCDE"), offsets = table("SCDO");
        const auto rows = lut.word();
        need(lut.b.size() / 16 == rows && lut.b.size() % 16 == 0);
        for (uint32_t i = 0; i < rows; ++i) {
            const auto from = lut.word(), to = lut.word();
            auto d = defs.at(lut.word()), o = offsets.at(lut.word());
            auto n = d.word();
            need(n == o.word());
            std::set<Edge> edges;
            for (; n; --n) {
                d.word();
                const auto param = d.word(), p = o.word();
                if (param == Connections) links(pool(p), edges);
            }
            if (!edges.empty()) cross_.emplace_back(from, to, std::move(edges));
        }
    }
    bool isolated(const Graph& v, const Graph& c, const Graph& m, uint32_t source, const Candidate& x) const {
        auto involved = v.sgs;
        involved.insert(c.sgs.begin(), c.sgs.end());
        involved.insert(m.sgs.begin(), m.sgs.end());
        const auto owns = [&](uint32_t miid) {
            const auto& owners = owners_.at(miid);
            const bool included = std::any_of(owners.begin(), owners.end(), [&](Owner o) { return involved.count(o.first); });
            if (included) need(owners.size() == 1);
            return included;
        };
        std::map<uint32_t, std::set<uint32_t>> next;
        const auto add = [&](const std::set<Edge>& from) {
            for (const auto& [a, b] : from) {
                const bool has_a = owns(a), has_b = owns(b);
                if (has_a && has_b) next[a].insert(b);
            }
        };
        add(v.edges); add(c.edges); add(m.edges);
        for (const auto& [a, b, edges] : cross_) if (involved.count(a) && involved.count(b)) add(edges);
        const auto path = [&](uint32_t a, uint32_t b, uint32_t excluding = 0) {
            std::set<uint32_t> seen{excluding};
            std::vector<uint32_t> todo{a};
            while (!todo.empty()) {
                const auto cur = todo.back(); todo.pop_back();
                if (!seen.insert(cur).second) continue;
                if (cur == b) return true;
                const auto it = next.find(cur);
                if (it != next.end()) todo.insert(todo.end(), it->second.begin(), it->second.end());
            }
            return false;
        };
        return std::set{source, x.cutoff, x.stream_mute, x.encoder, x.capture, x.music}.size() == 6 &&
            path(source, x.cutoff) && path(x.cutoff, x.stream_mute) && path(x.stream_mute, x.encoder) &&
            path(source, x.capture) && path(x.music, x.encoder) &&
            !path(source, x.encoder, x.cutoff) && !path(source, x.encoder, x.stream_mute) &&
            !path(x.cutoff, x.capture) && !path(x.music, x.capture) &&
            !path(x.music, x.cutoff) && !path(x.music, x.stream_mute);
    }
public:
    explicit Topology(Bytes bytes) {
        Reader file{bytes};
        need(file.take(8) == Bytes("ACDB\0\0\0\0", 8));
        auto chunks = Reader{file.record()};
        file.end();
        while (!chunks.b.empty()) {
            const auto tag = chunks.take(4);
            need(tables_.emplace(tag, chunks.record()).second);
        }
        profiles();
        tag_table("TMLU", [&](uint32_t sg, uint32_t tag, uint32_t offset) {
            if (relevant(tag)) tags_.emplace(Key{sg, tag}, offset);
        });
        cross_links();
    }
    Candidate resolve(const Keys& keys, const UsbStartEvidence& start) {
        need(start.count && start.count <= start.ids.size());
        const auto source = start.route.usb_miid;
        const auto [source_sg, mid] = one(owners_.at(source));
        need(mid == Usb && start.contains(source_sg));
        const auto& c = graphs_.at(one(profiles_.at(keys.capture)));
        const auto& m = graphs_.at(one(profiles_.at(keys.music)));
        const auto capture = tagged(c.sgs, Shmem, Read).second;
        const auto music = tagged(m.sgs, Shmem, Write).second;
        std::optional<Candidate> answer;
        std::set<uint32_t> evaluated;
        for (const auto& [profile, offsets] : profiles_) {
            if (!profile.count(keys.usb) || !profile.count(keys.voice)) continue;
            for (const auto offset : offsets) {
                if (!evaluated.insert(offset).second) continue;
                const auto& v = graphs_.at(offset);
                if (!std::all_of(start.ids.begin(), start.ids.begin() + start.count,
                                 [&](uint32_t sg) { return v.sgs.count(sg); })) continue;
                need(c.sgs.count(source_sg));
                for (const auto sg : v.sgs) need(sg == source_sg || !c.sgs.count(sg));
                for (const auto sg : m.sgs) need(!v.sgs.count(sg) && !c.sgs.count(sg));
                const auto [cutoff_sg, cutoff] = tagged(v.sgs, DeviceMute, Volume);
                const auto [mute_sg, stream_mute] = tagged(v.sgs, StreamMute, Volume);
                need(cutoff_sg != source_sg && cutoff_sg != mute_sg && start.contains(cutoff_sg));
                std::set<uint32_t> encoders;
                for (const auto& [miid, owners] : owners_)
                    if (owners.size() == 1 && *owners.begin() == Owner(mute_sg, Encoder)) encoders.insert(miid);
                const Candidate x{source_sg, cutoff_sg, cutoff, stream_mute, one(encoders), capture, music};
                mute_params(cutoff_sg, cutoff);
                need(isolated(v, c, m, source, x) && (!answer || *answer == x));
                answer = x;
            }
        }
        need(answer.has_value());
        return *answer;
    }
};
} // namespace detail

inline std::optional<Candidate> resolve(std::string_view bytes, const Keys& keys, const UsbStartEvidence& start) {
    try { return detail::Topology(bytes).resolve(keys, start); }
    catch (const std::exception&) { return {}; }
}
} // namespace avcrt::agm::usb_acdb
