#include "app/cli.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

dayo::app::Options parse(std::initializer_list<std::string> arguments) {
    std::vector<std::string> storage(arguments);
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& argument : storage)
        argv.push_back(argument.data());
    return dayo::app::parseOptions(static_cast<int>(argv.size()), argv.data());
}

template <typename Function> bool rejects(const Function& function, std::string_view name) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    std::cerr << "FAIL: accepted " << name << '\n';
    return false;
}

bool equalVideoExport(const dayo::app::VideoExportOptions& left, const dayo::app::VideoExportOptions& right) {
    return left.destination == right.destination && left.audioSource == right.audioSource &&
           left.width == right.width && left.height == right.height && left.fps == right.fps &&
           left.codec == right.codec && left.bitrate == right.bitrate && left.audioBitrate == right.audioBitrate &&
           left.preferHardware == right.preferHardware && left.fromFrame == right.fromFrame &&
           left.toFrame == right.toFrame && left.includeAudio == right.includeAudio &&
           left.overwrite == right.overwrite;
}

} // namespace

int main() {
    bool ok = true;

    const auto positional = parse({"mikumikudesu", "miku.pmx", "--renderer", "subayai", "motion.vmd", "--hidden"});
    ok &= positional.assets == std::vector<std::filesystem::path>{"miku.pmx", "motion.vmd"};
    ok &= positional.hidden && positional.renderer == dayo::graphics::RendererKind::subayai;

    const auto aliases = parse({"mikumikudesu", "--asset", "explicit.pmx", "--model", "motion.vmd", "pose.vpd"});
    ok &= aliases.assets == std::vector<std::filesystem::path>{"explicit.pmx", "motion.vmd", "pose.vpd"};

    const auto before = parse({"mikumikudesu", "--audio-source", "song.wav", "--audio-bitrate", "256", "--export-video",
                               "out.mp4", "--overwrite"});
    const auto after = parse({"mikumikudesu", "--export-video", "out.mp4", "--overwrite", "--audio-bitrate", "256",
                              "--audio-source", "song.wav"});
    ok &= before.videoExport && after.videoExport && equalVideoExport(*before.videoExport, *after.videoExport);
    ok &= before.videoExport && before.videoExport->audioBitrate == 256'000U;

    const auto audio = parse({"mikumikudesu", "--export-m4a", "out.m4a", "--audio-source", "song.wav", "--audio-from",
                              "1.5", "--audio-to", "2.5", "--audio-bitrate", "128"});
    ok &= audio.audioExport && audio.audioExport->destination == "out.m4a" &&
          audio.audioExport->source == std::filesystem::path{"song.wav"} && audio.audioExport->bitrate == 128'000U &&
          audio.audioExport->startSeconds == 1.5 && audio.audioExport->endSeconds == 2.5;

    const auto video = parse({"mikumikudesu", "--export-video", "out.mp4", "--video-width", "640", "--video-height",
                              "360", "--video-fps", "59.94", "--video-codec", "hevc", "--video-bitrate", "4000",
                              "--video-from-frame", "3", "--video-to-frame", "8", "--no-audio", "--video-hardware"});
    ok &= video.videoExport && video.videoExport->width == 640U && video.videoExport->height == 360U &&
          std::abs(video.videoExport->fps - 59.94) < 0.0001 &&
          video.videoExport->codec == dayo::core::VideoCodec::h265 && video.videoExport->bitrate == 4'000'000U &&
          video.videoExport->fromFrame == 3U && video.videoExport->toFrame == 8U && !video.videoExport->includeAudio &&
          video.videoExport->preferHardware;

    ok &= rejects([] { static_cast<void>(parse({"mikumikudesu", "--renderer", "invalid"})); }, "invalid renderer");
    ok &= rejects([] { static_cast<void>(parse({"mikumikudesu", "--frames", "0"})); }, "zero frames");
    ok &= rejects([] { static_cast<void>(parse({"mikumikudesu", "--video-width", "0"})); }, "zero video width");
    ok &=
        rejects([] { static_cast<void>(parse({"mikumikudesu", "--export-m4a", "a.m4a", "--export-video", "b.mp4"})); },
                "mutually exclusive exports");
    ok &= rejects([] { static_cast<void>(parse({"mikumikudesu", "--export-video", "out.mp4", "--audio-from", "1"})); },
                  "audio range on video export");
    ok &=
        rejects([] { static_cast<void>(parse({"mikumikudesu", "--video-from-frame", "8", "--video-to-frame", "3"})); },
                "reversed video range");

    if (!ok) {
        std::cerr << "CLI tests failed\n";
        return 1;
    }
    std::cout << "CLI tests passed\n";
    return 0;
}
