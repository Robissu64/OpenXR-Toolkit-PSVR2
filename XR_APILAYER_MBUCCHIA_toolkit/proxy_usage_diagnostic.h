// Diagnostic-only policy and portable reference comparison. No GPU commands here.
#pragma once

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <string>

namespace toolkit::proxy_usage {
    inline uint32_t ParseMode(const char* text, size_t length) {
        return length == 1 && text[0] == '1' ? 1u : 0u;
    }

    inline XrSwapchainCreateInfo DownstreamCreateInfo(const XrSwapchainCreateInfo& original,
                                                     uint32_t mode, bool hasProxy) {
        auto downstream = original;
        if (mode == 1 && hasProxy &&
            !(original.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            downstream.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        }
        return downstream;
    }

    using Snapshot = std::map<std::string, std::string>;
    struct Reference {
        std::string identity;
        bool complete{false};
        std::map<uint64_t, Snapshot> swapchains;
    };

    // Named fields avoid struct padding, pointers and Windows CRLF influencing equality.
    inline std::string Compare(const Snapshot& baseline, const Snapshot& current) {
        for (const auto& field : current) {
            auto found = baseline.find(field.first);
            if (found == baseline.end() || found->second != field.second) {
                return field.first;
            }
        }
        for (const auto& field : baseline) {
            if (!current.count(field.first)) {
                return field.first;
            }
        }
        return {};
    }

    inline bool WriteReference(std::ostream& output, const Reference& reference) {
        output << "PROXY_USAGE_V1\n" << std::quoted(reference.identity) << '\n'
               << reference.complete << ' ' << reference.swapchains.size() << '\n';
        for (const auto& entry : reference.swapchains) {
            output << entry.first << ' ' << entry.second.size() << '\n';
            for (const auto& field : entry.second) {
                output << std::quoted(field.first) << ' ' << std::quoted(field.second) << '\n';
            }
        }
        output.flush();
        return bool(output);
    }

    inline bool ReadReference(std::istream& input, Reference& reference) {
        Reference parsed;
        std::string version;
        size_t count = 0;
        if (!std::getline(input, version) || version != "PROXY_USAGE_V1" ||
            !(input >> std::quoted(parsed.identity) >> parsed.complete >> count) || count > 4096) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            uint64_t id;
            size_t fields;
            if (!(input >> id >> fields) || fields > 65536 || parsed.swapchains.count(id)) {
                return false;
            }
            Snapshot snapshot;
            for (size_t j = 0; j < fields; ++j) {
                std::string key, value;
                if (!(input >> std::quoted(key) >> std::quoted(value)) || snapshot.count(key)) {
                    return false;
                }
                snapshot.emplace(key, value);
            }
            parsed.swapchains.emplace(id, std::move(snapshot));
        }
        input >> std::ws;
        if (!input.eof()) {
            return false;
        }
        reference = std::move(parsed);
        return true;
    }
}
