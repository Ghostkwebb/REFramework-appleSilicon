#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <spdlog/spdlog.h>
#include "DarwinHost.hpp"

namespace utility {

namespace {
constexpr uint32_t kMachHeader64Magic = 0xFEEDFACFu;
constexpr uint32_t kLcSymtab = 0x2u;
constexpr uint32_t kLcDysymtab = 0xBu;
constexpr uint32_t kLcSegment64 = 0x19u;
constexpr uint32_t kLcDyldInfo = 0x22u;
constexpr uint32_t kLcDyldInfoOnly = 0x80000022u;
constexpr uint32_t kLcDyldExportsTrie = 0x80000033u;
constexpr uint32_t kSectionTypeMask = 0xFFu;
constexpr uint32_t kSectionNonLazySymbolPointers = 0x6u;
constexpr uint32_t kIndirectSymbolLocalOrAbs = 0xC0000000u;
constexpr uintptr_t kRtldDefault = static_cast<uintptr_t>(-2);

struct MachHeader64 {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};

struct LoadCommand {
    uint32_t cmd;
    uint32_t cmdsize;
};

struct SegmentCommand64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};

struct Section64 {
    char sectname[16];
    char segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
};

struct SymtabCommand {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
};

struct DysymtabCommand {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoff;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
};

struct Nlist64 {
    uint32_t n_strx;
    uint8_t n_type;
    uint8_t n_sect;
    uint16_t n_desc;
    uint64_t n_value;
};

struct LinkeditDataCommand {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dataoff;
    uint32_t datasize;
};

struct DyldInfoCommand {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t rebase_off;
    uint32_t rebase_size;
    uint32_t bind_off;
    uint32_t bind_size;
    uint32_t weak_bind_off;
    uint32_t weak_bind_size;
    uint32_t lazy_bind_off;
    uint32_t lazy_bind_size;
    uint32_t export_off;
    uint32_t export_size;
};

static_assert(sizeof(MachHeader64) == 32);
static_assert(sizeof(SegmentCommand64) == 72);
static_assert(sizeof(Section64) == 80);
static_assert(sizeof(Nlist64) == 16);

void* s_sysv_thunk = nullptr;
void* s_macos_dlsym = nullptr;
void* s_macos_getenv = nullptr;
void* s_macos_setenv = nullptr;
uintptr_t s_ntdll_so_base = 0;

void* get_sysv_thunk() {
    static std::once_flag s_thunk_once;
    std::call_once(s_thunk_once, []() {
        // Windows x64 ABI (caller): rcx = fn, rdx = a1, r8 = a2, r9 = a3
        // SysV AMD64 ABI (callee):  rdi = a1, rsi = a2, rdx = a3, rax = fn
        // Preserves Windows non-volatile XMM6-XMM15 and enforces 16-byte stack alignment.
        static const uint8_t thunk_code[] = {
            0x55,                                                 // push rbp
            0x48, 0x89, 0xe5,                                     // mov rbp, rsp
            0x57,                                                 // push rdi
            0x56,                                                 // push rsi
            0x48, 0x83, 0xe4, 0xf0,                               // and rsp, -0x10
            0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00,             // sub rsp, 0xa0
            0x0f, 0x29, 0x34, 0x24,                               // movaps [rsp], xmm6
            0x0f, 0x29, 0x7c, 0x24, 0x10,                         // movaps [rsp + 0x10], xmm7
            0x44, 0x0f, 0x29, 0x44, 0x24, 0x20,                   // movaps [rsp + 0x20], xmm8
            0x44, 0x0f, 0x29, 0x4c, 0x24, 0x30,                   // movaps [rsp + 0x30], xmm9
            0x44, 0x0f, 0x29, 0x54, 0x24, 0x40,                   // movaps [rsp + 0x40], xmm10
            0x44, 0x0f, 0x29, 0x5c, 0x24, 0x50,                   // movaps [rsp + 0x50], xmm11
            0x44, 0x0f, 0x29, 0x64, 0x24, 0x60,                   // movaps [rsp + 0x60], xmm12
            0x44, 0x0f, 0x29, 0x6c, 0x24, 0x70,                   // movaps [rsp + 0x70], xmm13
            0x44, 0x0f, 0x29, 0xb4, 0x24, 0x80, 0x00, 0x00, 0x00, // movaps [rsp + 0x80], xmm14
            0x44, 0x0f, 0x29, 0xbc, 0x24, 0x90, 0x00, 0x00, 0x00, // movaps [rsp + 0x90], xmm15
            0x48, 0x89, 0xc8,                                     // mov rax, rcx
            0x48, 0x89, 0xd7,                                     // mov rdi, rdx
            0x4c, 0x89, 0xc6,                                     // mov rsi, r8
            0x4c, 0x89, 0xca,                                     // mov rdx, r9
            0xff, 0xd0,                                           // call rax
            0x0f, 0x28, 0x34, 0x24,                               // movaps xmm6, [rsp]
            0x0f, 0x28, 0x7c, 0x24, 0x10,                         // movaps xmm7, [rsp + 0x10]
            0x44, 0x0f, 0x28, 0x44, 0x24, 0x20,                   // movaps xmm8, [rsp + 0x20]
            0x44, 0x0f, 0x28, 0x4c, 0x24, 0x30,                   // movaps xmm9, [rsp + 0x30]
            0x44, 0x0f, 0x28, 0x54, 0x24, 0x40,                   // movaps xmm10, [rsp + 0x40]
            0x44, 0x0f, 0x28, 0x5c, 0x24, 0x50,                   // movaps xmm11, [rsp + 0x50]
            0x44, 0x0f, 0x28, 0x64, 0x24, 0x60,                   // movaps xmm12, [rsp + 0x60]
            0x44, 0x0f, 0x28, 0x6c, 0x24, 0x70,                   // movaps xmm13, [rsp + 0x70]
            0x44, 0x0f, 0x28, 0xb4, 0x24, 0x80, 0x00, 0x00, 0x00, // movaps xmm14, [rsp + 0x80]
            0x44, 0x0f, 0x28, 0xbc, 0x24, 0x90, 0x00, 0x00, 0x00, // movaps xmm15, [rsp + 0x90]
            0x48, 0x8d, 0x65, 0xf0,                               // lea rsp, [rbp - 0x10]
            0x5e,                                                 // pop rsi
            0x5f,                                                 // pop rdi
            0x5d,                                                 // pop rbp
            0xc3                                                  // ret
        };

        void* mem = VirtualAlloc(nullptr, sizeof(thunk_code), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (mem != nullptr) {
            memcpy(mem, thunk_code, sizeof(thunk_code));
            DWORD old_prot = 0;
            if (VirtualProtect(mem, sizeof(thunk_code), PAGE_EXECUTE_READ, &old_prot)) {
                FlushInstructionCache(GetCurrentProcess(), mem, sizeof(thunk_code));
                s_sysv_thunk = mem;
            } else {
                VirtualFree(mem, 0, MEM_RELEASE);
            }
        }
    });
    return s_sysv_thunk;
}

uint64_t call_sysv(void* thunk, void* fn, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    if (thunk == nullptr || fn == nullptr) {
        return 0;
    }
    using thunk_fn_t = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t);
    return reinterpret_cast<thunk_fn_t>(thunk)(fn, a1, a2, a3);
}

bool seg_equals(const char seg[16], const char* name) {
    const size_t len = strlen(name);
    if (len > 16) {
        return false;
    }
    if (memcmp(seg, name, len) != 0) {
        return false;
    }
    return len == 16 || seg[len] == '\0';
}

bool read_uleb128(const uint8_t* buf, size_t size, size_t& pos, uint64_t& out) {
    out = 0;
    uint32_t shift = 0;
    while (pos < size) {
        const uint8_t b = buf[pos++];
        if (shift >= 64) {
            return false;
        }
        out |= static_cast<uint64_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) {
            return true;
        }
        shift += 7;
    }
    return false;
}

