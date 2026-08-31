#include "app/audio_export_command.hpp"

#include "app/application.hpp"
#include "core/asset.hpp"
#include "core/audio_export.hpp"
#include "core/log.hpp"
#include "core/media.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace dayo::app {
namespace {

std::filesystem::path resolveAudioSource(const Options& options) {
    const auto& exportOptions = *options.audioExport;
    if (exportOptions.source) {
        const auto source = std::filesystem::absolute(*exportOptions.source);
        core::MediaFile media(source);
        if (!media.info().hasAudio) {
            throw std::runtime_error("audio source has no audio stream: " + source.string());
        }
        return source;
    }

    std::vector<std::filesystem::path> candidates;
    for (const auto& asset : options.assets) {
        const auto kind = core::classifyAsset(asset);
        if (kind != core::AssetKind::audio && kind != core::AssetKind::video) continue;
        const auto source = std::filesystem::absolute(asset);
        core::MediaFile media(source);
        if (media.info().hasAudio
            && std::find(candidates.begin(), candidates.end(), source) == candidates.end()) {
            candidates.push_back(source);
        }
    }
    if (candidates.empty()) {
        throw std::runtime_error("no audio source found; add an audio/video --asset or use --audio-source PATH");
    }
    if (candidates.size() != 1) {
        throw std::runtime_error(
            "multiple audio sources found; use --audio-source PATH");
    }
    return candidates.front();
}

} // namespace

int runAudioExport(const Options& options) {
    if (!options.audioExport) throw std::invalid_argument("--export-m4a is required");
    if (!core::canExportM4a()) throw std::runtime_error("M4A export requires FFmpeg with an AAC encoder");

    const auto source = resolveAudioSource(options);
    core::AudioExportRequest request;
    request.source = source;
    request.destination = std::filesystem::absolute(options.audioExport->destination);
    request.bitrate = options.audioExport->bitrate;
    request.startSeconds = options.audioExport->startSeconds;
    request.endSeconds = options.audioExport->endSeconds;
    request.overwrite = options.audioExport->overwrite;

    double lastReportedRatio = -1.0;
    const auto result = core::exportM4a(
        request,
        [&](const core::AudioExportProgress& progress) {
            const double ratio = progress.ratio();
            if (progress.totalSeconds <= 0.0 || ratio >= 1.0 || ratio - lastReportedRatio >= 0.05) {
                if (progress.totalSeconds > 0.0) {
                    log::info("Audio export: ", static_cast<int>(std::round(ratio * 100.0)), "%");
                } else {
                    log::info("Audio export: processing");
                }
                lastReportedRatio = ratio;
            }
        });
    log::info("Exported M4A: ", result.output.string(), " (",
              result.durationSeconds, " s, ", result.encodedSamples, " samples)");
    return 0;
}

} // namespace dayo::app
