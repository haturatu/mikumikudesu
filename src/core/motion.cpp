#include "core/motion.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iconv.h>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace dayo::core {
namespace {

template <typename T>
T read(std::istream& input, std::string_view field) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value {};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) throw std::runtime_error("truncated VMD while reading " + std::string(field));
    return value;
}

template <std::size_t N>
std::array<float, N> readFloatArray(std::istream& input, std::string_view field) {
    std::array<float, N> value {};
    input.read(reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(sizeof(value)));
    if (!input) throw std::runtime_error("truncated VMD while reading " + std::string(field));
    return value;
}

template <std::size_t N>
std::string readName(std::istream& input, std::string_view field) {
    std::array<char, N> value {};
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    if (!input) throw std::runtime_error("truncated VMD while reading " + std::string(field));
    const auto end = std::find(value.begin(), value.end(), '\0');
    return decodeCp932(std::string_view(value.data(), static_cast<std::size_t>(end - value.begin())));
}

std::uint32_t readCount(std::istream& input, std::string_view field, std::uint32_t maximum = 100'000'000U) {
    const auto value = read<std::uint32_t>(input, field);
    if (value > maximum) throw std::runtime_error("invalid VMD " + std::string(field));
    return value;
}

void updateLastFrame(VmdMotion& motion, std::uint32_t frame) { motion.lastFrame = std::max(motion.lastFrame, frame); }

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

float parseFloat(std::string_view input, std::string_view field) {
    float value {};
    const auto begin = input.data();
    const auto [end, error] = std::from_chars(begin, begin + input.size(), value);
    if (error != std::errc {} || end != begin + input.size()) {
        throw std::runtime_error("invalid VPD " + std::string(field));
    }
    return value;
}

float cameraBezier(float x, const std::array<std::uint8_t, 24>& values, std::size_t offset) {
    const float x1 = static_cast<float>(values[offset]) / 127.0F;
    const float x2 = static_cast<float>(values[offset + 1]) / 127.0F;
    const float y1 = static_cast<float>(values[offset + 2]) / 127.0F;
    const float y2 = static_cast<float>(values[offset + 3]) / 127.0F;
    float low = 0.0F;
    float high = 1.0F;
    for (int iteration = 0; iteration < 16; ++iteration) {
        const float value = (low + high) * 0.5F;
        const float inverse = 1.0F - value;
        const float curveX = 3.0F * inverse * inverse * value * x1
                           + 3.0F * inverse * value * value * x2 + value * value * value;
        if (curveX < x) low = value; else high = value;
    }
    const float value = (low + high) * 0.5F;
    const float inverse = 1.0F - value;
    return 3.0F * inverse * inverse * value * y1
         + 3.0F * inverse * value * value * y2 + value * value * value;
}

template <std::size_t N>
std::array<float, N> parseVector(std::string value, std::string_view field) {
    if (!value.empty() && value.back() == ';') value.pop_back();
    std::array<float, N> result {};
    std::size_t begin = 0;
    for (std::size_t i = 0; i < N; ++i) {
        const auto end = value.find(',', begin);
        const auto part = trim(value.substr(begin, end == std::string::npos ? end : end - begin));
        result[i] = parseFloat(part, field);
        if (i + 1 < N && end == std::string::npos) throw std::runtime_error("short VPD vector");
        begin = end == std::string::npos ? value.size() : end + 1;
    }
    return result;
}

} // namespace

float catmullRom(float p0, float p1, float p2, float p3, float t) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5F * ((2.0F * p1) + (-p0 + p2) * t
        + (2.0F * p0 - 5.0F * p1 + 4.0F * p2 - p3) * t2
        + (-p0 + 3.0F * p1 - 3.0F * p2 + p3) * t3);
}

