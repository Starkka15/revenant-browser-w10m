// Revenant ARM32-UWP static-init failure probe (diagnostics only).
//
// The WebRTC-enabled appx dies BEFORE main() (debug.log empty, ExitCode 1, ~112ms of
// init work per Kernel-Process ETW) — a global constructor pulled in by webrtc.lib fails
// during C++ static initialization. The first probe proved SetUnhandledExceptionFilter
// does NOT fire (so it is not an SEH access-violation / __debugbreak), yet the process
// exits 1 with no exception reaching the top — i.e. a clean termination via std::terminate,
// the CRT invalid-parameter handler, abort(), or exit(). This probe installs ALL of those
// hooks (via an init_seg(lib) ctor, ahead of webrtc's user-segment globals) so whichever
// path fires records the reason to crashprobe.log in LocalFolder (pulled over WDP).

#include <windows.h>
#include <winternl.h>
#include <roapi.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

// AddVectoredExceptionHandler is hidden behind WINAPI_PARTITION_DESKTOP in the SDK headers,
// but kernelbase exports it at runtime on W10M. Declare it manually to bypass the guard.
extern "C" __declspec(dllimport) PVOID WINAPI
AddVectoredExceptionHandler(ULONG FirstHandler, PVECTORED_EXCEPTION_HANDLER VectoredHandler);

// GetModuleHandle{,Ex}W and GetModuleFileNameW are also guarded behind WINAPI_PARTITION_DESKTOP
// in the UWP SDK headers, but kernelbase exports them at runtime on W10M. Declare manually.
extern "C" __declspec(dllimport) HMODULE WINAPI GetModuleHandleW(LPCWSTR lpModuleName);
extern "C" __declspec(dllimport) BOOL WINAPI GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule);
extern "C" __declspec(dllimport) DWORD WINAPI GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
#ifndef GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#endif
#ifndef GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#endif
// Thread-inspection APIs are desktop-partition-guarded in the UWP headers but kernelbase exports
// them on W10M — declare manually. Used by the main-thread PC-sampling watchdog.
extern "C" __declspec(dllimport) DWORD WINAPI SuspendThread(HANDLE hThread);
extern "C" __declspec(dllimport) DWORD WINAPI ResumeThread(HANDLE hThread);
extern "C" __declspec(dllimport) BOOL WINAPI GetThreadContext(HANDLE hThread, LPCONTEXT lpContext);
extern "C" __declspec(dllimport) HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T,
    LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>

using namespace Windows::Storage;

namespace {

std::wstring g_probePath;
std::string g_hookSummary;

// Hardcoded fallback: the app's LocalState (same folder WDP pulls from). Used before
// ApplicationData::Current->LocalFolder is resolved, and by the exit hooks which may fire
// on the CRT glue path before the RoInitialize'd path is available.
static const wchar_t* kHardExitLog =
    L"C:\\Data\\Users\\DefApps\\APPDATA\\Local\\Packages\\"
    L"11D6A23B-C985-354B-A09B-B8CD4D11EB6B_d7c8pgvss6ysm\\LocalState\\crashprobe.log";

void rawWrite(const char* line)
{
    const wchar_t* path = g_probePath.empty() ? kHardExitLog : g_probePath.c_str();
    FILE* f = _wfopen(path, L"ab");
    if (!f)
        return;
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}

void probeWrite(const char* line)
{
    if (g_probePath.empty())
        return;
    FILE* f = _wfopen(g_probePath.c_str(), L"ab");
    if (!f)
        return;
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}

// ---- ExitProcess/TerminateProcess IAT hooks -------------------------------------------------
// ETW proved: all static ctors complete, then the process is hard-terminated (both threads stop
// together, exit code 1) BEFORE main() — no first-chance exception, no abort, no atexit. That is
// a direct ExitProcess()/TerminateProcess() call on the main thread. To find WHO, we hook those
// two imports in the EXE's IAT from the init_seg(lib) ctor (installed before webrtc's globals run)
// and log the caller's return address + owning module name. That names the culprit function.

extern "C" __declspec(dllimport) PVOID WINAPI
VirtualProtectFromApp(PVOID, SIZE_T, ULONG, PULONG); // UWP-partition VirtualProtect

typedef VOID (WINAPI* ExitProcess_t)(UINT);
typedef BOOL (WINAPI* TerminateProcess_t)(HANDLE, UINT);
static ExitProcess_t g_realExitProcess = nullptr;
static TerminateProcess_t g_realTerminateProcess = nullptr;

static void addrLine(const char* tag, void* addr, char* out, size_t outsz)
{
    HMODULE hmod = nullptr;
    wchar_t modw[MAX_PATH] = L"";
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)addr, &hmod) && hmod) {
        GetModuleFileNameW(hmod, modw, MAX_PATH);
    }
    char modn[MAX_PATH] = "?";
    WideCharToMultiByte(CP_UTF8, 0, modw, -1, modn, sizeof(modn), nullptr, nullptr);
    // Keep only the leaf filename of the module path to stay short.
    const char* leaf = modn;
    for (const char* p = modn; *p; ++p) if (*p == '\\' || *p == '/') leaf = p + 1;
    unsigned long off = hmod ? (unsigned long)((char*)addr - (char*)hmod) : 0;
    _snprintf(out, outsz, "%s 0x%p  %s +0x%lX", tag, addr, leaf, off);
}

