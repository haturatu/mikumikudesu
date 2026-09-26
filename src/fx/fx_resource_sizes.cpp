#include "fx/fx_resource_sizes.hpp"
#include "core/image.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <ranges>
#include <stdexcept>
namespace dayo::fx {
namespace {
[[nodiscard]] std::int64_t fxSizeContextValue(std::size_t value) {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("FX frame size context exceeds signed range");
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] core::fx::FxEvalContext makeSizeEvalContext(const FxFrameContext& context) {
    core::fx::FxEvalContext result;
    result.rtWidth = context.renderWidth;
    result.rtHeight = context.renderHeight;
    result.vertexCount = fxSizeContextValue(context.vertexCount);
    result.totalMaterial = fxSizeContextValue(context.totalMaterial);
    result.modelIndex = context.modelIndex;
    result.cloneCount = context.cloneCount;
    result.clonedVertexCount = fxSizeContextValue(context.clonedVertexCount);
    result.frameIndex = static_cast<std::int64_t>(context.frame);
    result.sampleIndex = fxSizeContextValue(static_cast<std::size_t>(context.sample));
    result.time = context.time;
    result.namedSymbols = context.expressionSymbols;
    return result;
}

} // namespace

core::fx::FxExtent resolveEffectSize(const core::EffectSize& source, std::uint32_t defaultDimension,
                                     bool defaultToScreen, const FxFrameContext& context,
                                     const core::fx::FxResourceTable& table) {
    core::fx::FxSizeExpr expression;
    expression.minimumOne = !source.absolute;
    expression.base = source.absolute ? std::string{} : source.base;
    expression.dimension =
        source.dimension != 0 ? source.dimension : (!source.base.empty() && !source.absolute ? 0U : defaultDimension);
    expression.widthRatio = source.absolute ? 1.0F : source.widthRatio;
    expression.heightRatio = source.absolute ? 1.0F : source.heightRatio;
    expression.depthRatio = source.absolute ? 1.0F : source.depthRatio;
    if (source.absolute || source.rounding == "trunc")
        expression.rounding = core::fx::FxSizeExpr::Rounding::truncate;
    else if (source.rounding == "round")
        expression.rounding = core::fx::FxSizeExpr::Rounding::nearest;
    else if (source.rounding == "ceil")
        expression.rounding = core::fx::FxSizeExpr::Rounding::ceil;
    else
        throw std::invalid_argument("unsupported YRZFX size rounding mode: " + source.rounding);
    const auto conversion = [](std::string_view base, std::string_view conv) {
        if (conv == "one")
            return std::string{"1"};
        std::string result;
        const bool scalarBase = base == "VERTEXCOUNT" || base == "CLONEDVERTEXCOUNT" || base == "TOTALMATERIAL" ||
                                base == "TOTALMATERIALCOUNT";
        for (const char axis : std::array<char, 3>{'x', 'y', 'z'}) {
            const bool containsAxis = std::ranges::any_of(
                conv, [axis](unsigned char character) { return static_cast<char>(std::tolower(character)) == axis; });
            if (!containsAxis)
                continue;
            if (!result.empty())
                result += '*';
            if (scalarBase && axis == 'x')
                result += base;
            else if (scalarBase)
                result += '1';
            else
                result += std::string(base) + '.' + axis;
        }
        if (result.empty())
            throw std::invalid_argument("unsupported YRZFX size conversion: " + std::string(conv));
        return result;
    };
    if (source.absolute && source.width != 0)
        expression.xExpr = std::to_string(source.width);
    if (source.absolute && source.height != 0)
        expression.yExpr = std::to_string(source.height);
    if (source.absolute && source.depth != 0)
        expression.zExpr = std::to_string(source.depth);
    if (expression.base.empty() && expression.xExpr.empty()) {
        if (defaultToScreen) {
            expression.base = "DEFAULT_RTSIZE";
            expression.dimension = defaultDimension;
        } else {
            expression.xExpr = "1";
            if (expression.dimension >= 2)
                expression.yExpr = "1";
            if (expression.dimension >= 3)
                expression.zExpr = "1";
        }
    }
    if (!source.absolute) {
        auto base = expression.base;
        if (base.empty())
            base = defaultToScreen ? "DEFAULT_RTSIZE" : "";
        if (expression.xExpr.empty() && !base.empty())
            expression.xExpr = conversion(base, source.convX);
        if (expression.yExpr.empty() && (expression.dimension == 0 || expression.dimension >= 2) && !base.empty())
            expression.yExpr = conversion(base, source.convY);
        if (expression.zExpr.empty() && (expression.dimension == 0 || expression.dimension >= 3) && !base.empty())
            expression.zExpr = conversion(base, source.convZ);
    }
    return core::fx::FxSizeResolver{}.resolve(expression, makeSizeEvalContext(context), table);
}

FxResourceSizeTable::FxResourceSizeTable(const FxProgram& program, const FxFrameContext& context,
                                         const core::fx::FxResourceTable* physical)
    : context_(context), directory_(program.sourcePath.parent_path()), physical_(physical) {
    const auto add = [&](const auto& declaration, std::uint32_t dimension, bool screenDefault, std::string filename) {
        auto shared = declaration.shared;
        std::ranges::transform(shared, shared.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        Node node{declaration.size, dimension, screenDefault, shared == "ref", std::move(filename)};
        if (dimension == 1 && !node.size.absolute && node.size.base.empty()) {
            node.size.base = program.category == core::fx::FxCategory::deform ? "CLONEDVERTEXCOUNT" : "DEFAULT_RTSIZE";
            node.size.dimension = program.category == core::fx::FxCategory::deform ? 1U : 2U;
        }
        if (declaration.name.empty() || !nodes_.emplace(declaration.name, std::move(node)).second)
            throw std::invalid_argument("empty or duplicate FX resource name: " + declaration.name);
    };
    for (const auto& texture : program.textures)
        add(texture, 2, true, texture.filename);
    for (const auto& texture : program.textures3D)
        add(texture, 3, false, texture.filename);
    for (const auto& buffer : program.buffers)
        add(buffer, 1, false, {});
}

std::optional<core::fx::FxExtent> FxResourceSizeTable::find(std::string_view name) const {
    const std::string key(name);
    const auto node = nodes_.find(key);
    if (node == nodes_.end())
        return std::nullopt;
    if (!stack_.empty() && node->second.sharedRef)
        throw std::invalid_argument("shared=ref resource cannot be a size.base: " + key);
    if (const auto found = resolved_.find(key); found != resolved_.end())
        return found->second;
    if (std::ranges::find(stack_, key) != stack_.end() || stack_.size() >= 128) {
        std::string chain;
        for (const auto& part : stack_)
            chain += part + " -> ";
        throw std::invalid_argument("FX size dependency cycle or depth limit: " + chain + key);
    }
    stack_.push_back(key);
    try {
        auto extent = physical_ != nullptr ? physical_->find(key) : std::nullopt;
        const auto& source = node->second;
        if (!extent && !source.filename.empty() && !source.sharedRef) {
            const auto path = directory_ / source.filename;
            auto extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".dds") {
                const auto image = core::loadDdsImageRgba8(path);
                extent = core::fx::FxExtent{image.width, image.height, image.depth, source.dimension};
            } else {
                const auto image = core::loadImageRgba8(path);
                extent = core::fx::FxExtent{image.width, image.height, 1, source.dimension};
            }
        }
        if (!extent)
            extent = resolveEffectSize(source.size, source.dimension, source.screenDefault, context_, *this);
        resolved_.emplace(key, *extent);
        stack_.pop_back();
        return extent;
    } catch (const std::exception& error) {
        stack_.pop_back();
        throw std::runtime_error("FX size dependency '" + key + "': " + error.what());
    }
}
void FxResourceSizeTable::resolveAll() const {
    for (const auto& [name, node] : nodes_) {
        static_cast<void>(node);
        static_cast<void>(find(name));
    }
}
} // namespace dayo::fx
