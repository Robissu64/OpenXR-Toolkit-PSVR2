#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace toolkit::proxy_index {
    struct Entry {
        uint32_t index;
        bool acquired{true};
        bool successfullyWaited{false};
    };

    struct Tracker {
        std::deque<Entry> queue;
        std::string error;
        uint32_t lastAcquiredIndex{UINT32_MAX};

        bool invalidate(const char* reason) {
            if (error.empty()) error = reason;
            return false;
        }

        bool acquire(uint32_t index, uint32_t imageCount) {
            if (index >= imageCount) return invalidate("acquire_index_out_of_range");
            for (const auto& entry : queue) {
                if (entry.acquired && entry.index == index) return invalidate("duplicate_acquired_index");
            }
            queue.push_back({index, true, false});
            lastAcquiredIndex = index;
            return true;
        }

        const Entry* waitTarget() const {
            for (const auto& entry : queue) {
                if (entry.acquired && !entry.successfullyWaited) return &entry;
            }
            return nullptr;
        }

        bool waitSucceeded() {
            for (auto& entry : queue) {
                if (entry.acquired && !entry.successfullyWaited) {
                    entry.successfullyWaited = true;
                    return true;
                }
            }
            return invalidate("successful_wait_without_acquire");
        }

        const Entry* releaseTarget() const {
            return queue.empty() ? nullptr : &queue.front();
        }

        bool canRelease() {
            if (queue.empty()) return invalidate("release_without_acquire");
            if (!queue.front().acquired || !queue.front().successfullyWaited)
                return invalidate("release_without_successful_wait");
            return true;
        }

        // Call only after canRelease(); Mode 0 is the exact last-acquire control.
        uint32_t selectedCopyIndex(uint32_t mode) const {
            return mode == 1 ? queue.front().index : lastAcquiredIndex;
        }

        bool releaseSucceeded() {
            if (!canRelease()) return false;
            queue.front().acquired = false;
            queue.pop_front();
            return true;
        }

        std::string state() const {
            std::string result;
            for (const auto& entry : queue) {
                if (!result.empty()) result += ',';
                result += std::to_string(entry.index);
                result += entry.acquired ? ":A" : ":-";
                result += entry.successfullyWaited ? 'W' : '-';
            }
            return result.empty() ? "empty" : result;
        }
    };
}
