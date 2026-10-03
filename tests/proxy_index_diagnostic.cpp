#include "proxy_index_diagnostic.h"

#include <cassert>

using toolkit::proxy_index::Tracker;

int main() {
    // The last acquired image is not necessarily the one released next.
    {
        Tracker t;
        assert(t.acquire(0, 3));
        assert(t.waitTarget()->index == 0 && t.waitSucceeded());
        assert(t.acquire(1, 3));
        const unsigned legacyIndex = 1;
        assert(t.canRelease() && t.releaseTarget()->index == 0);
        assert(t.selectedCopyIndex(0) == 1 && t.selectedCopyIndex(1) == 0);
        assert(legacyIndex != t.releaseTarget()->index);
        assert(t.releaseSucceeded() && t.queue.size() == 1);
        assert(t.queue.front().index == 1 && !t.queue.front().successfullyWaited);
        assert(t.lastAcquiredIndex == 1);
    }
    {
        Tracker t;
        assert(t.acquire(0, 3) && t.acquire(1, 3));
        assert(t.waitTarget()->index == 0 && t.waitSucceeded());
        assert(t.canRelease() && t.selectedCopyIndex(1) == 0 && t.releaseSucceeded());
        assert(t.waitTarget()->index == 1 && t.waitSucceeded());
        assert(t.canRelease() && t.selectedCopyIndex(1) == 1 && t.releaseSucceeded());
        assert(t.queue.empty());
    }
    {
        Tracker t;
        assert(t.acquire(0, 3));
        // XR_TIMEOUT_EXPIRED: the integration deliberately does not call waitSucceeded().
        assert(t.waitTarget()->index == 0 && !t.queue.front().successfullyWaited);
        assert(t.waitTarget()->index == 0 && t.waitSucceeded());
        assert(t.canRelease() && t.selectedCopyIndex(1) == 0 && t.releaseSucceeded());
    }
    {
        // A swapchain with XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT has one acquire/wait/release cycle.
        Tracker t;
        assert(t.acquire(0, 1) && t.waitSucceeded() && t.canRelease() &&
               t.selectedCopyIndex(1) == 0 && t.releaseSucceeded());
        assert(t.queue.empty());
    }
    {
        Tracker t;
        assert(t.acquire(0, 2));
        assert(!t.canRelease() && t.error == "release_without_successful_wait");
        assert(t.queue.size() == 1 && !t.queue.front().successfullyWaited);
    }
    {
        Tracker t;
        assert(t.acquire(0, 2) && t.waitSucceeded() && t.canRelease());
        // A failed downstream release never calls releaseSucceeded().
        assert(t.queue.size() == 1 && t.releaseTarget()->index == 0);
        assert(t.releaseSucceeded() && t.queue.empty());
    }
    {
        Tracker t;
        assert(!t.canRelease() && t.error == "release_without_acquire");
        assert(t.queue.empty());
    }
    {
        Tracker t;
        assert(t.acquire(0, 1));
        // Teardown may observe a pending acquisition without changing FIFO validity.
        assert(t.queue.size() == 1 && t.error.empty());
    }
}
