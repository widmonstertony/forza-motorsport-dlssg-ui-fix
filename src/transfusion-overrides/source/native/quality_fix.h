#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace quality_fix
{
// Registers for thin-geometry protection with tuned shadow rejection
// RenoDx Release 1.0 Validated Warp Blend: dual-track confidence boost and thin geometry recovery
inline constexpr std::string_view kRegisters = R"ptx(
.reg .pred %qv<7>;
.reg .f32 %qf<12>;
)ptx";

inline constexpr std::string_view kPolicy = R"ptx(
// QUALITY_VALID_WARP_V4
// MFGUNLOCK_VALIDATED_WARP_BLEND_V1_TUNED
cvt.rn.f32.u32 %qf0, %r10;
cvt.rn.f32.u32 %qf1, %r11;
rcp.approx.ftz.f32 %qf0, %qf0;
rcp.approx.ftz.f32 %qf1, %qf1;
mul.ftz.f32 %qf0, %qf0, 0f3F000000;
mul.ftz.f32 %qf1, %qf1, 0f3F000000;
sub.ftz.f32 %qf2, 0f3F800000, %qf0;
sub.ftz.f32 %qf3, 0f3F800000, %qf1;
setp.ge.f32 %qv0, %f123, %qf0;
setp.le.f32 %qv2, %f123, %qf2;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv2, %f124, %qf1;
and.pred %qv0, %qv0, %qv2;
setp.le.f32 %qv2, %f124, %qf3;
and.pred %qv0, %qv0, %qv2;
not.pred %qv2, %p17;
and.pred %qv0, %qv0, %qv2;
setp.ge.f32 %qv1, %f129, %qf0;
setp.le.f32 %qv2, %f129, %qf2;
and.pred %qv1, %qv1, %qv2;
setp.ge.f32 %qv2, %f130, %qf1;
and.pred %qv1, %qv1, %qv2;
setp.le.f32 %qv2, %f130, %qf3;
and.pred %qv1, %qv1, %qv2;
not.pred %qv2, %p16;
and.pred %qv1, %qv1, %qv2;
abs.f32 %qf4, %f125;
abs.f32 %qf5, %f126;
abs.f32 %qf6, %f127;
add.f32 %qf4, %qf4, %qf5;
add.f32 %qf4, %qf4, %qf6;
setp.lt.f32 %qv2, %qf4, 0f7F800000;
and.pred %qv0, %qv0, %qv2;
abs.f32 %qf5, %f131;
abs.f32 %qf6, %f132;
abs.f32 %qf7, %f133;
add.f32 %qf5, %qf5, %qf6;
add.f32 %qf5, %qf5, %qf7;
setp.lt.f32 %qv2, %qf5, 0f7F800000;
and.pred %qv1, %qv1, %qv2;

// Do not write candidate RGB. Only suppress a warped contribution when its
// projected sample is outside the source image, uses a sentinel vector, or is
// non-finite. The stock NVIDIA kernel still performs all final colour blending.
// This fixes clamped-edge contamination without creating block or stripe seams.
not.pred %qv3, %qv0;
not.pred %qv4, %qv1;
@%qv3 mov.f32 %f148, 0f00000000;
@%qv4 mov.f32 %f149, 0f00000000;
)ptx";

inline bool ReplaceOnce(std::string& text, const std::string_view from, const std::string_view to)
{
    const auto pos = text.find(from);
    if (pos == std::string::npos || text.find(from, pos + from.size()) != std::string::npos) return false;
    text.replace(pos, from.size(), to);
    return true;
}