std::string decodeCp932(std::string_view input) {
    if (input.empty()) return {};
    iconv_t converter = iconv_open("UTF-8", "CP932");
    if (converter == reinterpret_cast<iconv_t>(-1)) throw std::runtime_error("CP932 converter is unavailable");
    std::string output(input.size() * 4 + 4, '\0');
    char* source = const_cast<char*>(input.data());
    std::size_t sourceLeft = input.size();
    char* destination = output.data();
    std::size_t destinationLeft = output.size();
    while (sourceLeft != 0) {
        if (iconv(converter, &source, &sourceLeft, &destination, &destinationLeft) != static_cast<std::size_t>(-1)) continue;
        if (errno == EILSEQ || errno == EINVAL) {
            ++source;
            --sourceLeft;
            constexpr std::string_view replacement = "\xEF\xBF\xBD";
            if (destinationLeft < replacement.size()) break;
            std::memcpy(destination, replacement.data(), replacement.size());
            destination += replacement.size();
            destinationLeft -= replacement.size();
            continue;
        }
        iconv_close(converter);
        throw std::runtime_error("CP932 conversion failed");
    }
    iconv_close(converter);
    output.resize(static_cast<std::size_t>(destination - output.data()));
    return output;
}

VmdMotion loadVmd(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open VMD file: " + path.string());
    std::array<char, 30> header {};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    constexpr std::string_view signature = "Vocaloid Motion Data 0002";
    if (!input || std::string_view(header.data(), signature.size()) != signature) {
        throw std::runtime_error("unsupported VMD file: " + path.string());
    }
    VmdMotion motion;
    motion.modelName = readName<20>(input, "model name");
    motion.bones.resize(readCount(input, "bone key count"));
    for (auto& key : motion.bones) {
        key.name = readName<15>(input, "bone name");
        key.frame = read<std::uint32_t>(input, "bone frame");
        key.translation = readFloatArray<3>(input, "bone translation");
        key.rotation = readFloatArray<4>(input, "bone rotation");
        input.read(reinterpret_cast<char*>(key.interpolation.data()), static_cast<std::streamsize>(key.interpolation.size()));
        if (!input) throw std::runtime_error("truncated VMD bone interpolation");
        updateLastFrame(motion, key.frame);
    }
    motion.morphs.resize(readCount(input, "morph key count"));
    for (auto& key : motion.morphs) {
        key.name = readName<15>(input, "morph name");
        key.frame = read<std::uint32_t>(input, "morph frame");
        key.weight = read<float>(input, "morph weight");
        updateLastFrame(motion, key.frame);
    }
    if (input.peek() == std::char_traits<char>::eof()) return motion;
    motion.cameras.resize(readCount(input, "camera key count"));
    for (auto& key : motion.cameras) {
        key.frame = read<std::uint32_t>(input, "camera frame");
        key.distance = read<float>(input, "camera distance");
        key.position = readFloatArray<3>(input, "camera position");
        key.rotation = readFloatArray<3>(input, "camera rotation");
        input.read(reinterpret_cast<char*>(key.interpolation.data()), static_cast<std::streamsize>(key.interpolation.size()));
        if (!input) throw std::runtime_error("truncated VMD camera interpolation");
        key.viewAngle = read<std::uint32_t>(input, "camera view angle");
        key.perspective = read<std::uint8_t>(input, "camera perspective") == 0;
        updateLastFrame(motion, key.frame);
    }
    if (input.peek() == std::char_traits<char>::eof()) return motion;
    motion.lights.resize(readCount(input, "light key count"));
    for (auto& key : motion.lights) {
        key.frame = read<std::uint32_t>(input, "light frame");
        key.color = readFloatArray<3>(input, "light color");
        key.position = readFloatArray<3>(input, "light position");
        updateLastFrame(motion, key.frame);
    }
    if (input.peek() == std::char_traits<char>::eof()) return motion;
    motion.shadows.resize(readCount(input, "shadow key count"));
    for (auto& key : motion.shadows) {
        key.frame = read<std::uint32_t>(input, "shadow frame");
        key.mode = read<std::uint8_t>(input, "shadow mode");
        key.distance = read<float>(input, "shadow distance");
        updateLastFrame(motion, key.frame);
    }
    if (input.peek() == std::char_traits<char>::eof()) return motion;
    motion.ik.resize(readCount(input, "IK key count"));
    for (auto& key : motion.ik) {
        key.frame = read<std::uint32_t>(input, "IK frame");
        key.visible = read<std::uint8_t>(input, "model visibility") != 0;
        key.states.resize(readCount(input, "IK state count", 1'000'000));
        for (auto& state : key.states) {
            state.name = readName<20>(input, "IK bone name");
            state.enabled = read<std::uint8_t>(input, "IK state") != 0;
        }
        updateLastFrame(motion, key.frame);
    }
    std::stable_sort(motion.ik.begin(), motion.ik.end(),
                     [](const VmdIkKey& left, const VmdIkKey& right) { return left.frame < right.frame; });
    return motion;
}

