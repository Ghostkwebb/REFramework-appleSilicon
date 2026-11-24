#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "utility/Thread.hpp"

#include "WindowsMessageHook.hpp"

using namespace std;

static std::atomic<WindowsMessageHook*> g_windows_message_hook{ nullptr };
std::recursive_mutex g_proc_mutex{};

LRESULT WINAPI window_proc(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    WNDPROC original_proc = nullptr;
    WindowsMessageHook::OnMessageFn on_message_cb{};

    {
        std::lock_guard _{ g_proc_mutex };
        auto* hook = g_windows_message_hook.load(std::memory_order_acquire);
        if (hook == nullptr) {
            return DefWindowProc(wnd, message, w_param, l_param);
        }

        original_proc = hook->get_original(wnd);
        if (original_proc == nullptr) {
            original_proc = hook->get_original();
        }

        on_message_cb = hook->on_message;

        if (message == WM_NCDESTROY) {
            hook->unhook_window(wnd);
        }
    }

    if (on_message_cb) {
        // If it returns false we don't call the original window procedure.
        if (!on_message_cb(wnd, message, w_param, l_param)) {
            return DefWindowProc(wnd, message, w_param, l_param);
        }
    }

    if (original_proc != nullptr && original_proc != &window_proc) {
        // Call the original message procedure without holding g_proc_mutex to avoid deadlocks.
        return CallWindowProc(original_proc, wnd, message, w_param, l_param);
    }

    return DefWindowProc(wnd, message, w_param, l_param);
}

bool WindowsMessageHook::hook_window(HWND wnd) {
    if (wnd == nullptr || !IsWindow(wnd)) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return false;
    }

    std::lock_guard _{ m_procs_mutex };

    if (m_original_procs.contains(wnd)) {
        return true;
    }

    auto current_proc = (WNDPROC)GetWindowLongPtr(wnd, GWLP_WNDPROC);
    if (current_proc == nullptr || current_proc == (WNDPROC)&window_proc) {
        return true;
    }

    m_original_procs[wnd] = current_proc;
    SetWindowLongPtr(wnd, GWLP_WNDPROC, (LONG_PTR)&window_proc);
    spdlog::info("WindowsMessageHook: Successfully hooked window {:x} (original: {:p})", (uintptr_t)wnd, (void*)current_proc);
    return true;
}

void WindowsMessageHook::unhook_window(HWND wnd) {
    if (wnd == nullptr) {
        return;
    }

    std::lock_guard _{ m_procs_mutex };
    m_original_procs.erase(wnd);
    if (wnd == m_wnd) {
        m_wnd = nullptr;
    }
}

WindowsMessageHook::WindowsMessageHook(HWND wnd)
    : m_wnd{ wnd },
    m_original_proc{ nullptr }
{
    std::lock_guard _{ g_proc_mutex };
    spdlog::info("Initializing WindowsMessageHook with primary window: {:x}", (uintptr_t)wnd);

    utility::ThreadSuspender suspender{};

    g_windows_message_hook.store(this, std::memory_order_release);

    // Save the original window procedure of primary window.
    if (m_wnd != nullptr) {
        m_original_proc = (WNDPROC)GetWindowLongPtr(m_wnd, GWLP_WNDPROC);
        hook_window(m_wnd);

        // Also hook root/parent windows if they exist (critical for Wine/macOS CrossOver)
        HWND root = GetAncestor(m_wnd, GA_ROOT);
        if (root != nullptr && root != m_wnd) {
            spdlog::info("WindowsMessageHook: Found GA_ROOT window: {:x}", (uintptr_t)root);
            hook_window(root);
        }

        HWND root_owner = GetAncestor(m_wnd, GA_ROOTOWNER);
        if (root_owner != nullptr && root_owner != m_wnd && root_owner != root) {
            spdlog::info("WindowsMessageHook: Found GA_ROOTOWNER window: {:x}", (uintptr_t)root_owner);
            hook_window(root_owner);
        }

        HWND parent = GetParent(m_wnd);
        if (parent != nullptr && parent != m_wnd && parent != root) {
            spdlog::info("WindowsMessageHook: Found parent window: {:x}", (uintptr_t)parent);
            hook_window(parent);
        }
    }

    const auto current_pid = GetCurrentProcessId();
    HWND foreground = GetForegroundWindow();
    if (foreground != nullptr && IsWindowVisible(foreground)) {
        DWORD fg_pid = 0;
        GetWindowThreadProcessId(foreground, &fg_pid);
        if (fg_pid == current_pid) {
            hook_window(foreground);
        }
    }

    spdlog::info("Hooked Windows message handlers (total {} windows hooked)", m_original_procs.size());
}

WindowsMessageHook::~WindowsMessageHook() {
    std::lock_guard _{ g_proc_mutex };
    spdlog::info("Destroying WindowsMessageHook");

    utility::ThreadSuspender suspender{};

    remove();
    g_windows_message_hook.store(nullptr, std::memory_order_release);
}

bool WindowsMessageHook::remove() {
    std::lock_guard _{ m_procs_mutex };

    for (const auto& [wnd, orig_proc] : m_original_procs) {
        if (wnd != nullptr && IsWindow(wnd)) {
            auto current_proc = (WNDPROC)GetWindowLongPtr(wnd, GWLP_WNDPROC);
            if (current_proc == &window_proc && orig_proc != nullptr) {
                SetWindowLongPtr(wnd, GWLP_WNDPROC, (LONG_PTR)orig_proc);
            }
        }
    }
    m_original_procs.clear();

    m_wnd = nullptr;
    m_original_proc = nullptr;

    return true;
}

bool WindowsMessageHook::is_hook_intact() {
    std::lock_guard _{ m_procs_mutex };

    if (m_original_procs.empty() && m_wnd == nullptr) {
        return false;
    }

    for (const auto& [wnd, orig_proc] : m_original_procs) {
        if (wnd != nullptr && IsWindow(wnd)) {
            if (GetWindowLongPtr(wnd, GWLP_WNDPROC) == (LONG_PTR)&window_proc) {
                return true;
            }
        }
    }

    if (m_wnd != nullptr && IsWindow(m_wnd)) {
        return GetWindowLongPtr(m_wnd, GWLP_WNDPROC) == (LONG_PTR)&window_proc;
    }

    return false;
}
