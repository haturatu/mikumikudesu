#pragma once

#include "core/fx/fx_controller_resolver.hpp"

namespace dayo::app {

// Keep owning strings: pending focus picks and native controllers can retain a
// snapshot after a model is reloaded. Compare before assigning immutable names
// so ordinary animation frames do not copy them or replace their storage.
inline void updateEvaluationMetadata(core::fx::EvaluatedModelState& result, const core::ModelInstance& instance) {
    const auto assignChanged = [](auto& destination, const auto& source) {
        if (destination != source)
            destination = source;
    };
    result.id = instance.id;
    assignChanged(result.sourcePath, instance.sourcePath);
    assignChanged(result.displayName, instance.displayName);
    assignChanged(result.modelName, instance.model->metadata.modelName);
    assignChanged(result.englishName, instance.model->metadata.englishName);
    const auto updateNames = [&](auto& destination, const auto& source) {
        destination.resize(source.size());
        for (std::size_t index = 0; index < source.size(); ++index)
            assignChanged(destination[index], source[index].name);
    };
    updateNames(result.morphNames, instance.model->morphs);
    updateNames(result.boneNames, instance.model->bones);
}

} // namespace dayo::app
