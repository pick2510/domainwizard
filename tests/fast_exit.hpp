#pragma once

#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace wrftools_tests {
// Terminates the process without running static-destructor/atexit teardown
// - GDAL/netCDF's own driver-unregistration hooks among them, which on
// Windows have been confirmed (via GDAL's own CPL_DEBUG trace) to deadlock
// specifically when Qt is also loaded in the same process. std::_Exit()/
// std::quick_exit() do NOT avoid this on Windows: both still route through
// ExitProcess(), which - unlike TerminateProcess() - still notifies every
// loaded DLL via DllMain(DLL_PROCESS_DETACH) before actually terminating,
// and that is exactly where the deadlock happens (under the loader lock).
// TerminateProcess() skips DLL_PROCESS_DETACH entirely, which is what
// actually routes around it. The test process's own state doesn't need to
// survive past this call, so skipping teardown is safe here even though it
// wouldn't be in the shipped app.
inline void fastExit(int code) {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
#else
    std::_Exit(code);
#endif
}
}  // namespace wrftools_tests
