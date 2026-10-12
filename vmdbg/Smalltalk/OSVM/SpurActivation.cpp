#include "SpurActivation.h"
#include "SpurImage.h"
#include <set>

namespace osvm {

SpurCompiledMethod SpurActivation::method() const {
    return isFrame() ? stackFrame.method() : heapContext.method();
}

SpurObject SpurActivation::receiver() const {
    return isFrame() ? stackFrame.receiver() : heapContext.receiver();
}

bool SpurActivation::isBlock() const {
    return isFrame() ? stackFrame.isBlock() : heapContext.isBlockContext();
}

std::vector<SpurObject> SpurActivation::temporaries() const {
    return isFrame() ? stackFrame.temporaries() : heapContext.temporaries();
}

namespace {

// A widowed context outlived its frame, so the frame pointer it holds now
// belongs to some other activation, or to none.
bool isFrameOf(const CogFrame& frame, const SpurContext& context) {
    return frame.isValid() && frame.context().oop() == context.oop();
}

} // namespace

std::vector<SpurActivation> activationsFrom(SpurContext top, uint64_t framePointer,
                                            const SpurImage* image, size_t limit) {
    std::vector<SpurActivation> found;
    std::set<uint64_t> visited;
    SpurContext context = top;
    uint64_t fp = framePointer;
    while (found.size() < limit) {
        if (fp == 0) {
            if (!context || !visited.insert(context.oop()).second) break;
            if (!context.isMarried()) {
                found.emplace_back(context);
                context = context.sender();
                continue;
            }
            fp = context.framePointer();
            if (!isFrameOf(CogFrame(fp, image), context)) break;
        }
        const std::vector<CogFrame> frames = framesFrom(fp, image);
        if (frames.empty() || !visited.insert(fp).second) break;
        for (const CogFrame& frame : frames) {
            if (found.size() == limit) break;
            found.emplace_back(frame);
        }
        context = frames.back().callerContext();
        fp = 0;
    }
    return found;
}

} // namespace osvm