uintptr_t walk_backward_for_macho_header(uintptr_t addr, uint32_t max_pages) {
    if (addr < 0x10000ULL) {
        return 0;
    }
    uintptr_t page = addr & ~0xFFFULL;
    for (uint32_t i = 0; i < max_pages && page >= 0x10000ULL; ++i) {
        const auto* mh = reinterpret_cast<const MachHeader64*>(page);
        if (mh->magic == kMachHeader64Magic && mh->sizeofcmds > 0 && mh->ncmds > 0 && mh->ncmds < 4096) {
            return page;
        }
        page -= 0x1000;
    }
    return 0;
}

void resolve_macho_got_symbols(uintptr_t mh_addr, void** out_setenv, void** out_getenv, void** out_dlsym) {
    const auto* mh = reinterpret_cast<const MachHeader64*>(mh_addr);
    if (mh == nullptr || mh->magic != kMachHeader64Magic) {
        return;
    }

    uint64_t text_vmaddr = 0;
    bool has_text = false;
    uint64_t linkedit_vmaddr = 0;
    uint64_t linkedit_fileoff = 0;
    bool has_linkedit = false;
    const SymtabCommand* symtab_cmd = nullptr;
    const DysymtabCommand* dysymtab_cmd = nullptr;

    const uint8_t* lc_ptr = reinterpret_cast<const uint8_t*>(mh_addr + sizeof(MachHeader64));
    const uint8_t* lc_end = lc_ptr + mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds && lc_ptr + sizeof(LoadCommand) <= lc_end; ++i) {
        const auto* lc = reinterpret_cast<const LoadCommand*>(lc_ptr);
        if (lc->cmdsize < sizeof(LoadCommand) || lc_ptr + lc->cmdsize > lc_end) {
            break;
        }
        if (lc->cmd == kLcSegment64 && lc->cmdsize >= sizeof(SegmentCommand64)) {
            const auto* seg = reinterpret_cast<const SegmentCommand64*>(lc_ptr);
            if (seg_equals(seg->segname, "__TEXT")) {
                text_vmaddr = seg->vmaddr;
                has_text = true;
            } else if (seg_equals(seg->segname, "__LINKEDIT")) {
                linkedit_vmaddr = seg->vmaddr;
                linkedit_fileoff = seg->fileoff;
                has_linkedit = true;
            }
        } else if (lc->cmd == kLcSymtab && lc->cmdsize >= sizeof(SymtabCommand)) {
            symtab_cmd = reinterpret_cast<const SymtabCommand*>(lc_ptr);
        } else if (lc->cmd == kLcDysymtab && lc->cmdsize >= sizeof(DysymtabCommand)) {
            dysymtab_cmd = reinterpret_cast<const DysymtabCommand*>(lc_ptr);
        }
        lc_ptr += lc->cmdsize;
    }

    if (!has_text || !has_linkedit || symtab_cmd == nullptr || dysymtab_cmd == nullptr) {
        return;
    }

    const intptr_t slide = static_cast<intptr_t>(mh_addr) - static_cast<intptr_t>(text_vmaddr);
    const intptr_t linkedit_base = slide + static_cast<intptr_t>(linkedit_vmaddr) - static_cast<intptr_t>(linkedit_fileoff);
    const auto* symtab = reinterpret_cast<const Nlist64*>(linkedit_base + symtab_cmd->symoff);
    const char* strtab = reinterpret_cast<const char*>(linkedit_base + symtab_cmd->stroff);
    const auto* indirect_syms = reinterpret_cast<const uint32_t*>(linkedit_base + dysymtab_cmd->indirectsymoff);

    lc_ptr = reinterpret_cast<const uint8_t*>(mh_addr + sizeof(MachHeader64));
    for (uint32_t i = 0; i < mh->ncmds && lc_ptr + sizeof(LoadCommand) <= lc_end; ++i) {
        const auto* lc = reinterpret_cast<const LoadCommand*>(lc_ptr);
        if (lc->cmd == kLcSegment64 && lc->cmdsize >= sizeof(SegmentCommand64)) {
            const auto* seg = reinterpret_cast<const SegmentCommand64*>(lc_ptr);
            const auto* sects = reinterpret_cast<const Section64*>(lc_ptr + sizeof(SegmentCommand64));
            for (uint32_t s = 0; s < seg->nsects; ++s) {
                const auto& sect = sects[s];
                if ((sect.flags & kSectionTypeMask) != kSectionNonLazySymbolPointers) {
                    continue;
                }
                void** got_entries = reinterpret_cast<void**>(slide + static_cast<intptr_t>(sect.addr));
                const uint64_t count = sect.size / sizeof(void*);
                for (uint64_t idx = 0; idx < count; ++idx) {
                    if (sect.reserved1 + idx >= dysymtab_cmd->nindirectsyms) {
                        break;
                    }
                    const uint32_t sym_idx = indirect_syms[sect.reserved1 + idx];
                    if ((sym_idx & kIndirectSymbolLocalOrAbs) != 0 || sym_idx >= symtab_cmd->nsyms) {
                        continue;
                    }
                    const uint32_t strx = symtab[sym_idx].n_strx;
                    if (strx >= symtab_cmd->strsize) {
                        continue;
                    }
                    const char* name = strtab + strx;
                    if (out_setenv != nullptr && *out_setenv == nullptr && strcmp(name, "_setenv") == 0) {
                        *out_setenv = got_entries[idx];
                    } else if (out_getenv != nullptr && *out_getenv == nullptr && strcmp(name, "_getenv") == 0) {
                        *out_getenv = got_entries[idx];
                    } else if (out_dlsym != nullptr && *out_dlsym == nullptr && strcmp(name, "_dlsym") == 0) {
                        *out_dlsym = got_entries[idx];
                    }
                }
            }
        }
        lc_ptr += lc->cmdsize;
    }
}

