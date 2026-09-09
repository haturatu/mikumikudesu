#include "app/cli.hpp"

#include <argparse/argparse.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dayo::app {
namespace {

struct RawCliOptions {
    bool probe{};
    bool hidden{};
    bool validation{true};
    std::string renderer{"preview"};
    std::optional<std::uint64_t> frames;
    std::vector<std::string> assets;
    std::optional<std::string> saveProject;

    std::optional<std::string> exportM4a;
    std::optional<std::string> exportVideo;
    std::optional<std::string> audioSource;
    std::optional<std::uint32_t> audioBitrateKbps;
    std::optional<double> audioFrom;
    std::optional<double> audioTo;

    std::optional<std::uint32_t> videoWidth;
    std::optional<std::uint32_t> videoHeight;
    std::optional<double> videoFps;
    std::optional<std::string> videoCodec;
    std::optional<std::uint32_t> videoBitrateKbps;
    std::optional<std::uint64_t> videoFromFrame;
    std::optional<std::uint64_t> videoToFrame;
    bool videoHardware{};
    bool noAudio{};
    bool overwrite{};
};

struct ConfiguredParser {
    argparse::ArgumentParser program;
    std::vector<std::string> valueOptions;

    ConfiguredParser(std::string programName, std::string version)
        : program(std::move(programName), std::move(version)) {}

    template <typename... Names> argparse::Argument& addValueArgument(Names... names) {
        (valueOptions.emplace_back(names), ...);
        return program.add_argument(names...);
    }