static void logCaller(const char* api, UINT code, void* ret)
{
    char buf[400];
    _snprintf(buf, sizeof(buf), ">>> %s(code=%u) — call chain:", api, code);
    rawWrite(buf);
    addrLine("   ret ", ret, buf, sizeof(buf));
    rawWrite(buf);
    // Full backtrace for context (frame 0 is inside the hook; skip it).
    void* frames[16] = {0};
    USHORT n = CaptureStackBackTrace(1, 16, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        char tag[16];
        _snprintf(tag, sizeof(tag), "   #%02u ", (unsigned)i);
        addrLine(tag, frames[i], buf, sizeof(buf));
        rawWrite(buf);
    }
}

static VOID WINAPI hookExitProcess(UINT code)
{
    logCaller("ExitProcess", code, _ReturnAddress());
    if (g_realExitProcess) g_realExitProcess(code);
    else TerminateProcess(GetCurrentProcess(), code);
}

static BOOL WINAPI hookTerminateProcess(HANDLE h, UINT code)
{
    // Only log self-termination (the interesting case); pass others through untouched.
    if (h == GetCurrentProcess() || h == (HANDLE)(LONG_PTR)-1)
        logCaller("TerminateProcess", code, _ReturnAddress());
    if (g_realTerminateProcess) return g_realTerminateProcess(h, code);
    return FALSE;
}

// Patch one named import in a module's IAT to point at hook; return the original target.
static void* patchIat(HMODULE base, const char* wantName, void* hook)
{
    if (!base) return nullptr;
    BYTE* b = (BYTE*)base;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    IMAGE_DATA_DIRECTORY& impdir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!impdir.VirtualAddress) return nullptr;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(b + impdir.VirtualAddress);
    void* original = nullptr;
    for (; imp->Name; ++imp) {
        IMAGE_THUNK_DATA32* oft = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA32*)(b + imp->OriginalFirstThunk) : nullptr;
        IMAGE_THUNK_DATA32* ft = (IMAGE_THUNK_DATA32*)(b + imp->FirstThunk);
        IMAGE_THUNK_DATA32* nameThunk = oft ? oft : ft;
        for (; nameThunk->u1.AddressOfData; ++nameThunk, ++ft) {
            if (nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32)
                continue; // imported by ordinal, no name
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(b + nameThunk->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, wantName) != 0)
                continue;
            ULONG oldProt = 0;
            if (VirtualProtectFromApp(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                original = (void*)(uintptr_t)ft->u1.Function;
                ft->u1.Function = (DWORD)(uintptr_t)hook;
                ULONG tmp = 0;
                VirtualProtectFromApp(&ft->u1.Function, sizeof(void*), oldProt, &tmp);
            }
        }
    }
    return original;
}

