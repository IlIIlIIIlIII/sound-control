#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace personaltools {

enum class Channel { left, right };

struct PEQFilter {
    double frequencyHz = 0.0;
    double gainDB = 0.0;
    double q = 0.0;

    bool operator==(const PEQFilter&) const = default;
};

struct ParseResult {
    std::vector<PEQFilter> filters;
    std::optional<std::string> note;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

ParseResult parseREWConfigurablePEQ(std::string_view text, Channel expectedChannel);
ParseResult parseREWConfigurablePEQFile(const std::filesystem::path& path, Channel expectedChannel);
ParseResult importREWConfigurablePEQFile(const std::filesystem::path& source,
                                         const std::filesystem::path& destination,
                                         Channel expectedChannel);

}  // namespace personaltools
