#include "swapchain_image_fifo.h"

#include <cstdlib>
#include <iostream>

using toolkit::SwapchainImageFifo;

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

static void release(SwapchainImageFifo& fifo, uint32_t expected) {
    check(fifo.requestRelease(), "app release accepted");
    check(fifo.getReleaseImageIndex() == expected, "FIFO runtime release index");
    check(fifo.getSubmittedImageIndex() == expected, "composition image index");
    fifo.released(XR_SUCCESS);
}

int main() {
    { // A: one acquisition.
        SwapchainImageFifo fifo;
        fifo.acquired(XR_SUCCESS, 0);
        fifo.waited(XR_SUCCESS);
        release(fifo, 0);
        check(fifo.acquisitions().empty(), "A: released entry removed");
    }
    { // B: acquiring the next image must not replace the submitted image.
        SwapchainImageFifo fifo;
        fifo.acquired(XR_SUCCESS, 0);
        fifo.waited(XR_SUCCESS);
        fifo.acquired(XR_SUCCESS, 1);
        release(fifo, 0);
        check(fifo.acquisitions().size() == 1 && fifo.acquisitions().front().index == 1,
              "B: next acquisition retained");
        fifo.waited(XR_SUCCESS);
        release(fifo, 1);
    }
    { // C: timeout does not complete or advance a wait.
        SwapchainImageFifo fifo;
        fifo.acquired(XR_SUCCESS, 0);
        fifo.acquired(XR_SUCCESS, 1);
        fifo.waited(XR_TIMEOUT_EXPIRED);
        check(fifo.acquisitions().size() == 2 && !fifo.acquisitions().front().waited,
              "C: timeout leaves oldest acquisition unready");
        check(!fifo.requestRelease(), "C: release before completed wait rejected");
        fifo.waited(XR_SUCCESS);
        check(!fifo.acquisitions().back().waited, "C: retry waited same image");
        release(fifo, 0);
    }
    { // D/E/F: runtime errors preserve acquisition, wait and release state.
        SwapchainImageFifo fifo;
        fifo.acquired(XR_SUCCESS, 0);
        fifo.acquired(XR_ERROR_RUNTIME_FAILURE, 1);
        check(fifo.acquisitions().size() == 1, "D: failed acquire leaves queue intact");
        fifo.waited(XR_ERROR_CALL_ORDER_INVALID);
        check(!fifo.acquisitions().front().waited, "E: failed wait leaves state intact");
        fifo.waited(XR_SUCCESS);
        check(fifo.requestRelease(), "F: valid release requested");
        fifo.released(XR_ERROR_RUNTIME_FAILURE);
        check(fifo.acquisitions().size() == 1 && fifo.getReleaseImageIndex() == 0u,
              "F: failed downstream release retains FIFO entry");
        fifo.released(XR_SUCCESS);
        check(fifo.acquisitions().empty(), "F: successful retry removes entry");
    }
    { // G: a static-image swapchain has one acquire/wait/release cycle, then resubmissions.
        SwapchainImageFifo fifo;
        fifo.acquired(XR_SUCCESS, 0);
        fifo.waited(XR_SUCCESS);
        release(fifo, 0);
        check(fifo.getSubmittedImageIndex() == 0u && !fifo.getReleaseImageIndex(),
              "G: static image remains available for later composition");
    }
    { // H: three images rotate with overlapping acquisitions.
        SwapchainImageFifo fifo;
        for (int rotation = 0; rotation < 3; ++rotation) {
            for (uint32_t i = 0; i < 3; ++i) {
                fifo.acquired(XR_SUCCESS, i);
            }
            for (uint32_t i = 0; i < 3; ++i) {
                fifo.waited(XR_SUCCESS);
                release(fifo, i);
            }
            check(fifo.acquisitions().empty(), "H: rotation drained in FIFO order");
        }
    }
    { // Multiple app releases can be deferred until the existing forwarding point.
        SwapchainImageFifo fifo;
        fifo.acquired(XR_SUCCESS, 0);
        fifo.acquired(XR_SUCCESS, 1);
        fifo.waited(XR_SUCCESS);
        fifo.waited(XR_SUCCESS);
        check(fifo.requestRelease() && fifo.requestRelease(), "two app releases recorded");
        check(fifo.getSubmittedImageIndex() == 1u, "most recent app release used for composition");
        check(fifo.getReleaseImageIndex() == 0u, "downstream release still starts with oldest");
        fifo.released(XR_SUCCESS);
        check(fifo.getReleaseImageIndex() == 1u, "second downstream release follows FIFO");
        fifo.released(XR_SUCCESS);
        fifo.acquired(XR_SUCCESS, 2);
        check(fifo.getSubmittedImageIndex() == 1u, "later acquire does not replace last submitted image");
        check(!fifo.requestRelease(), "unwaited image cannot be released");
        check(fifo.acquisitions().size() == 1, "pending acquisition retained until teardown");
    }
    { // All three commands can complete successfully with session loss pending.
        SwapchainImageFifo fifo;
        fifo.acquired(XR_SESSION_LOSS_PENDING, 2);
        fifo.waited(XR_SESSION_LOSS_PENDING);
        check(fifo.requestRelease() && fifo.getReleaseImageIndex() == 2u, "session loss pending completes operations");
        fifo.released(XR_SESSION_LOSS_PENDING);
        check(fifo.acquisitions().empty(), "session loss pending completes release");
    }
    { // Independent swapchains must not share FIFO or submitted state.
        SwapchainImageFifo color, depth, quad;
        color.acquired(XR_SUCCESS, 0);
        depth.acquired(XR_SUCCESS, 1);
        quad.acquired(XR_SUCCESS, 2);
        color.waited(XR_SUCCESS);
        depth.waited(XR_SUCCESS);
        quad.waited(XR_SUCCESS);
        release(depth, 1);
        release(quad, 2);
        release(color, 0);
        check(!color.requestRelease(), "duplicate release rejected");
        check(!color.getReleaseImageIndex(), "no index fabricated for empty queue");
    }
    std::cout << "PASS: swapchain FIFO A-H, deferred releases, resubmission, session loss and independent swapchains\n";
}
