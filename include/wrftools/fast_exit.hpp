#pragma once

#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace wrftools {
// Terminates the process without running static-destructor/atexit teardown
// - GDAL/netCDF's own driver-unregistration hooks among them, which on
// Windows have been confirmed (via GDAL's own CPL_DEBUG trace, see
// tests/fast_exit.hpp - this is that same fix, promoted to a shared header
// because wrftools_reproject_worker needs it too, not just the test
// binaries) to deadlock specifically when Qt is also loaded in the same
// process. std::_Exit()/std::quick_exit() do NOT avoid this on Windows:
// both still route through ExitProcess(), which - unlike
// TerminateProcess() - still notifies every loaded DLL via
// DllMain(DLL_PROCESS_DETACH) before actually terminating, and that is
// exactly where the deadlock happens (under the loader lock).
// TerminateProcess() skips DLL_PROCESS_DETACH entirely, which is what
// actually routes around it.
//
// Safe to use here (unlike in the shipped GUI app, which keeps running
// after any one operation and must not skip its own teardown) precisely
// because wrftools_reproject_worker's job is finished by the time this is
// called: every output file has already been closed (flushed to disk) and
// every stdout line already explicitly flushed (see reproject_worker.cpp's
// emitLine) before main() ever reaches this call - the only thing being
// skipped is GDAL/Qt's own internal driver-unregistration bookkeeping,
// which has no externally observable effect for a process about to exit
// anyway.
[[noreturn]] inline void fastExit(int code) {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    std::_Exit(code);  // unreachable - satisfies [[noreturn]] if TerminateProcess somehow returned
#else
    std::_Exit(code);
#endif
}
}  // namespace wrftools