inline bool Patch(std::string& ptx, std::string& why)
{
    std::string normalized = ptx;
    normalized.erase(std::remove(normalized.begin(), normalized.end(), '\r'), normalized.end());
    while (!normalized.empty() && normalized.back() == '\0') normalized.pop_back();
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : normalized) hash = (hash ^ c) * 1099511628211ull;
    const bool known310_9_1 =
        normalized.size() == 39638 && hash == 0x7a6f5f41105c6d85ull;
    const bool known310_9_16 =
        normalized.size() == 39833 && hash == 0x4e730b28bfb4e935ull;
    if (!known310_9_1 && !known310_9_16)
    {
        why = "quality patch skipped: unrecognized BlendCandidatesFused PTX";
        return false;
    }
    // Insert before the stock candidate-to-unwarped FMAs so only the existing
    // confidence weights change; final RGB remains entirely NVIDIA-generated.
    const std::string anchor = "sub.ftz.f32 %f166, %f139, %f115;";
    const auto site = normalized.find(anchor);
    const auto registers = normalized.find(".reg .pred %p<260>;");
    if (site == std::string::npos || registers == std::string::npos)
    {
        why = "quality patch skipped: expected insertion sites missing";
        return false;
    }

    normalized.insert(site, kPolicy);
    normalized.insert(registers, kRegisters);

    // Fast reciprocal for UI chroma average (div by 3.0f -> mul by 0.33333334f)
    ReplaceOnce(normalized,
        "mov.f32 %f193, 0f40400000;\n"
        "div.approx.ftz.f32 %f194, %f192, %f193;",
        "mul.ftz.f32 %f194, %f192, 0f3EAAAAAB;");

    // Full-resolution tensor output: bypass 8x redundant SFU divisions by 1.0f
    ReplaceOnce(normalized,
        "mov.f32 %f537, 0f3F800000;\n"
        "div.approx.ftz.f32 %f529, %f39, %f537;\n"
        "div.approx.ftz.f32 %f530, %f38, %f537;\n"
        "div.approx.ftz.f32 %f531, %f37, %f537;\n"
        "div.approx.ftz.f32 %f532, %f36, %f537;\n"
        "div.approx.ftz.f32 %f533, %f43, %f537;\n"
        "div.approx.ftz.f32 %f534, %f42, %f537;\n"
        "div.approx.ftz.f32 %f535, %f41, %f537;\n"
        "div.approx.ftz.f32 %f536, %f40, %f537;",
        "mov.f32 %f529, %f39;\n"
        "mov.f32 %f530, %f38;\n"
        "mov.f32 %f531, %f37;\n"
        "mov.f32 %f532, %f36;\n"
        "mov.f32 %f533, %f43;\n"
        "mov.f32 %f534, %f42;\n"
        "mov.f32 %f535, %f41;\n"
        "mov.f32 %f536, %f40;");

    // Downsampling Mode 2: replace 8x div by 4.0f with mul by 0.25f (0f3E800000)
    ReplaceOnce(normalized,
        "mov.f32 %f528, 0f40800000;\n"
        "div.approx.ftz.f32 %f520, %f44, %f528;\n"
        "div.approx.ftz.f32 %f521, %f45, %f528;\n"
        "div.approx.ftz.f32 %f522, %f46, %f528;\n"
        "div.approx.ftz.f32 %f523, %f47, %f528;\n"
        "div.approx.ftz.f32 %f524, %f48, %f528;\n"
        "div.approx.ftz.f32 %f525, %f49, %f528;\n"
        "div.approx.ftz.f32 %f526, %f50, %f528;\n"
        "div.approx.ftz.f32 %f527, %f51, %f528;",
        "mov.f32 %f528, 0f3E800000;\n"
        "mul.ftz.f32 %f520, %f44, %f528;\n"
        "mul.ftz.f32 %f521, %f45, %f528;\n"
        "mul.ftz.f32 %f522, %f46, %f528;\n"
        "mul.ftz.f32 %f523, %f47, %f528;\n"
        "mul.ftz.f32 %f524, %f48, %f528;\n"
        "mul.ftz.f32 %f525, %f49, %f528;\n"
        "mul.ftz.f32 %f526, %f50, %f528;\n"
        "mul.ftz.f32 %f527, %f51, %f528;");

    // Downsampling Mode 4: replace 8x div by 16.0f with mul by 0.0625f (0f3D800000)
    ReplaceOnce(normalized,
        "mov.f32 %f495, 0f41800000;\n"
        "div.approx.ftz.f32 %f487, %f52, %f495;\n"
        "div.approx.ftz.f32 %f488, %f53, %f495;\n"
        "div.approx.ftz.f32 %f489, %f54, %f495;\n"
        "div.approx.ftz.f32 %f490, %f55, %f495;\n"
        "div.approx.ftz.f32 %f491, %f56, %f495;\n"
        "div.approx.ftz.f32 %f492, %f57, %f495;\n"
        "div.approx.ftz.f32 %f493, %f58, %f495;\n"
        "div.approx.ftz.f32 %f494, %f59, %f495;",
        "mov.f32 %f495, 0f3D800000;\n"
        "mul.ftz.f32 %f487, %f52, %f495;\n"
        "mul.ftz.f32 %f488, %f53, %f495;\n"
        "mul.ftz.f32 %f489, %f54, %f495;\n"
        "mul.ftz.f32 %f490, %f55, %f495;\n"
        "mul.ftz.f32 %f491, %f56, %f495;\n"
        "mul.ftz.f32 %f492, %f57, %f495;\n"
        "mul.ftz.f32 %f493, %f58, %f495;\n"
        "mul.ftz.f32 %f494, %f59, %f495;");

    // Downsampling Mode 8: replace 8x div by 64.0f with mul by 0.015625f (0f3C800000)
    ReplaceOnce(normalized,
        "mov.f32 %f430, 0f42800000;\n"
        "div.approx.ftz.f32 %f422, %f68, %f430;\n"
        "div.approx.ftz.f32 %f423, %f69, %f430;\n"
        "div.approx.ftz.f32 %f424, %f70, %f430;\n"
        "div.approx.ftz.f32 %f425, %f71, %f430;\n"
        "div.approx.ftz.f32 %f426, %f72, %f430;\n"
        "div.approx.ftz.f32 %f427, %f73, %f430;\n"
        "div.approx.ftz.f32 %f428, %f74, %f430;\n"
        "div.approx.ftz.f32 %f429, %f75, %f430;",
        "mov.f32 %f430, 0f3C800000;\n"
        "mul.ftz.f32 %f422, %f68, %f430;\n"
        "mul.ftz.f32 %f423, %f69, %f430;\n"
        "mul.ftz.f32 %f424, %f70, %f430;\n"
        "mul.ftz.f32 %f425, %f71, %f430;\n"
        "mul.ftz.f32 %f426, %f72, %f430;\n"
        "mul.ftz.f32 %f427, %f73, %f430;\n"
        "mul.ftz.f32 %f428, %f74, %f430;\n"
        "mul.ftz.f32 %f429, %f75, %f430;");

    // Downsampling Mode 16: replace 8x div by 256.0f with mul by 0.00390625f (0f3B800000)
    ReplaceOnce(normalized,
        "mov.f32 %f333, 0f43800000;\n"
        "div.approx.ftz.f32 %f325, %f84, %f333;\n"
        "div.approx.ftz.f32 %f326, %f85, %f333;\n"
        "div.approx.ftz.f32 %f327, %f86, %f333;\n"
        "div.approx.ftz.f32 %f328, %f87, %f333;\n"
        "div.approx.ftz.f32 %f329, %f88, %f333;\n"
        "div.approx.ftz.f32 %f330, %f89, %f333;\n"
        "div.approx.ftz.f32 %f331, %f90, %f333;\n"
        "div.approx.ftz.f32 %f332, %f91, %f333;",
        "mov.f32 %f333, 0f3B800000;\n"
        "mul.ftz.f32 %f325, %f84, %f333;\n"
        "mul.ftz.f32 %f326, %f85, %f333;\n"
        "mul.ftz.f32 %f327, %f86, %f333;\n"
        "mul.ftz.f32 %f328, %f87, %f333;\n"
        "mul.ftz.f32 %f329, %f88, %f333;\n"
        "mul.ftz.f32 %f330, %f89, %f333;\n"
        "mul.ftz.f32 %f331, %f90, %f333;\n"
        "mul.ftz.f32 %f332, %f91, %f333;");

    ptx = std::move(normalized);
    why = "invalid-candidate confidence suppression and SFU reciprocal optimizations applied";
    return true;
}
}

