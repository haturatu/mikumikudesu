#include "editor/selection.hpp"

#include <algorithm>

namespace dayo::editor {

void Selection::set(std::vector<MotionKeyId> ids) {
    ids_ = std::move(ids);
    std::ranges::sort(ids_, [](const MotionKeyId& left, const MotionKeyId& right) {
        if (left.track != right.track)
            return static_cast<int>(left.track) < static_cast<int>(right.track);
        return left.stableId < right.stableId;
    });
    ids_.erase(std::unique(ids_.begin(), ids_.end()), ids_.end());
}

void Selection::add(MotionKeyId id) {
    if (!contains(id))
        ids_.push_back(id);
}

bool Selection::remove(MotionKeyId id) noexcept {
    const auto found = std::ranges::find(ids_, id);
    if (found == ids_.end())
        return false;
    ids_.erase(found);
    return true;
}

void Selection::clear() noexcept {
    ids_.clear();
}

bool Selection::contains(MotionKeyId id) const noexcept {
    return std::ranges::find(ids_, id) != ids_.end();
}

std::vector<core::MotionKeyRef> Selection::resolveTransient(const core::MotionDocument& document,
                                                            const StableIdTable& table) const {
    std::vector<core::MotionKeyRef> refs;
    refs.reserve(ids_.size());
    for (const auto& id : ids_) {
        const auto index = table.resolve(document, id);
        if (index.has_value())
            refs.push_back({id.track, *index});
    }
    return refs;
}

} // namespace dayo::editor
