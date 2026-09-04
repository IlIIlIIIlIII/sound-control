#include "DisplayTools.hpp"

#include <algorithm>
#include <array>

namespace mactools::display {
namespace {

constexpr std::array<std::byte, 8> kEDIDHeader{
    std::byte{0x00}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
    std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x00},
};

std::uint16_t little16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1]) << 8);
}

std::uint16_t big16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset]) << 8) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

std::uint32_t little32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8);
    }
    return value;
}

}  // namespace

bool validateEDID(std::span<const std::byte> bytes, std::string& error) {
    if (bytes.size() < 128 || bytes.size() > 512 || bytes.size() % 128 != 0) {
        error = "EDID length must be 128-512 bytes in complete 128-byte blocks";
        return false;
    }
    if (!std::equal(kEDIDHeader.begin(), kEDIDHeader.end(), bytes.begin())) {
        error = "EDID header is invalid";
        return false;
    }
    const auto declaredBlocks = static_cast<std::size_t>(
        std::to_integer<std::uint8_t>(bytes[126])) + 1;
    if (declaredBlocks != bytes.size() / 128) {
        error = "EDID extension count does not match its length";
        return false;
    }
    for (std::size_t block = 0; block < bytes.size() / 128; ++block) {
        unsigned sum = 0;
        for (std::size_t index = 0; index < 128; ++index) {
            sum += std::to_integer<std::uint8_t>(bytes[block * 128 + index]);
        }
        if ((sum & 0xffu) != 0u) {
            error = "EDID block checksum is invalid";
            return false;
        }
    }
    error.clear();
    return true;
}

bool isTarget32RTX950EDID(std::span<const std::byte> bytes) {
    std::string ignored;
    if (!validateEDID(bytes, ignored)) return false;
    return big16(bytes, 8) == 0x0df3 &&
           little16(bytes, 10) == 0x3150 &&
           little32(bytes, 12) == 1;
}

std::vector<std::uint32_t> makeReinitializeOrder(
    std::vector<ReinitializeDisplay> displays) {
    const auto rank = [](const ReinitializeDisplay& display) {
        if (display.vendor == kMO32U24Vendor && display.model == kMO32U24Model) {
            return 0;
        }
        if (display.vendor == k32RTX950Vendor && display.model == k32RTX950Model) {
            if (display.uuid == kDock32RTX950UUID) return 1;
            if (display.uuid == kHDMI32RTX950UUID) return 2;
            return 3;
        }
        return 4;
    };
    std::stable_sort(displays.begin(), displays.end(), [&](const auto& lhs, const auto& rhs) {
        const int lhsRank = rank(lhs);
        const int rhsRank = rank(rhs);
        if (lhsRank != rhsRank) return lhsRank < rhsRank;
        if (lhs.uuid != rhs.uuid) return lhs.uuid < rhs.uuid;
        return lhs.id < rhs.id;
    });
    std::vector<std::uint32_t> result;
    result.reserve(displays.size());
    for (const auto& display : displays) result.push_back(display.id);
    return result;
}

std::optional<SwapPlan> makeMO32U24SwapPlan(
    const std::vector<DisplayGeometry>& displays,
    std::string& error) {
    std::vector<const DisplayGeometry*> matches;
    for (const auto& display : displays) {
        if (display.online && !display.builtin && !display.mirrored &&
            display.vendor == kMO32U24Vendor && display.model == kMO32U24Model &&
            display.serial == kMO32U24Serial) {
            matches.push_back(&display);
        }
    }
    if (matches.size() != 2) {
        error = "exactly two online MO32U24 displays are required";
        return std::nullopt;
    }
    if (matches[0]->width != matches[1]->width ||
        matches[0]->height != matches[1]->height) {
        error = "the two MO32U24 displays must use the same logical size";
        return std::nullopt;
    }
    if (matches[0]->x == matches[1]->x && matches[0]->y == matches[1]->y) {
        error = "the two MO32U24 displays have the same origin";
        return std::nullopt;
    }
    const auto validRotation = [](std::int32_t rotation) {
        return rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270;
    };
    if (!validRotation(matches[0]->rotation) || !validRotation(matches[1]->rotation)) {
        error = "the two MO32U24 displays must use a supported rotation";
        return std::nullopt;
    }
    error.clear();
    return SwapPlan{
        OriginChange{matches[0]->id, matches[1]->x, matches[1]->y,
                     matches[1]->rotation},
        OriginChange{matches[1]->id, matches[0]->x, matches[0]->y,
                     matches[0]->rotation},
    };
}

}  // namespace mactools::display