// Minimal loader structs (winternl.h leaves _TEB / LDR_DATA_TABLE_ENTRY opaque in this SDK).
// 32-bit ARM layout matches x86: TEB+0x30 -> PEB, PEB+0x0C -> Ldr.
struct My_LDR_ENTRY {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
};
struct My_PEB_LDR_DATA {
    ULONG Length;
    BYTE Initialized;
    PVOID SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
};

// ---- main-thread PC-sampling watchdog -------------------------------------------------------
// Death is on the main thread between ctor-completion and main(), with no in-process exit call and
// no exception. To find WHERE, a watchdog thread samples the main thread's PC (ARM32 CONTEXT.Pc)
// every few ms and logs each distinct module+offset. The LAST sample before the process dies names
// the function the main thread was executing when it was killed. Suspend is held only across
// GetThreadContext (no locks) then released immediately — logging happens after resume.
static HANDLE g_mainThread = nullptr;
static volatile LONG g_mainReached = 0;

// Dedicated file so the sampler never contends with the main thread's crashprobe.log writes
// (concurrent fopen on the same file drops lines on Windows).
static const wchar_t* kWatchLog =
    L"C:\\Data\\Users\\DefApps\\APPDATA\\Local\\Packages\\"
    L"11D6A23B-C985-354B-A09B-B8CD4D11EB6B_d7c8pgvss6ysm\\LocalState\\watchdog.log";

static void watchWrite(const char* line)
{
    FILE* f = _wfopen(kWatchLog, L"ab");
    if (!f) return;
    fputs(line, f); fputc('\n', f); fclose(f);
}

static DWORD WINAPI watchdogProc(LPVOID)
{
    for (int i = 0; i < 10000; ++i) {   // ~ up to 20s at 2ms
        if (g_mainReached) { watchWrite(">>> watchdog: main() reached, stopping sampler"); return 0; }
        if (!g_mainThread) { Sleep(2); continue; }
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        void* pc = nullptr; void* lr = nullptr; void* sp = nullptr;
        if (SuspendThread(g_mainThread) != (DWORD)-1) {
            if (GetThreadContext(g_mainThread, &ctx)) {
                pc = (void*)(uintptr_t)ctx.Pc;   // ARM32 program counter
                lr = (void*)(uintptr_t)ctx.Lr;   // ARM32 link register (return addr)
                sp = (void*)(uintptr_t)ctx.Sp;
            }
            ResumeThread(g_mainThread);
        }
        if (pc) {
            char pl[256], ll[256], line[560];
            addrLine("pc", pc, pl, sizeof(pl));
            addrLine("lr", lr, ll, sizeof(ll));
            _snprintf(line, sizeof(line), "#%04d sp=0x%p  %s  |  %s", i, sp, pl, ll);
            watchWrite(line);   // log EVERY sample; last line before death = the killing PC
        }
        Sleep(2);
    }
    return 0;
}

static void startWatchdog()
{
    HANDLE dup = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                        &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        g_mainThread = dup;
    DWORD tid = 0;
    CreateThread(nullptr, 0, watchdogProc, nullptr, 0, &tid);
}

