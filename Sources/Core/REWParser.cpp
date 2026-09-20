#include "REWParser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace macsound {
namespace {

std::string pathText(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::string trim(std::string_view value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseDouble(const std::string& token, double& output) {
    try {
        std::size_t consumed = 0;
        output = std::stod(token, &consumed);
        return consumed == token.size() && std::isfinite(output);
    } catch (...) {
        return false;
    }
}

bool noteMatches(const std::string& note, Channel channel) {
    const auto normalized = lower(trim(note));
    if (channel == Channel::left) {
        return normalized == "l" || normalized == "left";
    }
    return normalized == "r" || normalized == "right";
}

}  // namespace

ParseResult parseREWConfigurablePEQ(std::string_view text, Channel expectedChannel) {
    ParseResult result;
    std::istringstream input{std::string(text)};
    std::string line;
    bool sawFormatMarker = false;
    std::size_t lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto cleaned = trim(line);
        if (cleaned.empty()) continue;

        const auto lowered = lower(cleaned);
        if (lowered.starts_with("notes:")) {
            const auto note = trim(cleaned.substr(cleaned.find(':') + 1));
            result.note = note;
            if (!note.empty() && !noteMatches(note, expectedChannel)) {
                result.error = "Notes channel does not match selected L/R slot";
                return result;
            }
            continue;
        }
        if (lowered == "configurable_peq") {
            sawFormatMarker = true;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(cleaned.front()))) continue;

        std::istringstream row(cleaned);
        std::string number;
        std::string enabled;
        std::string control;
        std::string type;
        if (!(row >> number >> enabled >> control >> type)) {
            result.error = "Malformed filter row at line " + std::to_string(lineNumber);
            return result;
        }
        if (lower(enabled) != "true") continue;
        if (lower(type) == "none") continue;
        if (lower(type) != "pk") {
            result.error = "Unsupported enabled filter type '" + type + "' at line " +
                           std::to_string(lineNumber);
            return result;
        }

        std::string frequencyToken;
        std::string gainToken;
        std::string qToken;
        if (!(row >> frequencyToken >> gainToken >> qToken)) {
            result.error = "Incomplete PK filter at line " + std::to_string(lineNumber);
            return result;
        }

        PEQFilter filter;
        if (!parseDouble(frequencyToken, filter.frequencyHz) ||
            !parseDouble(gainToken, filter.gainDB) ||
            !parseDouble(qToken, filter.q)) {
            result.error = "Invalid numeric value at line " + std::to_string(lineNumber);
            return result;
        }
        if (filter.frequencyHz <= 0.0) {
            result.error = "Frequency must be positive at line " + std::to_string(lineNumber);
            return result;
        }
        if (filter.q <= 0.0) {
            result.error = "Q must be positive at line " + std::to_string(lineNumber);
            return result;
        }
        if (result.filters.size() == 32) {
            result.error = "More than 32 enabled filters are not supported";
            return result;
        }
        result.filters.push_back(filter);
    }

    if (!sawFormatMarker) {
        result.error = "Missing Configurable_PEQ marker";
    } else if (result.filters.empty()) {
        result.error = "No enabled PK filters found";
    }
    return result;
}

ParseResult parseREWConfigurablePEQFile(const std::filesystem::path& path, Channel expectedChannel) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ParseResult result;
        result.error = "Unable to open " + pathText(path);
        return result;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return parseREWConfigurablePEQ(contents.str(), expectedChannel);
}

ParseResult importREWConfigurablePEQFile(const std::filesystem::path& source,
                                         const std::filesystem::path& destination,
                                         Channel expectedChannel) {
    std::ifstream file(source, std::ios::binary);
    if (!file) {
        ParseResult result;
        result.error = "Unable to open " + pathText(source);
        return result;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string text = contents.str();
    auto parsed = parseREWConfigurablePEQ(text, expectedChannel);
    if (!parsed) return parsed;

    std::error_code filesystemError;
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path(), filesystemError);
    if (filesystemError) {
        parsed.error = "Unable to create filter directory: " + filesystemError.message();
        return parsed;
    }
    auto temporary = destination;
    temporary += ".importing";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !(output << text) || !output.flush()) {
            parsed.error = "Unable to write imported filter";
            std::filesystem::remove(temporary, filesystemError);
            return parsed;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        filesystemError = std::error_code(GetLastError(), std::system_category());
#else
    std::filesystem::rename(temporary, destination, filesystemError);
#endif
    if (filesystemError) {
        parsed.error = "Unable to replace imported filter: " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
    }
    return parsed;
}

}  // namespace macsound