VpdPose loadVpd(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open VPD file: " + path.string());
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto text = decodeCp932(bytes);
    if (text.find("Vocaloid Pose Data file") == std::string::npos) throw std::runtime_error("unsupported VPD file");
    VpdPose pose;
    std::size_t cursor = 0;
    while ((cursor = text.find("Bone", cursor)) != std::string::npos) {
        const auto open = text.find('{', cursor);
        const auto nameEnd = open == std::string::npos ? open : text.find('{', open + 1);
        const auto close = nameEnd == std::string::npos ? nameEnd : text.find('}', nameEnd + 1);
        if (open == std::string::npos || nameEnd == std::string::npos || close == std::string::npos) break;
        VpdBonePose bone;
        bone.name = trim(text.substr(open + 1, nameEnd - open - 1));
        const auto translationEnd = text.find('\n', nameEnd + 1);
        const auto rotationEnd = translationEnd == std::string::npos ? translationEnd : text.find('\n', translationEnd + 1);
        if (translationEnd == std::string::npos || rotationEnd == std::string::npos) break;
        bone.translation = parseVector<3>(trim(text.substr(nameEnd + 1, translationEnd - nameEnd - 1)), "translation");
        bone.rotation = parseVector<4>(trim(text.substr(translationEnd + 1, rotationEnd - translationEnd - 1)), "rotation");
        pose.bones.push_back(std::move(bone));
        cursor = close + 1;
    }
    return pose;
}

MotionDocument toMotionDocument(const VmdMotion& motion) {
    return { motion.modelName, motion.interpolation, motion.bones, motion.morphs,
             motion.cameras, motion.lights, motion.shadows, motion.ik };
}

VmdMotion toVmdMotion(MotionDocument document, std::string modelName) {
    VmdMotion result;
    result.modelName = modelName.empty() ? std::move(document.modelName) : std::move(modelName);
    result.interpolation = document.interpolation;
    result.bones = std::move(document.bones);
    result.morphs = std::move(document.morphs);
    result.cameras = std::move(document.cameras);
    result.lights = std::move(document.lights);
    result.shadows = std::move(document.shadows);
    result.ik = std::move(document.ik);
    std::stable_sort(result.ik.begin(), result.ik.end(),
                     [](const VmdIkKey& left, const VmdIkKey& right) { return left.frame < right.frame; });
    for (const auto& key : result.bones) result.lastFrame = std::max(result.lastFrame, key.frame);
    for (const auto& key : result.morphs) result.lastFrame = std::max(result.lastFrame, key.frame);
    for (const auto& key : result.cameras) result.lastFrame = std::max(result.lastFrame, key.frame);
    for (const auto& key : result.lights) result.lastFrame = std::max(result.lastFrame, key.frame);
    for (const auto& key : result.shadows) result.lastFrame = std::max(result.lastFrame, key.frame);
    for (const auto& key : result.ik) result.lastFrame = std::max(result.lastFrame, key.frame);
    return result;
}

