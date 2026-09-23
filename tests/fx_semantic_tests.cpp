#include "core/fx/fx_compat.hpp"
#include "core/fx/fx_expr.hpp"
#include "core/fx/fx_size.hpp"
#include "core/fx/fx_symbol.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>

namespace {

bool check(bool value, std::string_view message) {
    if (!value)
        std::cerr << "FAIL: " << message << '\n';
    return value;
}

bool expectDouble(double actual, double expected, std::string_view message, double eps = 1e-9) {
    const bool ok = std::fabs(actual - expected) <= eps;
    if (!ok)
        std::cerr << "FAIL: " << message << " actual=" << actual << " expected=" << expected << '\n';
    return ok;
}

template <typename F> bool expectThrows(const F& fn, std::string_view message) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    std::cerr << "FAIL: " << message << " (no exception)\n";
    return false;
}

} // namespace

int main() {
    using namespace dayo::core::fx;
    bool ok = true;

    FxEvalContext ctx;
    ctx.rtWidth = 1280;
    ctx.rtHeight = 720;
    ctx.vertexCount = 10000;
    ctx.totalMaterial = 8;
    ctx.modelIndex = 2;
    ctx.cloneCount = 4;
    ctx.clonedVertexCount = 40000;
    ctx.frameIndex = 30;
    ctx.sampleIndex = 2;
    ctx.time = 45.0;
    ctx.namedSymbols.emplace("ControllerGain", 2.5);

    // --- evaluation ---
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("1+2*3"), ctx)), 7.0, "arith precedence");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("(1+2)*3"), ctx)), 9.0, "parens");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("-5+10"), ctx)), 5.0, "unary minus");
    ok &= check(fxToBool(evaluateFxExpr(parseFxExpr("!false"), ctx)), "unary not");
    ok &= check(fxToBool(evaluateFxExpr(parseFxExpr("!0"), ctx)), "unary not zero");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("10%3"), ctx)), 1.0, "modulo");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("min(3,5)"), ctx)), 3.0, "min");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("max(3,5)"), ctx)), 5.0, "max");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("max(1,5,3)"), ctx)), 5.0, "max variadic");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("frac(Time/40)"), ctx)), 0.125,
                       "MikuMikuDayo material Time expression");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("pi"), ctx)), std::numbers::pi_v<double>,
                       "Expr.ixx pi constant");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("lerp(2,6,0.25)"), ctx)), 3.0, "Expr.ixx lerp");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("clamp(3,0,2)"), ctx)), 2.0, "Expr.ixx clamp");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("saturate(-1)"), ctx)), 0.0, "Expr.ixx saturate");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("frac(-0.25)"), ctx)), 0.75, "Expr.ixx frac negative");
    ok &= check(fxToBool(evaluateFxExpr(parseFxExpr("step(2,1)"), ctx)), "Expr.ixx step argument order");
    ok &= check(fxToDouble(evaluateFxExpr(parseFxExpr("hash(3)"), ctx)) >= 0.0 &&
                    fxToDouble(evaluateFxExpr(parseFxExpr("hash(3)"), ctx)) < 1.0,
                "Expr.ixx hash result range");
    ok &= check(isSupportedFxFunction("frac") && isSupportedFxFunction("noise") &&
                    !isSupportedFxFunction("notAnExprFunction"),
                "Expr.ixx function inventory");

    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("DEFAULT_RTSIZE.x"), ctx)), 1280.0, "DEFAULT_RTSIZE.x");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("DEFAULT_RTSIZE.y"), ctx)), 720.0, "DEFAULT_RTSIZE.y");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("VERTEXCOUNT"), ctx)), 10000.0, "VERTEXCOUNT");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("CLONEDVERTEXCOUNT"), ctx)), 40000.0, "CLONEDVERTEXCOUNT");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("TOTALMATERIAL"), ctx)), 8.0, "TOTALMATERIAL");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("CloneCount"), ctx)), 4.0, "CloneCount");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("DEFAULT_RTSIZE.x/2"), ctx)), 640.0, "rt half");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("VERTEXCOUNT+CLONEDVERTEXCOUNT"), ctx)), 50000.0,
                       "vertex sum");
    ok &= check(fxToBool(evaluateFxExpr(parseFxExpr("FRAME >= 30 && FRAME < 31"), ctx)),
                "comparison and logical precedence");
    ok &= check(fxToBool(evaluateFxExpr(parseFxExpr("FRAME == 30 || FRAME != 30"), ctx)), "equality and logical or");
    ok &= check(fxToBool(evaluateFxExpr(parseFxExpr("1.0 == true"), ctx)), "explicit scalar coercion comparison");
    ok &= check(!fxToBool(evaluateFxExpr(parseFxExpr("false && MISSING"), ctx)), "logical and short circuits");
    ok &= check(fxToBool(evaluateFxExpr(parseFxExpr("true || MISSING"), ctx)), "logical or short circuits");
    ok &= check(!fxToBool(evaluateFxExpr(parseFxExpr("9007199254740992 == 9007199254740993"), ctx)),
                "large integer equality stays exact");
    ok &= check(fxToBool(evaluateFxExpr(parseFxExpr("9007199254740992 != 9007199254740993"), ctx)),
                "large integer inequality stays exact");
    ok &= check(fxToBool(evaluateFxExpr(parseFxExpr("9223372036854775806 < 9223372036854775807"), ctx)),
                "near-limit integer comparison stays exact");
    {
        std::string deepUnary(65, '!');
        deepUnary += '1';
        ok &= expectThrows([&] { static_cast<void>(parseFxExpr(deepUnary)); }, "unary depth guard");
    }

    // --- dependencies ---
    ok &= check(fxDependencies(parseFxExpr("1+2")) == FxExprDependency::Static, "dep static");
    ok &= check(any(fxDependencies(parseFxExpr("DEFAULT_RTSIZE.x")), FxExprDependency::Resize), "dep resize");
    ok &= check(any(fxDependencies(parseFxExpr("VERTEXCOUNT+1")), FxExprDependency::Model), "dep model");
    ok &= check(any(fxDependencies(parseFxExpr("CLONEDVERTEXCOUNT")), FxExprDependency::Model), "dep cloned model");
    ok &= check(any(fxDependencies(parseFxExpr("TOTALMATERIAL")), FxExprDependency::Material), "dep material");
    ok &= check(any(fxDependencies(parseFxExpr("CloneCount")), FxExprDependency::Model), "dep clone");
    ok &= check(any(fxDependencies(parseFxExpr("frac(Time)")), FxExprDependency::Frame), "dep Time");
    {
        const auto mask = fxDependencies(parseFxExpr("DEFAULT_RTSIZE.x+VERTEXCOUNT"));
        ok &= check(any(mask, FxExprDependency::Resize) && any(mask, FxExprDependency::Model), "dep combined");
    }

    // --- pow profile split ---
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("pow(2,3)"), ctx, FxCompatibilityProfile::nativeExtended)),
                       8.0, "pow native");
    ok &= expectDouble(fxToDouble(evaluateFxExpr(parseFxExpr("pow(2,3)"), ctx)), 8.0, "Expr.ixx pow is supported");

    // --- symbol resolver ---
    {
        FakeFxResourceTable table;
        table.add("BaseTex", FxExtent{512, 512, 1, 2});
        const FxSymbolResolver resolver(ctx, &table);
        ok &= expectDouble(fxToDouble(resolver.resolveScalar("VERTEXCOUNT")), 10000.0, "symbol builtin");
        ok &= expectDouble(fxToDouble(resolver.resolveScalar("BaseTex")), 512.0, "symbol named scalar");
        const FxExtent base = resolver.resolveExtent("BaseTex");
        ok &= check(base.x == 512 && base.y == 512 && base.dimension == 2, "symbol named extent");
        const FxExtent rt = resolver.resolveExtent("DEFAULT_RTSIZE");
        ok &= check(rt.x == 1280 && rt.y == 720, "symbol rt extent");
        const FxExtent vc = resolver.resolveExtent("VERTEXCOUNT");
        ok &= check(vc.x == 10000 && vc.dimension == 1, "symbol vertex extent");
        ok &= expectDouble(fxToDouble(evaluateFxExprWithSymbols(parseFxExpr("BaseTex/2"), resolver)), 256.0,
                           "symbol expr eval");
        ok &= expectDouble(fxToDouble(evaluateFxExprWithSymbols(parseFxExpr("ControllerGain*2"), resolver)), 5.0,
                           "named constant-buffer/controller symbol");
        ok &= expectThrows([&] { static_cast<void>(resolver.resolveScalar("MISSING")); }, "symbol unknown throws");
    }

    // --- size resolver: ratio / rounding / inheritance / overflow ---
    {
        const FxSizeResolver resolver;
        FakeFxResourceTable table;

        // Absolute with ratio.
        FxSizeExpr half;
        half.dimension = 2;
        half.xExpr = "DEFAULT_RTSIZE.x";
        half.yExpr = "DEFAULT_RTSIZE.y";
        half.widthRatio = 0.5F;
        half.heightRatio = 0.5F;
        const FxExtent halved = resolver.resolve(half, ctx, table);
        ok &= check(halved.x == 640 && halved.y == 360 && halved.dimension == 2, "size ratio half");

        // Rounding: 640*0.33=211.2 -> nearest 211, floor 211, ceil 212.
        FxSizeExpr roundNearest;
        roundNearest.dimension = 2;
        roundNearest.xExpr = "100";
        roundNearest.yExpr = "100";
        roundNearest.widthRatio = 2.112F;
        roundNearest.heightRatio = 1.0F;
        roundNearest.rounding = FxSizeExpr::Rounding::nearest;
        ok &= check(resolver.resolve(roundNearest, ctx, table).x == 211, "size rounding nearest");

        FxSizeExpr roundFloor = roundNearest;
        roundFloor.rounding = FxSizeExpr::Rounding::floor;
        ok &= check(resolver.resolve(roundFloor, ctx, table).x == 211, "size rounding floor");

        FxSizeExpr roundCeil = roundNearest;
        roundCeil.rounding = FxSizeExpr::Rounding::ceil;
        ok &= check(resolver.resolve(roundCeil, ctx, table).x == 212, "size rounding ceil");

        // Base + dimension inheritance.
        table.add("WMap", FxExtent{64, 64, 16, 3});
        FxSizeExpr inherit;
        inherit.base = "WMap";
        inherit.xExpr = "";
        inherit.yExpr = "";
        inherit.zExpr = "";
        const FxExtent inherited = resolver.resolve(inherit, ctx, table);
        ok &= check(inherited.x == 64 && inherited.y == 64 && inherited.z == 16 && inherited.dimension == 3,
                    "size base inherit");

        // Dimension override narrows to 2D.
        FxSizeExpr narrow = inherit;
        narrow.dimension = 2;
        const FxExtent narrowed = resolver.resolve(narrow, ctx, table);
        ok &= check(narrowed.dimension == 2 && narrowed.z == 1, "size dimension override");

        // Buffer base VERTEXCOUNT.
        FxSizeExpr buffer;
        buffer.base = "VERTEXCOUNT";
        const FxExtent buf = resolver.resolve(buffer, ctx, table);
        ok &= check(buf.x == 10000 && buf.dimension == 1, "size vertex buffer");

        // Expr overriding base.
        FxSizeExpr doubled;
        doubled.base = "WMap";
        doubled.dimension = 2;
        doubled.xExpr = "64*2";
        doubled.yExpr = "64*2";
        const FxExtent dbl = resolver.resolve(doubled, ctx, table);
        ok &= check(dbl.x == 128 && dbl.y == 128, "size expr override");

        // Overflow: per-axis dimension breach.
        FxSizeExpr tooWide;
        tooWide.dimension = 2;
        tooWide.xExpr = "16385";
        tooWide.yExpr = "4";
        ok &= expectThrows([&] { static_cast<void>(resolver.resolve(tooWide, ctx, table)); },
                           "size dimension overflow throws");

        // Overflow: element count / byte guards.
        ok &= expectThrows([&] { static_cast<void>(FxSizeResolver::textureBytes(FxExtent{16384, 16384, 1, 2}, 64)); },
                           "texture bytes overflow throws");
        ok &= expectThrows([&] { static_cast<void>(FxSizeResolver::bufferBytes(FxExtent{1 << 30, 1, 1, 1}, 2)); },
                           "buffer bytes overflow throws");

        // DoS: huge literal / huge multiply must throw, never allocate.
        FxSizeExpr huge;
        huge.dimension = 2;
        huge.xExpr = "9999999999999999999999";
        huge.yExpr = "4";
        ok &= expectThrows([&] { static_cast<void>(resolver.resolve(huge, ctx, table)); }, "size huge literal throws");

        FxSizeExpr hugeMul;
        hugeMul.dimension = 1;
        hugeMul.xExpr = "99999999*99999999";
        ok &= expectThrows([&] { static_cast<void>(resolver.resolve(hugeMul, ctx, table)); },
                           "size huge multiply throws");

        FxSizeExpr hugePow;
        hugePow.dimension = 1;
        hugePow.xExpr = "pow(10,30)";
        ok &= expectThrows(
            [&] { static_cast<void>(resolver.resolve(hugePow, ctx, table, FxCompatibilityProfile::nativeExtended)); },
            "size huge pow throws");

        // Checked helpers throw on overflow.
        ok &= expectThrows([&] { static_cast<void>(fxCheckedMul(UINT64_MAX, 2)); }, "checked mul throws");
        ok &= expectThrows([&] { static_cast<void>(fxCheckedAdd(UINT64_MAX, 1)); }, "checked add throws");

        // pow is part of the upstream Expr.ixx function set.
        FxSizeExpr powExpr;
        powExpr.dimension = 1;
        powExpr.xExpr = "pow(2,10)";
        ok &= check(resolver.resolve(powExpr, ctx, table, FxCompatibilityProfile::nativeExtended).x == 1024,
                    "size pow native");
        ok &= check(resolver.resolve(powExpr, ctx, table, FxCompatibilityProfile::upstream130).x == 1024,
                    "size pow upstream");
    }

    if (!ok)
        std::cerr << "fx_semantic tests FAILED\n";
    return ok ? 0 : 1;
}
