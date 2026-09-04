#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mactools::display {

constexpr std::uint32_t kMO32U24Vendor = 0x1c54;
constexpr std::uint32_t kMO32U24Model = 0x3219;
constexpr std::uint32_t kMO32U24Serial = 0x01010101;
constexpr std::uint32_t k32RTX950Vendor = 0x0df3;
constexpr std::uint32_t k32RTX950Model = 0x3150;
constexpr auto kDock32RTX950UUID = "BB7B95DD-B9FB-4A24-8F6D-ADA79BB60017";
constexpr auto kHDMI32RTX950UUID = "9835F42D-CD9F-4F0E-AE6A-5C260B978E76";

struct ReinitializeDisplay {
    std::uint32_t id{};
    std::uint32_t vendor{};
    std::uint32_t model{};
    std::string uuid;
};

struct DisplayGeometry {
    std::uint32_t id{};
    std::uint32_t vendor{};
    std::uint32_t model{};
    std::uint32_t serial{};
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::int32_t rotation{};
    bool builtin{};
    bool online{};
    bool mirrored{};
};

struct OriginChange {
    std::uint32_t id{};
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t rotation{};
};

struct SwapPlan {
    OriginChange first;
    OriginChange second;
};

bool validateEDID(std::span<const std::byte> bytes, std::string& error);
bool isTarget32RTX950EDID(std::span<const std::byte> bytes);
std::vector<std::uint32_t> makeReinitializeOrder(
    std::vector<ReinitializeDisplay> displays);
std::optional<SwapPlan> makeMO32U24SwapPlan(
    const std::vector<DisplayGeometry>& displays,
    std::string& error);

}  // namespace mactools::display