void* resolve_macho_export_trie(uintptr_t mh_addr, const char* symbol_name) {
    const auto* mh = reinterpret_cast<const MachHeader64*>(mh_addr);
    if (mh == nullptr || mh->magic != kMachHeader64Magic || symbol_name == nullptr) {
        return nullptr;
    }

    uint64_t text_vmaddr = 0;
    bool has_text = false;
    uint64_t linkedit_vmaddr = 0;
    uint64_t linkedit_fileoff = 0;
    bool has_linkedit = false;
    uint32_t trie_off = 0;
    uint32_t trie_size = 0;

    const uint8_t* lc_ptr = reinterpret_cast<const uint8_t*>(mh_addr + sizeof(MachHeader64));
    const uint8_t* lc_end = lc_ptr + mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds && lc_ptr + sizeof(LoadCommand) <= lc_end; ++i) {
        const auto* lc = reinterpret_cast<const LoadCommand*>(lc_ptr);
        if (lc->cmdsize < sizeof(LoadCommand) || lc_ptr + lc->cmdsize > lc_end) {
            break;
        }
        if (lc->cmd == kLcSegment64 && lc->cmdsize >= sizeof(SegmentCommand64)) {
            const auto* seg = reinterpret_cast<const SegmentCommand64*>(lc_ptr);
            if (seg_equals(seg->segname, "__TEXT")) {
                text_vmaddr = seg->vmaddr;
                has_text = true;
            } else if (seg_equals(seg->segname, "__LINKEDIT")) {
                linkedit_vmaddr = seg->vmaddr;
                linkedit_fileoff = seg->fileoff;
                has_linkedit = true;
            }
        } else if (lc->cmd == kLcDyldExportsTrie && lc->cmdsize >= sizeof(LinkeditDataCommand)) {
            const auto* exp = reinterpret_cast<const LinkeditDataCommand*>(lc_ptr);
            trie_off = exp->dataoff;
            trie_size = exp->datasize;
        } else if ((lc->cmd == kLcDyldInfo || lc->cmd == kLcDyldInfoOnly) && lc->cmdsize >= sizeof(DyldInfoCommand) && trie_off == 0) {
            const auto* di = reinterpret_cast<const DyldInfoCommand*>(lc_ptr);
            trie_off = di->export_off;
            trie_size = di->export_size;
        }
        lc_ptr += lc->cmdsize;
    }

    if (!has_text || !has_linkedit || trie_off == 0 || trie_size == 0) {
        return nullptr;
    }

    const intptr_t slide = static_cast<intptr_t>(mh_addr) - static_cast<intptr_t>(text_vmaddr);
    const intptr_t linkedit_base = slide + static_cast<intptr_t>(linkedit_vmaddr) - static_cast<intptr_t>(linkedit_fileoff);
    const uint8_t* trie = reinterpret_cast<const uint8_t*>(linkedit_base + trie_off);

    const size_t sym_len = strlen(symbol_name);
    size_t sym_pos = 0;
    size_t pos = 0;

    while (pos < trie_size) {
        uint64_t term_size = 0;
        if (!read_uleb128(trie, trie_size, pos, term_size)) {
            return nullptr;
        }
        if (sym_pos == sym_len) {
            if (term_size == 0 || pos + term_size > trie_size) {
                return nullptr;
            }
            uint64_t flags = 0;
            uint64_t addr_off = 0;
            size_t info_pos = pos;
            if (!read_uleb128(trie, trie_size, info_pos, flags)) {
                return nullptr;
            }
            // Skip re-export (0x8) and stub/resolver (0x10)
            if ((flags & 0x18u) != 0) {
                return nullptr;
            }
            if (!read_uleb128(trie, trie_size, info_pos, addr_off)) {
                return nullptr;
            }
            return reinterpret_cast<void*>(mh_addr + addr_off);
        }
        if (pos + term_size >= trie_size) {
            return nullptr;
        }
        pos += static_cast<size_t>(term_size);

        const uint8_t child_count = trie[pos++];
        bool matched = false;
        for (uint8_t c = 0; c < child_count && pos < trie_size; ++c) {
            const char* edge = reinterpret_cast<const char*>(trie + pos);
            size_t edge_len = 0;
            while (pos + edge_len < trie_size && trie[pos + edge_len] != 0) {
                ++edge_len;
            }
            if (pos + edge_len >= trie_size) {
                return nullptr;
            }
            pos += edge_len + 1;

            uint64_t child_off = 0;
            if (!read_uleb128(trie, trie_size, pos, child_off)) {
                return nullptr;
            }

            if (sym_len - sym_pos >= edge_len && memcmp(symbol_name + sym_pos, edge, edge_len) == 0) {
                sym_pos += edge_len;
                pos = static_cast<size_t>(child_off);
                matched = true;
                break;
            }
        }
        if (!matched) {
            return nullptr;
        }
    }
    return nullptr;
}

