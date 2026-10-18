#include "editor/interpolation_bezier.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {
bool check(bool result, std::string_view description) {
    if (!result)
        std::cerr << "FAIL: " << description << '\n';
    return result;
}
} // namespace

int main() {
    using dayo::editor::detail::evaluateVmdBezier;
    bool ok = true;
    // x(u) = u^3 at x=1/8 implies u=1/2, then y(u)=1/2.
    // Evaluating y(t) without inverting x would return a very different value.
    ok &= check(std::abs(evaluateVmdBezier(0.125F, {0, 0, 0, 127}) - 0.5F) < 0.00001F,
                "preview inverts Bezier X before evaluating Y");
    // With y2=0 and x2=127, using x2 instead of y2 would yield 0.875.
    ok &= check(std::abs(evaluateVmdBezier(0.5F, {0, 127, 127, 0}) - 0.5F) < 0.00001F,
                "preview uses y2 instead of x2 for the second Y control point");
    ok &= check(std::abs(evaluateVmdBezier(0.25F, {20, 20, 107, 107}) - 0.25F) < 0.00001F,
                "identical X and Y controls make a linear preview");
    ok &= check(evaluateVmdBezier(-0.1F, {20, 20, 107, 107}) == 0.0F &&
                    evaluateVmdBezier(1.1F, {20, 20, 107, 107}) == 1.0F,
                "preview clamps endpoint inputs");
    return ok ? 0 : 1;
}