    template <typename... Names>
    argparse::Argument& addValueArgumentTo(argparse::ArgumentParser::MutuallyExclusiveGroup& group, Names... names) {
        (valueOptions.emplace_back(names), ...);
        return group.add_argument(names...);
    }
};

struct PreprocessedArguments {
    std::vector<std::string> argv;
};

template <typename T> std::optional<T> present(const argparse::ArgumentParser& program, std::string_view name) {
    return program.present<T>(name);
}

std::optional<std::filesystem::path> pathOption(const std::optional<std::string>& value) {
    if (!value)
        return std::nullopt;
    return std::filesystem::path(*value);
}

graphics::RendererKind rendererKind(const std::string& value) {
    if (value == "preview")
        return graphics::RendererKind::preview;
    if (value == "subayai")
        return graphics::RendererKind::subayai;
    if (value == "bdpt")
        return graphics::RendererKind::bdpt;
    throw std::invalid_argument("unknown renderer: " + value);
}

core::VideoCodec videoCodec(const std::string& value) {
    if (value == "h264")
        return core::VideoCodec::h264;
    if (value == "h265" || value == "hevc")
        return core::VideoCodec::h265;
    if (value == "av1")
        return core::VideoCodec::av1;
    throw std::invalid_argument("unknown video codec: " + value);
}

std::uint32_t bitrateFromKbps(std::uint32_t kbps, std::string_view option) {
    if (kbps == 0)
        throw std::invalid_argument(std::string(option) + " expects a positive integer");
    if (kbps > std::numeric_limits<std::uint32_t>::max() / 1000U)
        throw std::invalid_argument(std::string(option) + " is too large");
    return kbps * 1000U;
}

void validateNonNegativeNumber(const std::optional<double>& value, std::string_view option) {
    if (value && (!std::isfinite(*value) || *value < 0.0))
        throw std::invalid_argument(std::string(option) + " expects a finite non-negative number");
}

void validatePositiveNumber(const std::optional<double>& value, std::string_view option) {
    if (value && (!std::isfinite(*value) || *value <= 0.0))
        throw std::invalid_argument(std::string(option) + " expects a finite positive number");
}

Options normalizeOptions(const RawCliOptions& raw) {
    Options options;
    options.probeOnly = raw.probe;
    options.hidden = raw.hidden;
    options.validation = raw.validation;
    options.renderer = rendererKind(raw.renderer);
    options.frameLimit = raw.frames;
    options.saveProject = pathOption(raw.saveProject);

    options.assets.reserve(raw.assets.size());
    for (const auto& asset : raw.assets)
        options.assets.emplace_back(asset);

    const bool hasVideoOptions = raw.videoWidth || raw.videoHeight || raw.videoFps || raw.videoCodec ||
                                 raw.videoBitrateKbps || raw.videoFromFrame || raw.videoToFrame || raw.videoHardware ||
                                 raw.noAudio;
    const bool hasAudioOptions =
        raw.audioSource || raw.audioBitrateKbps || raw.audioFrom || raw.audioTo || raw.overwrite;

    if (hasVideoOptions && !raw.exportVideo)
        throw std::invalid_argument("video options require --export-video");
    if (hasAudioOptions && !raw.exportM4a && !raw.exportVideo)
        throw std::invalid_argument("export options require --export-m4a or --export-video");
    if (raw.exportVideo && (raw.audioFrom || raw.audioTo))
        throw std::invalid_argument("--audio-from and --audio-to are only available with --export-m4a");
    if (raw.videoFromFrame && raw.videoToFrame && *raw.videoFromFrame > *raw.videoToFrame)
        throw std::invalid_argument("--video-from-frame must not exceed --video-to-frame");

    validateNonNegativeNumber(raw.audioFrom, "--audio-from");
    validateNonNegativeNumber(raw.audioTo, "--audio-to");
    validatePositiveNumber(raw.videoFps, "--video-fps");

    if (raw.frames && *raw.frames == 0)
        throw std::invalid_argument("--frames expects a positive integer");
    if (raw.videoWidth && *raw.videoWidth == 0)
        throw std::invalid_argument("--video-width expects a positive integer");
    if (raw.videoHeight && *raw.videoHeight == 0)
        throw std::invalid_argument("--video-height expects a positive integer");

    if (raw.exportVideo) {
        VideoExportOptions video;
        video.destination = pathOption(raw.exportVideo).value_or(std::filesystem::path{});
        if (raw.audioSource)
            video.audioSource = pathOption(raw.audioSource);
        if (raw.videoWidth)
            video.width = *raw.videoWidth;
        if (raw.videoHeight)
            video.height = *raw.videoHeight;
        if (raw.videoFps)
            video.fps = *raw.videoFps;
        if (raw.videoCodec)
            video.codec = videoCodec(*raw.videoCodec);
        if (raw.videoBitrateKbps)
            video.bitrate = bitrateFromKbps(*raw.videoBitrateKbps, "--video-bitrate");
        if (raw.audioBitrateKbps)
            video.audioBitrate = bitrateFromKbps(*raw.audioBitrateKbps, "--audio-bitrate");
        if (raw.videoFromFrame)
            video.fromFrame = raw.videoFromFrame;
        if (raw.videoToFrame)
            video.toFrame = raw.videoToFrame;
        video.preferHardware = raw.videoHardware;
        video.includeAudio = !raw.noAudio;
        video.overwrite = raw.overwrite;
        options.videoExport = std::move(video);
    } else if (raw.exportM4a) {
        AudioExportOptions audio;
        audio.destination = pathOption(raw.exportM4a).value_or(std::filesystem::path{});
        audio.source = pathOption(raw.audioSource);
        if (raw.audioBitrateKbps)
            audio.bitrate = bitrateFromKbps(*raw.audioBitrateKbps, "--audio-bitrate");
        audio.startSeconds = raw.audioFrom;
        audio.endSeconds = raw.audioTo;
        audio.overwrite = raw.overwrite;
        options.audioExport = std::move(audio);
    }

    return options;
}

RawCliOptions extractOptions(const argparse::ArgumentParser& program) {
    RawCliOptions raw;
    raw.probe = program.get<bool>("--probe");
    raw.hidden = program.get<bool>("--hidden");
    raw.validation = !program.get<bool>("--no-validation");
    raw.renderer = program.get<std::string>("--renderer");
    raw.frames = present<std::uint64_t>(program, "--frames");
    raw.assets = present<std::vector<std::string>>(program, "--asset").value_or(std::vector<std::string>{});
    raw.saveProject = present<std::string>(program, "--save-project");

    raw.exportM4a = present<std::string>(program, "--export-m4a");
    raw.exportVideo = present<std::string>(program, "--export-video");
    raw.audioSource = present<std::string>(program, "--audio-source");
    raw.audioBitrateKbps = present<std::uint32_t>(program, "--audio-bitrate");
    raw.audioFrom = present<double>(program, "--audio-from");
    raw.audioTo = present<double>(program, "--audio-to");

    raw.videoWidth = present<std::uint32_t>(program, "--video-width");
    raw.videoHeight = present<std::uint32_t>(program, "--video-height");
    raw.videoFps = present<double>(program, "--video-fps");
    raw.videoCodec = present<std::string>(program, "--video-codec");
    raw.videoBitrateKbps = present<std::uint32_t>(program, "--video-bitrate");
    raw.videoFromFrame = present<std::uint64_t>(program, "--video-from-frame");
    raw.videoToFrame = present<std::uint64_t>(program, "--video-to-frame");
    raw.videoHardware = program.get<bool>("--video-hardware");
    raw.noAudio = program.get<bool>("--no-audio");
    raw.overwrite = program.get<bool>("--overwrite");
    return raw;
}

void configureParser(ConfiguredParser& parser) {
    auto& program = parser.program;
    program.add_description("Native MikuMikuDance-compatible editor and renderer");

    program.add_argument("--probe").flag().help("Print renderer/device capabilities and exit");
    program.add_argument("--hidden").flag().help("Create the application window hidden");
    program.add_argument("--no-validation").flag().help("Disable Vulkan validation");
    parser.addValueArgument("--renderer")
        .default_value(std::string{"preview"})
        .choices("preview", "subayai", "bdpt")
        .metavar("RENDERER")
        .help("Select renderer");
    parser.addValueArgument("--frames").scan<'u', std::uint64_t>().metavar("N").help("Exit after rendering N frames");
    parser.addValueArgument("--asset", "--model").append().metavar("PATH").help("Load an asset (may be repeated)");
    parser.addValueArgument("--save-project").metavar("PATH").help("Save the current project and exit or continue");

    auto& exportGroup = program.add_mutually_exclusive_group();
    parser.addValueArgumentTo(exportGroup, "--export-m4a").metavar("PATH").help("Export audio to M4A");
    parser.addValueArgumentTo(exportGroup, "--export-video").metavar("PATH").help("Export video to MP4");

    parser.addValueArgument("--audio-source").metavar("PATH").help("Select an audio/video source");
    parser.addValueArgument("--audio-bitrate")
        .scan<'u', std::uint32_t>()
        .metavar("KBPS")
        .help("Set AAC bitrate in kbps");
    parser.addValueArgument("--audio-from")
        .scan<'g', double>()
        .metavar("SEC")
        .help("Start audio export at this time in seconds");
    parser.addValueArgument("--audio-to")
        .scan<'g', double>()
        .metavar("SEC")
        .help("End audio export at this time in seconds");

    parser.addValueArgument("--video-width").scan<'u', std::uint32_t>().metavar("PX").help("Set video width");
    parser.addValueArgument("--video-height").scan<'u', std::uint32_t>().metavar("PX").help("Set video height");
    parser.addValueArgument("--video-fps").scan<'g', double>().metavar("FPS").help("Set video frame rate");
    parser.addValueArgument("--video-codec")
        .choices("h264", "h265", "hevc", "av1")
        .metavar("CODEC")
        .help("Select video codec");
    parser.addValueArgument("--video-bitrate")
        .scan<'u', std::uint32_t>()
        .metavar("KBPS")
        .help("Set video bitrate in kbps");
    program.add_argument("--video-hardware").flag().help("Prefer hardware video encoding");
    parser.addValueArgument("--video-from-frame")
        .scan<'u', std::uint64_t>()
        .metavar("N")
        .help("Start video export at this frame");
    parser.addValueArgument("--video-to-frame")
        .scan<'u', std::uint64_t>()
        .metavar("N")
        .help("End video export at this frame");
    program.add_argument("--no-audio").flag().help("Disable audio in video export");
    program.add_argument("--overwrite").flag().help("Replace an existing export destination");
}

bool isValueOption(std::string_view argument, const std::vector<std::string>& valueOptions) {
    return std::find(valueOptions.begin(), valueOptions.end(), argument) != valueOptions.end();
}

PreprocessedArguments preprocessArguments(int argc, char** argv, const std::vector<std::string>& valueOptions) {
    PreprocessedArguments result;
    result.argv.emplace_back(argv[0]);
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (isValueOption(argument, valueOptions) && argument.find('=') == std::string_view::npos) {
            result.argv.emplace_back(argument);
            if (index + 1 < argc)
                result.argv.emplace_back(argv[++index]);
        } else if (!argument.empty() && argument.front() == '-') {
            result.argv.emplace_back(argument);
        } else {
            result.argv.emplace_back("--asset");
            result.argv.emplace_back(argument);
        }
    }
    return result;
}

} // namespace

Options parseOptions(int argc, char** argv) {
    ConfiguredParser parser("mikumikudesu", DAYO_VERSION);
    configureParser(parser);
    const auto arguments = preprocessArguments(argc, argv, parser.valueOptions);
    std::vector<const char*> argumentPointers;
    argumentPointers.reserve(arguments.argv.size());
    for (const auto& argument : arguments.argv)
        argumentPointers.push_back(argument.c_str());
    parser.program.parse_args(static_cast<int>(argumentPointers.size()), argumentPointers.data());
    return normalizeOptions(extractOptions(parser.program));
}

} // namespace dayo::app
