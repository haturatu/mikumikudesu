#include "fx/fx_shader_compiler.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace dayo::fx {
namespace {

std::atomic<std::uint64_t> nextTemporaryId{1};

[[nodiscard]] std::string quoteShellArgument(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char character : value) {
        if (character == '\'')
            quoted += "'\"'\"'";
        else
            quoted.push_back(character);
    }
    quoted.push_back('\'');
    return quoted;
}

[[nodiscard]] std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::filesystem::path makeTemporaryDirectory() {
    std::error_code error;
    const auto root = std::filesystem::temp_directory_path(error);
    if (error)
        throw std::runtime_error("cannot locate temporary directory: " + error.message());
    const auto path = root / ("mikumikudesu-fx-" + std::to_string(nextTemporaryId.fetch_add(1)));
    std::filesystem::create_directory(path, error);
    if (error)
        throw std::runtime_error("cannot create shader compile directory: " + error.message());
    return path;
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() : path_(makeTemporaryDirectory()) {}
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] bool isDxc(const std::filesystem::path& executable) {
    const auto name = executable.filename().string();
    return name == "dxc" || name == "dxc.exe";
}

[[nodiscard]] bool commandAvailable(const std::filesystem::path& executable) noexcept {
    const std::string command = "command -v " + quoteShellArgument(executable.string()) + " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

[[nodiscard]] std::string compilerVersion(const std::filesystem::path& executable,
                                          const std::filesystem::path& logPath) {
    const std::string command =
        quoteShellArgument(executable.string()) + " --version > " + quoteShellArgument(logPath.string()) + " 2>&1";
    if (std::system(command.c_str()) != 0)
        return "unknown";
    std::string version = readTextFile(logPath);
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r'))
        version.pop_back();
    if (version.empty())
        return "unknown";
    return version;
}

[[nodiscard]] std::string dxcCommand(const std::filesystem::path& executable, const FxShaderCompileRequest& request,
                                     const std::filesystem::path& input, const std::filesystem::path& output,
                                     const std::filesystem::path& diagnostics) {
    std::ostringstream command;
    command << quoteShellArgument(executable.string())
            << " -spirv -fspv-target-env=" << quoteShellArgument(request.targetEnvironment) << " -fvk-use-dx-layout -E "
            << quoteShellArgument(request.entryPoint) << " -T " << FxShaderCompiler::profile(request.stage) << " -Fo "
            << quoteShellArgument(output.string());
    for (const auto& macro : request.macros)
        command << " -D" << quoteShellArgument(macro);
    for (const auto& include : request.includeDirectories)
        command << " -I " << quoteShellArgument(include.string());
    command << ' ' << quoteShellArgument(input.string()) << " > " << quoteShellArgument(diagnostics.string())
            << " 2>&1";
    return command.str();
}

[[nodiscard]] std::string glslcCommand(const std::filesystem::path& executable, const FxShaderCompileRequest& request,
                                       const std::filesystem::path& input, const std::filesystem::path& output,
                                       const std::filesystem::path& diagnostics) {
    std::ostringstream command;
    command << quoteShellArgument(executable.string())
            << " -x hlsl --target-env=" << quoteShellArgument(request.targetEnvironment)
            // glslc's HLSL frontend otherwise gives t/u/s/b register class
            // zero the same Vulkan binding. Keep each class in a disjoint
            // range so the generated descriptor layout remains valid.
            << " -fhlsl-iomap -fpreserve-bindings"
            << " -fuav-binding-base " << FxShaderCompiler::glslcStage(request.stage) << " 0"
            << " -ftexture-binding-base " << FxShaderCompiler::glslcStage(request.stage) << " 16"
            << " -fsampler-binding-base " << FxShaderCompiler::glslcStage(request.stage) << " 32"
            << " -fubo-binding-base " << FxShaderCompiler::glslcStage(request.stage) << " 48"
            << " -fshader-stage=" << FxShaderCompiler::glslcStage(request.stage)
            << " -fentry-point=" << quoteShellArgument(request.entryPoint) << " -o "
            << quoteShellArgument(output.string());
    for (const auto& macro : request.macros)
        command << " -D" << quoteShellArgument(macro);
    for (const auto& include : request.includeDirectories)
        command << " -I " << quoteShellArgument(include.string());
    command << ' ' << quoteShellArgument(input.string()) << " > " << quoteShellArgument(diagnostics.string())
            << " 2>&1";
    return command.str();
}

[[nodiscard]] std::vector<std::uint32_t> readSpirv(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("shader compiler did not produce SPIR-V");
    const auto end = input.tellg();
    if (end <= 0)
        throw std::runtime_error("shader compiler produced an empty SPIR-V module");
    const auto byteCount = static_cast<std::uintmax_t>(end);
    if (byteCount % sizeof(std::uint32_t) != 0 ||
        byteCount / sizeof(std::uint32_t) > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("shader compiler produced an invalid SPIR-V size");
    std::vector<std::uint32_t> words(static_cast<std::size_t>(byteCount / sizeof(std::uint32_t)));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(byteCount));
    if (!input)
        throw std::runtime_error("cannot read generated SPIR-V module");
    if (words.empty() || words.front() != 0x07230203U)
        throw std::runtime_error("shader compiler produced a non-SPIR-V module");
    return words;
}

} // namespace

