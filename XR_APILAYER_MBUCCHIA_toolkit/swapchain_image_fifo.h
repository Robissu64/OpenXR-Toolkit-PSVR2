#pragma once

#include <openxr/openxr.h>

#include <deque>
#include <optional>

namespace toolkit {
    // The runtime releases acquisitions in FIFO order. The application's latest release,
    // rather than its latest acquire, identifies the image used in composition layers.
    class SwapchainImageFifo {
      public:
        struct Acquisition {
            uint32_t index;
            bool waited{false};
            bool releaseRequested{false};
        };

        void acquired(XrResult result, uint32_t index) {
            if (completed(result)) {
                m_acquisitions.push_back({index});
            }
        }

        void waited(XrResult result) {
            // XR_TIMEOUT_EXPIRED is nonnegative, but does not complete a wait.
            if (completed(result)) {
                for (auto& acquisition : m_acquisitions) {
                    if (!acquisition.waited) {
                        acquisition.waited = true;
                        break;
                    }
                }
            }
        }

        bool requestRelease() {
            for (auto& acquisition : m_acquisitions) {
                if (!acquisition.releaseRequested) {
                    if (!acquisition.waited) {
                        return false;
                    }
                    acquisition.releaseRequested = true;
                    m_submittedImageIndex = acquisition.index;
                    return true;
                }
            }
            return false;
        }

        std::optional<uint32_t> getReleaseImageIndex() const {
            if (m_acquisitions.empty() || !m_acquisitions.front().waited ||
                !m_acquisitions.front().releaseRequested) {
                return std::nullopt;
            }
            return m_acquisitions.front().index;
        }

        void released(XrResult result) {
            if (completed(result) && getReleaseImageIndex()) {
                m_acquisitions.pop_front();
            }
        }

        std::optional<uint32_t> getSubmittedImageIndex() const {
            return m_submittedImageIndex;
        }

        const std::deque<Acquisition>& acquisitions() const {
            return m_acquisitions;
        }

      private:
        static bool completed(XrResult result) {
            return result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING;
        }

        std::deque<Acquisition> m_acquisitions;
        std::optional<uint32_t> m_submittedImageIndex;
    };
}