static void installExitHooks()
{
    // Patch EVERY loaded module's IAT — the CRT (ucrtbase/vcruntime) and abseil call ExitProcess
    // through their OWN module's import table, not the EXE's, so an EXE-only patch would miss them.
    // Walk the PEB loader list (always available under AppContainer; EnumProcessModules is not).
    int patchedExit = 0, patchedTerm = 0, modules = 0;
    BYTE* teb = (BYTE*)NtCurrentTeb();
    BYTE* peb = teb ? *(BYTE**)(teb + 0x30) : nullptr;
    My_PEB_LDR_DATA* ldr = peb ? *(My_PEB_LDR_DATA**)(peb + 0x0C) : nullptr;
    if (ldr) {
        LIST_ENTRY* head = &ldr->InLoadOrderModuleList;
        for (LIST_ENTRY* cur = head->Flink; cur && cur != head; cur = cur->Flink) {
            My_LDR_ENTRY* ent = CONTAINING_RECORD(cur, My_LDR_ENTRY, InLoadOrderLinks);
            HMODULE base = (HMODULE)ent->DllBase;
            if (!base) continue;
            ++modules;
            void* e = patchIat(base, "ExitProcess", (void*)&hookExitProcess);
            void* t = patchIat(base, "TerminateProcess", (void*)&hookTerminateProcess);
            if (e) { g_realExitProcess = (ExitProcess_t)e; ++patchedExit; }
            if (t) { g_realTerminateProcess = (TerminateProcess_t)t; ++patchedTerm; }
        }
    }
    char buf[200];
    _snprintf(buf, sizeof(buf),
              "=== exit hooks: scanned %d modules, patched ExitProcess in %d, TerminateProcess in %d (orig EP=0x%p TP=0x%p) ===",
              modules, patchedExit, patchedTerm, (void*)g_realExitProcess, (void*)g_realTerminateProcess);
    g_hookSummary.assign(buf);
}
// --------------------------------------------------------------------------------------------

