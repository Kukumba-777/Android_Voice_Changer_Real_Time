#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <string_view>

namespace avcrt::lpass {

inline constexpr std::array<std::string_view, 3> kNames = {
    "AVCRT LPASS DEC Power", "AVCRT LPASS DEC Test Mute", "AVCRT LPASS DEC Mute",
};
inline constexpr std::string_view kLeaseName = "AVCRT LPASS DEC Mute Lease ms";
inline constexpr long kMaxLeaseMs = 1000;

enum class Status { Absent, Incomplete, Ambiguous, Unreadable, Incompatible, Changed, Present };

struct Control {
    bool integer_scalar;
    bool readable, writable;
    long min, max, value;
};

struct Snapshot {
    Status status;
    long supported = 0, power = 0, test_mute = 0, mute = 0;
};

struct Lease {
    Status status;
    long ms = 0;
};

template <typename Id> class Probe {
public:
    void observe(std::string_view name, const Id& id) {
        if (name == kLeaseName) {
            if (!lease_count_) lease_id_ = id;
            if (lease_count_ < 2) ++lease_count_;
            return;
        }
        for (std::size_t i = 0; i < kNames.size(); ++i) {
            if (name != kNames[i]) continue;
            if (counts_[i] == 0) ids_[i] = id;
            if (counts_[i] < 2) ++counts_[i];
            return;
        }
    }

    template <typename Read> Snapshot snapshot(Read read) const {
        if (counts_ == std::array<unsigned, 3>{}) return {Status::Absent};
        for (auto count : counts_) if (count > 1) return {Status::Ambiguous};
        for (auto count : counts_) if (!count) return {Status::Incomplete};

        std::array<Control, 3> first{};
        for (std::size_t i = 0; i < ids_.size(); ++i) {
            auto& c = first[i];
            if (!read(ids_[i], c)) return {Status::Unreadable};
            if (!valid(c, i != 0) || c.max != first[0].max) return {Status::Incompatible};
        }
        // This is not an atomic kernel snapshot. Reject changes we can observe.
        for (std::size_t i = ids_.size(); i-- > 0;) {
            Control after{};
            if (!read(ids_[i], after)) return {Status::Unreadable};
            if (!valid(after, i != 0) || after.max != first[i].max || after.value != first[i].value)
                return {Status::Changed};
        }
        return {Status::Present, first[0].max, first[0].value, first[1].value, first[2].value};
    }

    // The original DEC controls still work without this optional extension.
    // A lease must not be inferred from their presence or from an invalid timer.
    template <typename Read> Lease lease(const Snapshot& dec, Read read) const {
        if (!lease_count_) return {Status::Absent};
        if (lease_count_ != 1) return {Status::Ambiguous};
        if (dec.status != Status::Present)
            return {dec.status == Status::Absent ? Status::Incomplete : dec.status};
        Control first{}, after{};
        if (!read(lease_id_, first)) return {Status::Unreadable};
        if (!valid_lease(first)) return {Status::Incompatible};
        if (!read(lease_id_, after)) return {Status::Unreadable};
        if (!valid_lease(after) || after.max != first.max || after.value != first.value)
            return {Status::Changed};
        return {Status::Present, first.value};
    }

private:
    static bool valid(const Control& c, bool writable) {
        return c.integer_scalar && c.readable && c.writable == writable && c.min == 0 &&
               c.max > 0 && c.value >= 0 && c.value <= c.max &&
               (c.max == std::numeric_limits<long>::max() || (c.max & (c.max + 1)) == 0);
    }

    static bool valid_lease(const Control& c) {
        return c.integer_scalar && c.readable && !c.writable && c.min == 0 &&
               c.value > 0 && c.value <= c.max && c.value <= kMaxLeaseMs;
    }

    std::array<Id, 3> ids_{};
    std::array<unsigned, 3> counts_{};
    Id lease_id_{};
    unsigned lease_count_ = 0;
};

} // namespace avcrt::lpass
