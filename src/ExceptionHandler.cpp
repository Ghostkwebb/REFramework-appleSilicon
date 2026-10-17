#include <windows.h>
#include <DbgHelp.h>
#include <ShlObj.h>
#include <filesystem>
#include <atomic>
#include <exception>
#include <spdlog/spdlog.h>

#include "utility/Module.hpp"
#include "utility/Scan.hpp"
#include "utility/Patch.hpp"

#include "utility/Exceptions.hpp"
#include "utility/FunctionHookMinHook.hpp"

#include <sdk/GameIdentity.hpp>
#include "utility/DarwinHost.hpp"
#include "REFramework.hpp"
#include "ExceptionHandler.hpp"

static bool is_ud2_instruction(uintptr_t rip) {
    __try {
        if (rip != 0 && *(const uint16_t*)rip == 0x0B0F) { // 0x0F, 0x0B in little-endian (UD2)
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

static uintptr_t read_stack_pointer(uintptr_t rsp) {
    __try {
        if (rsp != 0) {
            return *(const uintptr_t*)rsp;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

// First-priority Vectored Exception Handler (VEH):
// Runs at Step 1 before Wine ever attempts SEH stack unwinding (RtlVirtualUnwind2).
// Catches uncompiled IL2CPP generic delegate stubs and stripped AOT stubs (UD2)
// directly, preventing Wine stack limit faults (invalid frame outside stack limits).
LONG WINAPI reframework::vectored_exception_handler(struct _EXCEPTION_POINTERS* ei) {
    if (ei == nullptr || ei->ExceptionRecord == nullptr || ei->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto code = ei->ExceptionRecord->ExceptionCode;
    if (code == 0xC000001D) { // STATUS_ILLEGAL_INSTRUCTION
        const auto rip = (uintptr_t)ei->ContextRecord->Rip;
        const auto rsp = (uintptr_t)ei->ContextRecord->Rsp;

        if (is_ud2_instruction(rip)) {
            const auto retaddr = read_stack_pointer(rsp);
            // If caller pushed a valid return address within a loaded module, simulate a ret:
            if (retaddr != 0 && utility::get_module_within(retaddr).has_value()) {
                spdlog::warn("vectored_exception_handler: Dynamically recovered from uncompiled UD2 stub at 0x{:X}, returning to caller 0x{:X}", rip, retaddr);
                ei->ContextRecord->Rip = retaddr;
                ei->ContextRecord->Rsp = rsp + sizeof(void*);
                return EXCEPTION_CONTINUE_EXECUTION;
            } else {
                // Inline UD2 trap: advance instruction pointer past the 2-byte UD2 opcode
                spdlog::warn("vectored_exception_handler: Dynamically skipped inline UD2 trap at 0x{:X}", rip);
                ei->ContextRecord->Rip = rip + 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER s_previous_filter = nullptr;

LONG WINAPI reframework::global_exception_handler(struct _EXCEPTION_POINTERS* ei) {
    if (ei == nullptr || ei->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto code = ei->ExceptionRecord->ExceptionCode;
    // Filter out debugger notifications, thread naming exceptions, and Wine-internal assertions/stubs
    if (code == 0x406D1388 || // MSVC SetThreadName
        code == 0x80000100 || // STATUS_WINE_STUB
        code == 0x80000101 || // STATUS_WINE_ASSERTION
        code == 0x40010006 || // DBG_PRINTEXCEPTION_C
        code == 0x4001000A || // DBG_PRINTEXCEPTION_WIDE_C
        code == 0xE06D7363 || // MSVC C++ EH Exception
        code == 0x80000001 || // EXCEPTION_GUARD_PAGE
        code == 0x80000003 || // EXCEPTION_BREAKPOINT
        code == 0x80000004)   // EXCEPTION_SINGLE_STEP
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Dynamic AOT stub / UD2 recovery (fallback in case not caught at VEH level):
    if (code == 0xC000001D && ei->ContextRecord != nullptr) { // STATUS_ILLEGAL_INSTRUCTION
        const auto rip = (uintptr_t)ei->ContextRecord->Rip;
        const auto rsp = (uintptr_t)ei->ContextRecord->Rsp;

        if (is_ud2_instruction(rip)) {
            const auto retaddr = read_stack_pointer(rsp);
            if (retaddr != 0 && utility::get_module_within(retaddr).has_value()) {
                spdlog::warn("global_exception_handler: Dynamically recovered from uncompiled UD2 stub at 0x{:X}, returning to caller 0x{:X}", rip, retaddr);
                ei->ContextRecord->Rip = retaddr;
                ei->ContextRecord->Rsp = rsp + sizeof(void*);
                return EXCEPTION_CONTINUE_EXECUTION;
            } else {
                spdlog::warn("global_exception_handler: Dynamically skipped inline UD2 trap at 0x{:X}", rip);
                ei->ContextRecord->Rip = rip + 2;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }

    static std::recursive_mutex mtx{};
    std::scoped_lock _{ mtx };

    static std::atomic<bool> s_crash_logged{false};
    if (s_crash_logged.exchange(true)) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    spdlog::flush_on(spdlog::level::err);

    if (_ReturnAddress() == nullptr) {
        spdlog::warn("SUSPICIOUS EXCEPTION: _ReturnAddress() == nullptr");
    }

    spdlog::error("Exception occurred: {:x}", ei->ExceptionRecord->ExceptionCode);
    
    spdlog::error("RIP (exception record): {:x}", (uintptr_t)ei->ExceptionRecord->ExceptionAddress);
    spdlog::error("RIP: {:x}", ei->ContextRecord->Rip);
    spdlog::error("RSP: {:x}", ei->ContextRecord->Rsp);
    spdlog::error("RCX: {:x}", ei->ContextRecord->Rcx);
    spdlog::error("RDX: {:x}", ei->ContextRecord->Rdx);
    spdlog::error("R8: {:x}", ei->ContextRecord->R8);
    spdlog::error("R9: {:x}", ei->ContextRecord->R9);
    spdlog::error("R10: {:x}", ei->ContextRecord->R10);
    spdlog::error("R11: {:x}", ei->ContextRecord->R11);
    spdlog::error("R12: {:x}", ei->ContextRecord->R12);
    spdlog::error("R13: {:x}", ei->ContextRecord->R13);
    spdlog::error("R14: {:x}", ei->ContextRecord->R14);
    spdlog::error("R15: {:x}", ei->ContextRecord->R15);
    spdlog::error("RAX: {:x}", ei->ContextRecord->Rax);
    spdlog::error("RBX: {:x}", ei->ContextRecord->Rbx);
    spdlog::error("RBP: {:x}", ei->ContextRecord->Rbp);
    spdlog::error("RSI: {:x}", ei->ContextRecord->Rsi);
    spdlog::error("RDI: {:x}", ei->ContextRecord->Rdi);
    spdlog::error("EFLAGS: {:x}", ei->ContextRecord->EFlags);
    spdlog::error("CS: {:x}", ei->ContextRecord->SegCs);
    spdlog::error("DS: {:x}", ei->ContextRecord->SegDs);
    spdlog::error("ES: {:x}", ei->ContextRecord->SegEs);
    spdlog::error("FS: {:x}", ei->ContextRecord->SegFs);
    spdlog::error("GS: {:x}", ei->ContextRecord->SegGs);
    spdlog::error("SS: {:x}", ei->ContextRecord->SegSs);

    utility::exceptions::dump_callstack(ei);

    const auto module_within = utility::get_module_within(ei->ContextRecord->Rip);

    if (module_within) {
    if (sdk::GameIdentity::get().is_re8()) {
        // funny way to fix a crash.
        if (*module_within == utility::get_executable() && (uint32_t)ei->ContextRecord->Rcx == 0xFFFFFFFF) {
            spdlog::info("Attempting to fix RE8 overlay draw crash...");

            if (utility::scan(ei->ContextRecord->Rip, 4, "48 8B 9C CE")) {
                const auto offset = *(uint32_t*)(ei->ContextRecord->Rip + 4);
                std::vector<uint8_t> patch_bytes{ 0x48, 0x8B, 0x9E, 0x00, 0x00, 0x00, 0x00, 0x90 };
                *(uint32_t*)(patch_bytes.data() + 3) = offset;
                std::vector<int16_t> patch_int16_bytes{};

                for (auto& patch_byte : patch_bytes) {
                    patch_int16_bytes.push_back(patch_byte);
                }

                static auto patch = Patch::create(ei->ContextRecord->Rip, patch_int16_bytes);
                spdlog::info("Successfully patched RE8 overlay draw crash.");
                s_crash_logged = false;
                return EXCEPTION_CONTINUE_EXECUTION;
            } else {
                spdlog::info("Instructions did not match RE8 overlay draw crash.");
            }
        }
    }

        const auto module_path = utility::get_module_path(*module_within);

        if (module_path) {
            spdlog::error("Module: {:x} {}", (uintptr_t)*module_within, *module_path);
        } else {
            spdlog::error("Module: Unknown");
        }
    } else {
        spdlog::error("Module: Unknown");
    }

    if (auto l = spdlog::default_logger()) {
        l->flush();
    }

    // Write unbuffered crash summary to re2_framework_crash.txt so it is guaranteed to persist on disk
    try {
        const auto crash_log_path = REFramework::get_persistent_dir("re2_framework_crash.txt").string();
        HANDLE crash_file = CreateFileA(crash_log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (crash_file != INVALID_HANDLE_VALUE) {
            std::string report = std::format(
                "================================================================================\r\n"
                "REFramework Crash Report\r\n"
                "================================================================================\r\n"
                "Exception Code:    0x{:08X}\r\n"
                "Exception Address: 0x{:016X}\r\n"
                "Faulting RIP:      0x{:016X}\r\n"
                "Faulting RSP:      0x{:016X}\r\n"
                "Registers:\r\n"
                "  RAX: 0x{:016X}  RBX: 0x{:016X}  RCX: 0x{:016X}  RDX: 0x{:016X}\r\n"
                "  RSI: 0x{:016X}  RDI: 0x{:016X}  RBP: 0x{:016X}  RSP: 0x{:016X}\r\n"
                "  R8:  0x{:016X}  R9:  0x{:016X}  R10: 0x{:016X}  R11: 0x{:016X}\r\n"
                "  R12: 0x{:016X}  R13: 0x{:016X}  R14: 0x{:016X}  R15: 0x{:016X}\r\n"
                "  EFLAGS: 0x{:08X}\r\n",
                code,
                (uintptr_t)ei->ExceptionRecord->ExceptionAddress,
                ei->ContextRecord->Rip,
                ei->ContextRecord->Rsp,
                ei->ContextRecord->Rax, ei->ContextRecord->Rbx, ei->ContextRecord->Rcx, ei->ContextRecord->Rdx,
                ei->ContextRecord->Rsi, ei->ContextRecord->Rdi, ei->ContextRecord->Rbp, ei->ContextRecord->Rsp,
                ei->ContextRecord->R8,  ei->ContextRecord->R9,  ei->ContextRecord->R10, ei->ContextRecord->R11,
                ei->ContextRecord->R12, ei->ContextRecord->R13, ei->ContextRecord->R14, ei->ContextRecord->R15,
                ei->ContextRecord->EFlags
            );

            if (module_within) {
                const auto module_path = utility::get_module_path(*module_within);
                const auto mod_base = (uintptr_t)*module_within;
                const auto offset = ei->ContextRecord->Rip - mod_base;
                report += std::format("Module:            {} (Base: 0x{:X}, Offset: 0x{:X})\r\n",
                    module_path ? *module_path : "Unknown", mod_base, offset);
            } else {
                report += "Module:            Unknown\r\n";
            }

            if (code == EXCEPTION_ACCESS_VIOLATION && ei->ExceptionRecord->NumberParameters >= 2) {
                const auto access_type = ei->ExceptionRecord->ExceptionInformation[0];
                const auto access_addr = ei->ExceptionRecord->ExceptionInformation[1];
                report += std::format("Access Violation:  Attempted to {} address 0x{:016X}\r\n",
                    access_type == 0 ? "READ" : (access_type == 1 ? "WRITE" : "EXECUTE"), access_addr);
            }

            report += "================================================================================\r\n";

            DWORD bytes_written = 0;
            WriteFile(crash_file, report.data(), (DWORD)report.size(), &bytes_written, nullptr);
            FlushFileBuffers(crash_file);
            CloseHandle(crash_file);
        }
    } catch (...) {}

    auto dbghelp = LoadLibrary("dbghelp.dll");

    if (dbghelp) {
        const auto final_path = REFramework::get_persistent_dir("reframework_crash.dmp").string();

        spdlog::error("Attempting to write dump to {}", final_path);

        auto f = CreateFile(final_path.c_str(), 
            GENERIC_WRITE, 
            FILE_SHARE_WRITE, 
            nullptr, 
            CREATE_ALWAYS, 
            FILE_ATTRIBUTE_NORMAL, 
            nullptr
        );

        if (!f || f == INVALID_HANDLE_VALUE) {
            spdlog::error("Exception occurred, but could not create dump file");
            return EXCEPTION_CONTINUE_SEARCH;
        }

        MINIDUMP_EXCEPTION_INFORMATION ei_info{
            GetCurrentThreadId(),
            ei,
            FALSE
        };

        auto minidump_write_dump = (decltype(MiniDumpWriteDump)*)GetProcAddress(dbghelp, "MiniDumpWriteDump");

        minidump_write_dump(GetCurrentProcess(), 
            GetCurrentProcessId(),
            f,
            MINIDUMP_TYPE::MiniDumpNormal, 
            &ei_info, 
            nullptr, 
            nullptr
        );

        CloseHandle(f);
    } else {
        spdlog::error("Exception occurred, but could not load dbghelp.dll");
    }

    if (auto l = spdlog::default_logger()) {
        l->flush();
    }

    // On Wine / CrossOver / Proton, Capcom's external CrashReport.exe is unsupported and crashes
    // immediately with a winedbg popup, freezing the game even when the engine is still rendering.
    if (!utility::DarwinHost::is_wine() && s_previous_filter != nullptr) {
        return s_previous_filter(ei);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

static std::unique_ptr<FunctionHookMinHook> s_set_unhandled_exception_filter_hook{};

static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hooked_set_unhandled_exception_filter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter) {
    spdlog::info("SetUnhandledExceptionFilter called by module/game: {:p}", (void*)lpTopLevelExceptionFilter);
    auto old = s_previous_filter;
    if (lpTopLevelExceptionFilter != reframework::global_exception_handler) {
        s_previous_filter = lpTopLevelExceptionFilter;
    }
    // Prevent Capcom CrashHandler or game from unhooking our crash reporter
    return old;
}

void reframework::setup_exception_handler() {
    static bool s_handler_installed = false;
    if (!s_handler_installed) {
        s_handler_installed = true;

        std::set_terminate([]() {
            spdlog::critical("std::terminate called! Process terminating due to unhandled C++ exception.");
            if (auto l = spdlog::default_logger()) {
                l->flush();
            }
        });

        // Register Vectored Exception Handler to intercept UD2 stubs before stack unwinding
        AddVectoredExceptionHandler(1, vectored_exception_handler);
        spdlog::info("Registered Vectored Exception Handler successfully");

        // Set our unhandled exception filter first and save any existing filter
        s_previous_filter = SetUnhandledExceptionFilter(global_exception_handler);

        // Hook SetUnhandledExceptionFilter to prevent game / CrashHandler.dll from overriding it
        s_set_unhandled_exception_filter_hook = std::make_unique<FunctionHookMinHook>(
            SetUnhandledExceptionFilter,
            hooked_set_unhandled_exception_filter
        );

        if (!s_set_unhandled_exception_filter_hook->create()) {
            spdlog::error("Could not hook SetUnhandledExceptionFilter!");
        } else {
            spdlog::info("Hooked SetUnhandledExceptionFilter successfully");
        }
    }
}
