#include "core/asset.hpp"
#include "core/model_probe.hpp"

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
    }
    try {
        const auto metadata = dayo::core::probePmx(path);
        ok &= check(metadata.version == 2.0F, "PMX version");
        ok &= check(metadata.modelName == "Miku", "PMX UTF-8 model name");
        ok &= check(metadata.vertexCount == 3, "PMX vertex count");
        const auto mesh = dayo::core::loadPmxMesh(path);
        ok &= check(mesh.vertices.size() == 3, "PMX vertex loading");
        ok &= check(mesh.indices.size() == 3 && mesh.indices[2] == 2, "PMX index loading");
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: PMX probe: " << exception.what() << '\n';
        ok = false;
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    return ok ? 0 : 1;
}