bool __declspec(noinline) resolve_host_libc_seh(
    void* thunk,
    uintptr_t& out_ntdll_base,
    void*& out_dlsym,
    void*& out_getenv,
    void*& out_setenv,
    int& out_setenv_res,
    char (&out_env_val)[32])
{
    __try {
        const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll == nullptr) {
            return false;
        }

        void* disp_slot = reinterpret_cast<void*>(GetProcAddress(ntdll, "__wine_unix_call_dispatcher"));
        if (disp_slot == nullptr) {
            disp_slot = reinterpret_cast<void*>(GetProcAddress(ntdll, "__wine_syscall_dispatcher"));
        }
        if (disp_slot == nullptr) {
            return false;
        }

        const uintptr_t unix_entry = *reinterpret_cast<const uintptr_t*>(disp_slot);
        const uintptr_t ntdll_base = walk_backward_for_macho_header(unix_entry, 512);
        if (ntdll_base == 0) {
            return false;
        }
        out_ntdll_base = ntdll_base;

        void* setenv_fn = nullptr;
        void* getenv_fn = nullptr;
        void* dlsym_fn = nullptr;
        resolve_macho_got_symbols(ntdll_base, &setenv_fn, &getenv_fn, &dlsym_fn);

        if (dlsym_fn != nullptr) {
            if (setenv_fn == nullptr) {
                setenv_fn = reinterpret_cast<void*>(call_sysv(thunk, dlsym_fn, kRtldDefault, reinterpret_cast<uint64_t>("setenv")));
            }
            if (getenv_fn == nullptr) {
                getenv_fn = reinterpret_cast<void*>(call_sysv(thunk, dlsym_fn, kRtldDefault, reinterpret_cast<uint64_t>("getenv")));
            }
        }

        if (setenv_fn == nullptr && getenv_fn != nullptr) {
            const uintptr_t libsys_base = walk_backward_for_macho_header(reinterpret_cast<uintptr_t>(getenv_fn), 4096);
            if (libsys_base != 0) {
                setenv_fn = resolve_macho_export_trie(libsys_base, "_setenv");
            }
        }

        out_dlsym = dlsym_fn;
        out_getenv = getenv_fn;
        out_setenv = setenv_fn;

        if (setenv_fn != nullptr) {
            out_setenv_res = static_cast<int>(call_sysv(
                thunk,
                setenv_fn,
                reinterpret_cast<uint64_t>("D3DM_MTL4"),
                reinterpret_cast<uint64_t>("0"),
                0));
        }

        if (getenv_fn != nullptr) {
            const char* env_val = reinterpret_cast<const char*>(call_sysv(
                thunk,
                getenv_fn,
                reinterpret_cast<uint64_t>("D3DM_MTL4")));
            if (env_val != nullptr) {
                strncpy(out_env_val, env_val, sizeof(out_env_val) - 1);
                out_env_val[sizeof(out_env_val) - 1] = '\0';
            }
        }

        return setenv_fn != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool __declspec(noinline) verify_d3dmetal_seh(
    void* thunk,
    void* dlsym_fn,
    void* getenv_fn,
    uintptr_t& out_d3dmetal_base,
    uintptr_t& out_mtl4_addr,
    uint8_t& out_prior_val,
    uint8_t& out_final_val,
    bool& out_patchedfallback)
{
    __try {
        if (thunk == nullptr || dlsym_fn == nullptr) {
            return false;
        }

        void* fn_count = reinterpret_cast<void*>(call_sysv(thunk, dlsym_fn, kRtldDefault, reinterpret_cast<uint64_t>("_dyld_image_count")));
        void* fn_hdr = reinterpret_cast<void*>(call_sysv(thunk, dlsym_fn, kRtldDefault, reinterpret_cast<uint64_t>("_dyld_get_image_header")));
        void* fn_name = reinterpret_cast<void*>(call_sysv(thunk, dlsym_fn, kRtldDefault, reinterpret_cast<uint64_t>("_dyld_get_image_name")));
        if (fn_count == nullptr || fn_hdr == nullptr || fn_name == nullptr) {
            return false;
        }

        uintptr_t d3dmetal_base = 0;
        const uint32_t image_count = static_cast<uint32_t>(call_sysv(thunk, fn_count));
        for (uint32_t i = 0; i < image_count; ++i) {
            const char* img_name = reinterpret_cast<const char*>(call_sysv(thunk, fn_name, i));
            if (img_name != nullptr && strstr(img_name, "D3DMetal") != nullptr) {
                d3dmetal_base = static_cast<uintptr_t>(call_sysv(thunk, fn_hdr, i));
                break;
            }
        }

        if (d3dmetal_base == 0) {
            return false;
        }

        const auto* mh = reinterpret_cast<const MachHeader64*>(d3dmetal_base);
        if (mh->magic != kMachHeader64Magic) {
            return false;
        }
        out_d3dmetal_base = d3dmetal_base;

        uint64_t text_vmaddr = 0;
        bool has_text_seg = false;
        uint64_t text_sect_addr = 0;
        uint64_t text_sect_size = 0;
        uint64_t cstr_sect_addr = 0;
        uint64_t cstr_sect_size = 0;

        const uint8_t* lc_ptr = reinterpret_cast<const uint8_t*>(d3dmetal_base + sizeof(MachHeader64));
        const uint8_t* lc_end = lc_ptr + mh->sizeofcmds;
        for (uint32_t i = 0; i < mh->ncmds && lc_ptr + sizeof(LoadCommand) <= lc_end; ++i) {
            const auto* lc = reinterpret_cast<const LoadCommand*>(lc_ptr);
            if (lc->cmdsize < sizeof(LoadCommand) || lc_ptr + lc->cmdsize > lc_end) {
                break;
            }
            if (lc->cmd == kLcSegment64 && lc->cmdsize >= sizeof(SegmentCommand64)) {
                const auto* seg = reinterpret_cast<const SegmentCommand64*>(lc_ptr);
                if (seg_equals(seg->segname, "__TEXT")) {
                    text_vmaddr = seg->vmaddr;
                    has_text_seg = true;
                }
                const auto* sects = reinterpret_cast<const Section64*>(lc_ptr + sizeof(SegmentCommand64));
                for (uint32_t s = 0; s < seg->nsects; ++s) {
                    const auto& sect = sects[s];
                    if (seg_equals(sect.segname, "__TEXT") && seg_equals(sect.sectname, "__text")) {
                        text_sect_addr = sect.addr;
                        text_sect_size = sect.size;
                    } else if (seg_equals(sect.segname, "__TEXT") && seg_equals(sect.sectname, "__cstring")) {
                        cstr_sect_addr = sect.addr;
                        cstr_sect_size = sect.size;
                    }
                }
            }
            lc_ptr += lc->cmdsize;
        }

        if (!has_text_seg || text_sect_size < 7 || cstr_sect_size < 10) {
            return false;
        }

        const intptr_t slide = static_cast<intptr_t>(d3dmetal_base) - static_cast<intptr_t>(text_vmaddr);
        static const char target_str[] = "D3DM_MTL4";
        constexpr size_t target_len = sizeof(target_str);

        uintptr_t str_addr = 0;
        const uint8_t* cstr_begin = reinterpret_cast<const uint8_t*>(slide + static_cast<intptr_t>(cstr_sect_addr));
        const uint8_t* cstr_end = cstr_begin + cstr_sect_size - target_len;
        for (const uint8_t* p = cstr_begin; p <= cstr_end; ++p) {
            if (*p == 'D' && memcmp(p, target_str, target_len) == 0) {
                str_addr = reinterpret_cast<uintptr_t>(p);
                break;
            }
        }
        if (str_addr == 0) {
            return false;
        }

        const uint8_t* lea_instr = nullptr;
        const uint8_t* text_begin = reinterpret_cast<const uint8_t*>(slide + static_cast<intptr_t>(text_sect_addr));
        const uint8_t* text_end = text_begin + text_sect_size - 7;
        for (const uint8_t* p = text_begin; p <= text_end; ++p) {
            if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x3D) {
                const int32_t disp = *reinterpret_cast<const int32_t*>(p + 3);
                if (reinterpret_cast<uintptr_t>(p) + 7 + disp == str_addr) {
                    lea_instr = p;
                    break;
                }
            }
        }
        if (lea_instr == nullptr) {
            return false;
        }

        uint8_t* mtl4EV_addr = nullptr;
        for (int i = 7; i < 48 && lea_instr + i + 6 <= text_begin + text_sect_size; ++i) {
            const uint8_t* p = lea_instr + i;
            if (p[0] == 0x88 && p[1] == 0x1D) {
                const int32_t disp = *reinterpret_cast<const int32_t*>(p + 2);
                mtl4EV_addr = reinterpret_cast<uint8_t*>(const_cast<uint8_t*>(p) + 6 + disp);
                break;
            }
        }
        if (mtl4EV_addr == nullptr) {
            return false;
        }

        out_mtl4_addr = reinterpret_cast<uintptr_t>(mtl4EV_addr);
        out_prior_val = *mtl4EV_addr;
        out_final_val = out_prior_val;

        // Only force-clear mtl4EV if host env did not explicitly request D3DM_MTL4 != 0.
        bool user_requested_mtl4 = false;
        if (getenv_fn != nullptr) {
            const char* val = reinterpret_cast<const char*>(call_sysv(thunk, getenv_fn, reinterpret_cast<uint64_t>("D3DM_MTL4")));
            if (val != nullptr && strcmp(val, "0") != 0 && val[0] != '\0') {
                user_requested_mtl4 = true;
            }
        }

        if (out_prior_val != 0 && !user_requested_mtl4) {
            DWORD old_prot = 0;
            if (VirtualProtect(mtl4EV_addr, 1, PAGE_READWRITE, &old_prot)) {
                *mtl4EV_addr = 0;
                out_final_val = 0;
                out_patchedfallback = true;
                VirtualProtect(mtl4EV_addr, 1, old_prot, &old_prot);
            }
        }

        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool should_force_metal3() {
    char buf[16]{};
    const DWORD len = GetEnvironmentVariableA("REFRAMEWORK_FORCE_METAL3", buf, sizeof(buf));
    return len > 0 && len < sizeof(buf) && (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T');
}
} // namespace

void DarwinHost::initialize() {
    if (!is_darwin()) {
        return;
    }

    static std::once_flag s_init_once;
    std::call_once(s_init_once, []() {
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        spdlog::info("DarwinHost: Configured SetThreadExecutionState (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)");
        enforce_metal3_mode();
    });
}

bool DarwinHost::is_wine() {
    static const bool result = []() {
        const auto ntdll = GetModuleHandleA("ntdll.dll");
        const bool wine = (ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr);
        if (wine) {
            spdlog::info("DarwinHost: Wine environment detected");
        }
        return wine;
    }();
    return result;
}

bool DarwinHost::is_darwin() {
    static const bool result = []() {
        const auto ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll == nullptr || GetProcAddress(ntdll, "wine_get_version") == nullptr) {
            return false;
        }

        using wine_get_host_version_t = void(__cdecl*)(const char** sysname, const char** release);
        const auto wine_get_host_version = (wine_get_host_version_t)GetProcAddress(ntdll, "wine_get_host_version");
        if (wine_get_host_version == nullptr) {
            return false;
        }

        const char* sysname = nullptr;
        const char* release = nullptr;
        wine_get_host_version(&sysname, &release);

        const bool is_macos = (sysname != nullptr && strcmp(sysname, "Darwin") == 0);
        spdlog::info("DarwinHost: Wine host OS: {} {} -> macOS/Darwin paths {}",
            sysname != nullptr ? sysname : "?",
            release != nullptr ? release : "?",
            is_macos ? "enabled" : "disabled");

        return is_macos;
    }();
    return result;
}

// Isolated on purpose: functions containing __try/__except cannot contain C++ objects that need unwinding.
static HRESULT __declspec(noinline) d3d12_create_device_safe_seh(
    decltype(D3D12CreateDevice)* fn,
    IUnknown* adapter,
    D3D_FEATURE_LEVEL feature_level,
    ID3D12Device** device_out) 
{
    HRESULT hr = E_FAIL;
    __try {
        hr = fn(adapter, feature_level, IID_PPV_ARGS(device_out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = E_FAIL;
    }
    return hr;
}

HRESULT DarwinHost::create_d3d12_dummy_device(
    decltype(D3D12CreateDevice)* create_fn,
    D3D_FEATURE_LEVEL feature_level,
    ID3D12Device** out_device)
{
    if (create_fn == nullptr || out_device == nullptr) {
        return E_INVALIDARG;
    }

    const auto dxgi_module = LoadLibraryA("dxgi.dll");
    if (dxgi_module == nullptr) {
        spdlog::error("DarwinHost: Failed to load dxgi.dll");
        return E_FAIL;
    }

    auto create_dxgi_factory = (decltype(CreateDXGIFactory)*)GetProcAddress(dxgi_module, "CreateDXGIFactory");
    if (create_dxgi_factory == nullptr) {
        spdlog::error("DarwinHost: Failed to get CreateDXGIFactory export");
        return E_FAIL;
    }

    IDXGIFactory4* factory{ nullptr };
    if (FAILED(create_dxgi_factory(IID_PPV_ARGS(&factory))) || factory == nullptr) {
        spdlog::error("DarwinHost: Failed to create DXGI factory for adapter enumeration");
        return E_FAIL;
    }

    IDXGIAdapter* adapter{ nullptr };
    const auto enum_hr = factory->EnumAdapters(0, &adapter);
    factory->Release();

    if (FAILED(enum_hr) || adapter == nullptr) {
        spdlog::error("DarwinHost: No DXGI adapter found ({:x})", (uint32_t)enum_hr);
        return E_FAIL;
    }

    spdlog::info("DarwinHost: Calling D3D12CreateDevice with enumerated adapter {:x}", (uintptr_t)adapter);
    const auto hr = d3d12_create_device_safe_seh(create_fn, adapter, feature_level, out_device);
    adapter->Release();

    if (FAILED(hr)) {
        spdlog::error("DarwinHost: D3D12CreateDevice failed: 0x{:X}", (uint32_t)hr);
    }

    return hr;
}

uint32_t DarwinHost::find_command_queue_offset(void* swapchain, IUnknown* command_queue) {
    if (swapchain == nullptr) {
        return 0;
    }

    // 1. Dynamic scan: If command_queue is known, scan swapchain memory for the exact pointer match
    if (command_queue != nullptr) {
        for (uint32_t off = 0; off <= 0x800 - sizeof(void*); off += sizeof(void*)) {
            const auto addr = (uintptr_t)swapchain + off;
            if (IsBadReadPtr((void*)addr, sizeof(void*))) {
                break;
            }
            auto cand = *(void**)addr;
            if (cand == (void*)command_queue) {
                spdlog::info("DarwinHost: Dynamically matched exact command queue at offset 0x{:X}", off);
                return off;
            }
        }
    }

    // 2. Standard D3DMetal fallback (0x4C8 across GPTK 3/4)
    constexpr uint32_t WINE_D3DMETAL_CQ_OFFSET = 0x4C8;
    const auto d3dmetal_base = (uintptr_t)swapchain + WINE_D3DMETAL_CQ_OFFSET;
    if (!IsBadReadPtr((void*)d3dmetal_base, sizeof(void*))) {
        auto candidate = *(void**)d3dmetal_base;
        if (candidate != nullptr && !IsBadReadPtr(candidate, sizeof(void*))) {
            spdlog::info("DarwinHost: Using verified D3DMetal command queue offset: 0x{:X}", WINE_D3DMETAL_CQ_OFFSET);
            return WINE_D3DMETAL_CQ_OFFSET;
        }
    }

    spdlog::warn("DarwinHost: Defaulting to standard D3DMetal command queue offset: 0x{:X}", WINE_D3DMETAL_CQ_OFFSET);
    return WINE_D3DMETAL_CQ_OFFSET;
}

bool DarwinHost::matches_command_queue(IUnknown* candidate, IUnknown* command_queue) {
    if (candidate == nullptr || command_queue == nullptr) {
        return false;
    }
    return candidate == command_queue;
}

intptr_t DarwinHost::calculate_command_queue_delta(void* swapchain, uint32_t offset, IUnknown* command_queue) {
    if (swapchain == nullptr || offset < sizeof(void*) || (offset % sizeof(void*) != 0) || command_queue == nullptr) {
        return 0;
    }
    const auto raw_ptr_addr = (uintptr_t)swapchain + offset;
    if (IsBadReadPtr((void*)raw_ptr_addr, sizeof(void*))) {
        return 0;
    }
    const auto raw = *(uintptr_t*)raw_ptr_addr;
    if (raw == 0 || IsBadReadPtr((void*)raw, sizeof(void*))) {
        return 0;
    }
    const auto delta = (intptr_t)((uintptr_t)command_queue - raw);
    if (delta != 0) {
        spdlog::info("DarwinHost: Command queue pointer delta: 0x{:X}", (uintptr_t)delta);
    }
    return delta;
}

bool DarwinHost::is_host_darwin_pointer(uintptr_t addr) {
    if (addr < 0x10000ULL) {
        return true;
    }
    // Native Objective-C / Metal heap objects: 0x600000000000+
    if ((addr & 0xFFFFFF0000000000ULL) == 0x600000000000ULL) {
        return true;
    }
    // macOS thread stacks: 0x700000000000 - 0x7000ffffffff
    if ((addr & 0xFFFFFF0000000000ULL) == 0x700000000000ULL) {
        return true;
    }
    // macOS dyld shared cache / system dylibs: 0x7ff800000000+
    if (addr >= 0x7ff800000000ULL) {
        return true;
    }
    return false;
}

bool DarwinHost::is_valid_command_queue(ID3D12CommandQueue* queue) {
    if (queue == nullptr || IsBadReadPtr(queue, sizeof(void*))) {
        return false;
    }
    const auto ptr_val = (uintptr_t)queue;
    if (is_darwin() && is_host_darwin_pointer(ptr_val)) {
        return false;
    }
    void** vtable = *(void***)queue;
    if (vtable == nullptr || IsBadReadPtr(vtable, sizeof(void*) * 10)) {
        return false;
    }
    const auto vtable_val = (uintptr_t)vtable;
    if (is_darwin() && is_host_darwin_pointer(vtable_val)) {
        return false;
    }
    const auto fn = (uintptr_t)vtable[0];
    if (is_darwin() && is_host_darwin_pointer(fn)) {
        return false;
    }
    bool ok = false;
    __try {
        IUnknown* test = nullptr;
        if (SUCCEEDED(queue->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&test)) && test != nullptr) {
            test->Release();
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

uint32_t DarwinHost::get_default_menu_key() {
    return is_darwin() ? VK_OEM_3 : VK_INSERT;
}

bool DarwinHost::is_mac_command_key(uint32_t vk_code) {
    return is_darwin() && (vk_code == VK_LWIN || vk_code == VK_RWIN);
}

void DarwinHost::enforce_metal3_mode() {
    if (!is_darwin() || !should_force_metal3()) {
        return;
    }

    static std::once_flag s_enforce_once;
    std::call_once(s_enforce_once, []() {
        void* thunk = get_sysv_thunk();
        if (thunk == nullptr) {
            spdlog::warn("DarwinHost: Failed to allocate SysV thunk for Metal 3 enforcement");
            return;
        }

        uintptr_t ntdll_base = 0;
        void* dlsym_fn = nullptr;
        void* getenv_fn = nullptr;
        void* setenv_fn = nullptr;
        int setenv_res = -1;
        char env_val[32] = "(unset)";

        const bool ok = resolve_host_libc_seh(thunk, ntdll_base, dlsym_fn, getenv_fn, setenv_fn, setenv_res, env_val);
        s_ntdll_so_base = ntdll_base;
        s_macos_dlsym = dlsym_fn;
        s_macos_getenv = getenv_fn;
        s_macos_setenv = setenv_fn;

        if (ok) {
            spdlog::info(
                "DarwinHost: Enforced pre-device Metal 3 env (ntdll.so: {:p}, setenv: {:p} -> {}, D3DM_MTL4={})",
                reinterpret_cast<void*>(ntdll_base), setenv_fn, setenv_res, env_val);
        } else {
            spdlog::warn(
                "DarwinHost: Failed zero-offset setenv resolution (ntdll.so: {:p}, dlsym: {:p}, getenv: {:p}, setenv: {:p})",
                reinterpret_cast<void*>(ntdll_base), dlsym_fn, getenv_fn, setenv_fn);
        }
    });
}

void DarwinHost::verify_metal3_mode() {
    if (!is_darwin()) {
        return;
    }

    if (!should_force_metal3()) {
        static std::once_flag s_preserve_once;
        std::call_once(s_preserve_once, []() {
            spdlog::info("DarwinHost: Preserving native D3DMetal mode (set REFRAMEWORK_FORCE_METAL3=1 to force Metal 3)");
        });
        return;
    }

    static std::once_flag s_verify_once;
    std::call_once(s_verify_once, []() {
        enforce_metal3_mode();

        void* thunk = get_sysv_thunk();
        uintptr_t d3dmetal_base = 0;
        uintptr_t mtl4_addr = 0;
        uint8_t prior_val = 0;
        uint8_t final_val = 0;
        bool patched_fallback = false;

        if (verify_d3dmetal_seh(thunk, s_macos_dlsym, s_macos_getenv, d3dmetal_base, mtl4_addr, prior_val, final_val, patched_fallback)) {
            spdlog::info(
                "DarwinHost: Verified D3DMetal (base: {:p}, mtl4EV: {:p}, prior: {}, final: {}{})",
                reinterpret_cast<void*>(d3dmetal_base),
                reinterpret_cast<void*>(mtl4_addr),
                prior_val,
                final_val,
                patched_fallback ? ", cleared fallback" : "");
        } else {
            spdlog::info("DarwinHost: D3DMetal mtl4EV verification skipped or symbol not present");
        }
    });
}

bool DarwinHost::get_vram_budget_caps(uint64_t& local_cap_bytes, uint64_t& shared_cap_bytes) {
    if (!is_darwin()) {
        return false;
    }

    char env_buf[32]{};
    const DWORD env_len = GetEnvironmentVariableA("REFRAMEWORK_VRAM_BUDGET_MB", env_buf, sizeof(env_buf));
    if (env_len > 0 && env_len < sizeof(env_buf)) {
        if (_stricmp(env_buf, "0") == 0 || _stricmp(env_buf, "off") == 0 ||
            _stricmp(env_buf, "false") == 0 || _stricmp(env_buf, "disable") == 0) {
            static std::once_flag s_disabled_log_once;
            std::call_once(s_disabled_log_once, []() {
                spdlog::info("DarwinHost: DXGI VRAM budget cap disabled via REFRAMEWORK_VRAM_BUDGET_MB={}", "off");
            });
            return false;
        }

        char* end_ptr = nullptr;
        unsigned long long mb = std::strtoull(env_buf, &end_ptr, 10);
        if (end_ptr != env_buf && mb > 0) {
            mb = (std::clamp<unsigned long long>)(mb, 2048ULL, 131072ULL);
            local_cap_bytes = mb * 1024ULL * 1024ULL;
            shared_cap_bytes = (std::min<uint64_t>)(2048ULL * 1024ULL * 1024ULL, local_cap_bytes / 2ULL);
            return true;
        }
    }

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (!GlobalMemoryStatusEx(&mem) || mem.ullTotalPhys == 0) {
        return false;
    }

    const uint64_t total_phys = mem.ullTotalPhys;
    // Leave ~10.5 GiB headroom on Apple Silicon unified memory for macOS, Rosetta, Wine, and CPU heaps.
    constexpr uint64_t kReserveBytes = 10752ULL * 1024ULL * 1024ULL;
    constexpr uint64_t kMinLocalBytes = 3584ULL * 1024ULL * 1024ULL;
    local_cap_bytes = (total_phys > kReserveBytes + kMinLocalBytes) ? (total_phys - kReserveBytes) : kMinLocalBytes;
    shared_cap_bytes = (total_phys <= 20ULL * 1024ULL * 1024ULL * 1024ULL) ? (2048ULL * 1024ULL * 1024ULL) : (total_phys / 4ULL);
    return true;
}

} // namespace utility
