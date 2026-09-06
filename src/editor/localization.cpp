#include "editor/localization.hpp"

namespace dayo::editor {

const std::unordered_map<std::string, std::string>& Localization::english() {
    static const std::unordered_map<std::string, std::string> table{
        {"window.keyframe", "Keyframe"},       {"window.bone", "Bone"}, {"window.interpolation", "Interpolation"},
        {"window.external_parent", "External Parent"}, {"window.material", "Material"}, {"window.fx_debug", "FX Debug"},
        {"window.output", "Output"},           {"action.commit", "Commit"}, {"action.cancel", "Cancel"},
    };
    return table;
}

const std::unordered_map<std::string, std::string>& Localization::japanese() {
    static const std::unordered_map<std::string, std::string> table{
        {"window.keyframe", "キーフレーム"}, {"window.bone", "ボーン"}, {"window.interpolation", "補間"},
        {"window.external_parent", "外部親"}, {"window.material", "マテリアル"}, {"window.fx_debug", "FXデバッグ"},
        {"window.output", "出力"}, {"action.commit", "確定"}, {"action.cancel", "キャンセル"},
    };
    return table;
}

std::string_view Localization::get(std::string_view key) const noexcept {
    const auto& table = (language_ == "ja" || language_ == "jp") ? japanese() : english();
    const auto found = table.find(std::string(key));
    if (found == table.end())
        return key;
    return found->second;
}

} // namespace dayo::editor
