// Revenant ARM32-UWP static-init breadcrumb (diagnostics only).
//
// A separate TU because MSVC forbids two #pragma init_seg in one translation unit (C2356).
// This object is placed in .CRT$XCY, which sorts AFTER .CRT$XCU (the default segment where
// webrtc's global ctors live), so its ctor runs LAST among all static initializers. If every
// webrtc/WebCore global ctor completes, this writes "all-static-ctors-completed"; if the process
// dies inside one of them, this line never appears — pinpointing death to the ctor phase.
// g_probePath is set by RevenantCrashProbe's init_seg(lib) ctor, which runs first (XCL < XCY),
// so RevenantProbeMark is armed by the time this fires.

extern "C" void RevenantProbeMark(const char*);

namespace {
struct RevenantCtorsDoneMark {
    RevenantCtorsDoneMark() { RevenantProbeMark("=== all-static-ctors-completed (XCY, after webrtc globals) ==="); }
};
} // namespace

#pragma warning(push)
#pragma warning(disable : 4073 4075)
#pragma init_seg(".CRT$XCY")
#pragma warning(pop)
static RevenantCtorsDoneMark g_revenantCtorsDoneMark;
