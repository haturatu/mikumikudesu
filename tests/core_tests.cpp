#include "core/asset.hpp"
#include "core/animation.hpp"
#include "core/image.hpp"
#include "core/model_probe.hpp"
#include "core/motion.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

template <typename T>
void append(std::ofstream& output, T value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void appendText(std::ofstream& output, std::string_view text) {
    append(output, static_cast<std::int32_t>(text.size()));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool check(bool value, std::string_view message) {
    if (!value) std::cerr << "FAIL: " << message << '\n';
    return value;
}

} // namespace

int main() {
    using dayo::core::AssetKind;
    bool ok = true;
    ok &= check(dayo::core::classifyAsset("Miku.PMX") == AssetKind::pmx, "PMX extension");
    ok &= check(dayo::core::classifyAsset("motion.vmd") == AssetKind::vmd, "VMD extension");
    ok &= check(dayo::core::classifyAsset("sound.M4A") == AssetKind::audio, "audio extension");
    ok &= check(dayo::core::classifyAsset("movie.webm") == AssetKind::video, "video extension");

    const auto path = std::filesystem::temp_directory_path() / "mikumikudesu-core-test.pmx";
    {
        std::ofstream output(path, std::ios::binary);
        output.write("PMX ", 4);
        append(output, 2.0F);
        append(output, static_cast<std::uint8_t>(8));
        const std::array<std::uint8_t, 8> settings { 1, 0, 4, 4, 4, 4, 4, 4 };
        output.write(reinterpret_cast<const char*>(settings.data()), settings.size());
        appendText(output, "Miku");
        appendText(output, "Miku English");
        appendText(output, "comment");
        appendText(output, "comment en");
        append(output, static_cast<std::int32_t>(3));
        for (std::int32_t vertex = 0; vertex < 3; ++vertex) {
            const std::array<float, 3> position { static_cast<float>(vertex), static_cast<float>(vertex & 1), 0.0F };
            const std::array<float, 3> normal { 0.0F, 0.0F, 1.0F };
            const std::array<float, 2> uv { 0.0F, 0.0F };
            output.write(reinterpret_cast<const char*>(position.data()), sizeof(position));
            output.write(reinterpret_cast<const char*>(normal.data()), sizeof(normal));
            output.write(reinterpret_cast<const char*>(uv.data()), sizeof(uv));
            append(output, static_cast<std::uint8_t>(0));
            append(output, static_cast<std::int32_t>(0));
            append(output, 1.0F);
        }
        append(output, static_cast<std::int32_t>(3));
        append(output, static_cast<std::uint32_t>(0));
        append(output, static_cast<std::uint32_t>(1));
        append(output, static_cast<std::uint32_t>(2));
        append(output, static_cast<std::int32_t>(0)); // textures
        append(output, static_cast<std::int32_t>(1)); // materials
        appendText(output, "Material");
        appendText(output, "Material");
        const std::array<float, 4> diffuse { 1.0F, 1.0F, 1.0F, 1.0F };
        const std::array<float, 3> vector3 {};
        const std::array<float, 4> vector4 {};
        output.write(reinterpret_cast<const char*>(diffuse.data()), sizeof(diffuse));
        output.write(reinterpret_cast<const char*>(vector3.data()), sizeof(vector3));
        append(output, 0.0F);
        output.write(reinterpret_cast<const char*>(vector3.data()), sizeof(vector3));
        append(output, static_cast<std::uint8_t>(0));
        output.write(reinterpret_cast<const char*>(vector4.data()), sizeof(vector4));
        append(output, 0.0F);
        append(output, static_cast<std::int32_t>(-1));
        append(output, static_cast<std::int32_t>(-1));
        append(output, static_cast<std::uint8_t>(0));
        append(output, static_cast<std::uint8_t>(0));
        append(output, static_cast<std::int32_t>(-1));
        appendText(output, "");
        append(output, static_cast<std::int32_t>(3));
        // bones, morphs, display frames, bodies and joints
        for (int section = 0; section < 5; ++section) append(output, static_cast<std::int32_t>(0));
    }
    try {
        const auto metadata = dayo::core::probePmx(path);
        ok &= check(metadata.version == 2.0F, "PMX version");
        ok &= check(metadata.modelName == "Miku", "PMX UTF-8 model name");
        ok &= check(metadata.vertexCount == 3, "PMX vertex count");
        const auto mesh = dayo::core::loadPmxMesh(path);
        ok &= check(mesh.vertices.size() == 3, "PMX vertex loading");
        ok &= check(mesh.indices.size() == 3 && mesh.indices[2] == 2, "PMX index loading");
        const auto model = dayo::core::loadPmxModel(path);
        ok &= check(model.materials.size() == 1 && model.bones.empty(), "complete PMX sections");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: PMX probe: " << exception.what() << '\n';
        ok = false;
    }
    try {
        const auto icon = dayo::core::loadImageRgba8(
            std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/res/dayoicon.png");
        ok &= check(icon.width > 0 && icon.height > 0 && icon.pixels.size() == icon.width * icon.height * 4U,
                    "RGBA image decode");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: image load: " << exception.what() << '\n';
        ok = false;
    }
    try {
        std::filesystem::path sampleVmd;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/sample")) {
            if (entry.path().extension() == ".vmd") { sampleVmd = entry.path(); break; }
        }
        const auto motion = dayo::core::loadVmd(sampleVmd);
        ok &= check(!motion.modelName.empty(), "VMD CP932 model name");
        ok &= check(!motion.bones.empty(), "VMD bone keys");
        for (const auto& entry : std::filesystem::directory_iterator(
                 std::filesystem::path(DAYO_SOURCE_DIR) / "MikuMikuDayo/sample")) {
            if (entry.path().extension() != ".pmx") continue;
            try {
                auto candidate = dayo::core::loadPmxModel(entry.path());
                if (candidate.metadata.modelName != motion.modelName || candidate.vertices.empty()) continue;
                dayo::core::MmdAnimator animator(candidate);
                animator.setMotion(&motion);
                const auto first = animator.evaluate(0.0F);
                const auto animated = animator.evaluate(10.0F);
                bool changed = false;
                for (std::size_t i = 0; i < first.vertices.size(); ++i) {
                    if (first.vertices[i].position != animated.vertices[i].position) { changed = true; break; }
                }
                ok &= check(changed, "VMD CPU skinning changes vertices");
                break;
            } catch (const std::exception&) {
                // Some tiny effect descriptors use the PMX extension without model sections.
            }
        }
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: VMD load: " << exception.what() << '\n';
        ok = false;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return ok ? 0 : 1;
}