// Decode the mangled C++ type name of a thrown object from a 0xE06D7363 (MSVC throw) record.
// Layout (32-bit): info[2]=ThrowInfo* (absolute), info[3]=image base for the RVAs inside it.
//   ThrowInfo{+0 attrs,+4 pmfnUnwind,+8 pForwardCompat,+12 pCatchableTypeArray(RVA)}
//   CatchableTypeArray{+0 count,+4 firstType(RVA)}  CatchableType{+0 props,+4 pType(RVA)}
//   TypeDescriptor{+0 vftable,+4 spare,+8 name[]}   -> name like ".?AVbad_alloc@std@@"
// SEH-guarded: any bad pointer just yields "" (never faults the handler).
static const char* decodeCxxTypeName(const EXCEPTION_RECORD* er, char* out, size_t outsz)
{
    out[0] = 0;
    if (er->ExceptionCode != 0xE06D7363 || er->NumberParameters < 3)
        return out;
    __try {
        uintptr_t base = er->NumberParameters > 3 ? (uintptr_t)er->ExceptionInformation[3] : 0;
        const BYTE* ti = (const BYTE*)er->ExceptionInformation[2];
        if (!ti) return out;
        DWORD ctaRva = *(const DWORD*)(ti + 12);
        if (!ctaRva) return out;
        const BYTE* cta = (const BYTE*)(base + ctaRva);
        if (*(const int*)cta < 1) return out;
        const BYTE* ct = (const BYTE*)(base + *(const DWORD*)(cta + 4));
        const BYTE* td = (const BYTE*)(base + *(const DWORD*)(ct + 4));
        _snprintf(out, outsz, "%s", (const char*)(td + 8));
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
    return out;
}

static bool isFatalExceptionCode(DWORD code)
{
    switch (code) {
    case 0xC0000005: // access violation
    case 0xC0000409: // fastfail / stack-buffer-overrun (__fastfail, RoFailFast)
    case 0xC000027B: // STATUS_STOWED_EXCEPTION (unhandled C++/WinRT exception -> RoFailFast)
    case 0xC000001D: // illegal instruction
    case 0xC0000096: // privileged instruction
    case 0xC0000094: // integer divide by zero
    case 0x80000003: // breakpoint (__debugbreak in a fail path)
        return true;
    default:
        return false;
    }
}

// Vectored handler: fires on EVERY first-chance exception, before SEH frames / the unhandled
// filter / any fastfail. Routine first-chance C++ throws (0xE06D7363) are logged up to a cap;
// FATAL codes (AV, fastfail, STOWED — the WinRT path a scroll/compositing C++ exception dies
// through, which bypasses set_terminate) are ALWAYS logged with the thrown type name, the owning
// module of the throw, and a resolved backtrace. Benign thread-naming (0x406D1388) and the DBG
// name-probe codes are dropped entirely so they can't exhaust the budget before the real fault.
LONG CALLBACK vehHandler(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    DWORD code = er->ExceptionCode;
    // Drop pure noise: 0x406D1388 = SetThreadName, DBG_PRINTEXCEPTION_C/_WIDE_C = OutputDebugString.
    if (code == 0x406D1388 || code == DBG_PRINTEXCEPTION_C || code == 0x4001000A)
        return EXCEPTION_CONTINUE_SEARCH;

    bool fatal = isFatalExceptionCode(code);
    static int nRoutine = 0;
    static int nFatal = 0;
    if (!fatal && nRoutine++ >= 300)
        return EXCEPTION_CONTINUE_SEARCH; // cap routine throws only; fatals always pass
    if (fatal && nFatal++ >= 6)
        return EXCEPTION_CONTINUE_SEARCH; // avoid a re-raise storm on a wedged fatal

    char tn[220]; decodeCxxTypeName(er, tn, sizeof tn);
    char buf[400];
    _snprintf(buf, sizeof(buf),
        "VEH %s code=0x%08lX addr=0x%p p1=0x%p p2=0x%p p3=0x%p%s%s",
        fatal ? "FATAL" : "chance", (unsigned long)code, (void*)er->ExceptionAddress,
        (void*)(er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0),
        (void*)(er->NumberParameters > 2 ? er->ExceptionInformation[2] : 0),
        (void*)(er->NumberParameters > 3 ? er->ExceptionInformation[3] : 0),
        tn[0] ? " type=" : "", tn);
    probeWrite(buf);

    // For a C++ throw, the ThrowInfo (p2) lives in the throwing module's .rdata -> names WHO threw.
    if (code == 0xE06D7363 && er->NumberParameters > 2) {
        addrLine("   thrownBy", (void*)er->ExceptionInformation[2], buf, sizeof buf);
        probeWrite(buf);
    }
    // The fatal path is the one we came for: resolve a full backtrace to module+offset so the .map
    // names the function that let the exception escape (the scroll/compositing/frame-pump frame).
    if (fatal) {
        void* frames[24] = {0};
        USHORT nf = CaptureStackBackTrace(0, 24, frames, nullptr);
        for (USHORT i = 0; i < nf; ++i) {
            char t[16]; _snprintf(t, sizeof(t), "   f#%02u", (unsigned)i);
            addrLine(t, frames[i], buf, sizeof buf);
            probeWrite(buf);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI sehFilter(EXCEPTION_POINTERS* ep)
{
    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        char buf[400];
        _snprintf(buf, sizeof(buf), "SEH code=0x%08lX addr=0x%p p0=0x%p p1=0x%p",
            (unsigned long)er->ExceptionCode, (void*)er->ExceptionAddress,
            (void*)(er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0),
            (void*)(er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0));
        probeWrite(buf);
        // Resolve the faulting PC to module+offset so the .map names the function.
        addrLine("SEH faultPC", (void*)er->ExceptionAddress, buf, sizeof(buf));
        probeWrite(buf);
        // Backtrace from the crash context (ARM32: seed from the exception CONTEXT's Pc/Lr/Sp
        // is not available to CaptureStackBackTrace, but the current-thread walk still shows
        // the frames above the filter, which are the OS dispatcher + our fault frame's callers).
        void* frames[20] = {0};
        USHORT nf = CaptureStackBackTrace(0, 20, frames, nullptr);
        for (USHORT i = 0; i < nf; ++i) {
            char t[16]; _snprintf(t, sizeof(t), "  SEH#%02u", (unsigned)i);
            addrLine(t, frames[i], buf, sizeof(buf));
            probeWrite(buf);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate()
{
    probeWrite("TERMINATE handler fired");
    std::exception_ptr ex = std::current_exception();
    if (!ex) {
        probeWrite("  (no in-flight exception)");
    } else {
        try {
            std::rethrow_exception(ex);
        } catch (Platform::Exception^ pe) {
            char b[320];
            _snprintf(b, sizeof(b), "  Platform::Exception HRESULT=0x%08lX", (unsigned long)pe->HResult);
            probeWrite(b);
            if (pe->Message) {
                FILE* f = _wfopen(g_probePath.c_str(), L"ab");
                if (f) { fputs("  msg=", f); fputws(pe->Message->Data(), f); fputc('\n', f); fclose(f); }
            }
        } catch (const std::exception& e) {
            char b[320];
            _snprintf(b, sizeof(b), "  std::exception: %s", e.what());
            probeWrite(b);
        } catch (...) {
            probeWrite("  unknown/foreign exception type");
        }
    }
    _exit(42);
}

void onInvalidParameter(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned line, uintptr_t)
{
    probeWrite("INVALID_PARAMETER handler fired");
    FILE* f = _wfopen(g_probePath.c_str(), L"ab");
    if (f) {
        fputs("  func=", f); if (func) fputws(func, f);
        fputs(" file=", f); if (file) fputws(file, f);
        char b[64]; _snprintf(b, sizeof(b), " line=%u", line); fputs(b, f);
        fputs(" expr=", f); if (expr) fputws(expr, f);
        fputc('\n', f); fclose(f);
    }
    _exit(43);
}

void onPurecall()
{
    probeWrite("PURECALL handler fired (call through pure virtual)");
    _exit(44);
}

void onSigabrt(int)
{
    probeWrite("SIGABRT raised (abort() called)");
    _exit(45);
}

void onAtexit()
{
    probeWrite("ATEXIT fired: a clean exit()/return path was taken during startup");
}

struct RevenantCrashInstaller {
    RevenantCrashInstaller()
    {
        // Install exit hooks FIRST, before any path resolution or webrtc globals — they log to
        // the hardcoded LocalState path if g_probePath isn't set yet.
        installExitHooks();
        startWatchdog();
        RoInitialize(RO_INIT_MULTITHREADED);
        try {
            Platform::String^ p = ApplicationData::Current->LocalFolder->Path;
            g_probePath.assign(p->Data());
            g_probePath += L"\\crashprobe.log";
        } catch (...) {
            g_probePath =
                L"C:\\Data\\Users\\DefApps\\APPDATA\\Local\\Packages\\"
                L"11D6A23B-C985-354B-A09B-B8CD4D11EB6B_d7c8pgvss6ysm\\LocalState\\crashprobe.log";
        }
        AddVectoredExceptionHandler(1, vehHandler);
        SetUnhandledExceptionFilter(sehFilter);
        std::set_terminate(onTerminate);
        _set_invalid_parameter_handler(onInvalidParameter);
        _set_purecall_handler(onPurecall);
        signal(SIGABRT, onSigabrt);
        atexit(onAtexit);
        probeWrite("=== static-init: all probe handlers installed (before webrtc globals) ===");
        if (!g_hookSummary.empty())
            probeWrite(g_hookSummary.c_str());
    }
};

} // namespace

// Callable from main() as its literal first statement, and from the XCY last-ctor probe — logs
// to the KNOWN-WORKING crashprobe.log (independent of debug.log / PortSetDebugLogPathW).
// Disambiguates "died in a ctor" vs "died between ctors-end and main" vs "reached main".
extern "C" void RevenantProbeMark(const char* s)
{
    if (s && strstr(s, "main()"))
        g_mainReached = 1;   // stop the PC-sampling watchdog once main body runs
    probeWrite(s);
}

#pragma warning(push)
#pragma warning(disable : 4073)
#pragma init_seg(lib)
#pragma warning(pop)
static RevenantCrashInstaller g_revenantCrashInstaller;
