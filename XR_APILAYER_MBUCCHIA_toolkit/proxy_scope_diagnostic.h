// Scope decisions use persisted composition roles, never size heuristics.
#pragma once
#include "proxy_usage_diagnostic.h"

namespace toolkit::proxy_scope {
    struct Decision {
        bool hasProxy{true}, signatureMatch{false}, strict{false};
        uint32_t referenceRole{0};
        std::string reason;
    };

    inline uint32_t ParseMode(const char* text, size_t length) {
        return length == 1 && text[0] >= '0' && text[0] <= '2' ? uint32_t(text[0] - '0') : 0u;
    }

    inline proxy_usage::Snapshot Signature(const XrSwapchainCreateInfo& info) {
        return {{"width", std::to_string(info.width)}, {"height", std::to_string(info.height)},
                {"arraySize", std::to_string(info.arraySize)}, {"faceCount", std::to_string(info.faceCount)},
                {"mipCount", std::to_string(info.mipCount)}, {"sampleCount", std::to_string(info.sampleCount)},
                {"requestedFormat", std::to_string(info.format)}, {"createFlags", std::to_string(info.createFlags)},
                {"originalUsage", std::to_string(info.usageFlags)}};
    }

    inline Decision Select(const XrSwapchainCreateInfo& info, uint32_t mode,
                            const proxy_usage::Snapshot* reference, bool referenceValid) {
        Decision decision;
        const bool depth = (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        decision.hasProxy = !depth; // Safe control fallback, including direct depth.
        if (!referenceValid) { decision.reason = "reference_missing_incomplete_or_context_mismatch"; return decision; }
        if (!reference) { decision.reason = "creation_id_missing"; return decision; }
        if (info.next) { decision.reason = "uncompared_createInfo_next_chain"; return decision; }
        for (const auto& field : Signature(info)) {
            auto found = reference->find(field.first);
            if (found == reference->end() || found->second != field.second) {
                decision.reason = "signature_" + field.first;
                return decision;
            }
        }
        decision.signatureMatch = true;
        const auto role = reference->find("observedRoles");
        const auto proxy = reference->find("hasProxy");
        if (proxy == reference->end() || proxy->second != (depth ? "0" : "1")) {
            decision.reason = "reference_not_control_proxy_policy"; return decision;
        }
        if (depth) {
            // Depth is identified by original usage, not inferred from composition color roles.
            decision.strict = role != reference->end() && role->second == "0";
            if (!decision.strict) decision.reason = "depth_reference_role_mismatch";
            return decision;
        }
        if (role == reference->end()) { decision.reason = "reference_role_missing"; return decision; }
        if (role->second == "1") decision.referenceRole = 1;
        else if (role->second == "2") decision.referenceRole = 2;
        else {
            if (role->second == "3") decision.referenceRole = 3;
            decision.reason = role->second == "0" ? "color_role_unknown" : "color_role_ambiguous";
            return decision;
        }
        decision.strict = true;
        decision.hasProxy = mode == 0 || (mode == 1 && decision.referenceRole == 1) ||
                            (mode == 2 && decision.referenceRole == 2);
        return decision;
    }

    inline std::string CompareResources(const proxy_usage::Snapshot& baseline,
                                        const proxy_usage::Snapshot& current, bool hasProxy) {
        auto check = [&](const std::string& key) -> bool {
            auto before = baseline.find(key), after = current.find(key);
            return before != baseline.end() && after != current.end() && before->second == after->second;
        };
        for (const auto& key : {"imageCount", "initialLogicalState", "copyPolicy", "proxyCreation"}) {
            if (!check(key)) return key;
        }
        for (const auto& field : current) {
            if (field.first.find(".runtimeFormat") != std::string::npos && !check(field.first)) return field.first;
        }
        for (const auto& field : baseline) {
            const bool runtime = field.first.find(".runtime.") != std::string::npos;
            const bool proxy = hasProxy && field.first.find(".proxy.") != std::string::npos;
            if ((runtime || proxy) && !check(field.first)) return field.first;
        }
        // hasProxy changes deliberately. No proxy fields are compared on images made direct.
        return {};
    }
}