FxShaderCompiler::FxShaderCompiler(std::filesystem::path executable) {
    if (executable.empty()) {
        if (const char* selected = std::getenv("DAYO_FX_COMPILER"); selected != nullptr && *selected != '\0')
            executable = selected;
        else if (commandAvailable("dxc"))
            executable = "dxc";
        else
            executable = "glslc";
    }
    executable_ = std::move(executable);
}

bool FxShaderCompiler::available() const noexcept {
    return commandAvailable(executable_);
}

std::string_view FxShaderCompiler::profile(FxShaderStage stage) noexcept {
    switch (stage) {
    case FxShaderStage::vertex:
        return "vs_6_6";
    case FxShaderStage::fragment:
        return "ps_6_6";
    case FxShaderStage::compute:
        return "cs_6_6";
    case FxShaderStage::rayGeneration:
    case FxShaderStage::miss:
    case FxShaderStage::closestHit:
    case FxShaderStage::anyHit:
    case FxShaderStage::intersection:
    case FxShaderStage::callable:
        return "lib_6_6";
    }
    return "cs_6_6";
}

std::string_view FxShaderCompiler::glslcStage(FxShaderStage stage) noexcept {
    switch (stage) {
    case FxShaderStage::vertex:
        return "vert";
    case FxShaderStage::fragment:
        return "frag";
    case FxShaderStage::compute:
        return "compute";
    case FxShaderStage::rayGeneration:
        return "rgen";
    case FxShaderStage::miss:
        return "rmiss";
    case FxShaderStage::closestHit:
        return "rchit";
    case FxShaderStage::anyHit:
        return "rahit";
    case FxShaderStage::intersection:
        return "rint";
    case FxShaderStage::callable:
        return "rcall";
    }
    return "compute";
}

FxShaderArtifact FxShaderCompiler::compile(const FxShaderCompileRequest& request) const {
    if (request.hlsl.empty())
        throw std::invalid_argument("cannot compile an empty HLSL source");
    if (request.entryPoint.empty())
        throw std::invalid_argument("shader entry point must be non-empty");
    if (!available())
        throw std::runtime_error("shader compiler is unavailable: " + executable_.string());

    TemporaryDirectory temporary;
    const auto input = temporary.path() / "effect.hlsl";
    const auto output = temporary.path() / "effect.spv";
    const auto diagnostics = temporary.path() / "compiler.log";
    {
        std::ofstream file(input, std::ios::binary);
        if (!file)
            throw std::runtime_error("cannot write temporary HLSL source");
        file.write(request.hlsl.data(), static_cast<std::streamsize>(request.hlsl.size()));
        if (!file)
            throw std::runtime_error("cannot write temporary HLSL source");
    }

    const std::string command = isDxc(executable_) ? dxcCommand(executable_, request, input, output, diagnostics)
                                                   : glslcCommand(executable_, request, input, output, diagnostics);
    if (std::system(command.c_str()) != 0) {
        const std::string details = readTextFile(diagnostics);
        throw std::runtime_error("HLSL to SPIR-V compilation failed for " +
                                 (request.sourcePath.empty() ? input.string() : request.sourcePath.string()) +
                                 (details.empty() ? std::string{} : ":\n" + details));
    }

    FxShaderArtifact artifact;
    artifact.spirv = readSpirv(output);
    artifact.compiler = executable_.string();
    artifact.compilerVersion = compilerVersion(executable_, diagnostics);
    return artifact;
}

} // namespace dayo::fx