VmdCameraState evaluateCamera(const VmdMotion& motion, float frame) {
    VmdCameraState result;
    if (motion.cameras.empty()) return result;
    const VmdCameraKey* previous = &motion.cameras.front();
    const VmdCameraKey* next = previous;
    for (const auto& key : motion.cameras) {
        if (static_cast<float>(key.frame) <= frame
            && (static_cast<float>(previous->frame) > frame || key.frame >= previous->frame)) previous = &key;
        if (static_cast<float>(key.frame) >= frame
            && (static_cast<float>(next->frame) < frame || key.frame <= next->frame)) next = &key;
    }
    if (static_cast<float>(previous->frame) > frame) previous = next;
    if (static_cast<float>(next->frame) < frame) next = previous;
    const auto span = static_cast<float>(next->frame) - static_cast<float>(previous->frame);
    const float t = span > 1.0F ? std::clamp((frame - static_cast<float>(previous->frame)) / span, 0.0F, 1.0F) : 0.0F;
    const auto previousIndex = static_cast<std::size_t>(previous - motion.cameras.data());
    const auto nextIndex = static_cast<std::size_t>(next - motion.cameras.data());
    const auto& before = motion.cameras[previousIndex == 0 ? previousIndex : previousIndex - 1];
    const auto& after = motion.cameras[std::min(nextIndex + 1, motion.cameras.size() - 1)];
    const float interpolation = motion.interpolation == InterpolationMode::linear
        ? t : cameraBezier(t, next->interpolation, 16);
    result.distance = motion.interpolation == InterpolationMode::catmullRom
        ? catmullRom(before.distance, previous->distance, next->distance, after.distance, t)
        : previous->distance + (next->distance - previous->distance) * interpolation;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float axisInterpolation = motion.interpolation == InterpolationMode::linear
            ? t : cameraBezier(t, next->interpolation, axis * 4);
        result.position[axis] = motion.interpolation == InterpolationMode::catmullRom
            ? catmullRom(before.position[axis], previous->position[axis], next->position[axis], after.position[axis], t)
            : previous->position[axis] + (next->position[axis] - previous->position[axis]) * axisInterpolation;
        result.rotation[axis] = previous->rotation[axis]
                              + (next->rotation[axis] - previous->rotation[axis])
                              * (motion.interpolation == InterpolationMode::linear ? t
                                 : cameraBezier(t, next->interpolation, 12));
    }
    result.viewAngle = static_cast<float>(previous->viewAngle)
                     + (static_cast<float>(next->viewAngle) - static_cast<float>(previous->viewAngle))
                     * cameraBezier(t, next->interpolation, 20);
    result.perspective = previous->perspective;
    return result;
}

VmdLightKey evaluateLight(const VmdMotion& motion, float frame) {
    VmdLightKey result;
    result.color = { 0.6F, 0.6F, 0.6F };
    result.position = { -0.5F, -1.0F, 0.5F };
    if (motion.lights.empty()) return result;
    const VmdLightKey* previous = &motion.lights.front();
    const VmdLightKey* next = previous;
    for (const auto& key : motion.lights) {
        if (static_cast<float>(key.frame) <= frame
            && (static_cast<float>(previous->frame) > frame || key.frame >= previous->frame)) previous = &key;
        if (static_cast<float>(key.frame) >= frame
            && (static_cast<float>(next->frame) < frame || key.frame <= next->frame)) next = &key;
    }
    if (static_cast<float>(previous->frame) > frame) previous = next;
    if (static_cast<float>(next->frame) < frame) next = previous;
    const auto span = static_cast<float>(next->frame) - static_cast<float>(previous->frame);
    const float t = span > 0.0F ? std::clamp((frame - static_cast<float>(previous->frame)) / span, 0.0F, 1.0F) : 0.0F;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        result.color[axis] = previous->color[axis] + (next->color[axis] - previous->color[axis]) * t;
        result.position[axis] = previous->position[axis] + (next->position[axis] - previous->position[axis]) * t;
    }
    result.frame = static_cast<std::uint32_t>(std::max(frame, 0.0F));
    return result;
}

} // namespace dayo::core
