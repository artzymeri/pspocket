#pragma once

// Object IDs handed out as RPCS3's ID manager does for an id_step of 1, for HLE objects whose IDs the game sees.
// Reference: RPCS3 rpcs3/Emu/IdManager.h/.cpp (idm::make, allocate_id, get_index).

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "pspocket/core/types.hpp"

namespace pspocket::hle {

// The IDs of one kind of object: Base, Base + 1, ... in order until each has been used once, then the lowest free one;
// 0, idm's invalid ID for a non-zero base, when all Count are in use. Removing an ID never rewinds the order (idm keeps
// its highest index). No locking: the module's own lock guards it.
template <class T, u32 Base, u32 Count>
class IdTable {
    static_assert(Base != 0 && Count != 0, "idm's invalid ID is 0 only for a non-zero base");

public:
    u32 make(T value)
    {
        u32 i = next_;
        if (next_ < Count) next_++;
        else i = static_cast<u32>(std::find(slots_.begin(), slots_.end(), std::nullopt) - slots_.begin());
        if (i == Count) return 0;
        slots_[i] = std::move(value);
        return Base + i;
    }

    T* find(u32 id)
    {
        const u32 i = id - Base; // an ID below Base wraps past Count, as in idm's get_index
        return i < Count && slots_[i] ? &*slots_[i] : nullptr;
    }

    bool remove(u32 id)
    {
        if (!find(id)) return false;
        slots_[id - Base].reset();
        return true;
    }

    // Whether make() would fail.
    bool full() const
    {
        return std::all_of(slots_.begin(), slots_.end(), [](const std::optional<T>& slot) { return slot.has_value(); });
    }

private:
    std::array<std::optional<T>, Count> slots_{};
    u32 next_ = 0; // idm's highest_index
};

} // namespace pspocket::hle
