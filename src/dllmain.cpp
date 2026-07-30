// MistressDMC — in-game trainer overlay for Devil May Cry 4 Special Edition.
// dinput8.dll proxy (forwards to the system dinput8.dll at runtime). Hooks IDXGISwapChain::Present
// via kiero+MinHook and renders a Dear ImGui trainer through the DX10 backend.
//
// Cheats are ported from the DMC4SE-IW Cheat Engine table (2015 build, module
// base 0x400000). Two kinds:
//   * code patches  - overwrite N bytes at module+offset (verify original first)
//   * code caves    - alloc executable memory, copy relocatable template, fix up
//                     external rel32 fields, then jmp the hook site into it.
// Wrong offsets/build differences grey the cheat out instead of crashing.
// Toggle the menu with the O key. Logs to C:\overlay.log.

#include <Windows.h>
#include <mmsystem.h>
#include <dxgi.h>
#include <d3d10.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <functional>
#include <algorithm>
#include "imgui.h"
#include "backends/imgui_impl_dx10.h"
#include "backends/imgui_impl_win32.h"
#include "kiero.h"
#include "MinHook.h"
#include "cheats_gen.h"
#include "boot_stub.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------- logging
static FILE* g_log = nullptr;
static void logf(const char* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------- dinput8 proxy
// We export the same five entry points as the real dinput8.dll and forward them
// to the PLAYER'S OWN system dinput8.dll, loaded by full path at runtime. This
// means the mod is a single drag-in file: no separate "dinput8_real.dll" to ship
// or rename, and no "missing dinput8_real.dll" error on Windows. The 32-bit
// file-system redirector points System32 at SysWOW64 for this 32-bit process, so
// GetSystemDirectory resolves the correct 32-bit dinput8.dll on real Windows
// (and Wine resolves its own builtin the same way).
static HMODULE g_realDInput = nullptr;
static FARPROC realProc(const char* name) {
    if (!g_realDInput) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n && n + 13 < MAX_PATH) {
            lstrcatA(path, "\\dinput8.dll");
            g_realDInput = LoadLibraryA(path);
        }
        if (!g_realDInput) logf("[proxy] failed to load system dinput8.dll (%s)", path);
    }
    return g_realDInput ? GetProcAddress(g_realDInput, name) : nullptr;
}
static void replayHookDInput(void* idi8);   // fwd: arm keyboard record/playback (defined below)
extern "C" {
HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, const GUID& riid, void** out, void* unk) {
    typedef HRESULT (WINAPI *Fn)(HINSTANCE, DWORD, const GUID&, void**, void*);
    static Fn fn = (Fn)realProc("DirectInput8Create");
    HRESULT hr = fn ? fn(hinst, ver, riid, out, unk) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) replayHookDInput(*out);   // hook CreateDevice -> catch the keyboard
    return hr;
}
HRESULT WINAPI DllCanUnloadNow(void) {
    typedef HRESULT (WINAPI *Fn)(void);
    static Fn fn = (Fn)realProc("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}
HRESULT WINAPI DllGetClassObject(const GUID& rclsid, const GUID& riid, void** ppv) {
    typedef HRESULT (WINAPI *Fn)(const GUID&, const GUID&, void**);
    static Fn fn = (Fn)realProc("DllGetClassObject");
    return fn ? fn(rclsid, riid, ppv) : E_FAIL;
}
HRESULT WINAPI DllRegisterServer(void) {
    typedef HRESULT (WINAPI *Fn)(void);
    static Fn fn = (Fn)realProc("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}
HRESULT WINAPI DllUnregisterServer(void) {
    typedef HRESULT (WINAPI *Fn)(void);
    static Fn fn = (Fn)realProc("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}
}

// ---------------------------------------------------------------- model
struct Patch { uint32_t off; std::vector<uint8_t> patch, orig, saved; };
struct Cheat {
    std::string name, cat;
    bool active = false, unavailable = false;
    // code-patch type
    std::vector<Patch> patches;
    // code-cave type
    bool isCave = false;
    uint32_t hookOff = 0, B0 = 0;
    int slot = 0, size = 0;
    std::vector<uint8_t> caveOrig, tmpl, hookSaved;
    std::vector<int> rel;
    void* caveMem = nullptr;
};
static std::vector<Cheat> g_cheats;
static uintptr_t g_base = 0;
static size_t    g_modSize = 0;

void addPatch(const char* cat, const char* name, std::vector<GenPatch> ps) {
    Cheat c; c.cat = cat; c.name = name;
    for (auto& p : ps) c.patches.push_back({p.off, p.patch, p.orig, {}});
    g_cheats.push_back(std::move(c));
}
void addCave(const char* cat, const char* name, uint32_t hookOff, int slot,
             std::vector<uint8_t> orig, std::vector<uint8_t> tmpl,
             uint32_t B0, std::vector<int> rel, int size) {
    Cheat c; c.cat = cat; c.name = name; c.isCave = true;
    c.hookOff = hookOff; c.slot = slot; c.caveOrig = std::move(orig);
    c.tmpl = std::move(tmpl); c.B0 = B0; c.rel = std::move(rel); c.size = size;
    g_cheats.push_back(std::move(c));
}

// ---------------------------------------------------------------- patch engine
static void initModule() {
    g_base = (uintptr_t)GetModuleHandleA(NULL);
    auto dos = (IMAGE_DOS_HEADER*)g_base;
    auto nt  = (IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew);
    g_modSize = nt->OptionalHeader.SizeOfImage;
    logf("[module] base=%p size=%zu", (void*)g_base, g_modSize);
}
static bool inModule(uintptr_t a, size_t n) {
    return g_base && a >= g_base && (a + n) <= (g_base + g_modSize);
}
static bool writeBytes(uintptr_t addr, const uint8_t* data, size_t n) {
    DWORD old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) {
        logf("[write] VirtualProtect FAILED @%p n=%zu err=%lu", (void*)addr, n, (unsigned long)GetLastError());
        return false;
    }
    memcpy((void*)addr, data, n);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    // Force the x86->ARM translator (CrossOver/Rosetta) to DROP any cached
    // translation of this code. In-process byte writes land in memory but the
    // translator keeps executing its cached translation of the ORIGINAL bytes, so
    // the patch appears to do nothing. Cross-process trainers get
    // this invalidation for free via WriteProcessMemory; in-process we trigger it
    // by bouncing the page non-executable -> executable, which makes the translator
    // re-translate from the patched bytes on next execution.
    DWORD t1, t2;
    VirtualProtect((void*)addr, n, PAGE_READWRITE, &t1);           // non-exec: invalidate xlation
    VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &t2);   // executable again
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    DWORD tmp;
    VirtualProtect((void*)addr, n, old, &tmp);                     // restore original protection
    bool stuck = memcmp((void*)addr, data, n) == 0;
    if (!stuck) logf("[write] VERIFY MISMATCH @%p -- bytes did NOT stick in memory", (void*)addr);
    return stuck;
}

// Reconcile a patch against the actual bytes in THIS build. Fills out[] with the
// bytes to write and *len with how many. Returns false if it can't be applied safely.
static bool resolvePatch(const Patch& p, uint8_t* out, size_t* len) {
    uintptr_t a = g_base + p.off;
    if (!inModule(a, p.patch.size())) return false;
    uint8_t* act = (uint8_t*)a;
    // exact match (or no expected original) -> apply patch verbatim
    if (p.orig.empty() || memcmp(act, p.orig.data(), p.orig.size()) == 0) {
        memcpy(out, p.patch.data(), p.patch.size()); *len = p.patch.size(); return true;
    }
    // build differs: try to reconcile common cases safely.
    // (A) CT assumed a NEAR jcc->jmp (E9.. , 6 bytes) but this build has a SHORT jcc
    //     (0x70-0x7F, 2 bytes). Emit a 2-byte short jmp to the SAME target.
    if (p.patch.size() >= 2 && p.patch[0] == 0xE9 && act[0] >= 0x70 && act[0] <= 0x7F) {
        out[0] = 0xEB; out[1] = act[1]; *len = 2; return true;
    }
    // (B) same-length instruction, only the encoding/operand differs (e.g. mov ecx,ebx
    //     as 89 D9 vs 8B CB, or a spawn mov whose absolute data address shifted).
    //     Safe because we overwrite exactly one instruction's worth of bytes.
    if (p.patch.size() == p.orig.size() &&
        (act[0] == p.orig[0] || act[0] == 0x8B || act[0] == 0x89 || act[0] == 0xA1)) {
        memcpy(out, p.patch.data(), p.patch.size()); *len = p.patch.size(); return true;
    }
    return false;
}

static bool enablePatchCheat(Cheat& c) {
    uint8_t buf[32]; size_t len;
    for (auto& p : c.patches) if (!resolvePatch(p, buf, &len)) return false;
    bool allStuck = true;
    for (auto& p : c.patches) {
        uintptr_t a = g_base + p.off;
        resolvePatch(p, buf, &len);
        p.saved.assign((uint8_t*)a, (uint8_t*)a + len);
        if (!writeBytes(a, buf, len)) allStuck = false;
    }
    logf("[verify] patch '%s' bytes-in-memory=%s", c.name.c_str(), allStuck ? "YES" : "NO");
    return true;
}
static void disablePatchCheat(Cheat& c) {
    for (auto& p : c.patches)
        if (!p.saved.empty()) writeBytes(g_base + p.off, p.saved.data(), p.saved.size());
}

// A cave hook is safe if the slot is an exact match, or the instruction sequence
// has the same endpoints (same first/last byte) -> only inner encoding differs.
static bool caveHookOk(const Cheat& c) {
    uintptr_t hook = g_base + c.hookOff;
    if (!inModule(hook, c.slot)) return false;
    uint8_t* act = (uint8_t*)hook;
    if (memcmp(act, c.caveOrig.data(), c.slot) == 0) return true;
    return act[0] == c.caveOrig[0] && act[c.slot-1] == c.caveOrig[c.slot-1];
}

static bool enableCaveCheat(Cheat& c) {
    uintptr_t hook = g_base + c.hookOff;
    if (!caveHookOk(c)) return false;  // out of range or build mismatch
    void* mem = VirtualAlloc(NULL, c.size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;
    memcpy(mem, c.tmpl.data(), c.tmpl.size());
    uint32_t delta = (uint32_t)((uintptr_t)mem - c.B0);
    for (int r : c.rel) {
        uint32_t v; memcpy(&v, (uint8_t*)mem + r, 4);
        v -= delta;
        memcpy((uint8_t*)mem + r, &v, 4);
    }
    FlushInstructionCache(GetCurrentProcess(), mem, c.size);
    // patch hook site: E9 rel32 (jmp into cave) + NOP padding to slot length
    uint8_t buf[64]; memset(buf, 0x90, sizeof(buf));
    buf[0] = 0xE9;
    uint32_t rel = (uint32_t)((uintptr_t)mem - (hook + 5));
    memcpy(buf + 1, &rel, 4);
    c.hookSaved.assign((uint8_t*)hook, (uint8_t*)hook + c.slot);
    writeBytes(hook, buf, c.slot);
    c.caveMem = mem;
    return true;
}
static void disableCaveCheat(Cheat& c) {
    if (!c.hookSaved.empty()) writeBytes(g_base + c.hookOff, c.hookSaved.data(), c.slot);
    if (c.caveMem) { VirtualFree(c.caveMem, 0, MEM_RELEASE); c.caveMem = nullptr; }
}

// One-time pass: grey out only cheats that don't match THIS build (checked while
// nothing is patched yet). Runtime enable-failures are transient (e.g. a
// mutually-exclusive spawn in the same group is currently active).
static void dumpBytes(const char* tag, const char* name, uint32_t off, const uint8_t* exp, const uint8_t* act, size_t n) {
    char eb[128]={0}, ab[128]={0};
    for (size_t i=0;i<n && i<20;i++){ sprintf(eb+i*3,"%02X ",exp[i]); sprintf(ab+i*3,"%02X ",act[i]); }
    logf("[%s] '%s' +%X exp[%s] act[%s]", tag, name, off, eb, ab);
}
static void verifyAvailability() {
    for (auto& c : g_cheats) {
        bool ok = true;
        if (c.isCave) {
            ok = caveHookOk(c);
            if (!ok && inModule(g_base + c.hookOff, c.slot))
                dumpBytes("NA-cave", c.name.c_str(), c.hookOff, c.caveOrig.data(), (uint8_t*)(g_base+c.hookOff), c.slot);
        } else {
            uint8_t buf[32]; size_t len;
            for (auto& p : c.patches) {
                if (!resolvePatch(p, buf, &len)) {
                    ok = false;
                    if (inModule(g_base+p.off, p.patch.size()))
                        dumpBytes("NA-patch", c.name.c_str(), p.off, p.orig.data(), (uint8_t*)(g_base+p.off), p.orig.size());
                    break;
                }
            }
        }
        c.unavailable = !ok;
    }
}

static bool enableCheat(Cheat& c)  { return c.isCave ? enableCaveCheat(c) : enablePatchCheat(c); }
static void disableCheat(Cheat& c) { if (c.isCave) disableCaveCheat(c); else disablePatchCheat(c);
                                     c.active = false; logf("[cheat] off '%s'", c.name.c_str()); }
// Toggle a generated cheat by name (used to auto-engage the DT byte patches the
// "Instant Trigger" spam needs). No-op if already in the wanted state / not found.
static void setCheatByName(const char* name, bool on);

// ---------------------------------------------------------------- pointer cheats
// Teleport / Bloody Palace use a pointer chain off the game mediator. We validate
// every dereference so an invalid pointer (e.g. while in a menu) just no-ops.
static bool memReadable(void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD pr = mbi.Protect & 0xFF;
    if (pr == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return false;
    return ((uint8_t*)p + n) <= ((uint8_t*)mbi.BaseAddress + mbi.RegionSize);
}
static bool readPtr(uintptr_t a, uintptr_t& out) {
    if (!memReadable((void*)a, 4)) return false;
    out = *(uint32_t*)a; return true;
}
static bool writeMem(uintptr_t a, const void* data, size_t n) {
    if (!memReadable((void*)a, n)) return false;
    DWORD old;
    if (!VirtualProtect((void*)a, n, PAGE_READWRITE, &old)) return false;
    memcpy((void*)a, data, n);
    VirtualProtect((void*)a, n, old, &old);
    return true;
}
// Teleport / mission control. Two independent structures are involved:
//
//  * "MajinBegin" = [exe+0xF59F00] (the base the camera also resolves). Its
//    +0x154 is the CURRENT room id -- reliable for *display*, but writing it
//    does NOT load anything (the streamer overwrites it every frame). The
//    earlier rewrite wrote only here and dropped the trigger, so jumps silently
//    did nothing even though the value "stuck" on read-back.
//
//  * The area-jump REQUEST struct hangs off [exe+0xF23F38]:
//      b1 = [[exe+0xF23F38]+0x3830]   room / jump / trigger fields
//      b2 = [[exe+0xF23F38]+0x3834]   mission / bp-stage fields
//    A jump fires by filling the target fields and then writing 1 to the
//    "Initiate Jump" trigger at b1+0x84 -- the game acts on it next frame.
//    (The v4.0 "Area Jump" script does exactly this.)
static bool majinBase(uintptr_t& base) {
    return readPtr(g_base + 0xF59F00, base) && base != 0;
}
static bool readCurrentRoom(uint32_t& room) {
    uintptr_t b;
    if (!majinBase(b) || !memReadable((void*)(b + 0x154), 4)) return false;
    room = *(uint32_t*)(b + 0x154); return true;
}
static bool stageBase(uintptr_t& base) {        // +0x3830 (room / jump / trigger)
    uintptr_t p1;
    if (!readPtr(g_base + 0xF23F38, p1) || !p1) return false;
    return readPtr(p1 + 0x3830, base) && base != 0;
}
static bool stageBase2(uintptr_t& base) {       // +0x3834 (mission / bp)
    uintptr_t p1;
    if (!readPtr(g_base + 0xF23F38, p1) || !p1) return false;
    return readPtr(p1 + 0x3834, base) && base != 0;
}
// Set target mission (+b2:0x30), room (+b1:0x88, u16), optional BP stage
// (+b1:0x90 u16 and +b2:0x6C u32), then fire the Initiate-Jump trigger
// (+b1:0x84 = 1). mission/bpStage < 0 == leave unchanged.
static bool areaJump(int mission, int room, int bpStage) {
    uintptr_t b1, b2;
    if (!stageBase(b1)) { logf("[aj] FAIL: stage base unresolved (load a level first)"); return false; }
    bool haveB2 = stageBase2(b2);
    if (mission >= 0 && haveB2) { uint32_t m = (uint32_t)mission; writeMem(b2 + 0x30, &m, 4); }
    if (room   >= 0)           { uint16_t r = (uint16_t)room;    writeMem(b1 + 0x88, &r, 2); }
    if (bpStage > 0) {
        uint16_t s = (uint16_t)bpStage; writeMem(b1 + 0x90, &s, 2);
        if (haveB2) { uint32_t si = (uint32_t)bpStage; writeMem(b2 + 0x6C, &si, 4); }
    }
    uint32_t one = 1;
    bool trig = writeMem(b1 + 0x84, &one, 4);
    uint32_t cur = 0; readCurrentRoom(cur);
    logf("[aj] mission=%d room=%d bp=%d (b1=%p b2=%p) trigger=%s curRoom=%u",
         mission, room, bpStage, (void*)b1, (void*)b2, trig ? "set" : "FAILED", cur);
    return trig;
}
// Bloody Palace stage -> room mapping.
static int bpRoomForStage(int n) {
    if (n == 20) return 503; if (n == 40) return 504; if (n == 60) return 505;
    if (n == 80) return 506; if (n == 100) return 507; if (n == 101) return 700;
    if (n >= 1 && n <= 19) return 705; if (n >= 21 && n <= 39) return 704;
    if (n >= 41 && n <= 59) return 703; if (n >= 61 && n <= 79) return 701;
    if (n >= 81 && n <= 99) return 702;
    return 705;
}

// Boss rush: skip BP filler and bounce to the next boss floor (20/40/60/80/100).
// Filler floors map to rooms 705/704/703/701/702; the boss floors to 503-507.
// Each frame, if we're in a filler room we jump to the next boss; on a boss room
// we leave it alone to fight. The 1-19 band jumps to 20, so start is forced to 20.
static bool g_bossRush     = false;
static int  g_bossRushLast = -1;
static bool g_brCredoDone  = false;   // floor 100 (Credo) cleared -> next forced boss is 101 (Dante)
// Character / costume force. Read by inline hooks at the game's own load sites, so
// the value is injected at construction time (never a live write to a built actor,
// which crashes). -1 = off (game uses the menu's pick).
static volatile int g_forceChar = -1;  // BP char: 0=Dante 1=Nero 2=Vergil 3=Trish 4=Lady
static void bossRushTick() {
    if (!g_bossRush) { g_bossRushLast = -1; return; }
    uint32_t room;
    if (!readCurrentRoom(room)) return;
    int floor, troom;
    switch (room) {
        case 705: floor = 20;  troom = 503; g_brCredoDone = false; break;  // 1-19 -> 20 (new run: reset)
        case 704: floor = 40;  troom = 504; break;   // floors 21-39 -> Floor 40
        case 703: floor = 60;  troom = 505; break;   // floors 41-59 -> Floor 60
        case 701: floor = 80;  troom = 506; break;   // floors 61-79 -> Floor 80
        case 702:                                     // floors 81-99 filler
            // First time through -> Floor 100 (Credo). After Credo is cleared (we mark it
            // on room 507 below), this filler instead bounces to Floor 101 (Dante), so the
            // sequence is always Credo THEN Dante even if BP loops back to filler.
            if (g_brCredoDone) { floor = 101; troom = 700; }   // -> Dante
            else               { floor = 100; troom = 507; }   // -> Credo first
            break;
        case 507: g_brCredoDone = true; g_bossRushLast = -1; return;  // on Credo's floor: mark, fight him
        default:  g_bossRushLast = -1; return;        // other boss rooms / not in BP: let it ride
    }
    if (floor != g_bossRushLast) {
        areaJump(-1, troom, floor);
        g_bossRushLast = floor;
        logf("[bossrush] room %u (filler) -> Floor %d (room %d)", room, floor, troom);
    }
}

// Work Rate (slow-mo / fast-mo). A struct of 4 float multipliers hangs off
// [exe+0xF59F18]; 1.0 = normal speed, <1 = slow motion, >1 = sped up.
//   +0x20 Global  +0x28 Game  +0x2C Player  +0x30 Enemy
static const uint32_t kWorkRateOff[4] = { 0x20, 0x28, 0x2C, 0x30 };
static bool workRateAddr(int i, uintptr_t& addr) {
    uintptr_t base;
    if (!readPtr(g_base + 0xF59F18, base) || !base) return false;   // no level loaded yet
    addr = base + kWorkRateOff[i];
    return memReadable((void*)addr, 4);
}
static bool getWorkRate(int i, float& v) {
    uintptr_t a; if (!workRateAddr(i, a)) return false;
    v = *(float*)a; return true;
}
static bool setWorkRate(int i, float v) {
    uintptr_t a; if (!workRateAddr(i, a)) return false;
    return writeMem(a, &v, 4);
}

// FPS cap. A float target-FPS lives at [exe+0xF2429C]+0x44 (Extension Tool's FPS limit;
// game default 120). DMC4's physics is 60-locked, so >60 speeds the game up; "Unlimited"
// is just a huge cap (10000). 0 = leave the game's value alone. Pinned each frame.
static int g_fpsLimit = 0;     // 0=off(default), 60, 120, 240, 10000(unlimited)
static void applyFpsLimit() {
    if (g_fpsLimit == 0) return;
    uintptr_t base;
    if (!readPtr(g_base + 0xF2429C, base) || !base) return;     // render config not up yet
    uintptr_t a = base + 0x44;
    if (!memReadable((void*)a, 4)) return;
    float v = (float)g_fpsLimit;
    writeMem(a, &v, 4);
}


// ---- Full House fix: restores the vanilla DMC4 behaviour DMC4SE broke. ------
// Hooks the instruction `mov ecx,[ebx+0x301c]` at module+0x4FC5DC and detours
// through a code cave that forces two fields ([[esi+0x5E0]+0x44]+0x0C]=0,
// +0x14]=6) before running the original instruction and returning -- a clean,
// reversible in-process patch with no external DLL.
// NOTE: +0x0C must be 0 ("Full House very similar to Vanilla" -- the same value
// the external Fix Full House writes). DMC4SE's broken default is 4; the
// value 1 is the niche "JCable only when inertia already generated" variant,
// which is NOT the vanilla restore and feels like the fix does nothing.
static const uint32_t kFHHook    = 0x4FC5DC;
static const uint8_t  kFHOrig[6] = { 0x8B, 0x8B, 0x1C, 0x30, 0x00, 0x00 };  // mov ecx,[ebx+0x301c]
static void*   g_fhCave = nullptr;
static uint8_t g_fhSaved[6];
static bool    g_fhOn   = false;

static bool applyFullHouseFix() {
    if (g_fhOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kFHHook;
    if (memcmp((void*)site, kFHOrig, 6) != 0) {     // don't clobber a changed build / double-apply
        logf("[fullhouse] hook site unexpected (0x%02X) - aborting", *(uint8_t*)site);
        return false;
    }
    uint8_t cave[] = {
        0x50,                                       // push eax
        0x8B,0x86,0xE0,0x05,0x00,0x00,              // mov eax,[esi+0x5E0]
        0x8B,0x40,0x44,                             // mov eax,[eax+0x44]
        0xC7,0x40,0x0C,0x00,0x00,0x00,0x00,         // mov dword[eax+0x0C],0  (vanilla restore; was 1)
        0xC7,0x40,0x14,0x06,0x00,0x00,0x00,         // mov dword[eax+0x14],6
        0x58,                                       // pop eax
        0x8B,0x8B,0x1C,0x30,0x00,0x00,              // mov ecx,[ebx+0x301c]  (original)
        0xE9,0x00,0x00,0x00,0x00                    // jmp back (rel32 patched below)
    };
    const size_t JMP_OFF = sizeof(cave) - 5;
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[fullhouse] VirtualAlloc failed"); return false; }
    uintptr_t ret = site + 6;                       // resume after the overwritten instruction
    int32_t backRel = (int32_t)(ret - ((uintptr_t)mem + JMP_OFF + 5));
    memcpy(cave + JMP_OFF + 1, &backRel, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    // patch the hook site: jmp cave ; nop  (6 bytes, matches the original length)
    uint8_t hook[6] = { 0xE9, 0,0,0,0, 0x90 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_fhSaved, (void*)site, 6);
    if (!writeBytes(site, hook, 6)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_fhCave = mem; g_fhOn = true;
    logf("[fullhouse] applied (cave=%p)", mem);
    return true;
}
static void stopFullHouseFix() {
    if (!g_fhOn) return;
    writeBytes(g_base + kFHHook, g_fhSaved, 6);
    if (g_fhCave) { VirtualFree(g_fhCave, 0, MEM_RELEASE); g_fhCave = nullptr; }
    g_fhOn = false;
    logf("[fullhouse] removed");
}

// ---- Teleport crash guard. The stage-entity setup at module+0x7BDC60 does
// `mov [ecx+4],eax` with no null check; its caller (module+0x9EE5D) passes
// ecx=0 whenever an entity slot is the 0xFF "none" sentinel -- which an area
// jump to an arbitrary mission/room can produce -> null write -> crash. We cave
// the function to early-return when ecx==0. Pure crash prevention (the null path
// could never have done anything useful). Applied once at startup.
static const uint32_t kTpHook    = 0x7BDC60;
static const uint8_t  kTpOrig[7] = {0x8B,0x44,0x24,0x04, 0x89,0x41,0x04}; // mov eax,[esp+4]; mov [ecx+4],eax
static void* g_tpCave = nullptr;
static bool  g_tpOn   = false;
static void applyTeleportCrashFix() {
    if (g_tpOn || !g_base) return;
    uintptr_t site = g_base + kTpHook;
    if (!inModule(site, 7) || memcmp((void*)site, kTpOrig, 7) != 0) { logf("[tp] guard site unexpected"); return; }
    uint8_t cave[] = {
        0x85,0xC9,                 // test ecx,ecx
        0x75,0x03,                 // jnz +3 (skip the ret)
        0xC2,0x04,0x00,            // ret 4   (ecx==0 -> bail safely)
        0x8B,0x44,0x24,0x04,       // mov eax,[esp+4]   (original)
        0x89,0x41,0x04,            // mov [ecx+4],eax   (original)
        0xE9,0,0,0,0               // jmp back
    };
    const size_t JMP = sizeof(cave) - 5;
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int32_t back = (int32_t)((site + 7) - ((uintptr_t)mem + JMP + 5));
    memcpy(cave + JMP + 1, &back, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[7] = { 0xE9,0,0,0,0, 0x90,0x90 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    if (!writeBytes(site, hook, 7)) { VirtualFree(mem, 0, MEM_RELEASE); return; }
    g_tpCave = mem; g_tpOn = true;
    logf("[tp] teleport crash guard applied %p", mem);
}

// ---- Teleport crash guard #2. A second null-deref in the area-jump path:
// module+0x6B9F51 does `cmp [esi+0x48],0` (and +0x6B9F5D `inc [esi+0x48]`) on an
// object passed on the stack -- a refcount acquire. During an area jump to an
// arbitrary mission/room that object can be null (esi=0) -> page fault on read of
// 0x48. We cave the cmp site: if esi==0 we skip the refcount ops and rejoin at
// +0x6B9F60 (the edi check before the function's balancing `pop esi`), so the
// stack stays balanced and the null path is simply a no-op. Crash prevention only.
static const uint32_t kTp2Hook    = 0x6B9F51;
static const uint8_t  kTp2Orig[6] = {0x83,0x7E,0x48,0x00, 0x7F,0x06}; // cmp [esi+48],0 ; jg +6
static void* g_tp2Cave = nullptr;
static bool  g_tp2On   = false;
static void applyTeleportCrashFix2() {
    if (g_tp2On || !g_base) return;
    uintptr_t site = g_base + kTp2Hook;
    if (!inModule(site, 6) || memcmp((void*)site, kTp2Orig, 6) != 0) { logf("[tp2] guard site unexpected"); return; }
    uint8_t cave[] = {
        0x85,0xF6,                 // [0]  test esi,esi
        0x0F,0x84,0,0,0,0,         // [2]  jz  -> +0x6B9F60 (skip refcount on null)
        0x83,0x7E,0x48,0x00,       // [8]  cmp [esi+0x48],0   (original)
        0x0F,0x8F,0,0,0,0,         // [12] jg  -> +0x6B9F5D   (original jg target)
        0xE9,0,0,0,0               // [18] jmp -> +0x6B9F57   (original fallthrough)
    };
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int32_t r1 = (int32_t)((g_base + 0x6B9F60) - ((uintptr_t)mem + 8));   memcpy(cave + 4,  &r1, 4);
    int32_t r2 = (int32_t)((g_base + 0x6B9F5D) - ((uintptr_t)mem + 18));  memcpy(cave + 14, &r2, 4);
    int32_t r3 = (int32_t)((g_base + 0x6B9F57) - ((uintptr_t)mem + 23));  memcpy(cave + 19, &r3, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[6] = { 0xE9,0,0,0,0, 0x90 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    if (!writeBytes(site, hook, 6)) { VirtualFree(mem, 0, MEM_RELEASE); return; }
    g_tp2Cave = mem; g_tp2On = true;
    logf("[tp2] teleport crash guard #2 applied %p", mem);
}

// ====================== Enemy spawner (Bloody Palace) ======================
// MistressDMC enemy spawner (reverse-engineered in-house). The BP wave spawner
// at module+0x514590 calls `mgr->[vtbl+0x3D4](emID, 1, 0, 10.0, 0.0, 1.0)`
// (thiscall, ecx=mgr, emID passed BY VALUE = [esi]). We cave the call site
// (module+0x5145C8, orig `50 8B CF FF D2` = push eax; mov ecx,edi; call edx) to
// capture the live director `edi` -> g_spawnMgr each wave tick, then replay the
// call on demand with the chosen emID -- but only while the captured director is
// still a live in-module vtable (else it was freed -> skip, never crash). The
// spawn fn is BP-coupled, so this only works inside Bloody Palace.
static volatile uintptr_t g_spawnMgr = 0;
static volatile uint32_t  g_spawnCaptures = 0;
static volatile int g_spawnReqId = -1;      // pending emID; consumed IN-CAVE (-1 = none)
static const uint32_t kSpawnHook    = 0x5145C8;
static const uint8_t  kSpawnOrig[5] = {0x50,0x8B,0xCF,0xFF,0xD2};
static void* g_spawnCave = nullptr;
static bool  g_spawnOn   = false;

// The cave runs DURING a real wave spawn (director state valid), so instead of
// calling the spawn fn out-of-context (that crashed), it simply swaps the emID
// (eax) of the enemy already being spawned with the user's chosen id, then
// clears the request. Result: the next BP wave-spawn becomes your enemy -- safe,
// same valid context the swapper uses.
static bool applySpawnHook() {
    if (g_spawnOn || !g_base) return g_spawnOn;
    uintptr_t site = g_base + kSpawnHook;
    if (!inModule(site, 5) || memcmp((void*)site, kSpawnOrig, 5) != 0) { logf("[spawn] site unexpected"); return false; }
    uint8_t cave[] = {
        0x89,0x3D,0,0,0,0,                     // [0]  mov [g_spawnMgr], edi
        0xFF,0x05,0,0,0,0,                     // [6]  inc [g_spawnCaptures]
        0x83,0x3D,0,0,0,0,0x00,                // [12] cmp dword [g_spawnReqId], 0
        0x7C,0x0F,                             // [19] jl +15 (skip swap)
        0xA1,0,0,0,0,                          // [21] mov eax, [g_spawnReqId]
        0xC7,0x05,0,0,0,0,0xFF,0xFF,0xFF,0xFF, // [26] mov dword [g_spawnReqId], -1
        0x50,                                  // [36] push eax    (orig)
        0x8B,0xCF,                             // [37] mov ecx,edi
        0xFF,0xD2,                             // [39] call edx
        0xE9,0,0,0,0                           // [41] jmp back
    };
    uint32_t mgr = (uint32_t)(uintptr_t)&g_spawnMgr;
    uint32_t cap = (uint32_t)(uintptr_t)&g_spawnCaptures;
    uint32_t req = (uint32_t)(uintptr_t)&g_spawnReqId;
    memcpy(cave + 2,  &mgr, 4);
    memcpy(cave + 8,  &cap, 4);
    memcpy(cave + 14, &req, 4);
    memcpy(cave + 22, &req, 4);
    memcpy(cave + 28, &req, 4);
    const size_t JMP = sizeof(cave) - 5;
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;
    int32_t back = (int32_t)((site + 5) - ((uintptr_t)mem + JMP + 5));
    memcpy(cave + JMP + 1, &back, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[5] = { 0xE9,0,0,0,0 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    if (!writeBytes(site, hook, 5)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_spawnCave = mem; g_spawnOn = true;
    logf("[spawn] hook applied %p", mem);
    return true;
}
static void stopSpawnHook() {
    if (!g_spawnOn) return;
    writeBytes(g_base + kSpawnHook, kSpawnOrig, 5);
    if (g_spawnCave) { VirtualFree(g_spawnCave, 0, MEM_RELEASE); g_spawnCave = nullptr; }
    g_spawnOn = false; g_spawnMgr = 0; g_spawnReqId = -1;
}
static void serviceSpawn() { /* spawning now happens in-cave; nothing to do here */ }

// ====================== Doppelganger ========================================
// Spawns a fighting clone of the *active* character. Reverse-engineered in-house
// from flemia's closed-source Multi_Trainer: every address below is a fact about
// DevilMayCry4SpecialEdition.exe (2023 build, preferred image base 0x400000),
// used here as g_base+RVA so it survives ASLR. Because the doppelganger is a
// clone of whoever you are already playing, that character's resources are
// guaranteed resident -- so unlike a cross-character "heroes" spawn this needs
// no arc preloading, and the factory call is safe.
//
// Per spawn:
//   active = *(actor**)( *(char**)(g_base+RVA_PLAYERMGR) + 0x24 )   // live player
//   idx    = match *(void**)active (its vtable) to kDopp[].vtableRVA
//   pos    = active position pushed DOPP_DIST forward along its yaw
//   a worker thread builds the clone through the game's own factory:
//     p1 = *(g_base+classDescRVA); p2 = *p1; fn = *(p2+4); clone = fn();
//   then attaches it to the scene, copies the character's roster blob into the
//   player manager, and writes the spawn position.
// Despawn calls clone->vtbl[0x40](clone) (the actor's own kill path), guarded by
// the same attack-lock check (actor+0x3874 != 0) the original used.
// NOTE: these RVAs were recovered from flemia's 2023-JP build. The player-manager
// global is build-specific; on this install it is 0xF59F00 (same one majinRoot()
// uses, with the identical +0x24 to reach the active actor). The factory/vtable
// addresses below are still 2023-JP values and are being relocated -- doppSpawn
// gates on a vtable match, so it logs a probe and bails before the factory call
// while those are wrong.
static const uint32_t RVA_PLAYERMGR = 0xF59F00; // *(mgr); active actor at mgr+0x24
static const uint32_t RVA_WORLDGATE = 0xED86DC; // non-null => safe to attach to scene
static const uint32_t RVA_ATTACHFN  = 0x6A3000; // __stdcall attach(0xd, actor, 0, 0)
static const uint32_t RVA_ROSTERSRC = 0xEEEED8; // base of per-character roster blobs
static const float    DOPP_DIST     = 120.0f;   // spawn this far in front (0x42f00000)

struct DoppChar {
    uint32_t classDescRVA;  // entry+0: factory class descriptor (->[0]->[+4] = factory fn)
    uint32_t vtableRVA;     // entry+4: actor runtime vtable (identifies the character)
    uint32_t rosterOff;     // entry+8: byte offset into the roster source table
    uint32_t rosterLen;     // entry+c: dword count copied into playerMgr+0x4b8
    const char* name;
};
static const DoppChar kDopp[5] = {
    { 0xF15BBC, 0xC2A3C8, 0x270, 0x3c, "Nero"   },
    { 0xF161C8, 0xC28AD0, 0x168, 0x42, "Dante"  },
    { 0xF0FC0C, 0xBC6960, 0x360, 0x3f, "Vergil" },
    { 0xF10C60, 0xBC63B8, 0x45c, 0x42, "Trish"  },
    { 0xEDE5D4, 0xBC5E78, 0x564, 0x3e, "Lady"   },
};

static volatile void* g_doppActor = nullptr;   // the live clone (one at a time)
static bool g_doppNoMerge = true;              // soft-separate the clone so you don't merge into it
static void* g_doppOrigPlayer = nullptr;       // the real player active when we spawned (camera/control restore)
static int  g_doppKey = 'G';          // summon hotkey (VK code); rebindable in the menu
static bool g_doppKeyCapture = false; // when armed, the next key pressed becomes the bind

static void* activePlayer() {
    if (!g_base) return nullptr;
    char* mgr = *(char**)(g_base + RVA_PLAYERMGR);
    // mgr (and the actor it points to) can be a stale/garbage non-null pointer at
    // the title screen / between levels (seen as 0xff0000xx) -- a bare null check
    // isn't enough; deref'ing mgr+0x24 on junk page-faults. Validate readability.
    if (!mgr || !memReadable(mgr, 0x28)) return nullptr;
    void* a = *(void**)(mgr + 0x24);
    return (a && memReadable(a, 0x50)) ? a : nullptr;
}
static int doppIdentify(void* actor) {
    if (!actor) return -1;
    uintptr_t vt = *(uintptr_t*)actor;          // actor's vtable (absolute)
    for (int i = 0; i < 5; i++)
        if (vt == g_base + kDopp[i].vtableRVA) return i;
    return -1;
}

// ---- User-build (English/2015) spawn path, recovered from THIS exe. ---------
// The game's own player factory is a per-character switch (create dispatch at
// 0x516545, jump table 0x516894). Each character's branch is the same shape:
//     actor = alloc();                       // no-arg cdecl: getSingleton+vtbl[0x1c]
//     ctor(actor);                           // `this` in a register, returns this in eax
//     sceneMgr->reg(0xd, actor, 0, 0);       // *(0x13240a4) -> __thiscall 0xac32e0
// We replay that inline on the Present thread (= the game's main loop), so the
// call is synchronized with the game instead of racing it from a side thread.
// Addresses are RVAs (image base 0x400000, ASLR off -> g_base+RVA is exact).
// The only per-character quirk is which register the ctor reads `this` from
// (Dante/Trish/Lady=ESI, Nero=EAX, Vergil=EDI) -- ctorThis() sets all three.
struct DoppSpawnInfo { uint32_t allocRVA, ctorRVA; const char* name; };
static const DoppSpawnInfo kSpawn[5] = {
    { 0x4D27D0, 0x4D2800, "Dante"  },   // charId 0
    { 0x50EE10, 0x50EEF0, "Nero"   },   // charId 1 (normal Nero; 0x536CA0/CD0 is the tutorial stub)
    { 0x0CB400, 0x0CB430, "Vergil" },   // charId 2
    { 0x0C69C0, 0x0C69F0, "Trish"  },   // charId 3
    { 0x0C5140, 0x0C5170, "Lady"   },   // charId 4
};
static const uint32_t RVA_REG_MGR = 0xF240A4; // *(scene mgr) for the register call
static const uint32_t RVA_REG_FN  = 0x6C32E0; // __thiscall reg(mgr; 0xd, actor, 0, 0)
static const uint32_t OFF_CHARID  = 0x19AC;   // active+0x19AC = char id (0=Dante..4=Lady)
static const uint32_t OFF_POS_X   = 0x40;     // position floats (confirmed via probe)
static const uint32_t OFF_POS_Y   = 0x44;
static const uint32_t OFF_POS_Z   = 0x48;

// Call a constructor whose `this` is passed in a register (a non-standard
// convention the optimizer chose -- no C calling convention matches it). This
// is the exact form that worked for Dante: set ONLY the register the ctor reads
// and touch nothing else. Dante/Nero/Trish/Lady read `this` from ESI; Vergil
// reads it from EDI -- so there are two minimal thunks rather than one that
// forces several registers (which corrupted the object).
static void* ctorInEsi(void* mem, uintptr_t ctor) {
    void* ret = nullptr;
    __asm__ __volatile__(
        "movl %1, %%esi\n\t"
        "call *%2\n\t"
        : "=a"(ret)
        : "g"(mem), "r"(ctor)
        : "ecx", "edx", "esi", "memory", "cc");
    return ret;
}
static void* ctorInEdi(void* mem, uintptr_t ctor) {
    void* ret = nullptr;
    __asm__ __volatile__(
        "movl %1, %%edi\n\t"
        "call *%2\n\t"
        : "=a"(ret)
        : "g"(mem), "r"(ctor)
        : "ecx", "edx", "edi", "memory", "cc");
    return ret;
}

static void doppSpawn() {
    if (g_doppActor) { logf("[dopp] spawn: a clone is already active (max 2 characters)"); return; }
    void* active = activePlayer();
    if (!active) { logf("[dopp] spawn: no active player"); return; }
    g_doppOrigPlayer = active;   // remember the real player so despawn can restore camera/control to it
    int cid = *(int*)((char*)active + OFF_CHARID);
    if (cid < 0 || cid >= 5) { logf("[dopp] spawn: bad charId %d", cid); return; }
    const DoppSpawnInfo& s = kSpawn[cid];
    // 1) allocate the raw actor block via the character's allocator
    typedef void* (*AllocFn)();
    void* actor = ((AllocFn)(g_base + s.allocRVA))();
    if (!actor) { logf("[dopp] %s: alloc null", s.name); return; }
    // 2) construct it (Vergil reads `this` from EDI, everyone else from ESI)
    actor = (cid == 2) ? ctorInEdi(actor, g_base + s.ctorRVA)
                       : ctorInEsi(actor, g_base + s.ctorRVA);
    if (!actor) { logf("[dopp] %s: ctor null", s.name); return; }
    logf("[dopp] %s clone=%p vtableRVA=0x%X", s.name, actor, (unsigned)(*(uintptr_t*)actor - g_base));
    // 3) register the actor into the live scene
    void* regMgr = *(void**)(g_base + RVA_REG_MGR);
    if (regMgr) {
        typedef void (__thiscall *RegFn)(void*, int, void*, int, int);
        ((RegFn)(g_base + RVA_REG_FN))(regMgr, 0xd, actor, 0, 0);
    } else {
        logf("[dopp] %s: regMgr null (no live scene?)", s.name);
    }
    // 4) place the clone next to the player so you can see it pop in
    *(float*)((char*)actor + OFF_POS_X) = *(float*)((char*)active + OFF_POS_X) + 150.0f;
    *(float*)((char*)actor + OFF_POS_Y) = *(float*)((char*)active + OFF_POS_Y);
    *(float*)((char*)actor + OFF_POS_Z) = *(float*)((char*)active + OFF_POS_Z);
    g_doppActor = actor;
    logf("[dopp] %s spawn: done", s.name);
}

// Despawn. We CANNOT destruct the clone -- its destructor (and the vtbl[0x40]
// teardown) recursively walk sub-objects that the game's full player-spawn would
// have wired but our minimal alloc+ctor did not, so they fault on garbage. The
// clones are perfectly stable while *alive* though; they only die on teardown.
// So we just UNLINK the actor from the scene category-0xd list it was registered
// into (the exact inverse of the addToCategory at 0xac32e0): four pointer writes,
// no sub-object walking, so it physically cannot hit that crash. The game then
// stops updating/rendering it and it vanishes. Done under the same critical
// section the game's list code uses, on the Present (main) thread. We leak the
// block (~tens of KB) rather than free it -- safe and fine for a session.
//   list[cat] = sceneMgr + cat*0x18 + 0x20 ; head=list+0xc tail=list+0x10
//   actor+0x14 = next ; actor+0x18 = prev
static void doppDespawn() {
    void* a = (void*)g_doppActor;
    g_doppActor = nullptr;                              // re-arm spawn regardless
    if (!a || (uintptr_t)a < 0x10000 || (uintptr_t)a > 0x7ffeffff) { logf("[dopp] despawn: none"); return; }
    // First restore control/camera to the real player if the clone had become
    // the active player (a plain pointer write to the same +0x24 slot we read).
    char* pmgr = *(char**)(g_base + RVA_PLAYERMGR);
    if (pmgr && g_doppOrigPlayer && *(void**)(pmgr + 0x24) == a)
        *(void**)(pmgr + 0x24) = g_doppOrigPlayer;
    g_doppOrigPlayer = nullptr;
    // We can NOT destruct the clone (its teardown walks sub-objects the full
    // game-spawn would have wired) and we can NOT unlink it from the actor list
    // (its +0x14/+0x18 are float fields here, not list pointers). The only thing
    // that is provably safe is writing the position floats we verified via the
    // probe -- so park the clone far below the level. It drops out of view and
    // stays gone; the block leaks (~tens of KB), which is harmless per session.
    *(float*)((char*)a + OFF_POS_X) = 0.0f;
    *(float*)((char*)a + OFF_POS_Y) = -1.0e9f;
    *(float*)((char*)a + OFF_POS_Z) = 0.0f;
    logf("[dopp] despawn: clone parked off-map");
}

// ====================== Enemy-class probe ===================================
// To spawn enemies on THIS build we need each enemy's class descriptor, which is
// runtime-dependent (the create path is polymorphic). So we hook the scene
// register fn (0x6C32E0) -- the game calls it for EVERY actor it spawns -- and
// log each actor's category + vtable RVA. Play a fight once and the log lists the
// live enemy vtables for this build; from those we locate the factory descriptors.
static volatile uint32_t g_probeActor = 0;
static volatile uint32_t g_probeCat   = 0;
static volatile uint32_t g_probeRet   = 0;   // caller of the scene-register (= the spawner)
static void* g_emProbeCave = nullptr;
static bool  g_emProbeOn   = false;
static volatile uint32_t g_realDante  = 0;   // game-spawned Dante (hittable, has sword)
static volatile uint32_t g_ghostDante = 0;   // the one we spawn (set in emSpawnNow)
// Read a spawned actor's resource id ("emNNN") by scanning its header for a
// pointer to an em-name string inside the module. The 2019 build is DRM-encrypted
// on disk, so factories can't be named statically -- this lets the trainer
// self-identify what each factory actually builds, live. Returns "" if none found.
static const char* emIdOfActor(void* actor) {
    static char out[8];
    if (!actor || !memReadable(actor, 0x600)) return "";
    char* a = (char*)actor;
    for (int off = 0; off < 0x600; off += 4) {
        uintptr_t p = *(uintptr_t*)(a + off);
        if (!inModule(p, 6) || !memReadable((void*)p, 6)) continue;
        const unsigned char* s = (const unsigned char*)p;
        if (s[0] == 'e' && s[1] == 'm' && s[2] >= '0' && s[2] <= '9'
            && s[3] >= '0' && s[3] <= '9' && s[4] >= '0' && s[4] <= '9' && s[5] == 0) {
            out[0]='e'; out[1]='m'; out[2]=s[2]; out[3]=s[3]; out[4]=s[4]; out[5]=0;
            return out;
        }
    }
    return "";
}

extern "C" __attribute__((cdecl)) void emProbeLogger() {
    void* a = (void*)g_probeActor;
    if (!a || (uintptr_t)a < 0x10000) return;
    uintptr_t vt = *(uintptr_t*)a;
    if (vt < g_base || vt >= g_base + g_modSize) return;     // vtable must be in-module
    uint32_t vtRVA = (uint32_t)(vt - g_base);
    if (vtRVA == 0xC78230) {
        uint32_t ret = g_probeRet;
        bool game = (ret >= (uint32_t)g_base && ret < (uint32_t)(g_base + g_modSize));
        logf("[dante-spawn] actor=%p caller=0x%X rva=0x%X %s", a, ret,
             game ? (ret - (uint32_t)g_base) : ret, game ? "<< GAME ENCOUNTER SPAWN" : "(our DLL)");
        if (a != (void*)g_ghostDante) g_realDante = (uint32_t)(uintptr_t)a;
    }
    static uint32_t seen[96]; static int n = 0;
    for (int i = 0; i < n; i++) if (seen[i] == vtRVA) return; // dedupe
    if (n < 96) {
        seen[n++] = vtRVA;
        const char* eid = emIdOfActor(a);
        uint32_t ret = g_probeRet;
        bool game = (ret >= (uint32_t)g_base && ret < (uint32_t)(g_base + g_modSize));
        logf("[emprobe] cat=0x%X %s vtableRVA=0x%X spawnerRVA=0x%X actor=%p",
             g_probeCat, eid[0] ? eid : "(?)", vtRVA,
             game ? (ret - (uint32_t)g_base) : ret, a);
    }
}
static void applyEmProbe() {
    if (g_emProbeOn || !g_base) return;
    uintptr_t site = g_base + 0x6C32E0;                       // scene register fn entry
    static const uint8_t orig[5] = {0x56,0x8B,0x74,0x24,0x0C};// push esi; mov esi,[esp+0xc]
    if (!inModule(site, 5) || memcmp((void*)site, orig, 5) != 0) { logf("[emprobe] site unexpected"); return; }
    uint8_t cave[] = {
        0x8B,0x44,0x24,0x08,             // [0]  mov eax,[esp+8]   (actor arg)
        0xA3,0,0,0,0,                    // [4]  mov [g_probeActor], eax
        0x8B,0x44,0x24,0x04,             // [9]  mov eax,[esp+4]   (cat arg)
        0xA3,0,0,0,0,                    // [13] mov [g_probeCat], eax
        0x8B,0x04,0x24,                  // [18] mov eax,[esp]     (caller return addr)
        0xA3,0,0,0,0,                    // [21] mov [g_probeRet], eax
        0x60,                            // [26] pushad
        0xB8,0,0,0,0,                    // [27] mov eax, emProbeLogger
        0xFF,0xD0,                       // [32] call eax
        0x61,                            // [34] popad
        0x56,0x8B,0x74,0x24,0x0C,        // [35] push esi; mov esi,[esp+0xc]  (original 5 bytes)
        0xE9,0,0,0,0                     // [40] jmp back (site+5)
    };
    uint32_t pa = (uint32_t)(uintptr_t)&g_probeActor; memcpy(cave + 5,  &pa, 4);
    uint32_t pc = (uint32_t)(uintptr_t)&g_probeCat;   memcpy(cave + 14, &pc, 4);
    uint32_t pr = (uint32_t)(uintptr_t)&g_probeRet;   memcpy(cave + 22, &pr, 4);
    uint32_t fn = (uint32_t)(uintptr_t)&emProbeLogger;memcpy(cave + 28, &fn, 4);
    const size_t JMP = sizeof(cave) - 5;
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int32_t back = (int32_t)((site + 5) - ((uintptr_t)mem + JMP + 5));
    memcpy(cave + JMP + 1, &back, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[5] = { 0xE9,0,0,0,0 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    if (!writeBytes(site, hook, 5)) { VirtualFree(mem, 0, MEM_RELEASE); return; }
    g_emProbeCave = mem; g_emProbeOn = true;
    logf("[emprobe] installed at %p", mem);
}

// Enemy spawner. Each enemy has a no-arg factory (RVA below) that allocates and
// constructs the actor; we call it, place it in front of the player, and register
// it into the scene as an enemy (category 0xF).
struct EmType { const char* name; uint32_t createRVA; uint32_t arcRVA; };
// emID->name from flemia's Multi_Trainer (the build's REAL ids, not standard DMC4);
// createRVA = the no-arg factory, enumerated from the readable 2015 exe (= 2019 code
// + DRM wrapper, same layout -- verified vs the user's in-game spawns). arcRVA =
// the "rom\enemy\emNNN" package string; emSpawnNow mounts it before the factory so
// the enemy's model/AI is resident even on a floor that never uses it (Bloody
// Palace, wrong story room) -- without it the actor spawns with no model and never
// appears. Each factory's ctor writes its emID to actor+0x1920 (how the pairing was
// confirmed). NOTE: heavy bosses (Bael em019, Echidna em021, Angelo Credo em022,
// Angelo Agnus em023) are NOT here -- bare create-factory crashes them.
// Full roster -- valid on the 2015 build (the one the game is downgraded to).
// emID->name from flemia's Multi_Trainer; createRVA = the no-arg factory (starts
// with `push imm32`, validated at spawn time). Heavy bosses (Bael/Echidna/Credo/
// Agnus) are excluded -- bare create-factory crashes them on any build.
static const EmType kEmTypes[] = {
    { "Scarecrow (Leg)",   0x24CE40, 0xC21154 },  // em000
    { "Scarecrow (Arm)",   0x26E590, 0xC21144 },  // em001
    { "Mega Scarecrow",    0x26F6F0, 0xC21134 },  // em003
    { "Bianco Angelo",     0x2718A0, 0xC21124 },  // em005
    { "Alto Angelo",       0x287560, 0xC21114 },  // em006 (custom altoCreate path)
    { "Mephisto",          0x290130, 0xC21104 },  // em008
    { "Faust",             0x2A8080, 0xC210F4 },  // em009
    { "Frost",             0x2B6D60, 0xC210E4 },  // em010
    { "Assault",           0x2C80E0, 0xC210D4 },  // em011
    { "Blitz",             0x2E7600, 0xC210C4 },  // em012
    { "Chimera Seed",      0x2F2C60, 0xC210B4 },  // em013
    { "Cutlass",           0x30B370, 0xC210A4 },  // em015
    { "Gladius",           0x3244F0, 0xC21094 },  // em016
    { "Basilisk",          0x334D50, 0xC21084 },  // em017
    { "Berial",            0x34E9C0, 0xC21074 },  // em018
    { "Sanctus",           0x41F490, 0xC20FE0 },  // em029
    { "Sanctus Diabolica", 0x429480, 0xC20FD0 },  // em030
    { "Kyrie",             0x44C330, 0xC20F80 },  // em036
};
static const int kNEmTypes = (int)(sizeof(kEmTypes) / sizeof(kEmTypes[0]));
static int g_emSel = 0;

// Track what we spawn so we can despawn it. We can't safely free a live actor, so
// "despawn" parks it far off the map -- it drops out of the arena and out of sight.
static void* g_spawned[64];
static int   g_spawnedN = 0;
static void despawnEnemies() {
    const float away = 1.0e9f;
    int n = 0;
    for (int i = 0; i < g_spawnedN; i++) {
        char* e = (char*)g_spawned[i];
        if (!e || !memReadable(e, 0x60)) continue;
        *(float*)(e + 0x40) = away; *(float*)(e + 0x44) = -away; *(float*)(e + 0x48) = away;
        if (memReadable(e + 0x1890, 12)) {
            *(float*)(e + 0x1890) = away; *(float*)(e + 0x1894) = -away; *(float*)(e + 0x1898) = away;
        }
        n++;
    }
    logf("[em] despawned %d enemies (parked off-map)", n);
    g_spawnedN = 0;
}

// Zeroed scratch, stands in for a null model/render sub-object on a bare enemy.
// Any read returns 0 (the harmless branch); 0x20000 covers the deepest offset
// Credo touches.
static void* emModelStub() {
    static void* s = nullptr;
    if (!s) s = VirtualAlloc(nullptr, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return s;  // VirtualAlloc zeroes committed pages
}

// A lone `ret`, a safe target for vtable calls through sub-objects a bare enemy
// never built.
static void* emRetStub() {
    static void* s = nullptr;
    if (!s) {
        s = VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (s) { *(uint8_t*)s = 0xC3; FlushInstructionCache(GetCurrentProcess(), s, 16); }
    }
    return s;
}
// Vtable of all-ret stubs, so obj->vtbl[n]() is a no-op instead of a jump into junk.
static void** emFakeVtable() {
    static void** v = nullptr;
    if (!v) {
        v = (void**)VirtualAlloc(nullptr, 0x100 * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (v) { void* r = emRetStub(); for (int i = 0; i < 0x100; i++) v[i] = r; }
    }
    return v;
}

// Credo (em020) only. Its update at module+0x3A0F80 reads sub-object fields the
// bare factory leaves as junk. Patch the three it derefs:
//   +0x1880 model ptr   -> zeroed stub
//   +0x1888 anim flag    -> 0 (skips the +0x1884 deref)
//   +0x1004 controller   -> fake all-ret vtable when its vtbl ptr is junk
static void emCredoFixup(char* e) {
    if (*(void**)(e + 0x1880) == 0) { void* s = emModelStub(); if (s) *(void**)(e + 0x1880) = s; }
    *(uint8_t*)(e + 0x1888) = 0;
    uintptr_t vt = *(uintptr_t*)(e + 0x1004);
    if (vt < g_base || vt >= g_base + 0xFB2000) {            // not a real module vtable -> fake it
        void** fv = emFakeVtable();
        if (fv) *(void**)(e + 0x1004) = (void*)fv;
    }
}

// Alto Angelo (em006) has no plain factory -- the game builds it through a ctor
// table. Allocate through em005's allocator, then run em006's own ctor at
// module+0x287560 (takes `this` in EAX). emSpawn routes the marker RVA here.
static void* altoCreate() {
    typedef void* (__cdecl *GetSing)(void*);
    void* alloc = ((GetSing)(g_base + 0x5F1C50))((void*)(g_base + 0xF5FD8C));
    if (!alloc || (uintptr_t)alloc < 0x10000) return nullptr;
    void* ctx     = *(void**)(g_base + 0xF5FDA8);
    void* allocFn = (*(void***)alloc)[7];          // vtable + 0x1c
    void* mem = nullptr;
    __asm__ __volatile__(                          // thiscall: this=alloc, (size,0x10,ctx); callee cleans
        "push %3\n\t" "push $0x10\n\t" "push $0x6800\n\t"
        "mov %2, %%ecx\n\t" "call *%1\n\t"
        : "=a"(mem)
        : "r"(allocFn), "r"(alloc), "r"(ctx)
        : "ecx", "edx", "memory", "cc");
    if (!mem || (uintptr_t)mem < 0x10000) return nullptr;
    void* result = nullptr;
    uintptr_t ctor = g_base + 0x287560;            // em006 ctor, `this` in EAX
    __asm__ __volatile__(
        "mov %1, %%eax\n\t" "call *%2\n\t"
        : "=a"(result)
        : "r"(mem), "r"(ctor)
        : "ecx", "edx", "memory", "cc");
    return result;
}

// Dante boss is pl006, a separate AI character. His arc only mounts during his own
// fight; elsewhere the "chartbl\pl006_param" lookup returns null and the game
// crashes derefing it. Run that same lookup first -- null means not loaded, so we
// skip the spawn instead of crashing.
static bool dantePl006Loaded() {
    uintptr_t cat;
    if (!readPtr(g_base + 0xF23E44, cat) || !cat || !memReadable((void*)cat, 4)) return false;
    void** vt = *(void***)cat;
    if (!memReadable(vt, 0x34)) return false;
    void* loadOne = vt[12];                          // catalog vtbl + 0x30
    void* str  = (void*)(g_base + 0xC7765C);          // "chartbl\\pl006_param"
    void* arg0 = (void*)(g_base + 0xF5EB5C);
    void* res = nullptr;
    __asm__ __volatile__(                             // thiscall: this=cat, (arg0, str, 1); callee cleans
        "push $1\n\t" "push %2\n\t" "push %3\n\t"
        "mov %4, %%ecx\n\t" "call *%1\n\t"
        : "=a"(res)
        : "r"(loadOne), "r"(str), "r"(arg0), "r"(cat)
        : "ecx", "edx", "memory", "cc");
    return res != nullptr;
}

// Force-load Dante's pl006 data so he can spawn outside his own fight. This is the
// exact call the BP boss-preloader uses: catalog->LoadOne(out, "rom\id\libra\
// pl006", 2). The load is async, so we fire it and poll dantePl006Loaded() before
// the spawn actually goes through.
static void preloadPl006() {
    uintptr_t cat;
    if (!readPtr(g_base + 0xF23E44, cat) || !cat || !memReadable((void*)cat, 4)) return;
    void** vt = *(void***)cat;
    if (!memReadable(vt, 0x34)) return;
    void* loadOne = vt[12];
    void* out = (void*)(g_base + 0xF5EB5C);
    void* res;
    // Mount Dante's whole packages (arcs), not individual files -- this is flemia's
    // method. uPlayerDante = player logic, plmod_pl006 = boss model, em_dante =
    // hitbox/sword/reaction. The load-hook pins each file as it streams. Loading the
    // libra index too so his model database resolves. flag 2 = mount/keep.
    static const uint32_t arc[] = {
        0xBF2990,  // rom\id\libra\pl006
        0xC20BD8,  // rom\player\uPlayerDante
        0xC20A54,  // rom\player\costume\plmod_pl006
        0xC20FF0,  // rom\enemy\em_dante
    };
    for (unsigned i = 0; i < sizeof(arc) / sizeof(arc[0]); i++) {
        void* name = (void*)(g_base + arc[i]);
        __asm__ __volatile__("push $2\n\t push %2\n\t push %3\n\t mov %4,%%ecx\n\t call *%1\n\t"
            : "=a"(res) : "r"(loadOne), "r"(name), "r"(out), "r"(cat) : "ecx", "edx", "memory", "cc");
    }
    // His collision files only stream during the real fight, so cold-spawn never gets
    // them -> he's un-hittable. Load them explicitly (flag 1, safe -- the em_dante arc
    // above is mounted first). These are his hitbox/sword/reaction boxes.
    static const uint32_t coll[] = {
        0xC7815C, 0xC78174, 0xC78190, 0xC781AC, 0xC781C8,
        0xC781E4, 0xC78200, 0xC78758, 0xC78778,
    };
    for (unsigned i = 0; i < sizeof(coll) / sizeof(coll[0]); i++) {
        void* name = (void*)(g_base + coll[i]);
        __asm__ __volatile__("push $1\n\t push %2\n\t push %3\n\t mov %4,%%ecx\n\t call *%1\n\t"
            : "=a"(res) : "r"(loadOne), "r"(name), "r"(out), "r"(cat) : "ecx", "edx", "memory", "cc");
    }
    (void)res;
}

static void danteApplyStats(char* e);   // defined below; stamps real Dante's stats onto a ghost

static void emSpawnNow(uint32_t createRVA) {
    void* active = activePlayer();   // null-safe (returns null if no live player) -- placement
                                     // below tolerates null, exactly as the working 1.2 build did
    typedef void* (__cdecl *CreateFn)();
    // Safety: every real enemy factory starts with `push imm32` (0x68) -- the
    // getSingleton prologue. If the byte at the address isn't that, this build's
    // layout differs from the one these RVAs were taken from (the 2019 DRM build
    // moved everything), so calling it would run the WRONG code and crash. Bail
    // safely instead. (altoCreate path is exempt -- it doesn't call by RVA.)
    if (createRVA != 0x287560) {
        uint8_t* fp = (uint8_t*)(g_base + createRVA);
        if (!memReadable(fp, 5) || fp[0] != 0x68) {
            logf("[em] factory 0x%X invalid on this build (0x%02X) -- spawn unavailable",
                 createRVA, memReadable(fp, 1) ? fp[0] : 0xFF);
            return;
        }
    }
    void* enemy = (createRVA == 0x287560) ? altoCreate()         // em006 custom factory
                                          : ((CreateFn)(g_base + createRVA))();
    if (!enemy || (uintptr_t)enemy < 0x10000) { logf("[em] create rva 0x%X returned null", createRVA); return; }
    char* e = (char*)enemy;
    logf("[em] created %p vtableRVA=0x%X", enemy, (unsigned)(*(uintptr_t*)e - g_base));
    { const char* eid = emIdOfActor(enemy);
      logf("[emid] factory 0x%X -> %s vtableRVA=0x%X", createRVA,
           eid[0] ? eid : "(unknown)", (unsigned)(*(uintptr_t*)e - g_base)); }
    // Drop it in front of the player, not beside. The actor's transform sits at
    // +0x10; forward is row 2 (+0x30/34/38), position row 3 (+0x40). Walk along
    // forward so it lands ahead whichever way you're facing. Enemy position also
    // lives at +0x1890 -- set both, before registering.
    const float DIST = 280.0f;
    float x = 0, y = 0, z = 0;
    if (active) {
        float px = *(float*)((char*)active + 0x40);
        float py = *(float*)((char*)active + 0x44);
        float pz = *(float*)((char*)active + 0x48);
        float fx = *(float*)((char*)active + 0x30);   // forward.x (matrix row2)
        float fz = *(float*)((char*)active + 0x38);   // forward.z
        float mag2 = fx * fx + fz * fz;               // horizontal forward length^2
        if (mag2 > 0.25f && mag2 < 4.0f) {            // sane unit-ish vector -> use facing
            float inv = 1.0f / sqrtf(mag2);
            x = px + fx * inv * DIST;
            y = py;                                   // same height -> on the ground in front
            z = pz + fz * inv * DIST;
        } else {                                      // matrix not where expected -> old behaviour
            x = px + DIST; y = py; z = pz;
        }
    }
    bool dante = (createRVA == 0x4E19A0);         // player-class actor, different layout
    *(float*)(e + 0x40) = x; *(float*)(e + 0x44) = y; *(float*)(e + 0x48) = z;
    if (!dante) {
        // Enemy-only fields. On a player-class actor (+0x1890, +0x27f0, +0x32b4) these
        // offsets are different data, so writing them corrupts Dante -- skip for him.
        *(float*)(e + 0x1890) = x; *(float*)(e + 0x1894) = y; *(float*)(e + 0x1898) = z;
        if (*(void**)(e + 0x27f0) == 0) *(void**)(e + 0x27f0) = enemy;
        if (*(void**)(e + 0x32b4) == 0) *(void**)(e + 0x32b4) = enemy;
        if (createRVA == 0x3A0A50) emCredoFixup(e);   // Credo needs extra field patches
    }
    // register as a scene enemy (category 0xF)
    void* regMgr = *(void**)(g_base + RVA_REG_MGR);
    if (regMgr) {
        typedef void (__thiscall *RegFn)(void*, int, void*, int, int);
        ((RegFn)(g_base + RVA_REG_FN))(regMgr, 0xF, enemy, 0, 0);
    }
    if (g_spawnedN < 64) g_spawned[g_spawnedN++] = enemy;   // track for despawn
    if (createRVA == 0x4E19A0) g_ghostDante = (uint32_t)(uintptr_t)enemy;   // tag for the diff tool
    (void)&danteApplyStats;   // stat-stamp disabled -- the real fix is pinning em_dante collision
    logf("[em] spawn done");
}

// Snapshot the real Dante's combat stats on his floor, then stamp the non-pointer
// ones onto a ghost spawned elsewhere. Pointers are skipped (they'd dangle); this
// carries his scalar state -- health, flags, scales, timers.
#define DSNAP_DW 0x980                 // 0x2600 bytes of his struct
static uint32_t g_danteSnap[DSNAP_DW];
static bool     g_danteSnapped = false;
static bool danteLooksPtr(uint32_t v) { return v >= 0x0F000000 && v < 0x22000000; }
static void danteSnapTick() {
    if (g_danteSnapped) return;
    char* real = (char*)(uintptr_t)g_realDante;
    if (!real || !memReadable(real, DSNAP_DW * 4)) return;
    for (int i = 0; i < DSNAP_DW; i++) g_danteSnap[i] = *(uint32_t*)(real + i * 4);
    g_danteSnapped = true;
    logf("[dante] combat-stat snapshot captured from real Dante");
}
static void danteApplyStats(char* e) {
    if (!g_danteSnapped) return;
    int n = 0;
    for (int i = 8; i < DSNAP_DW; i++) {           // skip vtable + base header
        uint32_t v = g_danteSnap[i];
        if (v && !danteLooksPtr(v)) { *(uint32_t*)(e + i * 4) = v; n++; }
    }
    logf("[dante] stamped %d combat stats onto ghost", n);
}

// Diff the game's real Dante against our ghost: log every field where the real one
// holds a pointer (a combat sub-object: hitbox, weapon, AI...) and ours is null.
// Those are the candidates to clone to make him hittable.
static bool g_danteDiffed = false;
static void danteDiffTick() {
    if (g_danteDiffed) return;
    char* real  = (char*)(uintptr_t)g_realDante;
    char* ghost = (char*)(uintptr_t)g_ghostDante;
    if (!real || !ghost || real == ghost) return;
    if (!memReadable(real, 0x40) || !memReadable(ghost, 0x40)) return;
    g_danteDiffed = true;
    logf("[dantediff] real=%p ghost=%p -- real-has-ptr / ghost-null fields:", real, ghost);
    int cnt = 0;
    for (uint32_t off = 0x20; off < 0x18b00 && cnt < 120; off += 4) {
        if (!memReadable(real + off, 4) || !memReadable(ghost + off, 4)) continue;
        uint32_t rv = *(uint32_t*)(real + off);
        uint32_t gv = *(uint32_t*)(ghost + off);
        if (gv == 0 && rv > 0x10000 && memReadable((void*)(uintptr_t)rv, 4)) {
            logf("[dantediff] +0x%X  real=0x%X  ghost=0", off, rv);
            cnt++;
        }
    }
    logf("[dantediff] done -- %d candidate sub-object fields", cnt);
}

// Dante boss (pl006) needs his data loaded first. If it isn't, kick off the
// preload and remember to spawn him once it lands; danteServiceTick() polls each
// frame. Everything else spawns immediately.
static int g_dantePreload = 0;          // frames left to wait for pl006
static void emSpawn(uint32_t createRVA) {
    if (createRVA == 0x4E19A0 && !dantePl006Loaded()) {
        preloadPl006();                 // cold spawn: mount his arcs, then spawn when ready
        g_dantePreload = 1800;
        logf("[dante] cold spawn -- mounting arcs, he'll drop in once loaded (close menu so it streams)");
        return;
    }
    emSpawnNow(createRVA);
}
// Diagnostic: poll whether pl006 is loaded and log every change with the room, so
// we can see exactly when/where the game streams Dante in as you move through BP.
// Pure polling (flag-1 lookup, the safe one) -- never loads or mounts anything.
static int g_danteProbeLast = -2;
static int g_danteProbeCd   = 0;
static bool g_dantePinned   = false;

// Re-run a catalog lookup (flag 1, the safe one) and return the resource handle.
static void* danteLookup(void* fn, uintptr_t cat, uint32_t outRva, uint32_t nameRva) {
    void* out = (void*)(g_base + outRva), * name = (void*)(g_base + nameRva), * res;
    __asm__ __volatile__("push $1\n\t push %2\n\t push %3\n\t mov %4,%%ecx\n\t call *%1\n\t"
        : "=a"(res) : "r"(fn), "r"(name), "r"(out), "r"(cat) : "ecx", "edx", "memory", "cc");
    return res;
}

// Once pl006 is loaded (his own floor / room 700), bump a ref on his key resources
// so a floor change can't unload them. After that he stays resident and spawns on
// any floor for the rest of the session.
static void dantePinPl006() {
    uintptr_t cat;
    if (!readPtr(g_base + 0xF23E44, cat) || !cat || !memReadable((void*)cat, 4)) return;
    void** vt = *(void***)cat;
    if (!memReadable(vt, 0x34)) return;
    void* fn = vt[12];
    typedef void (__thiscall *AddRef)(void*);
    AddRef addref = (AddRef)(g_base + 0x68DEF0);   // resource addref the ctor uses
    // Every pl006 resource Dante's init pulls in. We add a ref to each (looked up by
    // name; out-struct reused) so a floor change can't unload them. Param survives
    // already -- it's the rest (jump/chartbl base/models) that were getting freed.
    // Every pl006 resource name in the binary -- libra, model, chartbl, motion,
    // sound, demo. Whatever's resident when we pin (his floor) gets a ref and
    // survives floor changes; the rest are no-ops.
    static const uint32_t nameRva[] = {
        0xB9F458, 0xB9F484, 0xBF21BC, 0xBF21D4, 0xBF21F0, 0xBF2370, 0xBF2388, 0xBF23A4, 0xBF2990,
        0xBF2A80, 0xC13310, 0xC1332C, 0xC1334C, 0xC1336C, 0xC1338C, 0xC133A8, 0xC133C8, 0xC133E4,
        0xC13404, 0xC13424, 0xC13444, 0xC13460, 0xC1DA0C, 0xC20A98, 0xC20ADC, 0xC5794C, 0xC58E48,
        0xC7711C, 0xC77144, 0xC7716C, 0xC77190, 0xC771AC, 0xC771CC, 0xC771EC, 0xC7720C, 0xC7722C,
        0xC7724C, 0xC7726C, 0xC77288, 0xC772A4, 0xC772C0, 0xC772DC, 0xC772F8, 0xC77314, 0xC77330,
        0xC7734C, 0xC77368, 0xC77384, 0xC7739C, 0xC773B4, 0xC773CC, 0xC773E4, 0xC773FC, 0xC77414,
        0xC7742C, 0xC77444, 0xC7745C, 0xC77630, 0xC7764C, 0xC7765C, 0xC77670, 0xC77684, 0xC7769C,
        0xC776B8, 0xC776D4, 0xC776EC, 0xC77708, 0xC7F498,
    };
    static int s_best = 0;
    int pinned = 0;
    for (unsigned i = 0; i < sizeof(nameRva) / sizeof(nameRva[0]); i++) {
        void* res = danteLookup(fn, cat, 0xF5EB5C, nameRva[i]);
        if ((uintptr_t)res > 0x10000) { addref(res); pinned++; }   // +1 each pass; harmless leak
    }
    if (pinned > s_best) {
        s_best = pinned;
        logf("[dante] pinned %d/%d pl006 resources (let his fight run to load more)", pinned, 68);
    }
    if (pinned >= 40) g_dantePinned = true;   // most of his set is held -> stop re-scanning
}

// The fix for "only 2 retrievable": hook the catalog's load function and pin every
// pl006 resource at the moment it streams in -- catches all of them, not just the
// 2 I can look up by name. Fight Dante once and his whole set gets a ref, so he
// survives floor changes and spawns anywhere.
typedef void* (__fastcall *LoadOneFn)(void* cat, void* edx, void* out, const char* name, int flag);
static LoadOneFn oLoadOne   = nullptr;
static bool      g_loadHook = false;
static int       g_dantePinCount = 0;
static void* __fastcall hkLoadOne(void* cat, void* edx, void* out, const char* name, int flag) {
    void* res = oLoadOne(cat, edx, out, name, flag);
    if (res && (uintptr_t)res > 0x10000 && name && memReadable((void*)name, 6)) {
        bool hit = false;                          // pl006 = body/motions, dante = hitbox/weapon/reaction
        for (int i = 0; i < 120 && name[i]; i++) {
            if (name[i]=='p'&&name[i+1]=='l'&&name[i+2]=='0'&&name[i+3]=='0'&&name[i+4]=='6') { hit = true; break; }
            if (name[i]=='d'&&name[i+1]=='a'&&name[i+2]=='n'&&name[i+3]=='t'&&name[i+4]=='e') { hit = true; break; }
        }
        if (hit) {
            typedef void (__thiscall *AddRef)(void*);
            ((AddRef)(g_base + 0x68DEF0))(res);    // pin it -- can't unload now
            g_dantePinCount++;
        }
    }
    return res;
}
static void installLoadHook() {
    if (g_loadHook) return;
    uintptr_t cat;
    if (!readPtr(g_base + 0xF23E44, cat) || !cat || !memReadable((void*)cat, 4)) return;
    void** vt = *(void***)cat;
    if (!memReadable(vt, 0x34)) return;
    void* loadOne = vt[12];
    MH_Initialize();   // no-op if kiero already did it
    if (MH_CreateHook(loadOne, (void*)hkLoadOne, (void**)&oLoadOne) == MH_OK &&
        MH_EnableHook(loadOne) == MH_OK) {
        g_loadHook = true;
        logf("[dante] load-hook armed -- every pl006 resource gets pinned as it loads");
    }
}

// ===================== Character / Costume force hooks =====================
// Inline cave that overrides the character the game loads:
//  * Char @ 0x10C9FD -- the player-model loader (movzx ecx,[ebx+0x30]; cmp ecx,4;
//    jmp [ecx*4+...]). ebx is the preload config; [ebx+0x30] is the 5-way char id
//    (0 Dante,1 Nero,2 Vergil,3 Trish,4 Lady). We only act when ebx is the Bloody
//    Palace config ([[0x1323F38]+0x3834]) so story missions are untouched, then we
//    WRITE the forced id back into [ebx+0x30] -- the BP scene factory and the HUD
//    read that same field later, so model + actor + HUD all agree. Construction-time
//    only, so no live-actor crash. (Costume is a plain per-character field write --
//    no hook needed; see setCostume.)
static bool  g_charHook = false;
static void installCharHook() {
    if (g_charHook || !g_base) return;
    uintptr_t site = g_base + 0x10C9FD;
    static const uint8_t orig[7] = {0x0F,0xB6,0x4B,0x30,0x83,0xF9,0x04};   // movzx ecx,[ebx+0x30]; cmp ecx,4
    if (!inModule(site, 7) || memcmp((void*)site, orig, 7) != 0) { logf("[swap] char site unexpected"); g_charHook = true; return; }
    uint32_t fc = (uint32_t)(uintptr_t)&g_forceChar;
    uint8_t cave[55];
    int o = 0;
    cave[o++]=0x0F; cave[o++]=0xB6; cave[o++]=0x4B; cave[o++]=0x30;          // 0  movzx ecx,[ebx+0x30]
    cave[o++]=0x83; cave[o++]=0x3D; memcpy(cave+o,&fc,4); o+=4; cave[o++]=0x00; // 4  cmp [g_forceChar],0
    cave[o++]=0x7C; cave[o++]=0x22;                                          // 11 jl DONE(+0x22 -> off 47)
    cave[o++]=0x50;                                                          // 13 push eax
    cave[o++]=0xA1; cave[o++]=0x38; cave[o++]=0x3F; cave[o++]=0x32; cave[o++]=0x01; // 14 mov eax,[0x1323f38]
    cave[o++]=0x85; cave[o++]=0xC0;                                          // 19 test eax,eax
    cave[o++]=0x74; cave[o++]=0x17;                                          // 21 jz POPDONE(+0x17 -> off 46)
    cave[o++]=0x8B; cave[o++]=0x80; cave[o++]=0x34; cave[o++]=0x38; cave[o++]=0x00; cave[o++]=0x00; // 23 mov eax,[eax+0x3834]
    cave[o++]=0x39; cave[o++]=0xD8;                                          // 29 cmp eax,ebx
    cave[o++]=0x75; cave[o++]=0x0D;                                          // 31 jne POPDONE(+0x0D -> off 46)
    cave[o++]=0xA1; memcpy(cave+o,&fc,4); o+=4;                              // 33 mov eax,[g_forceChar]
    cave[o++]=0x88; cave[o++]=0x43; cave[o++]=0x30;                          // 38 mov [ebx+0x30],al
    cave[o++]=0x89; cave[o++]=0xC1;                                          // 41 mov ecx,eax
    cave[o++]=0x58;                                                          // 43 pop eax
    cave[o++]=0xEB; cave[o++]=0x01;                                          // 44 jmp AFTER(+1 -> off 47)
    cave[o++]=0x58;                                                          // 46 POPDONE: pop eax
    cave[o++]=0x83; cave[o++]=0xF9; cave[o++]=0x04;                          // 47 AFTER/DONE: cmp ecx,4
    cave[o++]=0xE9;                                                          // 50 jmp 0x10CA04
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int32_t back = (int32_t)((g_base + 0x10CA04) - ((uintptr_t)mem + 50 + 5));
    memcpy(cave + 51, &back, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[7] = { 0xE9,0,0,0,0, 0x90,0x90 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    if (writeBytes(site, hook, 7)) { g_charHook = true; logf("[swap] char force hook applied %p", mem); }
    else VirtualFree(mem, 0, MEM_RELEASE);
}
// Costume crash guard. The costume-transform reader at 0x157C86 looks up the
// character's costume slot; when the slot is empty the table returns 0xFF and the
// game sets eax=0 but STILL does `movss xmm0,[eax+0x10]` -> null deref. Vergil /
// Trish / Lady have no _ex00/_ex01 entry, so forcing an alt costume on them hits
// this. We cave the faulting read: if eax is null, point it at a zeroed buffer so
// it reads zeros (costume falls back) instead of crashing. Pure defensive guard --
// only changes the otherwise-crashing null path.
static bool g_costGuard = false;
static void installCostGuard() {
    if (g_costGuard || !g_base) return;
    uintptr_t site = g_base + 0x157C86;
    static const uint8_t orig[5] = {0xF3,0x0F,0x10,0x40,0x10};   // movss xmm0,[eax+0x10]
    if (!inModule(site, 5) || memcmp((void*)site, orig, 5) != 0) { logf("[swap] cost-guard site unexpected"); g_costGuard = true; return; }
    static void* zbuf = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE); // zero-filled
    if (!zbuf) return;
    uint32_t zb = (uint32_t)(uintptr_t)zbuf;
    uint8_t cave[19];
    cave[0]=0x85; cave[1]=0xC0;                          // test eax,eax
    cave[2]=0x75; cave[3]=0x05;                          // jnz +5 (skip the mov)
    cave[4]=0xB8; memcpy(cave+5,&zb,4);                  // mov eax, zbuf
    cave[9]=0xF3; cave[10]=0x0F; cave[11]=0x10; cave[12]=0x40; cave[13]=0x10; // movss xmm0,[eax+0x10]
    cave[14]=0xE9;                                       // jmp 0x157C8B
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int32_t back = (int32_t)((g_base + 0x157C8B) - ((uintptr_t)mem + 14 + 5));
    memcpy(cave + 15, &back, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[5] = { 0xE9,0,0,0,0 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    if (writeBytes(site, hook, 5)) { g_costGuard = true; logf("[swap] costume crash guard applied %p", mem); }
    else VirtualFree(mem, 0, MEM_RELEASE);
}

// Effect-object crash guard. fn @0x4EE2F7 reads [edi+0x114] where edi is an effect/
// visual object just created by `call [edx+0x40]`. When a mod (esp. a BP environment
// mod) references an effect the engine can't build, the create returns null -> edi=0
// -> page fault at 0x4EE2F7. We cave the read: if edi is null, point it at a zeroed
// buffer so the init writes land on scratch and the game limps on (no effect) instead
// of crashing. Same defensive pattern as the costume guard.
static bool g_fxGuard = false;
static void installFxGuard() {
    if (g_fxGuard || !g_base) return;
    uintptr_t site = g_base + 0x4EE2F7;
    static const uint8_t orig[6] = {0x8B,0x87,0x14,0x01,0x00,0x00};   // mov eax,[edi+0x114]
    if (!inModule(site, 6) || memcmp((void*)site, orig, 6) != 0) { logf("[swap] fx-guard site unexpected"); g_fxGuard = true; return; }
    static void* zbuf = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE); // zero-filled, writable
    if (!zbuf) return;
    uint32_t zb = (uint32_t)(uintptr_t)zbuf;
    uint8_t cave[20];
    cave[0]=0x85; cave[1]=0xFF;                          // test edi,edi
    cave[2]=0x75; cave[3]=0x05;                          // jnz +5 (skip the mov edi,zbuf)
    cave[4]=0xBF; memcpy(cave+5,&zb,4);                  // mov edi, zbuf
    cave[9]=0x8B; cave[10]=0x87; cave[11]=0x14; cave[12]=0x01; cave[13]=0x00; cave[14]=0x00; // mov eax,[edi+0x114]
    cave[15]=0xE9;                                       // jmp 0x4EE2FD
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int32_t back = (int32_t)((g_base + 0x4EE2FD) - ((uintptr_t)mem + 15 + 5));
    memcpy(cave + 16, &back, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[6] = { 0xE9,0,0,0,0, 0x90 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    if (writeBytes(site, hook, 6)) { g_fxGuard = true; logf("[swap] effect crash guard applied %p", mem); }
    else VirtualFree(mem, 0, MEM_RELEASE);
}

// Keep the spawned Dante's combat lists consistent so the collision system (now
// active, since em_dante data is pinned) doesn't walk a null table. If a list's
// table pointer is null while its count is non-zero, point it at a zeroed buffer.
static void* danteCollBuf() {     // dedicated 1MB writable buffer for Dante's hit list
    static void* b = nullptr;
    if (!b) b = VirtualAlloc(nullptr, 0x100000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return b;
}
static void danteGuardTick() {
    char* g = (char*)(uintptr_t)g_ghostDante;
    if (!g || !memReadable(g, 0x120)) return;
    if (*(uint32_t*)(g + 0x114) == 0 && *(uint32_t*)(g + 0x118) != 0) {
        void* b = danteCollBuf();
        if (b) *(uint32_t*)(g + 0x114) = (uint32_t)(uintptr_t)b;
    }
}

static void modInitOnce();      // fwd: skin/BP mod loader init (defined after costume code)
static void modCostumeTick();   // fwd: keep costume aligned to assigned skin
static void hddCacheTick();     // fwd: maintain the loose-file (HDD-cache) flag
static void danteProbeTick() {
    installLoadHook();
    installCharHook();
    installCostGuard();
    installFxGuard();
    modInitOnce();
    modCostumeTick();
    hddCacheTick();
    if (--g_danteProbeCd > 0) return;
    g_danteProbeCd = 30;                       // ~twice a second
    int loaded = dantePl006Loaded() ? 1 : 0;
    if (loaded != g_danteProbeLast) {
        uint32_t room = 0; readCurrentRoom(room);
        logf("[danteprobe] pl006 loaded=%d  room=%u", loaded, room);
        g_danteProbeLast = loaded;
    }
    static int s_lastPin = 0;
    if (g_dantePinCount != s_lastPin) {        // report the running pin tally as you fight
        s_lastPin = g_dantePinCount;
        logf("[dante] pinned %d pl006 resources (via load-hook)", g_dantePinCount);
    }
    (void)g_dantePinned; (void)&dantePinPl006;
}
static void danteServiceTick() {
    if (g_dantePreload <= 0) return;
    if (dantePl006Loaded()) {
        g_dantePreload = 0;
        emSpawnNow(0x4E19A0);
        logf("[dante] pl006 ready -- spawned");
        return;
    }
    // Re-issue once a second, not every frame (re-requesting mid-load can restart it).
    float wr;                           // and don't burn the budget while paused
    bool running = getWorkRate(0, wr) ? (wr > 0.01f) : true;
    if (running) {
        if (g_dantePreload % 60 == 0) preloadPl006();
        if (--g_dantePreload == 0) logf("[dante] pl006 preload timed out");
    }
}

// ====================== Macro / autofire input hook ========================
// The input processor at module+0x45C2ED does `mov [esi+0x192c],eax` where eax =
// the raw controller buttons -- BEFORE the action system reads them and computes
// press-edges. We cave it to OR in g_macroInject (the pulsed macro bits we set
// each frame), so the game sees genuine taps. g_macroInject = 0 when no macro is
// active, so the hook is a no-op until used. Applied once at startup.
static volatile uint32_t g_macroInject = 0;
static volatile uint32_t g_macroHits = 0;   // cave-fire counter (diagnostic)
// Hold to Shoot: shoot is bit 0x2 in the +0x192c button word (verified live). When held we pulse
// the bit off every other frame so the game sees rapid press-edges -> autofire instead of a single
// shot / Nero charge. Driven from macroInputTick on the input thread.
static bool g_holdToShoot = false;
// ---- Input record / playback (macro replay) --------------------------------
// We hook the input-state store and, on the INPUT thread, call macroInputTick()
// once per input frame right after the processor wrote the live state. It
// records or replays the WHOLE input block -- buttons at +0x192c AND the analog
// stick/triggers spread through +0x1914..+0x1a10 -- so replays reproduce analog
// movement too, not just digital buttons, and the timing is frame-accurate
// (driven by the game's own input clock, not our render loop).
static const int MACRO_MAX_FRAMES   = 1800;     // ~30 s at 60 fps
// One recorded frame holds BOTH input sources the game reads that frame:
//   * pad  = the XInput gamepad (buttons + both sticks + both triggers) -- captured
//            and injected in the XInputGetState hook. Covers every controller that
//            presents as XInput (Xbox natively; DS4/DS5 via Steam Input / DS4Windows).
//   * keys = the 256-byte DirectInput keyboard state (DIK scancodes, 0x80 = down) --
//            captured and injected in the keyboard device's GetDeviceState hook.
// Both device hooks only STAGE (record) or INJECT (playback) using the current frame
// index; a single frame clock in hkPresent (replayFrameTick) commits one recorded
// frame per rendered frame and advances the play cursor, so the two streams never
// desync no matter what order the game polls them in. Layout mirrors XINPUT_GAMEPAD.
struct PadFrame { uint16_t wButtons; uint8_t bLT, bRT; int16_t sLX, sLY, sRX, sRY; };
static PadFrame g_padRec[MACRO_MAX_FRAMES];
static uint8_t  g_keyRec[MACRO_MAX_FRAMES][256]; // DI keyboard state per frame
static volatile int  g_recLen   = 0;            // recorded frame count
static volatile int  g_recState = 0;            // 0 idle, 1 recording, 2 playing
static volatile int  g_playIdx  = 0;
static bool g_liveOverride   = false;           // live input overrides playback (OFF = faithful full replay)
static bool g_recordOverride = false;           // punch-in: write live input back into the take
static bool g_replayLoop     = false;           // auto-replay: restart at end instead of stopping
static volatile uint32_t g_macroLive = 0;       // last live button word (menu activity readout)
// Per-frame staging: each device hook writes the live input it saw this frame here;
// replayFrameTick() snapshots them into the record when recording.
static PadFrame g_stagePad = {0,0,0,0,0,0,0};
static uint8_t  g_stageKeys[256] = {0};
static PadFrame g_lastXiPad = {0,0,0,0,0,0,0};  // last XInput gamepad seen (input diagnostic)
static bool     g_inpDiag = false;              // log merged game input vs XInput (diagnose macro coverage)
static volatile bool g_kbdHooked = false;       // true once the DI keyboard hook is armed
// "Is the player actively driving this frame?" -- any button, or a stick/trigger
// past its XInput dead-zone. Live Override uses this to yield playback to live input.
static inline bool padActive(const PadFrame& p) {
    const int DZL = 7849, DZR = 8689, TT = 30;  // XInput left/right stick dead-zones, trigger threshold
    if (p.wButtons) return true;
    if (p.bLT > TT || p.bRT > TT) return true;
    int lx = p.sLX < 0 ? -p.sLX : p.sLX, ly = p.sLY < 0 ? -p.sLY : p.sLY;
    int rx = p.sRX < 0 ? -p.sRX : p.sRX, ry = p.sRY < 0 ? -p.sRY : p.sRY;
    return lx > DZL || ly > DZL || rx > DZR || ry > DZR;
}
// "Is any key held?" over a 256-byte DI keyboard buffer (0x80 bit = pressed).
static inline bool keysActive(const uint8_t* k) {
    for (int i = 0; i < 256; i++) if (k[i] & 0x80) return true;
    return false;
}

// Called from the cave on the game's input thread, once per input frame.
extern "C" __attribute__((cdecl)) void macroInputTick(void* objv) {
    char* obj = (char*)objv;
    uint32_t live = *(uint32_t*)(obj + 0x192c);
    g_macroLive = live;
    g_macroHits++;                               // proves the hook is firing (shown in menu)
    // Hold to Shoot: pulse the shoot bit (0x2) -> a held shoot button reads as rapid taps.
    if (g_holdToShoot && (live & 0x2)) {
        static uint32_t afCtr = 0;
        if (afCtr++ & 1) { *(uint32_t*)(obj + 0x192c) = live & ~0x2u; }  // drop shoot every other frame
    }
    // NOTE: input record/playback is deliberately NOT done here. This game-code site
    // sits behind conditional branches and only executes on button *events* (a few
    // fires per second), which made recordings sparse and un-replayable. Record and
    // playback now live in hkXInputGetState, polled once per frame (frame-accurate).
}

// Hook the gameplay input processor (the one that reads the active character's
// action object at [ecx+0x4c4]) right after its LAST per-frame input write --
// mov [esi+0x1940],ecx at module+0x45C3A1 -- so buttons (+0x192c) and the analog
// stick (+0x1930..+0x1940) are all finalized before we capture/replay them. This
// is the function that actually runs every frame in gameplay (the earlier
// 0x4581xx/0x45829F builder does not), so the cave genuinely fires.
static const uint32_t kMacroHook    = 0x45C3A1;
static const uint8_t  kMacroOrig[6] = {0x89,0x8E,0x40,0x19,0x00,0x00};  // mov [esi+0x1940],ecx
static void* g_macroCave = nullptr;
static bool  g_macroHookOn = false;
static void applyMacroHook() {
    if (g_macroHookOn || !g_base) return;
    uintptr_t site = g_base + kMacroHook;
    if (!inModule(site, 6) || memcmp((void*)site, kMacroOrig, 6) != 0) { logf("[macro] hook site unexpected"); return; }
    uint8_t cave[] = {
        0x89,0x8E,0x40,0x19,0x00,0x00,   // [0]  mov [esi+0x1940], ecx (original last input write)
        0x60,                            // [6]  pushad
        0x56,                            // [7]  push esi   (input obj = arg, still live)
        0xB8,0,0,0,0,                    // [8]  mov eax, macroInputTick
        0xFF,0xD0,                       // [13] call eax
        0x83,0xC4,0x04,                  // [15] add esp, 4
        0x61,                            // [18] popad
        0xE9,0,0,0,0                     // [19] jmp back (site+6)
    };
    uint32_t fn = (uint32_t)(uintptr_t)&macroInputTick;  memcpy(cave + 9, &fn, 4);
    const size_t JMP = sizeof(cave) - 5;
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int32_t back = (int32_t)((site + 6) - ((uintptr_t)mem + JMP + 5));
    memcpy(cave + JMP + 1, &back, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[6] = { 0xE9,0,0,0,0, 0x90 };   // jmp rel32 + 1 nop (fill the 6-byte mov)
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    if (!writeBytes(site, hook, 6)) { VirtualFree(mem, 0, MEM_RELEASE); return; }
    g_macroCave = mem; g_macroHookOn = true;
    logf("[macro] input hook applied %p", mem);
}
static void macroToggleRecord() {
    if (g_recState == 1) {
        g_recState = 0;
        int nz = 0; for (int i = 0; i < g_recLen; i++) if (padActive(g_padRec[i])) nz++;
        logf("[macro] record stop: %d frames, %d with buttons, hookFires=%u, lastLive=0x%X",
             g_recLen, nz, g_macroHits, g_macroLive);
    } else { g_recLen = 0; g_playIdx = 0; g_replayLoop = false; g_recState = 1;
             logf("[macro] record start (hookFires=%u)", g_macroHits); }
}
static void macroTogglePlay() {                         // F3: play once
    if (g_recState == 2) { g_recState = 0; logf("[macro] play stop"); }
    else if (g_recLen > 0) { g_playIdx = 0; g_replayLoop = false; g_recState = 2;
                             logf("[macro] play start (%d frames)", g_recLen); }
}
static void macroToggleLoop() {                         // F4: auto-replay (loop until stopped)
    if (g_recState == 2) { g_recState = 0; g_replayLoop = false; logf("[macro] auto-replay stop"); }
    else if (g_recLen > 0) { g_playIdx = 0; g_replayLoop = true; g_recState = 2;
                             logf("[macro] auto-replay start (%d frames, looping)", g_recLen); }
}

// ====================== HUD element editor (experimental) ==================
// The whole HUD is one widget-tree walker (module+0x6AD8C4 -> 0xAB2C60). Every
// element (health bar, DT bar, style/rank meter, weapons) is a node, drawn only
// when bit 0x100000 of [widget+0x54] is set. To hide ONE element we capture the
// tree root each HUD frame via a code cave (mov ecx,[edi+0xf4] at +0x6AD8BD is
// the root load just before the walker call), walk the tree, and clear that bit
// on the chosen widget(s). All reads are guarded; nothing patches game code
// except the reversible capture cave.
static const uint32_t kHudCapHook    = 0x6AD8BD;                       // mov ecx,[edi+0xf4]
static const uint8_t  kHudCapOrig[6] = {0x8B,0x8F,0xF4,0x00,0x00,0x00};
static void*    g_hudCave = nullptr;
static uint8_t  g_hudCapSaved[6];
static bool     g_hudCapOn = false;
static volatile uintptr_t g_hudRoot = 0;     // written by the cave each HUD frame
static bool applyHudHideHook();              // fwd (in-walk hide cave, defined below)
static void stopHudHideHook();
static bool g_hudHpOn;                        // fwd ref used by stopHudCapture

static bool applyHudCapture() {
    if (g_hudCapOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kHudCapHook;
    if (memcmp((void*)site, kHudCapOrig, 6) != 0) {
        logf("[hud] capture site unexpected 0x%02X", *(uint8_t*)site); return false;
    }
    uint8_t cave[] = {
        0x8B,0x8F,0xF4,0x00,0x00,0x00,      // mov ecx,[edi+0xf4]   (original)
        0x89,0x0D,0,0,0,0,                  // mov [&g_hudRoot],ecx (imm patched below)
        0xE9,0,0,0,0                        // jmp back (rel patched below)
    };
    uint32_t addr = (uint32_t)(uintptr_t)&g_hudRoot;
    memcpy(cave + 8, &addr, 4);
    const size_t JMP_OFF = sizeof(cave) - 5;
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;
    uintptr_t ret = site + 6;
    int32_t backRel = (int32_t)(ret - ((uintptr_t)mem + JMP_OFF + 5));
    memcpy(cave + JMP_OFF + 1, &backRel, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[6] = { 0xE9,0,0,0,0, 0x90 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_hudCapSaved, (void*)site, 6);
    if (!writeBytes(site, hook, 6)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_hudCave = mem; g_hudCapOn = true;
    applyHudHideHook();                         // finder needs the in-walk cave to actually hide
    logf("[hud] capture cave applied %p", mem);
    return true;
}
static void stopHudCapture() {
    if (!g_hudCapOn) return;
    writeBytes(g_base + kHudCapHook, g_hudCapSaved, 6);
    if (g_hudCave) { VirtualFree(g_hudCave, 0, MEM_RELEASE); g_hudCave = nullptr; }
    g_hudCapOn = false; g_hudRoot = 0;
    if (!g_hudHpOn) stopHudHideHook();          // release the cave unless HP toggle still wants it
}

static std::vector<uintptr_t> g_hudNodes;    // widgets found this frame (finder)
static int      g_hudHideIdx  = -1;          // finder: index to hide (-1 = none)
static uint32_t g_hudSelVtbl  = 0;           // vtable RVA of the selected index (display)
static uint32_t g_hudLockVtbl = 0;           // saved HP-bar vtable RVA (persists)
// g_hudHpOn declared above (forward); "Hide HP HUD" active

// ---- Named HUD element toggles (4Hook-style). Each element is one or more widget
// vtables (RVAs from RE); the in-walk hide cave clears the draw bit for any widget
// whose vtable is in the active set. Per-character elements list every char's vtable
// (only the active char's is present in the tree, so listing all is harmless).
enum { HUD_HP, HUD_WEAPON, HUD_TIMER, HUD_STYLE, HUD_ORBS, HUD_MAP, HUD_BOSS, HUD_LOCKON, HUD_N };
static const char* kHudElemNames[HUD_N] = {
    "HP / DT gauge", "Weapon HUD", "Timer", "Style / rank dial",
    "Red Orbs", "Map / radar", "Boss HP", "Lock-on / enemy"
};
static const uint32_t kHudVtbls[HUD_N][6] = {
    {0xC116B8,0xC12B88,0xC11A20,0xC12D40,0xC12F80,0},  // HP/DT: Dante,Nero,Lady,Trish,Vergil
    {0xC11850,0xC11BC0,0xC13120,0,0,0},                // Weapon: DanteSub,LadySub,VergilSub
    {0xC127F0,0,0,0,0,0},                              // Timer
    {0xC12680,0,0,0,0,0},                              // Style/rank
    {0xC124F8,0,0,0,0,0},                              // Red Orbs
    {0xC12388,0,0,0,0,0},                              // Map/radar
    {0xC114E8,0,0,0,0,0},                              // Boss HP
    {0xC11D40,0xC11EB8,0xC121B0,0xC12038,0,0},         // Lock-on: Nero,Dante,Vergil,multi
};
static bool g_hudHide[HUD_N] = { false };
static bool g_hudHideAll = false;                       // hide the whole cockpit
static volatile uint32_t g_hideSet[64];                 // absolute vtables the cave hides
static volatile int      g_hideSetN = 0;
static volatile bool     g_hudProbe = false;            // log each live widget's fields once
static bool g_keepWeapons = false;                      // exempt weapon widgets from hide-all
// Per-element keep-boxes (1920x1080 virtual screen) the user dialed in with the
// sliders. Under Hide-all, a widget stays visible if it's inside ANY enabled box.
// Each element can use up to TWO boxes (e.g. health bar + its corner dial).
// Lock-on widgets are owned (at widget+0x6C) by one of these uHUD_GUI_LockOn* controllers
// (Nero/Dante/Vergil/multi) -- owner-based keep follows the marker wherever the enemy moves.
static const uint32_t kLockOnOwners[] = { 0xC11D40, 0xC11EB8, 0xC121B0, 0xC12038 };
struct KeepRegion { const char* name; bool on; float a[4]; float b[4]; const uint32_t* owners; int nOwners; };
static KeepRegion g_keep[] = {
    { "Style meter (rank + bar)", false, {1201,1980, 200, 500}, {0,0,0,0}, nullptr, 0 },
    { "Style names",              false, { 265, 278,  23, 227}, {0,0,0,0}, nullptr, 0 },
    { "Bloody Palace timer",      false, { 334,1115, 188, 313}, {264,626,274,385}, nullptr, 0 }, // Dante + Vergil
    // "Devil Trigger" keep-box removed: its region overlapped the HP/DT bar band, so
    // enabling it kept far more than the DT gauge (glitched).
    // "Style dial" keep-boxes (Dante/Vergil/Trish/Lady) removed per request.
    { "Health bar + dial",        false, {  28, 264, 149, 191}, {0,0,0,0}, nullptr, 0 },
    { "Lock-on (target marker)",  false, {0,0,0,0}, {0,0,0,0}, kLockOnOwners, 4 },               // owner-based
};
static uint32_t widgetOwnerVt(uint32_t widget) {          // [widget+0x6C] -> owner -> owner vtable RVA
    if (widget < 0x10000 || !memReadable((void*)(widget + 0x6C), 4)) return 0;
    uint32_t owner = *(uint32_t*)(widget + 0x6C);
    if (owner <= (uint32_t)g_base || !memReadable((void*)owner, 4)) return 0;
    uint32_t ovt = *(uint32_t*)owner;
    return (ovt >= (uint32_t)g_base) ? ovt - (uint32_t)g_base : 0;
}
static const int kNKeep = (int)(sizeof(g_keep) / sizeof(g_keep[0]));
static bool  g_keepStyle = false;                       // legacy: kept for weapon-mode coupling (unused box)
// Widgets carry their GUI screen position at float [w+0x40]=X, [w+0x44]=Y (1920x1080
// virtual). The region keeps exempt by box; "Keep weapons" matches the actual
// weapon-HUD widget by VTABLE (not by screen region -- the HP/DT bar sits in the same
// corner, so a region keep leaked the health bar). Returns true to stay visible.
static bool hudIsWeaponWidget(uint32_t widget) {
    if (widget < 0x10000 || !memReadable((void*)(widget + 0x44), 4)) return false;
    float x = *(float*)(widget + 0x40), y = *(float*)(widget + 0x44);
    if (g_keepWeapons && y >= 833.0f) return true;                  // weapon HUD = bottom strip (Y 833-1080); HP/DT bar is up top
    for (int i = 0; i < kNKeep; i++) {
        KeepRegion& k = g_keep[i];
        if (!k.on) continue;
        if (k.owners) {                                            // owner-based (e.g. lock-on)
            uint32_t ovt = widgetOwnerVt(widget);
            for (int j = 0; j < k.nOwners; j++) if (ovt == k.owners[j]) return true;
            continue;
        }
        if (x >= k.a[0] && x <= k.a[1] && y >= k.a[2] && y <= k.a[3]) return true;
        if (k.b[1] > k.b[0] && x >= k.b[0] && x <= k.b[1] && y >= k.b[2] && y <= k.b[3]) return true;
    }
    return false;
}
// Called per widget from the in-walk cave with the widget INSTANCE pointer: return 1
// to clear its draw bit. Also the probe -- logs each distinct widget's position fields
// so the bottom-corner weapon widgets can be identified and exempted.
extern "C" int __attribute__((cdecl)) hudHideTest(uint32_t widget) {
    if (widget < 0x10000) return 0;
    uint32_t vtblAbs = *(uint32_t*)widget;              // [widget+0] = vtable
    if (g_hudProbe) {
        static uint32_t seen[96]; static int seenN = 0;
        bool dup = false; for (int i = 0; i < seenN; i++) if (seen[i] == widget) { dup = true; break; }
        if (!dup && seenN < 96 && memReadable((void*)(widget + 0x60), 4)) {
            seen[seenN++] = widget;
            float* f = (float*)widget;
            logf("[hudprobe] w=0x%X vt=0x%X x=%.0f y=%.0f", widget,
                 (vtblAbs >= (uint32_t)g_base) ? vtblAbs - (uint32_t)g_base : vtblAbs, f[0x40/4], f[0x44/4]);
            // owner scan: any field pointing to a uHUD_GUI controller (vtable in the
            // 0xBF0000-0xC20000 RVA band) reveals which element this widget belongs to.
            uint32_t base = (uint32_t)g_base;
            for (uint32_t off = 4; off <= 0x90; off += 4) {
                if (!memReadable((void*)(widget + off), 4)) continue;
                uint32_t p = *(uint32_t*)(widget + off);
                if (p <= base + 0x1000 || !memReadable((void*)p, 4)) continue;
                uint32_t pvt = *(uint32_t*)p;
                if (pvt >= base + 0xBF0000 && pvt <= base + 0xC20000)
                    logf("[hudprobe]   w=0x%X owner@+0x%X vt=0x%X", widget, off, pvt - base);
            }
        }
    }
    // "Keep mode": active if Hide-all is on OR ANY keep-toggle is checked. In keep mode
    // every widget is hidden EXCEPT the ones a keep-box exempts -- so checking a piece
    // hides everything else automatically, no need to click Hide-all first.
    bool keepMode = g_hudHideAll || g_keepWeapons;
    for (int i = 0; i < kNKeep && !keepMode; i++) if (g_keep[i].on) keepMode = true;
    if (!keepMode) {
        int n = g_hideSetN;
        for (int i = 0; i < n; i++) if (g_hideSet[i] == vtblAbs) return 1;
        return 0;
    }
    if (hudIsWeaponWidget(widget)) return 0;   // kept
    return 1;                                  // everything else hidden
}

// ---- In-walk hide cave. The renderer recomputes each top-level widget's draw
// bit every frame, so a post-frame flag clear never sticks. Instead we hook the
// draw-gate INSIDE the walker (at +0x6B2CFC, right after the recompute, before
// the gate test): for the widget whose vtable == g_hideVtblAbs we clear bit
// 0x100000 in-place, so the very next instruction skips its draw. Survives the
// recompute because it runs after it, mid-walk.
static const uint32_t kHudHideHook    = 0x6B2CFC;                              // mov eax,[esi+0x54]; shr eax,0x14
static const uint8_t  kHudHideOrig[6] = {0x8B,0x46,0x54,0xC1,0xE8,0x14};
static void*    g_hudHideCave = nullptr;
static uint8_t  g_hudHideSaved[6];
static bool     g_hudHideOn   = false;

static bool applyHudHideHook() {
    if (g_hudHideOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kHudHideHook;
    if (memcmp((void*)site, kHudHideOrig, 6) != 0) { logf("[hud] hide site unexpected 0x%02X", *(uint8_t*)site); return false; }
    uint8_t cave[] = {
        0x51,                      // [0]  push ecx
        0x52,                      // [1]  push edx
        0x56,0x90,                 // [2]  push esi (widget instance = arg) + nop (keep layout)
        0xB8,0,0,0,0,              // [4]  mov eax, hudHideTest (imm32 @5)
        0xFF,0xD0,                 // [9]  call eax
        0x83,0xC4,0x04,            // [11] add esp,4
        0x5A,                      // [14] pop edx
        0x59,                      // [15] pop ecx
        0x84,0xC0,                 // [16] test al,al
        0x8B,0x46,0x54,            // [18] mov eax,[esi+0x54]  (original load)
        0x74,0x08,                 // [21] jz +8 (skip the clear -> shr)
        0x25,0xFF,0xFF,0xEF,0xFF,  // [23] and eax,0xFFEFFFFF  (clear draw bit)
        0x89,0x46,0x54,            // [28] mov [esi+0x54],eax
        0xC1,0xE8,0x14,            // [31] shr eax,0x14        (original)
        0xE9,0,0,0,0               // [34] jmp back (rel32 @ 35)
    };
    uint32_t fnaddr = (uint32_t)(uintptr_t)&hudHideTest;
    memcpy(cave + 5, &fnaddr, 4);
    const size_t JMP_OFF = sizeof(cave) - 5;
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;
    uintptr_t ret = site + 6;
    int32_t backRel = (int32_t)(ret - ((uintptr_t)mem + JMP_OFF + 5));
    memcpy(cave + JMP_OFF + 1, &backRel, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[6] = { 0xE9,0,0,0,0, 0x90 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_hudHideSaved, (void*)site, 6);
    if (!writeBytes(site, hook, 6)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_hudHideCave = mem; g_hudHideOn = true;
    logf("[hud] hide cave applied %p", mem);
    return true;
}
static void stopHudHideHook() {
    if (!g_hudHideOn) return;
    writeBytes(g_base + kHudHideHook, g_hudHideSaved, 6);
    if (g_hudHideCave) { VirtualFree(g_hudHideCave, 0, MEM_RELEASE); g_hudHideCave = nullptr; }
    g_hudHideOn = false; g_hideSetN = 0;
}

// "Hide HP HUD" maps to the HP element toggle now.
static void setHideHp(bool on) {
    g_hudHpOn = on;
    g_hudHide[HUD_HP] = on;
    if (on) applyHudHideHook();
}
static bool hudAnyActive() {
    if (g_hudHideAll || g_hudCapOn || g_hudProbe || g_keepWeapons) return true;
    for (int i = 0; i < kNKeep; i++) if (g_keep[i].on) return true;
    for (int e = 0; e < HUD_N; e++) if (g_hudHide[e]) return true;
    return false;
}
// Weapon HUD control. The weapon HUD's visibility is driven by a fade object: a
// per-frame countdown `dec byte [ecx+0x2ac]` at RVA 0x89148 (ecx=fade obj; +0x2ac
// counter, +0x2a0 alpha). We cave that site and switch behaviour on g_wpnMode:
//   0 normal | 1 always show (keep counter alive) | 2 always hide (alpha=0).
static const uint32_t kWpnFadeDec     = 0x89148;    // RVA (vaddr 0x489148): dec byte [ecx+0x2ac]
static const uint8_t  kWpnFadeOrig[6] = {0xFE,0x89,0xAC,0x02,0x00,0x00};
static volatile int g_wpnMode = 0;                  // 0 normal, 1 always-show, 2 always-hide
static volatile uintptr_t g_wpnFadeObj = 0;         // captured by the cave (ecx at the fade tick)
static void*  g_wpnCave = nullptr;
static bool   g_wpnCaveOn = false;
// The cave just captures the weapon fade object (ecx) and runs the original dec. The
// trainer then pins its alpha each frame (weaponFadeTick), which runs at Present --
// after the game's fade animation -- so always-show truly never fades.
static void installWeaponCave() {
    if (g_wpnCaveOn || !g_base) return;
    uintptr_t site = g_base + kWpnFadeDec;
    if (!inModule(site, 6) || memcmp((void*)site, kWpnFadeOrig, 6) != 0) { logf("[hud] wpn-fade site unexpected"); g_wpnCaveOn = true; return; }
    uint32_t objAddr = (uint32_t)(uintptr_t)&g_wpnFadeObj;
    uint8_t cave[17];
    cave[0]=0x89; cave[1]=0x0D; memcpy(cave+2,&objAddr,4);            // mov [g_wpnFadeObj], ecx
    cave[6]=0xFE; cave[7]=0x89; cave[8]=0xAC; cave[9]=0x02; cave[10]=0x00; cave[11]=0x00; // dec byte[ecx+0x2ac]
    cave[12]=0xE9;                                                    // jmp back
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int32_t back = (int32_t)((g_base + kWpnFadeDec + 6) - ((uintptr_t)mem + 12 + 5));
    memcpy(cave+13, &back, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[6] = { 0xE9,0,0,0,0, 0x90 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook+1, &to, 4);
    if (writeBytes(site, hook, 6)) { g_wpnCave = mem; g_wpnCaveOn = true; logf("[hud] weapon cave applied %p", mem); }
    else VirtualFree(mem, 0, MEM_RELEASE);
}
static void setWeaponMode(int m) { g_wpnMode = m; installWeaponCave(); }
// Per-frame (at Present): force the weapon fade object's alpha so always-show never
// fades and always-hide stays gone, regardless of weapon switches.
static void weaponFadeTick() {
    uintptr_t obj = g_wpnFadeObj;
    if (!g_wpnMode || !obj || !memReadable((void*)(obj + 0x2b0), 4)) return;
    if (g_wpnMode == 1) {                            // always show: pin alpha + target full, keep counter alive
        *(float*)(obj + 0x2a0) = 9.0f;
        *(float*)(obj + 0x2a4) = 9.0f;
        *(uint16_t*)(obj + 0x2ac) = 0x105;
    } else if (g_wpnMode == 2) {                     // always hide
        *(float*)(obj + 0x2a0) = 0.0f;
    }
}

// DFS the widget tree (child 0x60, alt 0x64, sibling 0x68), de-duped & capped.
static void buildHudNodes() {
    g_hudNodes.clear();
    uintptr_t root = g_hudRoot;
    if (!root || !memReadable((void*)root, 4)) return;
    std::vector<uintptr_t> stack;
    uintptr_t c0 = 0; if (readPtr(root + 0x60, c0) && c0) stack.push_back(c0);
    while (!stack.empty() && (int)g_hudNodes.size() < 256) {
        uintptr_t n = stack.back(); stack.pop_back();
        if (!n || !memReadable((void*)(n + 0x58), 4)) continue;
        bool seen = false; for (auto p : g_hudNodes) if (p == n) { seen = true; break; }
        if (seen) continue;
        g_hudNodes.push_back(n);
        uintptr_t a = 0;
        if (readPtr(n + 0x60, a) && a) stack.push_back(a);
        if (readPtr(n + 0x64, a) && a) stack.push_back(a);
        if (readPtr(n + 0x68, a) && a) stack.push_back(a);
    }
}
static void updateHud() {
    if (g_keepWeapons) { g_wpnMode = 1; installWeaponCave(); }   // kept weapons never fade
    weaponFadeTick();                               // pin weapon alpha (always-show never fades)
    if (!hudAnyActive()) { g_hideSetN = 0; return; }
    if (!g_hudHideOn) applyHudHideHook();       // engage the in-walk cave once needed
    // Build the hide set from the named element toggles...
    int n = 0;
    for (int e = 0; e < HUD_N; e++) if (g_hudHide[e])
        for (int j = 0; j < 6 && kHudVtbls[e][j]; j++)
            if (n < 64) g_hideSet[n++] = (uint32_t)(g_base + kHudVtbls[e][j]);
    // ...plus the finder's currently-selected widget (manual element ID).
    if (g_hudCapOn) {
        buildHudNodes();
        g_hudSelVtbl = 0;
        if (g_hudHideIdx >= 0 && g_hudHideIdx < (int)g_hudNodes.size()) {
            uintptr_t nd = g_hudNodes[g_hudHideIdx], vt = 0;
            if (readPtr(nd, vt) && vt >= g_base) { g_hudSelVtbl = (uint32_t)(vt - g_base); if (n < 64) g_hideSet[n++] = (uint32_t)vt; }
        }
    }
    g_hideSetN = n;
}

// ---- Increased jump-cancel range (ported from a community CE table). The
// site at module+0x1B6430 runs `addss xmm1,xmm2 ; mulss xmm3,xmm3`; we cave it to
// first multiply xmm3 by an adjustable window multiplier (1.0 = vanilla, higher =
// bigger jump-cancel range on enemies). The multiplier lives in the cave so the
// slider updates it live. ------------------------------------------------------
static const uint32_t kJCHook = 0x1B6430;
static const uint8_t  kJCOrig[8] = { 0xF3,0x0F,0x58,0xCA, 0xF3,0x0F,0x59,0xDB };
static void*   g_jcCave = nullptr;
static uint8_t g_jcSaved[8];
static bool    g_jcOn   = false;
static float   g_jcMult = 1.5f;                 // jump-cancel window size
static const size_t kJCMultOff = 25;            // offset of the float inside the cave

static void setJumpCancelMult(float m) {        // live-update the multiplier
    g_jcMult = m;
    if (g_jcOn && g_jcCave) memcpy((uint8_t*)g_jcCave + kJCMultOff, &m, 4);
}
static bool applyJumpCancel() {
    if (g_jcOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kJCHook;
    if (memcmp((void*)site, kJCOrig, 8) != 0) {
        logf("[jc] hook site unexpected (0x%02X) - aborting", *(uint8_t*)site);
        return false;
    }
    uint8_t cave[] = {
        0xF3,0x0F,0x10,0x3D, 0,0,0,0,     // movss xmm7,[mult]   (abs addr patched at +4)
        0xF3,0x0F,0x59,0xDF,              // mulss xmm3,xmm7
        0xF3,0x0F,0x58,0xCA,              // addss xmm1,xmm2     (original)
        0xF3,0x0F,0x59,0xDB,              // mulss xmm3,xmm3     (original)
        0xE9, 0,0,0,0,                    // jmp back            (rel32 patched at +21)
        0,0,0,0                           // multiplier float    (offset 25)
    };
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[jc] VirtualAlloc failed"); return false; }
    uint32_t multAbs = (uint32_t)((uintptr_t)mem + kJCMultOff);
    memcpy(cave + 4, &multAbs, 4);
    int32_t backRel = (int32_t)((site + 8) - ((uintptr_t)mem + 21 + 5));
    memcpy(cave + 21, &backRel, 4);
    memcpy(cave + kJCMultOff, &g_jcMult, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[8] = { 0xE9, 0,0,0,0, 0x90,0x90,0x90 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_jcSaved, (void*)site, 8);
    if (!writeBytes(site, hook, 8)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_jcCave = mem; g_jcOn = true;
    logf("[jc] applied (cave=%p mult=%.2f)", mem, g_jcMult);
    return true;
}
static void stopJumpCancel() {
    if (!g_jcOn) return;
    writeBytes(g_base + kJCHook, g_jcSaved, 8);
    if (g_jcCave) { VirtualFree(g_jcCave, 0, MEM_RELEASE); g_jcCave = nullptr; }
    g_jcOn = false;
    logf("[jc] removed");
}

// ===== MistressDMC moveset ports (3 cheats not previously in the menu) ============
// All three follow the jump-cancel pattern: alloc an exec cave, hand-assembled and
// byte-verified with i686-w64-mingw32-as, runtime-patch the absolute data/return
// addresses, redirect the hook site with a rel32 jmp, and guard the original bytes
// so a build mismatch fails safely (logs + returns false) instead of crashing.

// ---- Charge Rate Increase Universal (table entry #38). Hook exe+0x53BA45 runs
// `addss xmm0,[ecx+10]` (the per-frame charge accumulate); we add an adjustable
// rate on top so all charges (Nero gun, Dante guns/Gilgamesh, etc.) fill faster.
static const uint32_t kCRHook = 0x53BA45;
static const uint8_t  kCROrig[5] = { 0xF3,0x0F,0x58,0x41,0x10 };
static void*   g_crCave = nullptr;
static uint8_t g_crSaved[5];
static bool    g_crOn   = false;                // installed (hook live) -- managed automatically
static bool    g_crWant  = false;               // user wants it (UI toggle)
static float   g_crRate = 1.0f;                 // extra charge added per frame
static const size_t kCRRateOff = 25;
static void setChargeRate(float r) {
    g_crRate = r;
    if (g_crOn && g_crCave) memcpy((uint8_t*)g_crCave + kCRRateOff, &r, 4);
}
static bool applyChargeRate() {
    if (g_crOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kCRHook;
    if (memcmp((void*)site, kCROrig, 5) != 0) { logf("[crate] site unexpected (0x%02X)", *(uint8_t*)site); return false; }
    uint8_t cave[] = {
        0xF3,0x0F,0x10,0x1D, 0,0,0,0,   // movss xmm3,[rate]   (abs @4)
        0xF3,0x0F,0x58,0x41,0x10,       // addss xmm0,[ecx+10]
        0xF3,0x0F,0x58,0xC3,            // addss xmm0,xmm3
        0x0F,0x57,0xDB,                 // xorps xmm3,xmm3
        0xE9, 0,0,0,0,                  // jmp returnhere      (rel32 @21)
        0,0,0,0                         // rate float          (offset 25)
    };
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[crate] VirtualAlloc failed"); return false; }
    uint32_t rateAbs = (uint32_t)((uintptr_t)mem + kCRRateOff);
    memcpy(cave + 4, &rateAbs, 4);
    // back-jmp rel32 is relative to the END of the E9 instruction (E9 at offset 20).
    int32_t backRel = (int32_t)((site + 5) - ((uintptr_t)mem + 20 + 5));
    memcpy(cave + 21, &backRel, 4);
    memcpy(cave + kCRRateOff, &g_crRate, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[5] = { 0xE9, 0,0,0,0 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_crSaved, (void*)site, 5);
    if (!writeBytes(site, hook, 5)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_crCave = mem; g_crOn = true;
    logf("[crate] applied (cave=%p rate=%.2f)", mem, g_crRate);
    return true;
}
static void stopChargeRate() {
    if (!g_crOn) return;
    writeBytes(g_base + kCRHook, g_crSaved, 5);
    if (g_crCave) { VirtualFree(g_crCave, 0, MEM_RELEASE); g_crCave = nullptr; }
    g_crOn = false;
    logf("[crate] removed");
}

// ---- Lady Jump Cancel Animations. EXACT port of DMC Revamped's cancel_generator.py
// (the proven, shipping implementation). Hook exe+0xA1B18 (the cancel writer
// `mov [ecx+39A8],eax`). For Lady's shotgun stinger-knockback (0x439), trigger happy
// (0x347), gun throw (0x357) and double barrels (0x432), once the move timer
// [ebx+52C] passes the per-move threshold, route eax+2 into the jump-cancel slot
// [ecx+3924]. Reads move id/timer from EBX and writes via ECX -- both are the actor
// here (the routine runs per-frame during a move, so the threshold opens the cancel
// mid-recovery exactly as Revamped intends). This is a CAVE (game-thread hook); the
// earlier per-frame "force" raced the game thread and never stuck -- that was the bug.
static const uint32_t kLJHook = 0xA1B18;
static const uint8_t  kLJOrig[6] = { 0x89,0x81,0xA8,0x39,0x00,0x00 };
static void*   g_ljCave = nullptr;
static uint8_t g_ljSaved[6];
static bool    g_ljOn   = false;                // installed (hook live) -- managed automatically
static bool    g_ljWant = false;               // user wants it (UI toggle)
static bool applyLadyJC() {
    if (g_ljOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kLJHook;
    if (memcmp((void*)site, kLJOrig, 6) != 0) { logf("[ladyjc] site unexpected (0x%02X)", *(uint8_t*)site); return false; }
    // Mask the move id to 16 bits (strips 0x10000/0x40000/0x1000000 state bits) and cancel
    // any of Lady's WEAPON moves in one range, 0x200..0x52F: Kalina Ann incl. Hysteric/
    // Multiple (0x2xx, confirmed from isolated capture), her melee strings (0x3xx), pistols
    // (0x340-0x35F), shotgun (0x4xx), and the 0x5xx set. Basic locomotion/jump is < 0x200
    // so it's left alone. Tiny 1.0 timer gate so the move fires before it's cancelable.
    // edx is dead across the hook (game does `mov dl,1` a few instrs later); push/pop to be
    // safe. Capstone-verified branch targets.
    // Exclusions (captured via the [ladyid] logger): Lady's STYLE-move / weapon-pull
    // ids sit inside the 0x200-0x52F cancel range, so the cave was cancel-routing them
    // and skipping the animation write -> the pull glitched (it flickered id<->id+2,
    // where id+2 is the cave's own corrupted echo from `add eax,2`). We send those ids
    // down the normal (abort) path so the pull animates cleanly; every other Lady
    // cancel is unchanged. Ground pull: 0x500 / 0x51E (echo 0x520). Air pull (jumping):
    // 0x50F / 0x51F (echo 0x521).
    uint8_t cave[] = {
        0x52,                                          // [0]   push edx
        0x8b,0x93,0xe4,0x04,0x00,0x00,                 // [1]   mov edx,[ebx+4E4]
        0x81,0xe2,0xff,0xff,0x00,0x00,                 // [7]   and edx,0xFFFF
        0x81,0xfa,0x00,0x02,0x00,0x00,                 // [13]  cmp edx,0x200
        0x72,0x58,                                     // [19]  jb abort (->109)
        0x81,0xfa,0x30,0x05,0x00,0x00,                 // [21]  cmp edx,0x530
        0x73,0x50,                                     // [27]  jae abort (->109)   keep 0x200-0x52F
        0x81,0xfa,0x00,0x05,0x00,0x00,                 // [29]  cmp edx,0x500   (ground style -- exclude)
        0x74,0x48,                                     // [35]  je abort (->109)
        0x81,0xfa,0x1e,0x05,0x00,0x00,                 // [37]  cmp edx,0x51E   (ground pull -- exclude)
        0x74,0x40,                                     // [43]  je abort (->109)
        0x81,0xfa,0x20,0x05,0x00,0x00,                 // [45]  cmp edx,0x520   (ground +2 echo -- exclude)
        0x74,0x38,                                     // [51]  je abort (->109)
        0x81,0xfa,0x0f,0x05,0x00,0x00,                 // [53]  cmp edx,0x50F   (air style -- exclude)
        0x74,0x30,                                     // [59]  je abort (->109)
        0x81,0xfa,0x1f,0x05,0x00,0x00,                 // [61]  cmp edx,0x51F   (air pull -- exclude)
        0x74,0x28,                                     // [67]  je abort (->109)
        0x81,0xfa,0x21,0x05,0x00,0x00,                 // [69]  cmp edx,0x521   (air +2 echo -- exclude)
        0x74,0x20,                                     // [75]  je abort (->109)
        0xf3,0x0f,0x10,0x93, 0x2c,0x05,0x00,0x00,      // [77]  movss xmm2,[ebx+52C]
        0xf3,0x0f,0x10,0x3d, 0,0,0,0,                  // [85]  movss xmm7,[thresh]  abs@89
        0x0f,0x2f,0xd7,                                // [93]  comiss xmm2,xmm7
        0x72,0x0b,                                     // [96]  jb abort (->109)
        0x83,0xc0,0x02,                                // [98]  add eax,2
        0x89,0x81,0x24,0x39,0x00,0x00,                 // [101] mov [ecx+3924],eax  (jump cancel)
        0xeb,0x06,                                     // [107] jmp done (->115)
        0x89,0x81,0xa8,0x39,0x00,0x00,                 // [109] abort: mov [ecx+39A8],eax
        0x0f,0x57,0xd2,                                // [115] done: xorps xmm2,xmm2
        0x0f,0x57,0xff,                                // [118] xorps xmm7,xmm7
        0x5a,                                          // [121] pop edx
        0xE9, 0,0,0,0,                                 // [122] jmp returnhere   rel32@123
        0,0,0,0                                        // [127] thresh (float)
    };
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[ladyjc] VirtualAlloc failed"); return false; }
    const float thresh = 1.0f;
    memcpy(cave + 127, &thresh, 4);
    uint32_t threshAbs = (uint32_t)((uintptr_t)mem + 127);
    memcpy(cave + 89, &threshAbs, 4);
    // back-jmp rel32 relative to END of the E9 (E9 at offset 122, rel32 field @123).
    int32_t backRel = (int32_t)((site + 6) - ((uintptr_t)mem + 122 + 5));
    memcpy(cave + 123, &backRel, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[6] = { 0xE9, 0,0,0,0, 0x90 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_ljSaved, (void*)site, 6);
    if (!writeBytes(site, hook, 6)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_ljCave = mem; g_ljOn = true;
    logf("[ladyjc] applied (cave=%p)", mem);
    return true;
}
static void stopLadyJC() {
    if (!g_ljOn) return;
    writeBytes(g_base + kLJHook, g_ljSaved, 6);
    if (g_ljCave) { VirtualFree(g_ljCave, 0, MEM_RELEASE); g_ljCave = nullptr; }
    g_ljOn = false;
    logf("[ladyjc] removed");
}

// ---- Lady/Trish/Vergil height restriction bypass (table entry #39). Hook
// the selective-cancel gate `cmp dword [esi-24],0 / je` at exe+0x538B80. Only for
// the player-controlled entity ([exe+F59F00]->+24 == eax), force the cancel flag
// to 2 for a set of aerial move states, removing the height restriction on cancels.
static const uint32_t kHRHook = 0x538B80;
static const uint8_t  kHROrig[6] = { 0x83,0x7E,0xDC,0x00,0x74,0x49 };
static void*   g_hrCave = nullptr;
static uint8_t g_hrSaved[6];
static bool    g_hrOn   = false;                // installed (hook live) -- managed automatically
static bool    g_hrWant = false;               // user wants it (UI toggle)
static bool applyHeightBypass() {
    if (g_hrOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kHRHook;
    if (memcmp((void*)site, kHROrig, 6) != 0) { logf("[height] site unexpected (0x%02X) - aborting", *(uint8_t*)site); return false; }
    uint8_t cave[] = {
        0x52,                                       // push edx
        0x8b,0x15, 0,0,0,0,                          // mov edx,[player_global]  (abs @3)
        0x8b,0x52,0x24,                              // mov edx,[edx+24]
        0x39,0xc2,                                   // cmp edx,eax
        0x5a,                                        // pop edx
        0x75,0x4a,                                   // jne code
        0x80,0xb8,0x14,0x1a,0x00,0x00,0x07, 0x74,0x38,  // cmp [eax+1A14],7 ; je cancellable
        0x80,0xb8,0x14,0x1a,0x00,0x00,0x06, 0x74,0x2f,
        0x80,0xb8,0x14,0x1a,0x00,0x00,0x04, 0x74,0x26,
        0x80,0xb8,0x14,0x1a,0x00,0x00,0x0f, 0x74,0x1d,
        0x80,0xb8,0x14,0x1a,0x00,0x00,0x0f, 0x74,0x14,
        0x80,0xb8,0x14,0x1a,0x00,0x00,0x1e, 0x74,0x0b,
        0x80,0xb8,0x14,0x1a,0x00,0x00,0x06, 0x74,0x02,
        0xeb,0x09,                                   // jmp code
        0xc7,0x46,0xdc,0x02,0x00,0x00,0x00,          // cancellable: mov dword [esi-24],2
        0xeb,0x00,                                   // jmp code
        0x83,0x7e,0xdc,0x00,                         // code: cmp dword [esi-24],0
        0x0f,0x84, 0,0,0,0,                          // je <orig je target>   (rel32 @95)
        0xE9, 0,0,0,0                                // jmp returnhere        (rel32 @100)
    };
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[height] VirtualAlloc failed"); return false; }
    uint32_t playerAbs = (uint32_t)(g_base + 0xF59F00);
    memcpy(cave + 3, &playerAbs, 4);
    int32_t jeRel = (int32_t)((site + 6 + 0x49) - ((uintptr_t)mem + 99));  // je dest (end of je @99)
    memcpy(cave + 95, &jeRel, 4);
    // back-jmp rel32 relative to END of the E9 (E9 at offset 99, rel32 field @100).
    int32_t backRel = (int32_t)((site + 6) - ((uintptr_t)mem + 99 + 5)); // returnhere
    memcpy(cave + 100, &backRel, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[6] = { 0xE9, 0,0,0,0, 0x90 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_hrSaved, (void*)site, 6);
    if (!writeBytes(site, hook, 6)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_hrCave = mem; g_hrOn = true;
    logf("[height] applied (cave=%p)", mem);
    return true;
}
static void stopHeightBypass() {
    if (!g_hrOn) return;
    writeBytes(g_base + kHRHook, g_hrSaved, 6);
    if (g_hrCave) { VirtualFree(g_hrCave, 0, MEM_RELEASE); g_hrCave = nullptr; }
    g_hrOn = false;
    logf("[height] removed");
}

// ===== Combat tweaks (Section 6). Ports of community DSNE-table cheats. The four
// patches below are SIMPLE in-place edits (NOPs / forced branches), each guarded by
// its original bytes so a build mismatch just won't install -- fail-safe, never a
// crash from a bad address. All default OFF; toggled by hand; not auto-loaded. =====
// A patch can touch more than one site (e.g. Table Hopper has two hop-entry gates).
// All sites are verified before ANY is written, so a build mismatch leaves the exe
// untouched (fail-safe, never a half-applied patch).
// orig  = the vanilla bytes we expect; alts = OTHER acceptable current states (e.g. a
// community CE table may have already flipped this branch -- accept that too). saved =
// whatever was actually there when we patched, so toggling off restores reality, not a
// guess. alts/saved default-empty for the simple single-state patches.
struct PatchSite { uint32_t rva; std::vector<uint8_t> orig, patch;
                   std::vector<std::vector<uint8_t>> alts; std::vector<uint8_t> saved; };
struct CombatPatch { const char* name; std::vector<PatchSite> sites; bool on; };
static CombatPatch g_pFastTrick    { "Fast Trick", {
    {0x4EF012, {0x0F,0x85,0x47,0x05,0x00,0x00}, {0x90,0x90,0x90,0x90,0x90,0x90}} }, false };
static CombatPatch g_pKnockback    { "Always Knockback Release", {
    {0x548013, {0xF3,0x0F,0x11,0x80,0x30,0x75,0x01,0x00}, {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}} }, false };
static CombatPatch g_pMovingTarget { "Moving Target Change", {
    {0x55613A, {0x74,0x08}, {0xEB,0x08}} }, false };
static CombatPatch g_pTrickRange   { "Infinite Trick Range", {
    {0x4EF0A6, {0x0F,0x2F,0xC1,0x76,0x03}, {0xEB,0x06,0x90,0x90,0x90}} }, false };
// Enemy AI Max -- flip the AI aggression gate jb (0x72) -> ja (0x77) at VA 0x564C68
// (RVA 0x164C68). Ported from the Non-JP debug cheat table (verified byte 0x72 here).
// Makes enemies fight at maximum aggression (no passive/idle backoff windows).
static CombatPatch g_pEnemyAIMax   { "Enemy AI Max (aggression)", {
    {0x164C68, {0x72}, {0x77}} }, false };
// Vergil: Free Summoned Swords -- no Devil Trigger cost to summon/throw swords. NOPs the
// DT-debit write `movss [esi+0x2504],xmm0` at RVA 0x4D004B (esi+0x2504 = the DT-power
// float). 8 bytes -> 8 NOPs. Ported from the DSNE debug table ("Free Summoned Swords").
static CombatPatch g_pFreeSwords   { "Vergil: Free Summoned Swords (no DT cost)", {
    {0x4D004B, {0xF3,0x0F,0x11,0x86,0x04,0x25,0x00,0x00}, {0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90}} }, false };
// Infinite Concentration (Vergil), from the Non-JP debug CT. The concentration float lives at
// activePlayer()+0x7B58 (0=Lv0, 50=Lv1, 200=Lv2, 300=Lv3). We pin Lv2 (200.0) every frame so
// Judgement Cut End is always available WITHOUT the super-speed that a maxed value causes.
// The "fixity" patch (cmp byte[esi+0x2773] imm 00->01 @0xD6F30) stops the game's own write.
static bool g_infConc = false;
static CombatPatch g_pConcFix { "Concentration fixity", {
    {0xD6F30, {0x80,0xBE,0x73,0x27,0x00,0x00,0x00}, {0x80,0xBE,0x73,0x27,0x00,0x00,0x01}} }, false };
// Nero Table Hopper (the Devil-Bringer evade). Verified by disassembling this build.
// There are TWO hop entry points, each gated by the once-per-window limiter
// `cmp byte[esi+0xF328],1 ; jne skip` (the flag is set to 1 to ALLOW a hop and
// cleared to 0 once consumed). Patching the immediate (the old approach) inverted
// the gate instead of removing it. The correct "infinite" is to NOP each `jne` so
// the limiter never blocks -- the state guards just above each gate stay intact, so
// it still only fires from a valid state. Both gates fall through to the same
// dispatcher at 0x529960. Sites: gate A jne @0x529943 (75 14), gate B jne @0x52967B
// (75 1B).
static CombatPatch g_pInfTableHop  { "Infinite Table Hopper (Nero)", {
    {0x529943, {0x75,0x14}, {0x90,0x90}},
    {0x52967B, {0x75,0x1B}, {0x90,0x90}} }, false };
// Super: the hop-phase dispatcher bound `mov ecx,3` -> `mov ecx,0`. The dispatcher
// (entered from both gates) does `cmp eax,ecx ; ja default ; jmp [eax*4+table]` on
// the hop phase [esi+0x1a18]; forcing the bound to 0 routes the follow-up phases to
// the soft default path for the snappier "super" behaviour.
static CombatPatch g_pSuperTableHop{ "Super Table Hopper (Nero)", {
    {0x529972, {0xB9,0x03,0x00,0x00,0x00}, {0xB9,0x00,0x00,0x00,0x00}} }, false };
// True if the bytes at `site` match orig or any accepted alternate.
static bool siteAccepted(const PatchSite& s, uintptr_t site) {
    if (memcmp((void*)site, s.orig.data(), s.orig.size()) == 0) return true;
    for (auto& a : s.alts)
        if (a.size() == s.orig.size() && memcmp((void*)site, a.data(), a.size()) == 0) return true;
    return false;
}
static bool applyCombatPatch(CombatPatch& p) {
    if (p.on) return true;
    if (!g_base) return false;
    for (auto& s : p.sites) {                                   // verify every site first
        uintptr_t site = g_base + s.rva;
        if (!inModule(site, s.orig.size()) || !siteAccepted(s, site)) {
            const uint8_t* b = (const uint8_t*)site;
            logf("[combat] '%s' site 0x%X mismatch (have %02X %02X %02X %02X %02X %02X) -- skip",
                 p.name, s.rva, b[0],b[1],b[2],b[3],b[4],b[5]);
            return false; }
    }
    for (auto& s : p.sites) {
        uintptr_t site = g_base + s.rva;
        s.saved.assign((const uint8_t*)site, (const uint8_t*)site + s.patch.size());  // capture reality
        writeBytes(site, s.patch.data(), s.patch.size());
    }
    p.on = true; logf("[combat] '%s' on (%zu site%s)", p.name, p.sites.size(), p.sites.size()==1?"":"s"); return true;
}
static void stopCombatPatch(CombatPatch& p) {
    if (!p.on) return;
    for (auto& s : p.sites) {
        const uint8_t* src = s.saved.empty() ? s.orig.data() : s.saved.data();  // restore what was there
        size_t n = s.saved.empty() ? s.orig.size() : s.saved.size();
        writeBytes(g_base + s.rva, src, n);
    }
    p.on = false; logf("[combat] '%s' off", p.name);
}
static void combatPatchToggle(CombatPatch& p) {
    bool v = p.on;
    if (ImGui::Checkbox(p.name, &v)) { if (v) applyCombatPatch(p); else stopCombatPatch(p); }
}

// ===================== Super Cancel ==========================================
// Keep every move cancellable: pin the 8 cancel-table slots (actor +0x3874..+0x39A8, 0x2C
// apart -- same slots the [cancel] diag reads) to 2 while enabled, so any move's recovery
// can be cancelled into the next action. Gated on a live, readable player object.
static bool g_superCancel = false;
static void applySuperCancel() {
    if (!g_superCancel) return;
    char* a = (char*)activePlayer();
    if (!a || !memReadable(a, 0x3A00)) return;
    static const int slot[8] = {0x3874,0x38A0,0x38CC,0x38F8,0x3924,0x3950,0x397C,0x39A8};
    for (int s : slot) *(volatile int*)(a + s) = 2;
}

// ===================== Difficulty / Game Mode (God Must Die, Deicide) =========
// Ported from the DMC4SE Extension Tool (non-JPN tables) and VERIFIED byte-for-byte
// against this build's exe. Each mode is a full deterministic state write -- attack
// multipliers (player def down / enemy dmg up) + the enemy-always-Devil-Trigger
// threshold tables + the difficulty-tier code patches -- so ANY transition
// (Default<->God Must Die<->Deicide) lands exactly right. Default restores the true
// vanilla values read out of the exe. All RVAs are exe VA - 0x400000.
struct DPatch { uint32_t rva; uint8_t n; uint8_t b[8]; };
static const DPatch kDiffReset[27] = {
    {0x9163,2,{0x74,0x7B}}, {0x6B455,2,{0x74,0x7B}}, {0x351F36,2,{0x03,0x75}}, {0x351F45,1,{0x5C}},
    {0x36C99A,2,{0x03,0x75}}, {0x36C9A9,1,{0x5C}}, {0x3AA8D1,2,{0x03,0x75}}, {0x3AA8E0,1,{0x5C}},
    {0x3CEBDA,2,{0x03,0x75}}, {0x3CEBE9,1,{0x5C}}, {0x3E4BE9,1,{0x24}}, {0x3E4EC4,1,{0x14}},
    {0x40120E,1,{0x72}}, {0x401224,2,{0x03,0x75}}, {0x4214E3,1,{0x30}}, {0x4214F0,1,{0x5C}},
    {0x42B523,1,{0x30}}, {0x42B530,1,{0x5C}}, {0x4594BD,2,{0x03,0x74}}, {0x46D129,2,{0x74,0x7B}},
    {0x4E2335,1,{0x00}}, {0x5E541A,2,{0x74,0x7B}}, {0x53A59F,2,{0x98,0x71}}, {0x4C81BC,1,{0x40}},
    {0x4E1D6F,8,{0xF3,0x0F,0x10,0x80,0xAC,0x01,0x00,0x00}}, {0x4E1DB7,2,{0x98,0x71}}, {0x4E1DED,2,{0xB4,0x72}},
};
static const DPatch kDiffGmd[26] = {
    {0x9163,2,{0x5C,0x7C}}, {0x6B455,2,{0x5C,0x7C}}, {0x351F36,2,{0x04,0x77}}, {0x351F45,1,{0x60}},
    {0x36C99A,2,{0x04,0x77}}, {0x36C9A9,1,{0x60}}, {0x3AA8D1,2,{0x04,0x77}}, {0x3AA8E0,1,{0x60}},
    {0x3CEBDA,2,{0x04,0x77}}, {0x3CEBE9,1,{0x60}}, {0x3E4BE9,1,{0xC2}}, {0x3E4EC4,1,{0x5A}},
    {0x40120E,1,{0x74}}, {0x401224,2,{0x04,0x77}}, {0x4214E3,1,{0x00}}, {0x4214F0,1,{0x60}},
    {0x42B523,1,{0x00}}, {0x42B530,1,{0x60}}, {0x4594BD,2,{0x04,0x76}}, {0x46D129,2,{0x5C,0x7C}},
    {0x4E2335,1,{0x01}}, {0x5E541A,2,{0x5C,0x7C}}, {0x4C81BC,1,{0x3C}},
    {0x4E1D6F,8,{0xF3,0x0F,0x10,0x05,0x08,0x9F,0x14,0x01}}, {0x4E1DB7,2,{0xB4,0x72}}, {0x4E1DED,2,{0x34,0x7B}},
};
// attack-multiplier tables (5 floats: Human/DevilHunter/SonOfSparda/DMD/LDK).
// rows: Player, Player-DT, Enemy-DT, Enemy.
static const uint32_t kAtkRva[4] = { 0xE791E4, 0xE79394, 0xE79494, 0xE79684 };
static const float kAtkVan [4][5] = { {1.5f,1.0f,0.85f,0.7f,1.3f},{1.5f,1.0f,0.85f,0.7f,0.7f},{0.5f,1.0f,1.75f,3.0f,1.75f},{0.5f,1.0f,1.75f,3.0f,1.75f} };
static const float kAtkGmd [4][5] = { {0.5f,0.5f,0.5f,0.5f,0.7f},{0.5f,0.5f,0.5f,0.5f,0.7f},{4.f,4.f,4.f,4.f,3.f},{4.f,4.f,4.f,4.f,3.f} };
static const float kAtkGmd2[4][5] = { {0.4f,0.4f,0.4f,0.4f,0.5f},{0.4f,0.4f,0.4f,0.4f,0.5f},{5.f,5.f,5.f,5.f,4.f},{5.f,5.f,5.f,5.f,4.f} };
// enemy Devil-Trigger threshold tables (0x127AF30 / 0x127AE00 -> rva 0xE7AF30 / 0xE7AE00).
static const uint8_t kEmDT_van[72] = {0x00,0xC0,0x28,0x45,0x00,0xC0,0x28,0x45,0x00,0x00,0x80,0xBF,0x00,0xC0,0xA8,0x45,0x00,0x00,0x80,0xBF,0x00,0x00,0x61,0x45,0x00,0x00,0xE1,0x45,0x00,0x00,0x80,0xBF,0x00,0xC0,0x28,0x45,0x00,0xA0,0x0C,0x46,0x00,0xA0,0x8C,0x45,0x00,0x00,0x61,0x45,0x00,0xA0,0x0C,0x46,0x00,0x00,0xE1,0x44,0x00,0x00,0x80,0xBF,0x00,0x00,0x61,0x45,0x00,0xC0,0x28,0x45,0x00,0xC0,0x28,0x45};
static const uint8_t kEmDT2_van[80] = {0xCD,0xCC,0x4C,0x3F,0xCD,0xCC,0x4C,0x3F,0xCD,0xCC,0x4C,0x3F,0xCD,0xCC,0x4C,0x3F,0xCD,0xCC,0x4C,0x3F,0xCD,0xCC,0x4C,0x3F,0xCD,0xCC,0x4C,0x3F,0xCD,0xCC,0x4C,0x3F,0xCD,0xCC,0x4C,0x3F,0xCD,0xCC,0x4C,0x3F,0x00,0x40,0x9C,0x46,0x00,0x40,0x1C,0x46,0x00,0x40,0x9C,0x45,0x00,0x00,0xFA,0x44,0x00,0x40,0x9C,0x45,0x00,0x00,0xFA,0x44,0x00,0x00,0xFA,0x44,0x00,0x00,0x7A,0x44,0x00,0x00,0xC8,0x43,0x00,0x00,0x7A,0x44};
static const uint8_t kEmDT_gmd[72] = {0x00,0xC0,0xA8,0x44,0x00,0xC0,0xA8,0x44,0x00,0x00,0x80,0xBF,0x00,0xC0,0x28,0x45,0x00,0x00,0x80,0xBF,0x00,0x00,0xE1,0x44,0x00,0x00,0x61,0x45,0x00,0x00,0x80,0xBF,0x00,0xC0,0xA8,0x44,0x00,0xA0,0x8C,0x45,0x00,0xA0,0x0C,0x45,0x00,0x00,0xE1,0x44,0x00,0xA0,0x8C,0x45,0x00,0x00,0x61,0x44,0x00,0x00,0x80,0xBF,0x00,0x00,0xE1,0x44,0x00,0xC0,0xA8,0x44,0x00,0xC0,0xA8,0x44};
static const uint8_t kEmDT2_gmd[80] = {0x9A,0x99,0x19,0x3F,0x9A,0x99,0x19,0x3F,0x9A,0x99,0x19,0x3F,0x9A,0x99,0x19,0x3F,0x9A,0x99,0x19,0x3F,0x9A,0x99,0x19,0x3F,0x9A,0x99,0x19,0x3F,0x9A,0x99,0x19,0x3F,0x9A,0x99,0x19,0x3F,0x9A,0x99,0x19,0x3F,0x00,0x00,0x7A,0x44,0x00,0x00,0x7A,0x44,0x00,0x00,0x7A,0x44,0x00,0x00,0x7A,0x44,0x00,0x00,0x7A,0x44,0x00,0x00,0x48,0x43,0x00,0x00,0x48,0x43,0x00,0x00,0x48,0x43,0x00,0x00,0x48,0x43,0x00,0x00,0x48,0x43};
static const uint32_t kEmDTRva = 0xE7AF30, kEmDT2Rva = 0xE7AE00;
static int g_diffMode = 0;   // 0 = Default, 1 = God Must Die, 2 = Deicide Must Die

static void diffPatches(const DPatch* p, int n) { for (int i = 0; i < n; i++) writeBytes(g_base + p[i].rva, p[i].b, p[i].n); }
static void diffAtk(const float t[4][5]) { for (int i = 0; i < 4; i++) writeBytes(g_base + kAtkRva[i], (const uint8_t*)t[i], 20); }
static void applyDifficulty(int mode) {
    if (!g_base) return;
    uintptr_t mv = g_base + 0x4E1D6F;                       // build sanity check: must be the movss site
    if (!inModule(mv, 4) || ((uint8_t*)mv)[0] != 0xF3 || ((uint8_t*)mv)[1] != 0x0F || ((uint8_t*)mv)[2] != 0x10) {
        logf("[diff] build check failed @0x4E1D6F -- aborting"); return;
    }
    if (mode == 0) {
        diffPatches(kDiffReset, (int)(sizeof(kDiffReset)/sizeof(DPatch)));
        diffAtk(kAtkVan);
        writeBytes(g_base + kEmDTRva,  kEmDT_van,  sizeof(kEmDT_van));
        writeBytes(g_base + kEmDT2Rva, kEmDT2_van, sizeof(kEmDT2_van));
    } else {
        diffPatches(kDiffGmd, (int)(sizeof(kDiffGmd)/sizeof(DPatch)));
        uint8_t b233c = (mode == 2) ? 0x01 : 0x00;          // Deicide flips the extra gate
        writeBytes(g_base + 0x4E233C, &b233c, 1);
        uint8_t bp[2]; bp[0] = (mode == 2) ? 0x90 : 0x60; bp[1] = (mode == 2) ? 0x8D : 0x54;
        writeBytes(g_base + 0xD062AF, bp, 2);
        diffAtk(mode == 2 ? kAtkGmd2 : kAtkGmd);
        writeBytes(g_base + kEmDTRva,  kEmDT_gmd,  sizeof(kEmDT_gmd));
        writeBytes(g_base + kEmDT2Rva, kEmDT2_gmd, sizeof(kEmDT2_gmd));
    }
    g_diffMode = mode;
    logf("[diff] mode %d applied (0=Default 1=GodMustDie 2=Deicide)", mode);
}

// ---- Player Damage Modifier (DSNE). Cave at exe+0x22DDE0 (the HP write after a
// hit: movss [edi+30],xmm0): scales the damage delta by a multiplier (0 = take no
// damage, <1 = reduced, >1 = increased). edi = the damaged actor; guarded by the
// original bytes; the multiplier float lives in the cave and updates live.
static const uint32_t kDmgHook = 0x22DDE0;
static const uint8_t  kDmgOrig[5] = {0xF3,0x0F,0x11,0x47,0x30};
static void*   g_dmgCave = nullptr;
static uint8_t g_dmgSaved[5];
static bool    g_dmgOn = false;
static float   g_dmgMult = 1.0f;
static const size_t kDmgMultOff = 45;
static void setDamageMult(float m) {
    g_dmgMult = m;
    if (g_dmgOn && g_dmgCave) memcpy((uint8_t*)g_dmgCave + kDmgMultOff, &m, 4);
}
static bool applyDamageMod() {
    if (g_dmgOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kDmgHook;
    if (memcmp((void*)site, kDmgOrig, 5) != 0) { logf("[dmgmod] site mismatch -- skip"); return false; }
    uint8_t cave[] = {
        0x81,0x7F,0x34,0x00,0x40,0x9C,0x46,   // cmp dword [edi+34],469C4000
        0x74,0x1A,                            // je originalcode
        0xF3,0x0F,0x10,0x67,0x30,             // movss xmm4,[edi+30]
        0xF3,0x0F,0x5C,0xE0,                  // subss xmm4,xmm0
        0xF3,0x0F,0x10,0x47,0x30,             // movss xmm0,[edi+30]
        0xF3,0x0F,0x59,0x25, 0,0,0,0,         // mulss xmm4,[mult]  (abs @27)
        0xF3,0x0F,0x5C,0xC4,                  // subss xmm0,xmm4
        0xF3,0x0F,0x11,0x47,0x30,             // originalcode: movss [edi+30],xmm0
        0xE9, 0,0,0,0,                        // jmp returnhere    (rel32 @41)
        0,0,0,0                               // multiplier float  (offset 45)
    };
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[dmgmod] VirtualAlloc failed"); return false; }
    uint32_t multAbs = (uint32_t)((uintptr_t)mem + kDmgMultOff);
    memcpy(cave + 27, &multAbs, 4);
    int32_t backRel = (int32_t)((site + 5) - ((uintptr_t)mem + 40 + 5));
    memcpy(cave + 41, &backRel, 4);
    memcpy(cave + kDmgMultOff, &g_dmgMult, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[5] = { 0xE9, 0,0,0,0 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_dmgSaved, (void*)site, 5);
    if (!writeBytes(site, hook, 5)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_dmgCave = mem; g_dmgOn = true;
    logf("[dmgmod] applied (cave=%p mult=%.2f)", mem, g_dmgMult);
    return true;
}
static void stopDamageMod() {
    if (!g_dmgOn) return;
    writeBytes(g_base + kDmgHook, g_dmgSaved, 5);
    if (g_dmgCave) { VirtualFree(g_dmgCave, 0, MEM_RELEASE); g_dmgCave = nullptr; }
    g_dmgOn = false;
    logf("[dmgmod] removed");
}


// ---- Allow Trick Down (Air) without DT (ported from the IW/debug CE table). A
// 176-byte cave hooked into Vergil's air input handler at module+0xD2C77 that
// adds the missing Lock-On + Back + Jump air trick-down. It runs inside the game's
// own input thread (esi=player), so the game-function call is in valid context.
// Runtime-patched: jmp-back, the lock-on listener call, and the option flag addr.
static const uint32_t kATHook = 0xD2C77;
static const uint8_t  kATOrig[7] = { 0xF6,0x86,0x64,0x1A,0x00,0x00,0x01 };
static const uint32_t kATXor = 0xD2C80;            // xor al,al -> nop'd while active
static const uint8_t  kATXorOrig[2] = { 0x32,0xC0 };
static void*   g_atCave = nullptr;
static uint8_t g_atSaved[7];
static bool    g_atOn = false;
static const uint8_t kAirTrickCave[176] = {
    0xf6,0x86,0x64,0x1a,0x00,0x00,0x01, 0x9c, 0x74,0x08, 0x30,0xc0,
    0x9d, 0xe9,0x00,0x00,0x00,0x00, 0x83,0xbe,0x24,0x39,0x00,0x00,
    0x02, 0x75,0xef, 0x80,0xbe,0x28,0x39,0x00,0x00,0x01, 0x75,0xe6,
    0xf6,0x86,0x2c,0x39,0x00,0x00,0x10, 0x74,0xdd, 0xf3,0x0f,0x7e,
    0x86,0x30,0x39,0x00,0x00, 0x8b,0x8e,0x40,0x39,0x00,0x00, 0x83,
    0xec,0x14, 0x89,0xe0, 0x66,0x0f,0xd6,0x00, 0xf3,0x0f,0x7e,0x86,
    0x38,0x39,0x00,0x00, 0x66,0x0f,0xd6,0x40,0x08, 0x89,0x48,0x10,
    0xe8,0x00,0x00,0x00,0x00, 0x8b,0x06, 0x8b,0x80,0x34,0x02,0x00,
    0x00, 0x89,0xf1, 0x83,0xbe,0xfc,0x1b,0x00,0x00,0x0c, 0x74,0x14,
    0x83,0xbe,0xfc,0x1b,0x00,0x00,0x06, 0x74,0x0b, 0x80,0x3d,0x00,
    0x00,0x00,0x00,0x00, 0x7f,0x0a, 0xeb,0x8c, 0x6a,0x2a, 0xff,0xd0,
    0xb0,0x01, 0xeb,0x84, 0x83,0xbe,0xfc,0x1b,0x00,0x00,0x01, 0x0f,
    0x85,0x77,0xff,0xff,0xff, 0x80,0xbe,0x50,0x7c,0x00,0x00,0x00,
    0x0f,0x8f,0x6a,0xff,0xff,0xff, 0x6a,0x27, 0xff,0xd0, 0xb0,0x01,
    0xe9,0x5f,0xff,0xff,0xff, 0x00, 0x90,0x90
};
static void* g_swCave = nullptr;   // Swordless Air Trick (defined below; shares D2C77)
static bool  g_swOn = false;
static void stopSwordTrick();   // fwd (shares the D2C77 hook)
static bool applyAirTrick() {
    if (g_atOn) return true;
    if (!g_base) return false;
    if (g_swOn) stopSwordTrick();   // free the shared D2C77 hook first
    uintptr_t site = g_base + kATHook;
    if (memcmp((void*)site, kATOrig, 7) != 0) { logf("[airtrick] site unexpected (0x%02X)", *(uint8_t*)site); return false; }
    void* mem = VirtualAlloc(nullptr, sizeof(kAirTrickCave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[airtrick] VirtualAlloc failed"); return false; }
    uint8_t* c = (uint8_t*)mem;
    memcpy(c, kAirTrickCave, sizeof(kAirTrickCave));
    int32_t r1 = (int32_t)((g_base + 0xD2C7E) - ((uintptr_t)mem + 18));   // jmp returnhere @14
    memcpy(c + 14, &r1, 4);
    int32_t r2 = (int32_t)((g_base + 0x4CCD40) - ((uintptr_t)mem + 89));  // call lock-on @85
    memcpy(c + 85, &r2, 4);
    uint32_t optAbs = (uint32_t)((uintptr_t)mem + 173);                   // option_trickup byte @119
    memcpy(c + 119, &optAbs, 4);
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(kAirTrickCave));
    uint8_t hook[7] = { 0xE9,0,0,0,0, 0x90,0x90 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_atSaved, (void*)site, 7);
    if (!writeBytes(site, hook, 7)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    uint8_t nopnop[2] = { 0x90,0x90 };
    writeBytes(g_base + kATXor, nopnop, 2);                               // nop the xor al,al
    g_atCave = mem; g_atOn = true;
    logf("[airtrick] applied (cave=%p)", mem);
    return true;
}
static void stopAirTrick() {
    if (!g_atOn) return;
    writeBytes(g_base + kATHook, g_atSaved, 7);
    writeBytes(g_base + kATXor, kATXorOrig, 2);
    if (g_atCave) { VirtualFree(g_atCave, 0, MEM_RELEASE); g_atCave = nullptr; }
    g_atOn = false;
    logf("[airtrick] removed");
}

// ---- Swordless Air Trick (teleport to aimed enemy, no sword embedded) ----------
// IW table's "Configure Vergil's Trick Moves" rebind. Two hooks:
//   * module+0xD2C77 (airlock @cave+0)   -> read lock-on inputs every air frame
//   * module+0xD259F (rebind @cave+0x45) -> on air-trick commit, remap by aim dir;
//     a neutral lock-on does a swordless air trick (push 0x24) straight to the
//     locked-on enemy with no summoned sword needed in it.
// This SHARES the D2C77 hook with Allow-Trick-Down, so the two are mutually
// exclusive -- but this is a superset (trickdown_DTrequire defaults to 0, so it
// also gives trick-down without DT). DT-require dwords baked in: side=1, back=1,
// down=0. Reversible: restores D2C77 (7), D259F (6), D25A5 (2).
static const uint32_t kSWHook = 0xD2C77;
static const uint8_t  kSWHookOrig[7]  = { 0xF6,0x86,0x64,0x1A,0x00,0x00,0x01 };
static const uint32_t kSWRemap = 0xD259F;
static const uint8_t  kSWRemapOrig[6] = { 0x6A,0x24,0x8B,0xCB,0xFF,0xD0 };
static const uint32_t kSWAl = 0xD25A5;
static const uint8_t  kSWAlOrig[2]    = { 0xB0,0x01 };
static const uint8_t kSwordTrickCave[212] = {
    0xf6,0x86,0x64,0x1a,0x00,0x00,0x01,0x9c,0x83,0xbe,0x24,0x39,
    0x00,0x00,0x02,0x75,0x2c,0xf3,0x0f,0x7e,0x86,0x30,0x39,0x00,
    0x00,0x8b,0x8e,0x40,0x39,0x00,0x00,0x83,0xec,0x14,0x89,0xe0,
    0x66,0x0f,0xd6,0x00,0xf3,0x0f,0x7e,0x86,0x38,0x39,0x00,0x00,
    0x66,0x0f,0xd6,0x40,0x08,0x89,0x48,0x10,0xe8,0x00,0x00,0x00,
    0x00,0x30,0xc0,0x9d,0xe9,0x00,0x00,0x00,0x00,0x57,0x8b,0xbb,
    0xfc,0x1b,0x00,0x00,0x83,0xff,0x01,0x74,0x28,0x83,0xff,0x02,
    0x74,0x30,0x83,0xff,0x08,0x74,0x2b,0x83,0xff,0x06,0x74,0x38,
    0x83,0xff,0x0c,0x74,0x33,0x6a,0x24,0xeb,0x04,0x30,0xc0,0xeb,
    0x06,0x89,0xd9,0xff,0xd0,0xb0,0x01,0x5f,0xe9,0x00,0x00,0x00,
    0x00,0x80,0xbb,0x50,0x7c,0x00,0x00,0x00,0x7f,0xe7,0x6a,0x27,
    0xeb,0xe7,0x8b,0xbb,0xe4,0x78,0x00,0x00,0x39,0x3d,0x00,0x00,
    0x00,0x00,0x7f,0xd5,0x6a,0x28,0xeb,0xd5,0xf6,0x83,0x64,0x1a,
    0x00,0x00,0x01,0x75,0x12,0x8b,0xbb,0xe4,0x78,0x00,0x00,0x39,
    0x3d,0x00,0x00,0x00,0x00,0x7f,0xba,0x6a,0x2a,0xeb,0xba,0x8b,
    0xbb,0xe4,0x78,0x00,0x00,0x39,0x3d,0x00,0x00,0x00,0x00,0x7f,
    0xa8,0x6a,0x29,0xeb,0xa8,0x01,0x00,0x00,0x00,0x01,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x90,0x90,0x90
};
static void stopAirTrick();   // fwd (shares the D2C77 hook)
static bool applySwordTrick() {
    if (g_swOn) return true;
    if (!g_base) return false;
    if (g_atOn) stopAirTrick();   // free the shared D2C77 hook first
    uintptr_t h0 = g_base + kSWHook, h1 = g_base + kSWRemap, h2 = g_base + kSWAl;
    if (memcmp((void*)h0, kSWHookOrig, 7) != 0 ||
        memcmp((void*)h1, kSWRemapOrig, 6) != 0 ||
        memcmp((void*)h2, kSWAlOrig, 2) != 0) { logf("[swordtrick] site unexpected"); return false; }
    void* mem = VirtualAlloc(nullptr, sizeof(kSwordTrickCave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[swordtrick] VirtualAlloc failed"); return false; }
    uint8_t* c = (uint8_t*)mem;
    memcpy(c, kSwordTrickCave, sizeof(kSwordTrickCave));
    int32_t a = (int32_t)((g_base + 0x4CCD40) - ((uintptr_t)mem + 0x3D)); memcpy(c + 0x39, &a, 4); // call lock-on
    int32_t b = (int32_t)((g_base + 0xD2C7E) - ((uintptr_t)mem + 0x45)); memcpy(c + 0x41, &b, 4);   // jmp return_from_read
    int32_t e = (int32_t)((g_base + 0xD25A5) - ((uintptr_t)mem + 0x79)); memcpy(c + 0x75, &e, 4);   // jmp returnhere
    uint32_t ds = (uint32_t)((uintptr_t)mem + 0xC5); memcpy(c + 0x8E, &ds, 4);  // [dq_sidetrick]
    uint32_t dd = (uint32_t)((uintptr_t)mem + 0xCD); memcpy(c + 0xA9, &dd, 4);  // [dq_trickdown]
    uint32_t dk = (uint32_t)((uintptr_t)mem + 0xC9); memcpy(c + 0xBB, &dk, 4);  // [dq_trickback]
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(kSwordTrickCave));
    // hook D2C77 -> airlock (cave+0): jmp rel32 + nop nop
    uint8_t hk0[7] = { 0xE9,0,0,0,0, 0x90,0x90 };
    int32_t r0 = (int32_t)((uintptr_t)mem - (h0 + 5)); memcpy(hk0 + 1, &r0, 4);
    // hook D259F -> rebind (cave+0x45): jmp rel32 + nop
    uint8_t hk1[6] = { 0xE9,0,0,0,0, 0x90 };
    int32_t r1 = (int32_t)(((uintptr_t)mem + 0x45) - (h1 + 5)); memcpy(hk1 + 1, &r1, 4);
    if (!writeBytes(h0, hk0, 7)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    writeBytes(h1, hk1, 6);
    uint8_t nopnop[2] = { 0x90,0x90 }; writeBytes(h2, nopnop, 2);   // D25A5 mov al,1 -> nop nop
    g_swCave = mem; g_swOn = true;
    logf("[swordtrick] applied (cave=%p)", mem);
    return true;
}
static void stopSwordTrick() {
    if (!g_swOn) return;
    writeBytes(g_base + kSWHook,  kSWHookOrig, 7);
    writeBytes(g_base + kSWRemap, kSWRemapOrig, 6);
    writeBytes(g_base + kSWAl,    kSWAlOrig, 2);
    if (g_swCave) { VirtualFree(g_swCave, 0, MEM_RELEASE); g_swCave = nullptr; }
    g_swOn = false;
    logf("[swordtrick] removed");
}

// ===== MistressDMC: Increased Snatch Range (Nero) — high confidence cave =====
// Hooks the snatch-length lookup `movss xmm0,[ecx+eax*4+0x214]` (SE RVA 0x5154FE)
// and forces the result to a constant 2550.0f (3x Lv3 reach). No actor-struct
// offsets touched. Reversible; rels computed at runtime.
static void*   g_snatchCave  = nullptr;
static uint8_t g_snatchSaved[9];
static bool    g_snatchOn    = false;
static const uint32_t kSnatchRva = 0x5154FE;
static void stopSnatchRange() {
    if (!g_snatchOn) return;
    writeBytes(g_base + kSnatchRva, g_snatchSaved, 9);
    if (g_snatchCave) { VirtualFree(g_snatchCave, 0, MEM_RELEASE); g_snatchCave = nullptr; }
    g_snatchOn = false; logf("[snatch] off");
}
static bool applySnatchRange() {
    if (g_snatchOn || !g_base) return true;
    uintptr_t site = g_base + kSnatchRva;
    if (!inModule(site, 9)) return false;
    static const uint8_t orig9[9] = {0xF3,0x0F,0x10,0x84,0x81,0x14,0x02,0x00,0x00};
    const uint8_t* a = (const uint8_t*)site;
    if (!(memcmp(a, orig9, 9) == 0 || (a[0]==orig9[0] && a[8]==orig9[8]))) {
        logf("[snatch] site 0x%X mismatch (%02X..%02X) -- skip", kSnatchRva, a[0], a[8]); return false; }
    uint8_t cave[32]; int n = 0;
    cave[n++]=0x68; cave[n++]=0x00; cave[n++]=0x60; cave[n++]=0x1F; cave[n++]=0x45; // push 0x451F6000 (2550.0f)
    cave[n++]=0xF3; cave[n++]=0x0F; cave[n++]=0x10; cave[n++]=0x04; cave[n++]=0x24; // movss xmm0,[esp]
    cave[n++]=0x83; cave[n++]=0xC4; cave[n++]=0x04;                                 // add esp,4
    void* mem = VirtualAlloc(nullptr, 64, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;
    int32_t backRel = (int32_t)((site + 9) - ((uintptr_t)mem + n + 5));
    cave[n++]=0xE9; memcpy(cave+n,&backRel,4); n+=4;
    memcpy(mem, cave, n); FlushInstructionCache(GetCurrentProcess(), mem, n);
    memcpy(g_snatchSaved, (void*)site, 9);
    uint8_t hook[9]; hook[0]=0xE9;
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook+1,&toRel,4);
    for (int k=5;k<9;k++) hook[k]=0x90;
    writeBytes(site, hook, 9);
    g_snatchCave = mem; g_snatchOn = true; logf("[snatch] on -> cave %p", mem);
    return true;
}

// ===== MistressDMC: Rose Removes Pins (Lucifer) — high confidence cave =====
// Hooks 0x54B5D4 (steals `mov ebx,4`); when g_rosePins and pin state==3, sets
// state(=[esi+0x04]) to 0 and returns via epilogue 0x54B696 so the pin despawns
// instead of detonating. Installed once; checkbox flips the runtime flag.
static volatile uint8_t g_rosePins   = 0;
static const uint32_t   kRoseHook     = 0x54B5D4;
static const uint8_t    kRoseOrig[5]  = { 0xBB,0x04,0x00,0x00,0x00 };
static const uint32_t   kRoseResume   = 0x54B5D9;
static const uint32_t   kRoseEpilogue = 0x54B696;
static void* g_roseCave = nullptr;
static bool  g_roseHookOn = false;
static bool installRoseRemovesPins() {
    if (g_roseHookOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kRoseHook;
    if (!inModule(site, 5) || memcmp((void*)site, kRoseOrig, 5) != 0) {
        logf("[rosepins] hook site unexpected (0x%02X) - aborting", *(uint8_t*)site);
        return false;
    }
    uint8_t cave[] = {
        0x80,0x3D, 0,0,0,0, 0x00,        // [0]  cmp byte[&g_rosePins],0
        0x0F,0x84, 0,0,0,0,              // [7]  je  restore
        0x80,0x7E,0x04,0x03,             // [13] cmp byte[esi+0x04],3
        0x0F,0x85, 0,0,0,0,              // [17] jne restore
        0xC6,0x46,0x04,0x00,             // [23] mov byte[esi+0x04],0
        0xE9, 0,0,0,0,                   // [27] jmp epilogue
        0xBB,0x04,0x00,0x00,0x00,        // [32] restore: mov ebx,4
        0xE9, 0,0,0,0                    // [37] jmp resume
    };
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[rosepins] VirtualAlloc failed"); return false; }
    uint32_t flagAddr = (uint32_t)(uintptr_t)&g_rosePins;  memcpy(cave + 2,  &flagAddr, 4);
    int32_t  jeRel  = (int32_t)((uintptr_t)mem + 32) - ((uintptr_t)mem + 13); memcpy(cave + 9,  &jeRel, 4);
    int32_t  jneRel = (int32_t)((uintptr_t)mem + 32) - ((uintptr_t)mem + 23); memcpy(cave + 19, &jneRel,4);
    int32_t  epiRel = (int32_t)(g_base + kRoseEpilogue) - ((uintptr_t)mem + 32); memcpy(cave + 28, &epiRel,4);
    int32_t  resRel = (int32_t)(g_base + kRoseResume)   - ((uintptr_t)mem + 42); memcpy(cave + 38, &resRel,4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[5] = { 0xE9, 0,0,0,0 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));  memcpy(hook + 1, &to, 4);
    if (!writeBytes(site, hook, 5)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_roseCave = mem; g_roseHookOn = true;
    logf("[rosepins] installed (cave=%p)", mem);
    return true;
}

// ===== MistressDMC: No Helmbreaker Knockdown (Dante Helm Breaker / Nero Helm Splitter) =====
// Mid-function cave at the reaction/knockback copy loop (SE RVA 0x6A648 = cmp ecx,5 ; jl 0x6A5E1).
// When the active char is mid-helm-move, suppress = exit the copy loop immediately (no knockback
// components written -> only stun). Gated by MOOD-verified active player + mAtckId.
static bool g_noHelmBreaker = false;   // Dante
static bool g_noHelmSplit   = false;   // Nero
extern "C" __attribute__((cdecl)) int helmSuppress() {
    char* a = (char*)activePlayer();
    if (!a || !memReadable(a, 0x1B00)) return 0;
    int cid = *(volatile int32_t*)(a + OFF_CHARID);    // +0x19AC (0=Dante,1=Nero)
    uint32_t atck = *(volatile uint32_t*)(a + 0x1A74); // mAtckId
    if (g_noHelmBreaker && cid == 0 && atck == 7) return 1;            // Helm Breaker
    if (g_noHelmSplit   && cid == 1 && (atck == 27 || atck == 40)) return 1; // Split / Double Down
    return 0;
}
static const uint32_t kHelmHook    = 0x6A648;
static const uint8_t  kHelmOrig[5] = {0x83,0xF9,0x05,0x7C,0x94};
static void* g_helmCave   = nullptr;
static bool  g_helmHookOn = false;
static void applyHelmHook() {
    if (g_helmHookOn || !g_base) return;
    uintptr_t site = g_base + kHelmHook;
    if (!inModule(site, 5) || (memcmp((void*)site, kHelmOrig, 5) != 0 &&
                               memcmp((void*)site, kHelmOrig, 4) != 0)) { // rel8 byte may drift
        logf("[helm] hook site unexpected -- aborting"); return;
    }
    uintptr_t jeTgt = g_base + 0x6A5E1;   // write-component branch
    uintptr_t cont  = g_base + 0x6A64D;   // loop tail / exit
    uint8_t cave[] = {
        0x60,                       // [0]  pushad
        0x9C,                       // [1]  pushf
        0xB8,0,0,0,0,               // [2]  mov eax, helmSuppress  (operand @3)
        0xFF,0xD0,                  // [7]  call eax
        0x84,0xC0,                  // [9]  test al,al
        0x9D,                       // [11] popf
        0x61,                       // [12] popad
        0x75,0x0E,                  // [13] jnz do_suppress (->@29)
        0x83,0xF9,0x05,             // [15] cmp ecx,5
        0x0F,0x8C,0,0,0,0,          // [18] jl jeTgt   (operand @20, end @24)
        0xE9,0,0,0,0,               // [24] jmp cont   (operand @25, end @29)
        0xE9,0,0,0,0                // [29] do_suppress: jmp cont (operand @30, end @34)
    };
    uint32_t fn = (uint32_t)(uintptr_t)&helmSuppress; memcpy(cave + 3, &fn, 4);
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    *(int32_t*)(cave + 20) = (int32_t)(jeTgt - ((uintptr_t)mem + 24)); // jl jeTgt
    *(int32_t*)(cave + 25) = (int32_t)(cont  - ((uintptr_t)mem + 29)); // jmp cont (normal ecx>=5)
    *(int32_t*)(cave + 30) = (int32_t)(cont  - ((uintptr_t)mem + 34)); // jmp cont (suppress)
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[5] = { 0xE9,0,0,0,0 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    if (!writeBytes(site, hook, 5)) { VirtualFree(mem, 0, MEM_RELEASE); return; }
    g_helmCave = mem; g_helmHookOn = true; logf("[helm] knockback hook applied %p", mem);
}
static void stopHelmHook() {
    if (!g_helmHookOn) return;
    writeBytes(g_base + kHelmHook, kHelmOrig, 5);
    if (g_helmCave) { VirtualFree(g_helmCave, 0, MEM_RELEASE); g_helmCave = nullptr; }
    g_helmHookOn = false; logf("[helm] removed");
}

// ===== MistressDMC: Skip Shotgun/Gilgamesh/Pandora/Lucifer (Dante weapon-cycle) =====
static volatile uint8_t g_skipShotgun  = 0;
static volatile uint8_t g_skipPandora  = 0;
static volatile uint8_t g_skipGilgamesh= 0;
static volatile uint8_t g_skipLucifer  = 0;
// Vergil melee weapons share the SAME melee cycle site; ids captured from the game
// via the weapon-cycle logger and confirmed in-game: Yamato=10, Force Edge=12,
// Beowulf=11 (cycle 10->12->11->10).
static volatile uint8_t g_skipYamato   = 0;
static volatile uint8_t g_skipBeowulf  = 0;
static volatile uint8_t g_skipForceEdge= 0;
static const uint32_t kSkipGunRVA   = 0xDE01F;
static const uint32_t kSkipSwordRVA = 0xDE05F;
static const uint8_t  kSkipGunOrig[5]   = {0x89,0x47,0x30,0xB0,0x01};
static const uint8_t  kSkipSwordOrig[5] = {0x89,0x47,0x2C,0xB0,0x01};
static void*   g_skipGunCave   = nullptr;  static uint8_t g_skipGunSaved[5];   static bool g_skipGunOn   = false;
static void*   g_skipSwordCave = nullptr;  static uint8_t g_skipSwordSaved[5]; static bool g_skipSwordOn = false;
static bool skipInstall(uint32_t rva, const uint8_t* orig, uint8_t memOff,
                        uint8_t a, uint8_t tgtA, volatile uint8_t* skA,
                        uint8_t b, uint8_t tgtB, volatile uint8_t* skB,
                        void** caveOut, uint8_t* saved, bool* onFlag) {
    if (*onFlag || !g_base) return true;
    uintptr_t site = g_base + rva;
    if (!inModule(site, 5) || memcmp((void*)site, orig, 5) != 0) {
        logf("[skipwpn] site 0x%X mismatch -- skip", rva); return false; }
    uint8_t c[96]; int n = 0;
    uint32_t addrA = (uint32_t)(uintptr_t)skA, addrB = (uint32_t)(uintptr_t)skB;
    c[n++]=0x89; c[n++]=0x47; c[n++]=memOff;
    c[n++]=0x83; c[n++]=0xF8; c[n++]=a;
    c[n++]=0x75; int jA = n++;
    c[n++]=0x80; c[n++]=0x3D; memcpy(c+n,&addrA,4); n+=4; c[n++]=0x01;
    c[n++]=0x75; int jAdone = n++;
    c[n++]=0xC7; c[n++]=0x47; c[n++]=memOff; c[n++]=tgtA; c[n++]=0; c[n++]=0; c[n++]=0;
    int chkB = n;
    c[(size_t)jA]    = (uint8_t)(chkB - (jA+1));
    c[(size_t)jAdone]= (uint8_t)(chkB - (jAdone+1));
    c[n++]=0x83; c[n++]=0xF8; c[n++]=b;
    c[n++]=0x75; int jB = n++;
    c[n++]=0x80; c[n++]=0x3D; memcpy(c+n,&addrB,4); n+=4; c[n++]=0x01;
    c[n++]=0x75; int jBdone = n++;
    c[n++]=0xC7; c[n++]=0x47; c[n++]=memOff; c[n++]=tgtB; c[n++]=0; c[n++]=0; c[n++]=0;
    int done = n;
    c[(size_t)jB]    = (uint8_t)(done - (jB+1));
    c[(size_t)jBdone]= (uint8_t)(done - (jBdone+1));
    c[n++]=0xB0; c[n++]=0x01;
    void* mem = VirtualAlloc(nullptr, 128, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;
    int32_t back = (int32_t)((site + 5) - ((uintptr_t)mem + n + 5));
    c[n++]=0xE9; memcpy(c+n,&back,4); n+=4;
    memcpy(mem, c, n); FlushInstructionCache(GetCurrentProcess(), mem, n);
    memcpy(saved, (void*)site, 5);
    uint8_t hook[5] = {0xE9,0,0,0,0};
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook+1,&to,4);
    writeBytes(site, hook, 5);
    *caveOut = mem; *onFlag = true;
    logf("[skipwpn] site 0x%X hooked -> cave %p (%d bytes)", rva, mem, n);
    return true;
}
static void skipRemove(uint32_t rva, const uint8_t* saved, void** caveOut, bool* onFlag) {
    if (!*onFlag) return;
    writeBytes(g_base + rva, saved, 5);
    if (*caveOut) { VirtualFree(*caveOut, 0, MEM_RELEASE); *caveOut = nullptr; }
    *onFlag = false;
}
static void skipSyncGun() {
    if (g_skipShotgun || g_skipPandora)
        skipInstall(kSkipGunRVA, kSkipGunOrig, 0x30, 7, 8, &g_skipShotgun, 8, 9, &g_skipPandora,
                    &g_skipGunCave, g_skipGunSaved, &g_skipGunOn);
    else skipRemove(kSkipGunRVA, g_skipGunSaved, &g_skipGunCave, &g_skipGunOn);
}
// Unified melee-cycle remapper: chase to the next NON-skipped weapon in each
// character's melee cycle. Dante and Vergil weapon ids don't overlap, so a single
// cave serves both -- only the active character's ids ever match. The <=2-skip rule
// (enforced in the UI) means the chase always lands on an enabled weapon; the guard
// just bounds it defensively.
extern "C" __attribute__((cdecl)) uint32_t meleeSkipRemap(uint32_t id) {
    for (int guard = 0; guard < 4; guard++) {
        if      (id == 5  && g_skipGilgamesh)  id = 6;    // Dante:  Gilgamesh   -> Lucifer
        else if (id == 6  && g_skipLucifer)    id = 4;    // Dante:  Lucifer     -> (id 4)
        else if (id == 10 && g_skipYamato)     id = 12;   // Vergil: Yamato      -> Force Edge
        else if (id == 12 && g_skipForceEdge)  id = 11;   // Vergil: Force Edge  -> Beowulf
        else if (id == 11 && g_skipBeowulf)    id = 10;   // Vergil: Beowulf     -> Yamato
        else break;
    }
    return id;
}
// Generic remap-cave installer for a weapon-cycle write (mov [edi+memOff],eax; mov al,1):
// remap(id) -> write result -> mov al,1 -> jmp back. edi stays live throughout. Serves
// both the gun (0x30) and melee (0x2c) sites -- one shared cave per site covers every
// character since their weapon ids never overlap.
static bool cycleRemapInstall(uint32_t rva, const uint8_t* orig, uint8_t memOff, void* remapFn,
                              void** caveOut, uint8_t* saved, bool* onFlag) {
    if (*onFlag || !g_base) return true;
    uintptr_t site = g_base + rva;
    if (!inModule(site, 5) || memcmp((void*)site, orig, 5) != 0) {
        logf("[skipwpn] site 0x%X busy/mismatch (id logger on?) -- skip", rva); return false; }
    uint8_t c[64]; int n = 0;
    c[n++]=0x60;                                     // pushad
    c[n++]=0x50;                                     // push eax                (arg: new weapon id)
    uint32_t fn = (uint32_t)(uintptr_t)remapFn;
    c[n++]=0xB8; memcpy(c+n,&fn,4); n+=4;            // mov eax, remapFn
    c[n++]=0xFF; c[n++]=0xD0;                        // call eax                -> eax = remapped id
    c[n++]=0x83; c[n++]=0xC4; c[n++]=0x04;           // add esp, 4
    c[n++]=0x89; c[n++]=0x47; c[n++]=memOff;         // mov [edi+memOff], eax   (write remapped)
    c[n++]=0x61;                                     // popad
    c[n++]=0xB0; c[n++]=0x01;                        // mov al, 1
    void* mem = VirtualAlloc(nullptr, 96, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return false;
    int32_t back = (int32_t)((site + 5) - ((uintptr_t)mem + n + 5));
    c[n++]=0xE9; memcpy(c+n,&back,4); n+=4;
    memcpy(mem, c, n); FlushInstructionCache(GetCurrentProcess(), mem, n);
    memcpy(saved, (void*)site, 5);
    uint8_t hook[5] = {0xE9,0,0,0,0};
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook+1,&to,4); writeBytes(site, hook, 5);
    *caveOut = mem; *onFlag = true;
    logf("[skipwpn] remap cave on 0x%X installed %p (%d bytes)", rva, mem, n);
    return true;
}
static void skipSyncSword() {
    if (g_skipGilgamesh || g_skipLucifer || g_skipYamato || g_skipBeowulf || g_skipForceEdge)
        cycleRemapInstall(kSkipSwordRVA, kSkipSwordOrig, 0x2C, (void*)&meleeSkipRemap,
                          &g_skipSwordCave, g_skipSwordSaved, &g_skipSwordOn);
    else skipRemove(kSkipSwordRVA, g_skipSwordSaved, &g_skipSwordCave, &g_skipSwordOn);
}

// ===== MistressDMC: Easy Enemy Step — 50% bigger enemy-step hitspheres =====
static const uint32_t kESHook = 0xA44CE0;
static const uint8_t  kESOrig[5] = { 0xF3,0x0F,0x10,0x5E,0xF0 };   // movss xmm3,[esi-0x10]
static float   g_easyStepScale = 1.5f;
static bool    g_easyStepFlag  = false;
static bool    g_easyStepWant  = false;
static void*   g_esCave  = nullptr;
static uint8_t g_esSaved[5];
static bool    g_esOn    = false;
static bool applyEasyStep() {
    if (g_esOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kESHook;
    if (!inModule(site,5) || memcmp((void*)site, kESOrig, 5) != 0) {
        logf("[estep] site unexpected - aborting"); return false;
    }
    uint8_t cave[] = {
        0xF3,0x0F,0x10,0x5E,0xF0,                 // movss xmm3,[esi-0x10]
        0x80,0x3D, 0,0,0,0, 0x00,                 // cmp byte[g_easyStepFlag],0
        0x74,0x08,                                // je skip
        0xF3,0x0F,0x59,0x1D, 0,0,0,0,             // mulss xmm3,[g_easyStepScale]
        0xE9, 0,0,0,0                             // jmp back
    };
    uint32_t flagAbs  = (uint32_t)(uintptr_t)&g_easyStepFlag;
    uint32_t scaleAbs = (uint32_t)(uintptr_t)&g_easyStepScale;
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[estep] VirtualAlloc failed"); return false; }
    memcpy(cave + 7,  &flagAbs,  4);
    memcpy(cave + 18, &scaleAbs, 4);
    int32_t backRel = (int32_t)((site + 5) - ((uintptr_t)mem + 22 + 5));
    memcpy(cave + 23, &backRel, 4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[5] = { 0xE9, 0,0,0,0 };
    int32_t toRel = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &toRel, 4);
    memcpy(g_esSaved, (void*)site, 5);
    if (!writeBytes(site, hook, 5)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_esCave = mem; g_esOn = true; logf("[estep] applied (cave=%p)", mem);
    return true;
}
static void stopEasyStep() {
    if (!g_esOn) return;
    writeBytes(g_base + kESHook, g_esSaved, 5);
    if (g_esCave) { VirtualFree(g_esCave, 0, MEM_RELEASE); g_esCave = nullptr; }
    g_esOn = false; logf("[estep] removed");
}

// ===== MistressDMC: Force Lucifer — rose never force-despawns on weapon change =====
static const uint32_t kFLHook   = 0x56D0EC;
static const uint8_t  kFLOrig[6]= {0x88,0x86,0x8C,0x18,0x00,0x00}; // mov [esi+0x188C],al
static volatile uint8_t g_forceLucifer = 0;
static void* g_flCave = nullptr;
static uint8_t g_flSaved[6];
static bool  g_flOn = false;
static bool applyForceLucifer() {
    if (g_flOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kFLHook;
    if (!inModule(site, 6) || memcmp((void*)site, kFLOrig, 6) != 0) {
        logf("[forcelucifer] hook site unexpected (0x%02X) - aborting", *(uint8_t*)site);
        return false;
    }
    uintptr_t flagAddr = (uintptr_t)&g_forceLucifer;
    uint8_t cave[] = {
        0x80,0x3D,0,0,0,0, 0x01,         // [0]  cmp byte[g_forceLucifer],1
        0x75,0x0D,                       // [7]  jne do_store
        0x83,0xFF,0x06,                  // [9]  cmp edi,6
        0x75,0x09,                       // [12] jne do_store (-> @23, end@14: 23-14=9)
        0x84,0xC0,                       // [14] test al,al
        0x75,0x04,                       // [16] jnz do_store
        0xE9,0,0,0,0,                    // [18] jmp skip_store
        0x88,0x86,0x8C,0x18,0x00,0x00,   // [23] do_store: mov [esi+0x188C],al
        0xE9,0,0,0,0                     // [29] jmp resume
    };
    cave[8]  = 0x0E;                      // jne -> do_store (23-9)
    cave[17] = 0x05;                      // jnz -> do_store (23-18)
    const size_t SKIP_JMP = 18, DO_JMP = 29;
    void* mem = VirtualAlloc(nullptr, sizeof(cave), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[forcelucifer] VirtualAlloc failed"); return false; }
    memcpy(cave + 2, &flagAddr, 4);
    uintptr_t resume = site + 6;
    int32_t backSkip = (int32_t)(resume - ((uintptr_t)mem + SKIP_JMP + 5));
    int32_t backDo   = (int32_t)(resume - ((uintptr_t)mem + DO_JMP   + 5));
    memcpy(cave + SKIP_JMP + 1, &backSkip, 4);
    memcpy(cave + DO_JMP   + 1, &backDo,   4);
    memcpy(mem, cave, sizeof(cave));
    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(cave));
    uint8_t hook[6] = { 0xE9,0,0,0,0, 0x90 };
    int32_t to = (int32_t)((uintptr_t)mem - (site + 5));
    memcpy(hook + 1, &to, 4);
    memcpy(g_flSaved, (void*)site, 6);
    if (!writeBytes(site, hook, 6)) { VirtualFree(mem, 0, MEM_RELEASE); return false; }
    g_flCave = mem; g_flOn = true; logf("[forcelucifer] cave installed (%p)", mem);
    return true;
}
static void stopForceLucifer() {
    if (!g_flOn) return;
    writeBytes(g_base + kFLHook, g_flSaved, 6);
    if (g_flCave) { VirtualFree(g_flCave, 0, MEM_RELEASE); g_flCave = nullptr; }
    g_flOn = false; g_forceLucifer = 0; logf("[forcelucifer] removed");
}

// ---- Restore Lucifer Bug: reverts SE's Lucifer needles to vanilla DMC4 behaviour --
// DMC4SE raised Dante's Lucifer needle cap to 128 and widened the write-index mask
// to 0x7F; the original 2008 game capped at 16 (mask 0x0F) -- the classic "Lucifer
// bug" feel that detonates the whole rose set at once. Three same-size immediate
// edits, fully reversible. Addresses + replacement bytes were lifted verbatim from
// the "Restore Lucifer Bug" proxy DLL's own embedded patch table:
//   module+0x1B2C6F: `and eax,0x8000007f` (25 7F 00 00 80) -> `and eax,0x0800000f` (25 0F 00 00 08)
//   module+0x1B2C94: `cmp byte[esi+0x1745],0x80`           -> 0x10  (live-count cap 128 -> 16)
//   module+0x1B2C9D: `mov byte[esi+0x1745],0x80`           -> 0x10  (clamp value   128 -> 16)
static const uint32_t kLucMask = 0x1B2C6F;                          // 5-byte `and eax,imm32`
static const uint32_t kLucCmp  = 0x1B2C94;                          // cmp immediate (1 byte)
static const uint32_t kLucMov  = 0x1B2C9D;                          // mov immediate (1 byte)
static const uint8_t  kLucMaskOrig[5] = { 0x25, 0x7F, 0x00, 0x00, 0x80 };
static const uint8_t  kLucMaskNew [5] = { 0x25, 0x0F, 0x00, 0x00, 0x08 };
static bool g_lucOn = false;
static bool applyLuciferBug() {
    if (g_lucOn) return true;
    if (!g_base) return false;
    uintptr_t mask = g_base + kLucMask, cmp = g_base + kLucCmp, mov = g_base + kLucMov;
    if (memcmp((void*)mask, kLucMaskOrig, 5) != 0 ||                // don't clobber a changed build / double-apply
        *(uint8_t*)cmp != 0x80 || *(uint8_t*)mov != 0x80) {
        logf("[lucifer] hook sites unexpected (mask %02X, cmp %02X, mov %02X) - aborting",
             *(uint8_t*)mask, *(uint8_t*)cmp, *(uint8_t*)mov);
        return false;
    }
    uint8_t v16 = 0x10;
    bool a = writeBytes(mask, kLucMaskNew, 5);
    bool b = writeBytes(cmp, &v16, 1);
    bool c = writeBytes(mov, &v16, 1);
    g_lucOn = a && b && c;
    logf("[lucifer] %s", g_lucOn ? "applied" : "partial/failed");
    return g_lucOn;
}
static void stopLuciferBug() {
    if (!g_lucOn) return;
    uint8_t v80 = 0x80;
    writeBytes(g_base + kLucMask, kLucMaskOrig, 5);
    writeBytes(g_base + kLucCmp, &v80, 1);
    writeBytes(g_base + kLucMov, &v80, 1);
    g_lucOn = false;
    logf("[lucifer] removed");
}

// ---- Hide HUD: flips the HUD-draw branch at module+0x6AD88C (JE <-> JNE). ----
static const uint32_t kHudHook = 0x6AD88C;
static bool g_hudHidden = false;
static void setHideHud(bool on) {
    if (!g_base) return;
    uintptr_t a = g_base + kHudHook;
    uint8_t cur = *(uint8_t*)a;
    if (cur != 0x74 && cur != 0x75) { logf("[hud] hook unexpected 0x%02X", cur); return; }
    uint8_t want = on ? 0x75 : 0x74;    // 0x74 JE = HUD shown, 0x75 JNE = HUD hidden
    writeBytes(a, &want, 1);
    g_hudHidden = on;
    logf("[hud] %s", on ? "hidden" : "shown");
}

// ---- HUD piece toggles. The in-game HUD's elements are individual draw calls
// inside the root HUD draw method (0xAB5150). NOPing a call removes that piece.
// Each call is __thiscall (object in ecx) with N stack-arg bytes pushed first;
// skipping it means cleaning those N bytes, so we replace the 5-byte `call`
// with `add esp,N` (+ NOP pad), or a plain NOP when N==0. Reversible. Labelled
// generically; the user toggles to leave only the style-meter circle.
struct HudPiece { uint32_t rva; int args; uint8_t saved[5]; bool on; };
static HudPiece g_hudPieces[] = {
    { 0x6B51C0, 0,  {0}, false },   // A
    { 0x6B51DA, 12, {0}, false },   // B
    { 0x6B51F3, 8,  {0}, false },   // C
    { 0x6B52D5, 0,  {0}, false },   // D
    { 0x6B53C5, 4,  {0}, false },   // E
    { 0x6B555B, 0,  {0}, false },   // F
    { 0x6B55EF, 4,  {0}, false },   // G
    { 0x6B55F7, 4,  {0}, false },   // H
};
static const int kNHudPieces = (int)(sizeof(g_hudPieces)/sizeof(g_hudPieces[0]));
static void setHudPiece(int i, bool on) {
    if (!g_base || i < 0 || i >= kNHudPieces) return;
    uintptr_t a = g_base + g_hudPieces[i].rva;
    if (on) {
        if (*(uint8_t*)a != 0xE8) { logf("[hud] piece %d site unexpected 0x%02X", i, *(uint8_t*)a); return; }
        memcpy(g_hudPieces[i].saved, (void*)a, 5);
        uint8_t buf[5];
        if (g_hudPieces[i].args == 0) { memset(buf, 0x90, 5); }
        else { buf[0] = 0x83; buf[1] = 0xC4; buf[2] = (uint8_t)g_hudPieces[i].args; buf[3] = 0x90; buf[4] = 0x90; }
        writeBytes(a, buf, 5);
    } else {
        if (g_hudPieces[i].saved[0] == 0xE8) writeBytes(a, g_hudPieces[i].saved, 5);
    }
    g_hudPieces[i].on = on;
}


// ---- Disable Center Camera: NOPs the two movss blocks --
// that write the auto-centre camera angle, at module+0x12BEB4 (8B) and +0x12BEBF (24B).
static bool    g_noCenterCam = false;
static uint8_t g_ccSave1[8], g_ccSave2[24];
static void setDisableCenterCamera(bool on) {
    if (!g_base) return;
    uintptr_t a1 = g_base + 0x12BEB4, a2 = g_base + 0x12BEBF;
    if (on) {
        if (*(uint8_t*)a1 != 0xF3) { logf("[cam] site unexpected 0x%02X", *(uint8_t*)a1); return; }
        memcpy(g_ccSave1, (void*)a1, 8);
        memcpy(g_ccSave2, (void*)a2, 24);
        uint8_t nop[24]; memset(nop, 0x90, sizeof(nop));
        writeBytes(a1, nop, 8);
        writeBytes(a2, nop, 24);
    } else {
        writeBytes(a1, g_ccSave1, 8);
        writeBytes(a2, g_ccSave2, 24);
    }
    g_noCenterCam = on;
    logf("[cam] center camera %s", on ? "disabled" : "enabled");
}

// ---- Free camera: Disable most of fixed cameras (ported from zhm86's CT v1.1) ----
// The game decides the camera each frame at RVA 0x207FE7 (orig: `cmp ecx,3` + `ja 0x2080AF`,
// 9 bytes). zhm86's table hooks it: read the room/camera mode = [base+0xF59F00]+0x154; a
// whitelist of modes runs the original logic, a few modes write a fixed distance, and every
// OTHER mode SKIPS the fixed-camera setup (jmp 0x2080E7) -> the free follow-cam stays. Also
// NOPs the companion write at 0x121A97 (5 bytes). We build the cave with all internal jumps
// pointing BACKWARD (targets emitted before the entry) so there are no fragile forward refs.
static bool    g_freeCam = false;
static uint8_t* g_freeCamCave = nullptr;
static uint8_t g_freeCamHookSave[9];
static uint8_t g_freeCamNopSave[5];
// DISABLED: the hand-assembled fixed-camera cave (hook @0x207FE7 + NOP @0x121A97) crashed the
// game on this build (page fault in the camera path) — it skipped the camera setup and a later
// write hit a bad pointer. Reverting to the safe data-only third-person (camera struct + centre
// patch). Kept as a no-op stub so the toggle/reset calls compile and the disable path restores
// anything if it was ever installed by an older build.
static bool setFreeCamFixed(bool on) {
    if (!on) {
        if (g_freeCamCave) {
            if (g_base) writeBytes(g_base + 0x207FE7, g_freeCamHookSave, 9);
            if (g_base && inModule(g_base + 0x121A97, 5)) writeBytes(g_base + 0x121A97, g_freeCamNopSave, 5);
            VirtualFree(g_freeCamCave, 0, MEM_RELEASE); g_freeCamCave = nullptr;
        }
        g_freeCam = false;
    }
    return false;   // never installs the cave
}

// ---- Disable Depth of Field / Motion Blur (from a community HxD guide) ----
// The game looks up its post-process shaders by name; corrupting one byte of a
// name makes the lookup miss, disabling that effect. The guide edits the .exe
// on disk (mbGSDOF->mxGSDOF, uDOFFilter->xDOFFilter, uMotionBlurFilter->
// xMotionBlurFilter); we do the same byte flips in module memory at runtime so
// they toggle cleanly and need no exe edit. Scan the loaded image for each name.
static uintptr_t findBytesInModule(const char* pat, size_t n) {
    if (!g_base || !g_modSize || n == 0) return 0;
    const uint8_t* base = (const uint8_t*)g_base;
    for (size_t i = 0; i + n <= g_modSize; ++i)
        if (base[i] == (uint8_t)pat[0] && memcmp(base + i, pat, n) == 0) return g_base + i;
    return 0;
}
// one reversible single-byte rename: find `name`, flip byte[idx] to `to`.
struct StrFlip { uintptr_t addr; char orig; };
static bool applyStrFlip(StrFlip& s, const char* name, int idx, char to) {
    uintptr_t a = findBytesInModule(name, strlen(name));
    if (!a) { logf("[gfx] string not found: %s", name); return false; }
    s.addr = a + idx; s.orig = *(char*)s.addr;
    writeMem(s.addr, &to, 1);
    return true;
}
static void revertStrFlip(StrFlip& s) {
    if (s.addr) { writeMem(s.addr, &s.orig, 1); s.addr = 0; }
}
static bool g_noDOF = false, g_noMotionBlur = false;
static StrFlip g_dofMb{}, g_dofFilter{}, g_mbFilter{};
static void setDisableDOF(bool on) {
    if (on) {                                   // mbGSDOF -> mxGSDOF, uDOFFilter -> xDOFFilter
        applyStrFlip(g_dofMb,     "mbGSDOF",    1, 'x');
        applyStrFlip(g_dofFilter, "uDOFFilter", 0, 'x');
    } else { revertStrFlip(g_dofMb); revertStrFlip(g_dofFilter); }
    g_noDOF = on;
    logf("[gfx] depth of field %s", on ? "disabled" : "enabled");
}
static void setDisableMotionBlur(bool on) {
    if (on) applyStrFlip(g_mbFilter, "uMotionBlurFilter", 0, 'x');   // -> xMotionBlurFilter
    else    revertStrFlip(g_mbFilter);
    g_noMotionBlur = on;
    logf("[gfx] motion blur %s", on ? "disabled" : "enabled");
}
static bool g_noGodRays = false;
static StrFlip g_godRay{};
static void setDisableGodRays(bool on) {
    if (on) applyStrFlip(g_godRay, "GodRay", 0, 'x');   // GodRay -> xodRay (lookup misses)
    else    revertStrFlip(g_godRay);
    g_noGodRays = on;
    logf("[gfx] god rays %s", on ? "disabled" : "enabled");
}

// ---- Camera tool: live CameraData struct reached via --
// [[[base+0xF59F00]+0x140]+0x468]. Distance (0xD8) pulls the camera BACK (the
// COD-style zoom-out); distanceLockOn (0xDC) is the same while locked onto an
// enemy; height 0xD0; fov 0xE4. The game rewrites these every frame, so we
// reapply the targets each frame while enabled.
static bool  g_camOn     = false;
static float g_camDist   = 600.0f;
static float g_camHeight = 0.0f;
static float g_camOrigH = 0, g_camOrigD = 0, g_camOrigDL = 0;   // defaults to restore on disable
static bool  g_camSaved = false;
static uint8_t* getCameraData() {
    uintptr_t p1, p2, p3;
    if (!readPtr(g_base + 0xF59F00, p1) || !p1) return nullptr;
    if (!readPtr(p1 + 0x140, p2)       || !p2) return nullptr;
    if (!readPtr(p2 + 0x468, p3)       || !p3) return nullptr;
    if (!memReadable((void*)(p3 + 0xE4), 4))   return nullptr;
    return (uint8_t*)p3;
}
static void updateCamera() {
    if (!g_camOn) return;
    uint8_t* cd = getCameraData();
    if (!cd) return;
    *(float*)(cd + 0xD0) = g_camHeight;   // height
    *(float*)(cd + 0xD8) = g_camDist;     // distance (pull back)
    *(float*)(cd + 0xDC) = g_camDist;     // distanceLockOn (same -> pulls back in combat too)
}

// ====================================================================
// Per-move Speed tweaks (Section 4).
// Ported from a community Cheat Engine table (speed_generator.py). A
// code-cave hook branches per move id; we keep the per-move float table in OUR
// allocated cave memory so the UI sliders edit it live with no rebuild.
//
//   SPEED  hook exe+0x787B2A  (movss xmm4,[eax+edi+0x53C] -- the move's anim
//          rate). move id at [eax+edi+0x4E4]; matched moves load a custom rate
//          into xmm4 instead. >1.0 = faster.
//
// Like every other cheat here: verify the original bytes first; a build
// mismatch greys the toggle out instead of crashing.
struct RevMove { uint32_t id; const char* name; float def; };
// All defaults are 1.0x (vanilla animation rate): the sliders start at 1.00x and
// "Reset speeds" returns every move to 1.0. Tune up from there per move.
static RevMove kSpeedMoves[] = {
    { 0x00000732, "Pandora: Argument open",          1.0f },
    { 0x00000733, "Pandora: Argument close",         1.0f },
    { 0x0000073C, "Pandora: Argument shot",          1.0f },
    { 0x0000020C, "Round Trip",                      1.0f },
    { 0x00000734, "Pandora: Boomerang start",        1.0f },
    { 0x00000739, "Pandora: Boomerang return",       1.0f },
    { 0x01000739, "Pandora: Boomerang warmup",       1.0f },
    { 0x00000208, "Million Stabs",                   1.0f },
    { 0x00000306, "Gilgamesh: third hit",            1.0f },
    { 0x00000408, "Lucifer: combo E",                1.0f },
};
static const int kNSpeed  = (int)(sizeof(kSpeedMoves) / sizeof(kSpeedMoves[0]));
static float g_speedVal[kNSpeed];   // values the sliders edit (seed the cave on enable)
static bool  g_revInit = false;
static void initRevDefaults() {
    if (g_revInit) return;
    for (int k = 0; k < kNSpeed;  ++k) g_speedVal[k]  = kSpeedMoves[k].def;
    g_revInit = true;
}

// Tiny relocatable code emitter: emit bytes with internal/external labels, then
// resolve rel32 / abs32 fields once the cave's runtime address is known. Keeps
// us from hand-computing branch offsets across a per-move chain.
struct Emit {
    std::vector<uint8_t> b;
    struct Lbl { bool ext; size_t off; uintptr_t abs; };
    std::vector<Lbl> L;
    struct Fix { size_t at; bool isAbs; int lbl; };
    std::vector<Fix> F;
    int  label()            { L.push_back({false, 0, 0}); return (int)L.size() - 1; }
    int  extLabel(uintptr_t a){ L.push_back({true, 0, a}); return (int)L.size() - 1; }
    void here(int l)        { L[l].off = b.size(); }
    void u32(uint32_t v)    { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); }
    template <class... A> void raw(A... a) { uint8_t t[] = { (uint8_t)a... }; for (uint8_t x : t) b.push_back(x); }
    void rel32(int l)       { F.push_back({ b.size(), false, l }); u32(0); }
    void abs32(int l)       { F.push_back({ b.size(), true,  l }); u32(0); }
    void resolve(uint8_t* mem) {
        for (auto& f : F) {
            uintptr_t tgt = L[f.lbl].ext ? L[f.lbl].abs : (uintptr_t)mem + L[f.lbl].off;
            uint32_t v = f.isAbs ? (uint32_t)tgt : (uint32_t)(tgt - ((uintptr_t)mem + f.at + 4));
            memcpy(mem + f.at, &v, 4);
        }
    }
};

// ---- Speed cave (exe+0x787B2A) ----
static const uint32_t kSpeedHook   = 0x787B2A;
static const uint8_t  kSpeedOrig[9] = { 0xF3,0x0F,0x10,0xA4,0x38,0x3C,0x05,0x00,0x00 };
static void*   g_speedMem  = nullptr;
static float*  g_speedData = nullptr;     // -> the float table inside the cave
static uint8_t g_speedSaved[9];
static bool    g_speedOn   = false;
static bool speedAvailable() {
    uintptr_t a = g_base + kSpeedHook;
    return g_speedOn || (inModule(a, 9) && memcmp((void*)a, kSpeedOrig, 9) == 0);
}
static bool enableSpeedCave() {
    if (g_speedOn) return true;
    if (!g_base) return false;
    uintptr_t site = g_base + kSpeedHook;
    if (memcmp((void*)site, kSpeedOrig, 9) != 0) { logf("[rev-speed] site 0x%02X mismatch", *(uint8_t*)site); return false; }
    Emit e; int n = kNSpeed;
    std::vector<int> slot(n), load(n);
    for (int k = 0; k < n; ++k) { slot[k] = e.label(); e.here(slot[k]); e.u32(*(uint32_t*)&g_speedVal[k]); }
    int ret = e.extLabel(site + 9), Lexit = e.label();
    for (int k = 0; k < n; ++k) load[k] = e.label();
    int code = e.label(); e.here(code);
    e.raw(0xF3,0x0F,0x10,0xA4,0x38,0x3C,0x05,0x00,0x00);          // default: movss xmm4,[eax+edi+53C]
    for (int k = 0; k < n; ++k) {
        e.raw(0x81,0xBC,0x38,0xE4,0x04,0x00,0x00); e.u32(kSpeedMoves[k].id);  // cmp [eax+edi+4E4],id
        e.raw(0x0F,0x84); e.rel32(load[k]);                       // je load_k
    }
    e.here(Lexit); e.raw(0xE9); e.rel32(ret);                     // jmp returnhere
    for (int k = 0; k < n; ++k) {
        e.here(load[k]);
        e.raw(0xF3,0x0F,0x10,0x25); e.abs32(slot[k]);             // movss xmm4,[slot_k]
        e.raw(0xE9); e.rel32(ret);                                // jmp returnhere
    }
    void* mem = VirtualAlloc(nullptr, e.b.size(), MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { logf("[rev-speed] alloc fail"); return false; }
    memcpy(mem, e.b.data(), e.b.size());
    e.resolve((uint8_t*)mem);
    FlushInstructionCache(GetCurrentProcess(), mem, e.b.size());
    uintptr_t entry = (uintptr_t)mem + e.L[code].off;
    uint8_t hook[9]; memset(hook, 0x90, 9); hook[0] = 0xE9;
    uint32_t rel = (uint32_t)(entry - (site + 5)); memcpy(hook + 1, &rel, 4);
    memcpy(g_speedSaved, (void*)site, 9);
    writeBytes(site, hook, 9);
    g_speedMem = mem; g_speedData = (float*)mem; g_speedOn = true;     // table is at cave offset 0
    logf("[rev-speed] on (cave=%p, %d moves)", mem, n);
    return true;
}
static void disableSpeedCave() {
    if (!g_speedOn) return;
    writeBytes(g_base + kSpeedHook, g_speedSaved, 9);
    if (g_speedMem) { VirtualFree(g_speedMem, 0, MEM_RELEASE); g_speedMem = nullptr; }
    g_speedData = nullptr; g_speedOn = false; logf("[rev-speed] off");
}

// ====================================================================
// ---- Section 5: Unlockables (moves / weapons / styles), no requirements -----
// Save block pointer = [exe+0xF59F10]; each character's flags/counters sit at a
// fixed offset off it (mapped from the DMC4SE-IW CE table). We set the weapon /
// style / ability unlock bytes to 1 and max out Proud Souls so any remaining
// purchasable move costs nothing in the customise menu. One-shot per button;
// every write is validated so it no-ops if no save is loaded.
static const uint32_t kSaveBaseOff = 0xF59F10;
static const int      kProudMax    = 9000000;
// The resolved save block must also be READABLE, not just non-null: at the title
// screen / during a level transition the slot can hold a stale, freed, or garbage
// pointer. Without this check the per-frame modCostumeTick() (and the unlock
// buttons) deref p+off on junk and page-fault. 0x800 covers every field we touch
// (largest is Lady proud-souls 0x578 + the costume fields).
static bool savePtr(uintptr_t& p) {
    return readPtr(g_base + kSaveBaseOff, p) && p && memReadable((void*)p, 0x800);
}
static void uB(uintptr_t p, uint32_t off) { uint8_t v = 1; writeMem(p + off, &v, 1); }
static void uBv(uintptr_t p, uint32_t off, uint8_t v) { writeMem(p + off, &v, 1); }
static void uI(uintptr_t p, uint32_t off, int v) { writeMem(p + off, &v, 4); }

static void unlockDante() {
    uintptr_t p; if (!savePtr(p)) return;
    const uint32_t fl[] = { 0x168,0x169,0x16A,0x16C,0x16D,          // DT, Air Hike, Enemy Step, Orb Magnet, Trigger Heart
                            0x1AB,0x1AC,0x1AD,0x1AE,                 // styles: Trickster, Royal Guard, Gunslinger, Swordmaster
                            0x1AF,0x1B0,0x1B1,0x1B2 };               // weapons: Yamato, Gilgamesh, Pandora, Lucifer
    for (uint32_t o : fl) uB(p, o);
    const uint32_t up[] = { 0x1B4,0x1B8,0x1BC,0x1C0 };               // style levels -> 3 (max)
    for (uint32_t o : up) uI(p, o, 3);
    uI(p, 0x17C, kProudMax);                                         // Proud Souls
}
static void unlockNero() {
    uintptr_t p; if (!savePtr(p)) return;
    const uint32_t fl[] = { 0x270,0x271,0x272,0x273,0x274,0x275,     // DT, Air Hike, Enemy Step, Speed, Orb Magnet, Trigger Heart
                            0x2B0,0x2B1,0x2B2 };                     // Table Hopper, Rusalka Corpse, Aegis Shield
    for (uint32_t o : fl) uB(p, o);
    uBv(p, 0x2B5, 3); uBv(p, 0x2B6, 3);                             // Exceed / Table Hopper upgrades -> max
    uI(p, 0x284, kProudMax);
}
static void unlockVergil() {
    // NOTE: the old flag writes (0x360-0x3A0 / 0x378-0x37E) corrupted Vergil's
    // data -- he could no longer attack and saves were bricked. Vergil has no
    // weapons/styles to toggle anyway, so we only max Proud Souls; every one of
    // his moves is then free to buy in the Customise menu. Safe, non-destructive.
    uintptr_t p; if (savePtr(p)) uI(p, 0x374, kProudMax);
}
static void unlockTrish() { uintptr_t p; if (savePtr(p)) uI(p, 0x470, kProudMax); }
static void unlockLady()  { uintptr_t p; if (savePtr(p)) uI(p, 0x578, kProudMax); }

// ---------------------------------------------------------------- overlay
static bool g_imguiInit = false, g_show = true;
static WNDPROC g_oWndProc = nullptr;
static HWND    g_window = nullptr;

// ---- Window & system toggles (Section 4) ----
static bool      g_forceFocus = false;   // game keeps running when alt-tabbed
static bool      g_borderless = false;   // borderless full-screen window
static bool      g_pauseOnOpen = false;  // freeze the game while the menu is open
static LONG_PTR  g_savedStyle = 0;
static RECT      g_savedRect = {0,0,0,0};
static bool      g_styleSaved = false;
// NOTE: this runs on the render thread (called from the ImGui checkbox), but the
// window is owned by another thread. A *synchronous* SetWindowPos there blocks
// waiting on the window thread and can deadlock the whole game -- which froze the
// menu (no clicks, no keys, no escape). SWP_ASYNCWINDOWPOS posts the change to the
// window thread instead of waiting, so it can never deadlock.
static const UINT kWinPosFlags = SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE;
static void setBorderless(bool on) {
    if (!g_window) { g_borderless = on; return; }
    if (on) {
        if (!g_styleSaved) {
            g_savedStyle = GetWindowLongPtr(g_window, GWL_STYLE);
            GetWindowRect(g_window, &g_savedRect);
            g_styleSaved = true;
        }
        // Zoom to fill the monitor the window is on. The backbuffer stays at the
        // game's render size; DXGI's present stretches it (and the overlay, which is
        // now drawn at backbuffer size + cursor-scaled in hkPresent) to fill the
        // borderless window. MonitorFromWindow keeps this correct on multi-monitor /
        // Retina setups -- the old offset/glitch came from the UI being window-sized,
        // which the hkPresent re-base now fixes, so it's safe to grow the window.
        HMONITOR mon = MonitorFromWindow(g_window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        RECT r;
        if (GetMonitorInfo(mon, &mi)) r = mi.rcMonitor;
        else { r.left = r.top = 0; r.right = GetSystemMetrics(SM_CXSCREEN); r.bottom = GetSystemMetrics(SM_CYSCREEN); }
        SetWindowLongPtr(g_window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_window, HWND_TOP, r.left, r.top,
                     r.right - r.left, r.bottom - r.top, kWinPosFlags);
    } else if (g_styleSaved) {
        SetWindowLongPtr(g_window, GWL_STYLE, g_savedStyle);
        SetWindowPos(g_window, HWND_TOP, g_savedRect.left, g_savedRect.top,
                     g_savedRect.right - g_savedRect.left,
                     g_savedRect.bottom - g_savedRect.top, kWinPosFlags);
    }
    g_borderless = on;
    logf("[window] borderless %s", on ? "on" : "off");
}
static int     g_frames = 0;
static char    g_filter[64] = {0};
static ID3D10Device*           g_device = nullptr;
static ID3D10RenderTargetView* g_rtv    = nullptr;

// ---- Menu music: loops (from memory) while the menu is open; volume slider. --
// We hold the WAV in memory and play a volume-scaled copy via SND_MEMORY so the
// volume slider works (PlaySound has no per-sound volume of its own).
static HINSTANCE g_self      = nullptr;          // our DLL module (for asset paths)
static bool  g_music    = true;        // master on/off
static float g_musicVol = 0.10f;       // 0..1
static int   g_curTrack = 0;           // current track index
static int   g_loopMode = 0;           // 0=loop one, 1=playlist loop-all, 2=play through
static bool  g_bgMusic  = false;       // keep playing in-game (menu closed)
static bool  g_shuffle  = false;       // pick a random next track instead of sequential

static void buildAssetPath(char* out, const char* name) {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(g_self, buf, MAX_PATH);    // assets ship beside dinput8.dll
    std::string p = buf;
    size_t slash = p.find_last_of("\\/");
    p = (slash != std::string::npos) ? p.substr(0, slash + 1) : "";
    p += name;
    strncpy(out, p.c_str(), MAX_PATH - 1);
}

// ---- Skip Boot / Opening Movie (Section 2) -----------------------------------
// The boot opening is the 425MB regional cinematic at
//   nativeDX10\movie\rom_for\adv_for.wmv
// (Capcom pre-roll + the DMC4 intro). There is no per-frame "cutscene playing"
// flag for it the way the per-mission intro/outro have, so instead of an in-memory
// byte patch we swap the movie file for a tiny black+silent stub (embedded in this
// dll). The game's movie player plays the ~0.16s stub and drops straight to the
// title screen. Fully reversible: the real movie is kept beside it as <name>.orig.
//
// Movies are only read at boot, so toggling this takes effect on the NEXT launch.
static bool g_skipBoot = false;
static bool g_skipWarnings = true;   // auto-confirm the boot caution screens (used by bootSkipTick)
static const char* const kBootMovieRel  = "nativeDX10\\movie\\rom_for\\adv_for.wmv";

// True when the stub is currently installed (i.e. the .orig backup exists on disk).
static bool bootSkipInstalled() {
    char orig[MAX_PATH]; buildAssetPath(orig, "nativeDX10\\movie\\rom_for\\adv_for.wmv.orig");
    return GetFileAttributesA(orig) != INVALID_FILE_ATTRIBUTES;
}

// Install (on=true) or remove (on=false) the boot-movie stub. Idempotent: keyed off
// the actual on-disk state, never double-renames or loses the original. Returns the
// resulting on-disk state.
static bool setSkipBoot(bool on) {
    char movie[MAX_PATH], orig[MAX_PATH];
    buildAssetPath(movie, kBootMovieRel);
    buildAssetPath(orig,  "nativeDX10\\movie\\rom_for\\adv_for.wmv.orig");
    bool installed = bootSkipInstalled();

    if (on && !installed) {
        // Preserve the real movie, then drop the stub in its place.
        if (GetFileAttributesA(movie) != INVALID_FILE_ATTRIBUTES &&
            !MoveFileA(movie, orig)) { logf("[boot] backup move failed (%lu)", GetLastError()); return installed; }
        FILE* f = fopen(movie, "wb");
        if (!f) { logf("[boot] stub write open failed"); MoveFileA(orig, movie); return false; }
        fwrite(kBootStub, 1, kBootStubLen, f); fclose(f);
        logf("[boot] opening movie stubbed (skip ON)");
        g_skipBoot = true;  return true;
    }
    if (!on && installed) {
        DeleteFileA(movie);                       // remove the stub
        if (!MoveFileA(orig, movie)) { logf("[boot] restore move failed (%lu)", GetLastError()); }
        else logf("[boot] opening movie restored (skip OFF)");
        g_skipBoot = bootSkipInstalled();  return g_skipBoot;
    }
    g_skipBoot = installed;  return installed;
}

// ---- Multi-track music player (waveOut streaming over music\NN.wav). Songs are
// 44.1kHz/16-bit/stereo. Pick with the dropdown or < / > ; loop one, loop the whole
// playlist, or play through once; optional in-game background playback. Closing the
// menu pauses (doesn't restart) so it resumes exactly where it left off. -----------
// Tracks are discovered by scanning the `music\` folder for *.wav at launch, so
// anyone can drop in their OWN songs (any WAV format) and they appear by filename.
static std::vector<std::string> g_trackNames;   // display names (filename minus .wav)
static std::vector<std::string> g_trackFiles;   // file names in the music folder
static int nTracks() { return (int)g_trackNames.size(); }
static void scanMusicFolder() {
    // remember the current track BY FILE so a rescan doesn't jump to a different
    // song just because the list index shifted.
    std::string cur = (g_curTrack >= 0 && g_curTrack < (int)g_trackFiles.size())
                      ? g_trackFiles[g_curTrack] : std::string();
    g_trackNames.clear(); g_trackFiles.clear();
    char glob[MAX_PATH]; buildAssetPath(glob, "music\\*.wav");
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(glob, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { std::string fn = fd.cFileName; if (fn.size() > 4) g_trackFiles.push_back(fn); }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(g_trackFiles.begin(), g_trackFiles.end());
    for (auto& fn : g_trackFiles) g_trackNames.push_back(fn.substr(0, fn.size() - 4));
    if (!cur.empty())
        for (int i = 0; i < (int)g_trackFiles.size(); ++i)
            if (g_trackFiles[i] == cur) { g_curTrack = i; break; }
    if (g_curTrack >= nTracks()) g_curTrack = 0;
    logf("[music] found %d track(s)", nTracks());
}

static HWAVEOUT g_wo = nullptr;
static bool g_woOpen = false, g_woPaused = false;
static const int kNBuf = 6, kBufBytes = 8192;   // small buffers -> snappy volume response
static WAVEHDR g_hdr[kNBuf];
static std::vector<char> g_bufMem[kNBuf];
static std::vector<uint8_t> g_pcm;     // current track's PCM data chunk
static size_t g_pcmPos = 0;
static int g_chs = 2, g_rate = 44100, g_bits = 16;             // current track format
static int g_openChs = 0, g_openRate = 0, g_openBits = 0;      // format the device is open at
static void closeWave();

static bool loadTrackPCM(int idx) {
    if (idx < 0 || idx >= nTracks()) return false;
    char rel[MAX_PATH]; snprintf(rel, sizeof rel, "music\\%s", g_trackFiles[idx].c_str());
    char path[MAX_PATH]; buildAssetPath(path, rel);
    FILE* f = fopen(path, "rb"); if (!f) { logf("[music] missing %s", path); return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 44) { fclose(f); return false; }
    std::vector<uint8_t> all(sz);
    if (fread(all.data(), 1, sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);
    // walk chunks for both 'fmt ' (format) and 'data' (PCM) -- accept any rate/
    // channels/bit-depth so user-supplied WAVs in any format still play.
    size_t o = 12, doff = 0, dlen = 0;
    while (o + 8 <= all.size()) {
        uint32_t cs = *(uint32_t*)&all[o + 4];
        if (memcmp(&all[o], "fmt ", 4) == 0 && o + 8 + 16 <= all.size()) {
            const uint8_t* fmt = &all[o + 8];
            g_chs  = *(const uint16_t*)(fmt + 2);
            g_rate = (int)*(const uint32_t*)(fmt + 4);
            g_bits = *(const uint16_t*)(fmt + 14);
        } else if (memcmp(&all[o], "data", 4) == 0) { doff = o + 8; dlen = cs; }
        o += 8 + cs + (cs & 1);
    }
    if (!dlen || g_chs < 1 || g_rate < 8000 || (g_bits != 8 && g_bits != 16)) return false;
    if (doff + dlen > all.size()) dlen = all.size() - doff;
    g_pcm.assign(all.begin() + doff, all.begin() + doff + dlen);
    g_pcmPos = 0;
    // if the device is open at a different format, close it so it reopens to match
    if (g_openChs && (g_chs != g_openChs || g_rate != g_openRate || g_bits != g_openBits)) closeWave();
    return true;
}
static void applyMusicVolume() {
    if (!g_woOpen) return;
    DWORD v = (DWORD)(g_musicVol * 0xFFFF); if (v > 0xFFFF) v = 0xFFFF;
    waveOutSetVolume(g_wo, (v << 16) | v);
}
static void closeWave() {
    if (!g_woOpen) return;
    waveOutReset(g_wo);
    for (int i = 0; i < kNBuf; i++)
        if (g_hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof(WAVEHDR));
    waveOutClose(g_wo);
    g_wo = nullptr; g_woOpen = false; g_woPaused = false;
    g_openChs = g_openRate = g_openBits = 0;
}
static void openWave() {
    if (g_woOpen) return;
    WAVEFORMATEX wfx{}; wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)g_chs; wfx.nSamplesPerSec = (DWORD)g_rate; wfx.wBitsPerSample = (WORD)g_bits;
    wfx.nBlockAlign = (WORD)(g_chs * g_bits / 8);
    wfx.nAvgBytesPerSec = (DWORD)g_rate * wfx.nBlockAlign;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        logf("[music] waveOutOpen failed (%dHz %dch %dbit)", g_rate, g_chs, g_bits); return;
    }
    for (int i = 0; i < kNBuf; i++) {
        g_bufMem[i].resize(kBufBytes); memset(&g_hdr[i], 0, sizeof(WAVEHDR));
        g_hdr[i].lpData = g_bufMem[i].data(); g_hdr[i].dwBufferLength = kBufBytes;
        g_hdr[i].dwFlags = WHDR_DONE;
    }
    g_woOpen = true; g_woPaused = false;
    g_openChs = g_chs; g_openRate = g_rate; g_openBits = g_bits;
    applyMusicVolume();
}
static unsigned g_rng = 0;
static int randTrack() {                           // xorshift, lazily seeded
    if (!g_rng) g_rng = (unsigned)GetTickCount() | 1u;
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    int r = (int)(g_rng % (unsigned)nTracks());
    if (nTracks() > 1 && r == g_curTrack) r = (r + 1) % nTracks();  // avoid immediate repeat
    return r;
}
static void advanceTrack() {                       // called when the current track ends
    if (g_loopMode == 0) { g_pcmPos = 0; return; } // loop this song (shuffle n/a)
    int next;
    if (g_shuffle) next = randTrack();             // random next, endless
    else { next = g_curTrack + 1; if (next >= nTracks()) next = (g_loopMode == 1) ? 0 : -1; }
    if (next >= 0) { g_curTrack = next; loadTrackPCM(g_curTrack); }
    // play-through (no shuffle) finished: g_pcmPos stays at the end so playback stops
}
static bool fillBuffer(WAVEHDR* h) {
    if (g_pcm.empty()) return false;
    if (g_pcmPos >= g_pcm.size()) { advanceTrack(); if (g_pcmPos >= g_pcm.size()) return false; }
    size_t avail = g_pcm.size() - g_pcmPos;
    size_t n = avail < (size_t)kBufBytes ? avail : (size_t)kBufBytes;
    float vol = g_musicVol; if (vol < 0) vol = 0; if (vol > 1) vol = 1;
    // Scale samples by the volume here -- waveOutSetVolume is a no-op under Wine.
    if (g_bits == 16) {
        const int16_t* src = (const int16_t*)&g_pcm[g_pcmPos];
        int16_t* dst = (int16_t*)h->lpData;
        size_t samples = n / 2;
        for (size_t i = 0; i < samples; ++i) {
            int v = (int)(src[i] * vol);
            dst[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        if (n & 1) ((char*)h->lpData)[n - 1] = ((const char*)&g_pcm[g_pcmPos])[n - 1];
    } else {   // 8-bit unsigned PCM (centred at 128)
        const uint8_t* src = (const uint8_t*)&g_pcm[g_pcmPos];
        uint8_t* dst = (uint8_t*)h->lpData;
        for (size_t i = 0; i < n; ++i) dst[i] = (uint8_t)(128 + (int)((src[i] - 128) * vol));
    }
    g_pcmPos += n;
    h->dwBufferLength = (DWORD)n;
    return n > 0;
}
static void pumpWave() {
    if (!g_woOpen || g_woPaused) return;
    for (int i = 0; i < kNBuf; i++) {
        WAVEHDR* h = &g_hdr[i];
        if (h->dwFlags & WHDR_DONE) {
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, h, sizeof(WAVEHDR));
            h->dwFlags = 0;
            if (!fillBuffer(h)) { h->dwFlags = WHDR_DONE; continue; }
            waveOutPrepareHeader(g_wo, h, sizeof(WAVEHDR));
            if (waveOutWrite(g_wo, h, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) h->dwFlags = WHDR_DONE;
        }
    }
}
static void selectTrack(int idx) {
    if (idx < 0) idx = nTracks() - 1; if (idx >= nTracks()) idx = 0;
    g_curTrack = idx;
    loadTrackPCM(idx);
    if (g_woOpen) { waveOutReset(g_wo); for (int i = 0; i < kNBuf; i++) g_hdr[i].dwFlags |= WHDR_DONE; }
}
static void updateMenuMusic() {
    bool want = g_music && (g_show || g_bgMusic);
    if (want) {
        if (!g_woOpen) { if (g_pcm.empty()) loadTrackPCM(g_curTrack); openWave(); }
        if (g_woOpen && g_woPaused) { waveOutRestart(g_wo); g_woPaused = false; }
        pumpWave();
    } else if (g_woOpen && !g_woPaused) {
        waveOutPause(g_wo); g_woPaused = true;
    }
}

// ---- Centered logo watermark (black art on transparent, matches white theme). -
static ID3D10ShaderResourceView* g_logoSRV = nullptr;
static int g_logoW = 0, g_logoH = 0;
static void loadLogo(const char* path) {
    if (!g_device) return;
    FILE* f = fopen(path, "rb");
    if (!f) { logf("[logo] not found: %s", path); return; }
    uint32_t wh[2];
    if (fread(wh, 8, 1, f) != 1) { fclose(f); return; }
    g_logoW = (int)wh[0]; g_logoH = (int)wh[1];
    size_t n = (size_t)g_logoW * g_logoH * 4;
    std::vector<uint8_t> px(n);
    if (fread(px.data(), 1, n, f) != n) { fclose(f); logf("[logo] short read"); return; }
    fclose(f);
    D3D10_TEXTURE2D_DESC td{};
    td.Width = g_logoW; td.Height = g_logoH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_IMMUTABLE; td.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    D3D10_SUBRESOURCE_DATA sd{}; sd.pSysMem = px.data(); sd.SysMemPitch = g_logoW * 4;
    ID3D10Texture2D* tex = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&td, &sd, &tex)) && tex) {
        g_device->CreateShaderResourceView(tex, nullptr, &g_logoSRV);
        tex->Release();
        logf("[logo] loaded %dx%d", g_logoW, g_logoH);
    }
}
static void drawLogoWatermark() {
    if (!g_logoSRV) return;
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    float scale = (ws.x * 0.82f) / (float)g_logoW;
    float w = g_logoW * scale, h = g_logoH * scale;
    ImVec2 ctr(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f);
    ImVec2 a(ctr.x - w * 0.5f, ctr.y - h * 0.5f), b(ctr.x + w * 0.5f, ctr.y + h * 0.5f);
    // red logo watermark behind the controls (red art on transparent; alpha mask)
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)g_logoSRV, a, b,
                                         ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,80));
}

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(__stdcall* SetFullscreenState_t)(IDXGISwapChain*, BOOL, IDXGIOutput*);
static Present_t            oPresent = nullptr;
static ResizeBuffers_t      oResizeBuffers = nullptr;
static SetFullscreenState_t oSetFullscreenState = nullptr;
static volatile LONG        g_presentGuard = 0;   // 1 while one present drives the overlay

// Backbuffer (render-target) size. The overlay is rendered at THIS size, not the
// window size, so when borderless zooms the window past the backbuffer, DXGI's
// present stretches game + overlay together and the UI stays aligned + clickable.
static UINT g_bbW = 0, g_bbH = 0;
static void CreateRTV(IDXGISwapChain* sc) {
    // hkResizeBuffers can fire during startup/resolution setup BEFORE the first
    // Present has resolved the device (common on the DXMT backend), so g_device is
    // still null -- deref'ing it here page-faults. No-op until the device exists;
    // the first hkPresent init calls CreateRTV again once g_device is set.
    if (!g_device) return;
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }   // release before re-acquiring (no leak when called per-frame)
    ID3D10Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&bb)) && bb) {
        g_device->CreateRenderTargetView(bb, nullptr, &g_rtv);
        D3D10_TEXTURE2D_DESC td{}; bb->GetDesc(&td);
        g_bbW = td.Width; g_bbH = td.Height;            // remember the real render size
        bb->Release();
    }
}
static LRESULT WINAPI hkWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Force Focus: make the game believe it's always the active window so it
    // keeps running (and accepting input) when alt-tabbed or another window is up.
    if (g_forceFocus) {
        if      (m == WM_ACTIVATEAPP) w = TRUE;
        else if (m == WM_NCACTIVATE)  w = TRUE;
        else if (m == WM_ACTIVATE)    w = MAKEWPARAM(WA_ACTIVE, HIWORD(w));
        else if (m == WM_KILLFOCUS)   return 0;     // pretend we never lose focus
    }
    // '7' toggles the menu (works on Mac keyboards, unlike the Fn-keys) and is
    // swallowed so the game never sees it -- EXCEPT while typing in the search box.
    bool textActive = g_imguiInit && ImGui::GetIO().WantTextInput;
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && w == '7' && !textActive) { g_show = !g_show; return 0; }
    if (g_show && g_imguiInit) {
        ImGuiIO& io = ImGui::GetIO();
        // DMC4SE never calls TranslateMessage, so ImGui never receives WM_CHAR.
        // Synthesise text characters from key-down events for the search box.
        if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && io.WantTextInput) {
            BYTE ks[256];
            if (GetKeyboardState(ks)) {
                WCHAR buf[8];
                int n = ToUnicode((UINT)w, (UINT)((l >> 16) & 0xFF), ks, buf, 8, 0);
                for (int i = 0; i < n; ++i) io.AddInputCharacterUTF16((unsigned short)buf[i]);
            }
        }
        if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
        // Swallow input the overlay is using so it doesn't reach the game.
        if (io.WantCaptureMouse && (m==WM_LBUTTONDOWN||m==WM_LBUTTONUP||m==WM_RBUTTONDOWN||
            m==WM_RBUTTONUP||m==WM_MBUTTONDOWN||m==WM_MBUTTONUP||m==WM_MOUSEWHEEL||m==WM_MOUSEMOVE))
            return true;
        if (io.WantCaptureKeyboard && (m==WM_KEYDOWN||m==WM_KEYUP||m==WM_SYSKEYDOWN||m==WM_SYSKEYUP||m==WM_CHAR))
            return true;
    }
    return CallWindowProc(g_oWndProc, h, m, w, l);
}

static bool matchFilter(const std::string& s) {
    if (!g_filter[0]) return true;
    std::string a = s, b = g_filter;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}

// Which tab a category belongs to (0=Combat, 1=Buffs, 2=Spawns, 3=Section 4 no-limits).
static int categoryTab(const std::string& s) {
    if (s.rfind("Spawn", 0) == 0)              return -1; // old Scarecrow-swap matrix -> hidden (Direct Spawn replaces it)
    if (s.rfind("Jump Button Inputs", 0) == 0) return 2;
    if (s.rfind("Berial Fire Lost", 0) == 0)   return 2;
    if (s == "Disable Darkslayer")             return 2;
    if (s == "Bloody Palace Scripts")                              return 2;  // Environment
    if (s == "Devil Trigger"       || s == "Nullify DT Gauge Costs" ||
        s == "Summoned Swords"     || s == "Mobility Modifiers")   return 3;  // Character
    if (s == "Auto-Skip Cutscenes")                                return 1;  // General
    if (s == "No Height Limit"     || s == "No Limits")            return 3;  // Character
    return 0;
}

// Draw all generated cheat categories assigned to `tab`; spawn matrix at bottom.
// SliderFloat that keeps drag-to-position but also lets you RIGHT-CLICK the slider
// to type an exact value. Typed values may exceed the drag range (e.g. "make it way
// faster than the bar allows") and are clamped to [hardLo, hardHi]; if those are
// left at 0 the typed range defaults to the slider's own [lo, hi]. Returns true on
// any change (drag or typed), so existing `if (slider) apply()` call-sites still work.
static bool sliderRC(const char* label, float* v, float lo, float hi,
                     const char* fmt = "%.2f", float hardLo = 0.0f, float hardHi = 0.0f) {
    bool changed = ImGui::SliderFloat(label, v, lo, hi, fmt);
    if (hardHi <= hardLo) { hardLo = lo; hardHi = hi; }
    ImGui::PushID(label);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##rcin");
    if (ImGui::BeginPopup("##rcin")) {
        ImGui::TextDisabled("Type a value, Enter to set:");
        ImGui::SetNextItemWidth(120);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        float tmp = *v;
        if (ImGui::InputFloat("##rcv", &tmp, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (tmp < hardLo) tmp = hardLo;
            if (tmp > hardHi) tmp = hardHi;
            *v = tmp; changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("Range %g .. %g", hardLo, hardHi);
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

// Cheats surfaced under a dedicated Section-5 character group are hidden from
// their auto-generated category so they only appear in one place.
static bool isRelocated(const std::string& name) {
    return name == "Infinite Grenades"     // shown under "Lady" in Section 5
        || name == "Infinite Trick Up";    // shown under "Vergil" in Section 5
}
// ---- Bottom-right options: language + theme toggles --------------------------
static bool g_zh     = false; // Chinese (Simplified) localization on/off
static bool g_blue   = false; // blue accent theme (instead of the default red)
static bool g_showFps= true;  // corner FPS counter while the menu is closed (on by default)
static int  g_hkKey[16]  = {0};       // bound VK per hotkey action (0 = unbound)
static bool g_hkCap[16]  = {false};   // capturing a new bind
static bool g_hkPrev[16] = {false};   // rising-edge detection
// FPS readout color now FOLLOWS the active theme: blue theme -> blue, red theme ->
// red. Used by both the corner overlay (menu closed) and the in-menu readout.
static ImVec4 fpsColVec() { return g_blue ? ImVec4(0.20f,0.45f,0.95f,1.0f)
                                          : ImVec4(0.90f,0.16f,0.16f,1.0f); }
static ImU32  fpsColU32() { return g_blue ? IM_COL32(52,116,242,255)
                                          : IM_COL32(230, 40, 40,255); }
// Accent colors used by the hand-colored TextColored() calls; refreshed by
// applyTheme() so those bits of text follow the red<->blue switch too.
static ImVec4 g_accent    = ImVec4(0.80f,0.12f,0.12f,1.0f);  // glowing accent (headers/links)
static ImVec4 g_accentDim = ImVec4(0.62f,0.08f,0.08f,1.0f);  // dim accent (footer/discord)

// Returns the Chinese string when localization is on, else the English one. Bake a
// stable "###id" into both arguments for widgets whose label is also their ID
// (tabs), so switching language never resets which tab/state is selected.
static inline const char* tr(const char* en, const char* zh) { return g_zh ? zh : en; }
static void applyTheme(bool blue);   // fwd (defined near the UI; used by loadConfig)
// ---- GUI scale state (declared early; used by save/loadConfig below) ----------
static float      g_uiScale       = 1.00f;   // user slider, 0.50..3.00
static bool       g_uiAutoDPI     = true;    // auto-fit base scale to resolution
static const float kUiBaseFont    = 0.90f;   // base font scale (bumped from 0.75 for readability)
static ImGuiStyle g_styleBase;               // unscaled snapshot (colors + base sizes)
static bool       g_styleCaptured = false;
static float      g_uiScaleApplied = -1.0f;  // last effective scale written
static float effectiveUiScale();             // fwd
static void  applyUiScale();                 // fwd (defined near applyTheme)

// ---- Bindable hotkeys -------------------------------------------------------
// Any headline action can be bound to a key (rebindable, saved to the profile) and
// fired from a button. Each entry is an action fn, so toggles with side-effects
// (Hide HUD, theme, etc.) all behave correctly.
struct HotkeyDef { const char* name; void (*fn)(); };
static void hkSpawn()    { emSpawn(kEmTypes[g_emSel].createRVA); }
static void hkDespawn()  { despawnEnemies(); }
static void hkDopp()     { if (g_doppActor) doppDespawn(); else doppSpawn(); }
static void hkBossRush() { g_bossRush = !g_bossRush; }
static void hkHud()      { setHideHud(!g_hudHidden); }
static void hkCam()      { g_camOn = !g_camOn; }
static void hkPause()    { g_pauseOnOpen = !g_pauseOnOpen; }
static void hkFps()      { g_showFps = !g_showFps; }
static void hkTheme()    { g_blue = !g_blue; applyTheme(g_blue); }
static void hkDof()      { g_noDOF = !g_noDOF; }
static void macroToggleRecord();   // fwd (Replay: Input Record/Playback)
static void macroTogglePlay();     // fwd
static void macroToggleLoop();     // fwd (Replay: auto-replay / loop)
static void hkRecord()   { macroToggleRecord(); }
static void hkPlay()     { macroTogglePlay(); }
static void hkLoop()     { macroToggleLoop(); }
static const HotkeyDef kHotkeys[] = {
    {"Spawn Enemy",       hkSpawn},   {"Despawn Enemies", hkDespawn},
    {"Doppelganger",      hkDopp},    {"Boss Rush (BP)",  hkBossRush},
    {"Hide HUD",          hkHud},     {"Camera Tool",     hkCam},
    {"Pause with Menu",   hkPause},   {"FPS Counter",     hkFps},
    {"Theme Red/Blue",    hkTheme},   {"No Depth of Field", hkDof},
    {"Replay: Record",    hkRecord},  {"Replay: Play",    hkPlay},
    {"Replay: Auto (loop)", hkLoop},
};
// Indices of the Replay hotkeys (last three entries); used to seed F2/F3/F4 defaults.
static const int kHkReplayRecord = 10;
static const int kHkReplayPlay   = 11;
static const int kHkReplayLoop   = 12;
static const int kNHotkeys = (int)(sizeof(kHotkeys) / sizeof(kHotkeys[0]));

// Quick Spawn (Bloody Palace): a one-pick front-end over the TESTED spawn matrix.
// Selecting an enemy enables "Spawn <enemy> Instead Of Scarecrow" (Scarecrow is the
// common BP filler) and disables the rest of that group -- so in Bloody Palace the
// basic Scarecrow waves arrive as your pick. Pure byte-patch toggles via the normal
// enable/disable path: no new code cave, no enemy IDs, can't crash from this.
static const char* const kSpawnPick[] = {
    "(off)","Scarecrow","Mega Scarecrow","Bianco Angelo","Alto Angelo","Mephisto",
    "Faust","Frost","Assault","Blitz","Chimera Seed","Gladius","Basilisk","Berial",
    "Angelo Credo","Angelo Agnus","Sanctus","Sanctus Diabolica","Kyrie"
};
static const int kNSpawnPick = (int)(sizeof(kSpawnPick) / sizeof(kSpawnPick[0]));
static int g_spawnPickSel = 0;
static void applySpawnPick(int sel) {
    static const std::string CAT = "Spawn ____ Instead Of Scarecrow";
    const char* want = (sel > 0 && sel < kNSpawnPick) ? kSpawnPick[sel] : nullptr;
    for (auto& c : g_cheats) {
        if (c.cat != CAT) continue;
        bool on = (want && c.name == want);
        if (on && !c.active) { if (enableCheat(c)) c.active = true; }
        else if (!on && c.active) disableCheat(c);
    }
    g_spawnPickSel = sel;
}

static void drawCategories(int tab) {
    std::vector<std::string> cats;
    for (auto& c : g_cheats)
        if (categoryTab(c.cat) == tab && !isRelocated(c.name) &&
            std::find(cats.begin(), cats.end(), c.cat) == cats.end())
            cats.push_back(c.cat);
    std::stable_partition(cats.begin(), cats.end(),
        [](const std::string& s){ return s.rfind("Spawn", 0) != 0; });
    for (auto& cat : cats) {
        bool any = false;
        for (auto& c : g_cheats) if (c.cat == cat && matchFilter(c.name) && !isRelocated(c.name)) { any = true; break; }
        if (!any) continue;
        bool spawn = cat.rfind("Spawn", 0) == 0;
        if (ImGui::CollapsingHeader(cat.c_str(),
                spawn ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            // Width-aware two-column flow: a short item holds the row open so the
            // next one sits on the right; a long item takes the whole row, so
            // labels never overlap.
            const float half = ImGui::GetWindowWidth() * 0.5f;
            bool leftPending = false;
            for (auto& c : g_cheats) {
                if (c.cat != cat || !matchFilter(c.name) || isRelocated(c.name)) continue;
                bool placedRight = false;
                if (leftPending) { ImGui::SameLine(half); leftPending = false; placedRight = true; }
                ImGui::PushID((void*)&c);                 // unique ID: names repeat across the spawn matrix
                if (c.unavailable) {
                    ImGui::BeginDisabled();
                    bool d = false; ImGui::Checkbox(c.name.c_str(), &d);
                    ImGui::EndDisabled();
                    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.45f,0.45f,0.45f,1.0f), "(n/a)");
                } else {
                    bool v = c.active;
                    if (ImGui::Checkbox(c.name.c_str(), &v)) {
                        // enable may fail transiently (mutually-exclusive group member active) -> just stay off
                        if (v) { if (enableCheat(c)) { c.active = true; logf("[cheat] on '%s'", c.name.c_str()); } }
                        else disableCheat(c);
                    }
                }
                ImGui::PopID();
                if (!placedRight && ImGui::GetItemRectMax().x < ImGui::GetWindowPos().x + half)
                    leftPending = true;   // short left item -> pair the next one on the right
            }
            if (cat == "Mobility Modifiers") {
                ImGui::Spacing();
                ImGui::TextColored(g_accent, "GitHub page");
                ImGui::TextWrapped("To stay up with updates, my GitHub is:");
                ImGui::TextWrapped("https://github.com/adreamyvoice/DMC4SE-MOOD");
            }
            ImGui::Unindent(8.0f);
        }
    }
}

// ===========================================================================
// Character Select (memory-only, from the DMC4SE-IW Cheat Engine table).
// Writes a character I.D through the game's own pointer chains -- no function
// calls, so it can't crash. IDs: 0=Dante 1=Nero 2=Vergil 3=Trish 4=Lady.
//   Bloody Palace slot:     [[base+0xF240A4]+0x48]+0x380
//   Current-character slot: [[base+0xF59F00]+0x24]+0x19AC
// ===========================================================================
// ===========================================================================
// Per-character value cheats from the v4.0 Cheat Engine table. The table's
// "Majin" symbol resolves to the live player object: Majin = [[base+0xF59F00]
// +0x24]. Values hang off it via CE pointer chains (offsets in CE order, the
// first/outermost added last without a deref). Pure memory read/write -> safe,
// no game-function calls.
// ===========================================================================
static bool ceChain(uintptr_t root, const uint32_t* offs, int n, uintptr_t& out) {
    uintptr_t p;
    if (!readPtr(root, p) || !p) return false;
    for (int i = n - 1; i >= 1; --i) { if (!readPtr(p + offs[i], p) || !p) return false; }
    out = p + offs[0];
    return memReadable((void*)out, 4);
}
static bool majinRoot(uintptr_t& out) {            // readPtr(out) == Majin (player object)
    uintptr_t ecx;
    if (!readPtr(g_base + 0xF59F00, ecx) || !ecx) return false;
    out = ecx + 0x24;
    return true;
}
static bool majinChain(const uint32_t* offs, int n, uintptr_t& out) {
    uintptr_t root; if (!majinRoot(root)) return false;
    return ceChain(root, offs, n, out);
}

// ---- Bloody Palace Character Select (memory-only, from the DMC4SE-IW table).
// Writes ONLY the Bloody Palace selector slot ([[F240A4]+0x48]+0x380) -- a passive
// menu value the game reads when Bloody Palace STARTS. We deliberately do NOT touch
// the live current-character id or the level-transition spawn id: writing those on
// the running game makes it deref a character object that isn't instantiated yet
// (esi=null -> crash). The write only lands if the slot already holds a valid id
// (0-4), so it can never clobber a wrong field. IDs: 0=Dante..4=Lady.
static const char* kCharNames[5] = { "Dante", "Nero", "Vergil", "Trish", "Lady" };
// Arm the char-force hook. id 0-4 forces that character at the next Bloody Palace
// round load (the hook injects it into the BP config at construction); -1 = off.
static void setCharacter(int id) {
    if (id < -1 || id > 4) return;
    g_forceChar = id;
    if (id >= 0) logf("[charsel] force -> %s (%d) -- starts on next BP round", kCharNames[id], id);
    else         logf("[charsel] force off");
}

// ---- Costume swap. The costume index is PER-CHARACTER: each char has its own
//      dword on the save struct that the model builder reads at level load.
//      0/2 = base, 1 = _ex00, 3 = _ex01. Writing the right field is exactly what
//      the stock costume menu does -- safe, reversible, applies on next load.
//      Arc availability on disk: Dante/Nero/Vergil have _ex00 AND _ex01; Trish &
//      Lady have ONLY _ex01 (writing _ex00 to them = a missing arc = the 0x157C86
//      miss the crash-guard catches), so we don't offer _ex00 for those two.
static const uint32_t kCostFieldOff[5] = { 0x1A0, 0x2A8, 0x398, 0x494, 0x59C }; // Dante,Nero,Vergil,Trish,Lady
static bool charHasEx00(int c) { return c >= 0 && c <= 2; }   // Trish(3)/Lady(4): ex01 only
// Costume applies to the character chosen in Character Switch; if that's off, the
// live active character ([[F59F00]+0x24]+0x19AC), else Dante.
static int costumeTargetChar() {
    if (g_forceChar >= 0 && g_forceChar <= 4) return g_forceChar;
    uintptr_t s;
    if (readPtr(g_base + 0xF59F00, s) && s && memReadable((void*)(s + 0x24), 4)) {
        uintptr_t p = *(uintptr_t*)(s + 0x24);
        if (p && memReadable((void*)(p + 0x19AC), 4)) {
            int c = *(int*)(p + 0x19AC);
            if (c >= 0 && c <= 4) return c;
        }
    }
    return 0;
}
static void setCostume(int charId, int idxVal) {
    if (charId < 0 || charId > 4) return;
    if (idxVal == 1 && !charHasEx00(charId)) return;   // guard: no _ex00 arc for Trish/Lady
    uintptr_t p;
    if (!savePtr(p)) { logf("[costume] no save/game-state yet (load a level first)"); return; }
    writeMem(p + kCostFieldOff[charId], &idxVal, 4);
    logf("[costume] %s -> idx %d -- reload level to apply", kCharNames[charId], idxVal);
}
static int getCostume(int charId) {   // read current field (for UI highlight)
    uintptr_t p;
    if (charId < 0 || charId > 4 || !savePtr(p)) return -1;
    return *(int*)(p + kCostFieldOff[charId]);
}

// ======================= Skin / Bloody Palace mod loader =======================
// 4Hook-style "HDD priority": the engine opens resource arcs through CreateFileA
// (cFileWin::open). We hook it and, when the user has assigned a mod to an arc, open
// that mod's arc from the MODS folder instead. A mod file that fails to open falls
// back to the original path, so a bad mod can never hard-fail a load -- it just
// doesn't apply. Nothing in the game folder is overwritten. Each subfolder of MODS
// is one mod (scanned at startup); selections persist to MODS\active.cfg.
struct ModArc   { std::string key, wine; };     // key = lowercased '\'-normalized rom\...\x.arc
// A mod is tagged "<char>:<variant>" -- char in Dante/Nero/Vergil/Trish/Lady/BP,
// variant 0=original (plmod_plNNN), 1=alt1 (_ex00), 2=alt2 (_ex01), 3=BP rooms.
struct ModEntry { std::string name, ch; int variant; bool otherOn; std::vector<ModArc> arcs; };
static std::vector<ModEntry> g_mods;
// 18 slots: each char gets one slot per costume variant they actually have
// (Dante/Nero/Vergil 3, Trish/Lady 2 -- they have no _ex00), plus 5 Bloody Palace.
struct ModSlot { const char* label; const char* ch; int variant; int costChar; }; // costChar: 0-4 (kCharNames) or -1
static const ModSlot kSlots[] = {
    {"Dante - original","Dante",0,0}, {"Dante - alt 1","Dante",1,0}, {"Dante - alt 2","Dante",2,0},
    {"Nero - original","Nero",0,1},   {"Nero - alt 1","Nero",1,1},   {"Nero - alt 2","Nero",2,1},
    {"Vergil - original","Vergil",0,2},{"Vergil - alt 1","Vergil",1,2},{"Vergil - alt 2","Vergil",2,2},
    {"Trish - original","Trish",0,3}, {"Trish - alt","Trish",2,3},
    {"Lady - original","Lady",0,4},   {"Lady - alt","Lady",2,4},
    {"Bloody Palace 1","BP",3,-1},{"Bloody Palace 2","BP",3,-1},{"Bloody Palace 3","BP",3,-1},
    {"Bloody Palace 4","BP",3,-1},{"Bloody Palace 5","BP",3,-1},
};
static const int kNSlots = (int)(sizeof(kSlots)/sizeof(kSlots[0]));
static int  g_modSel[32];                           // per slot: selected mod index, -1 none
static std::vector<ModArc> g_redir;                 // active key->wine
static volatile bool g_redirOn = false;
static volatile uint32_t g_modHits = 0;             // redirect-fire counter (diagnostic)
static bool g_looseLoad = false;                    // loose-file (HDD-cache) loading toggle (persisted)
static volatile bool g_looseArmed = false;          // diagnostic: flag byte reached/written
static std::vector<ModArc> g_hddArcs;               // override arcs under MODS\HDD (active while HDD on)

static std::string lowerbs(std::string s) {         // lowercase + '/'->'\'
    for (char& c : s) { if (c == '/') c = '\\'; else if (c >= 'A' && c <= 'Z') c += 32; }
    return s;
}
static bool modEligible(const ModEntry& m, int slot) {
    return m.ch == kSlots[slot].ch && m.variant == kSlots[slot].variant;
}
// Keep only the arcs that belong to a slot's variant, so each variant slot is
// independent (an alt mod doesn't clobber the base body via uPlayerXxx, etc.).
static bool arcMatchesVariant(const std::string& key, int v) {
    bool ex0  = key.find("_ex00") != std::string::npos || key.find("_demo_00") != std::string::npos;
    bool ex1  = key.find("_ex01") != std::string::npos || key.find("_demo_01") != std::string::npos;
    bool room = key.find("\\room\\") != std::string::npos;
    if (v == 3) return room;
    if (v == 1) return ex0;
    if (v == 2) return ex1;
    return !ex0 && !ex1 && !room;                   // v==0 (original)
}
static void rebuildRedir() {
    g_redir.clear();
    for (int slot = 0; slot < kNSlots; slot++) {
        int sel = g_modSel[slot];
        if (sel < 0 || sel >= (int)g_mods.size()) continue;
        for (auto& a : g_mods[sel].arcs)
            if (arcMatchesVariant(a.key, kSlots[slot].variant)) g_redir.push_back(a);
    }
    for (auto& m : g_mods)                            // "Other" mods: redirect all arcs when toggled on
        if (m.ch == "Other" && m.otherOn)
            for (auto& a : m.arcs) g_redir.push_back(a);
    if (g_looseLoad)                                  // HDD on: redirect override arcs (e.g. GUI mods)
        for (auto& a : g_hddArcs) g_redir.push_back(a);
    g_redirOn = !g_redir.empty();
}
// The canonical game path for a known arc, derived from its FILENAME (so it works
// no matter how the mod folder is laid out -- arc at root, wrong subfolder name like
// "costumes" instead of "costume", etc.). Returns "" if the name isn't recognized.
static std::string canonicalDest(const std::string& base) {   // base = lowercase basename incl .arc
    auto pre = [&](const char* p){ return base.rfind(p, 0) == 0; };
    if (pre("uplayer"))                                          return "rom\\player\\" + base;
    if (pre("plmod_pl"))                                         return "rom\\player\\costume\\" + base;
    if (pre("pl") && base.find("_demo") != std::string::npos)    return "rom\\demo\\player\\" + base;
    if (pre("st"))                                               return "rom\\room\\" + base;     // stNNN.arc
    if (pre("em_"))                                              return "rom\\enemy\\" + base;
    return "";
}
// Recursively collect every .arc under a mod folder. key = the rom\... tail the game
// opens (derived from the arc's name so packaging quirks don't matter); wine = the
// real in-game path we redirect to.
static void walkArcs(const std::string& dir, std::vector<ModArc>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char* nm = fd.cFileName;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
        std::string full = dir + "\\" + nm;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { walkArcs(full, out); continue; }
        std::string low = lowerbs(full);
        if (low.size() <= 4 || low.compare(low.size() - 4, 4, ".arc") != 0) continue;
        std::string key = canonicalDest(low.substr(low.find_last_of('\\') + 1));  // name-derived (preferred)
        if (key.empty()) { size_t r = low.find("\\rom\\"); if (r != std::string::npos) key = low.substr(r + 1); }
        if (key.empty()) continue;
        // de-dupe: a mod may ship the same arc twice (e.g. "with mesh/")
        bool dup = false; for (auto& a : out) if (a.key == key) { dup = true; break; }
        if (!dup) { ModArc a; a.key = key; a.wine = full; out.push_back(a); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
// Decide a mod's character + costume variant from the arcs it contains.
static void classifyMod(ModEntry& e) {
    std::string s;
    for (auto& a : e.arcs) s += a.key + " ";
    // Character first (a costume mod can ship a stray stage/room arc -- don't let that
    // make it look like a Bloody Palace mod).
    static const char* pl[5][2] = {{"pl006","Dante"},{"pl000","Nero"},{"pl030","Vergil"},{"pl007","Trish"},{"pl008","Lady"}};
    e.ch.clear();
    for (auto& p : pl) if (s.find(p[0]) != std::string::npos) { e.ch = p[1]; break; }
    if (e.ch.empty()) {
        static const char* up[5][2] = {{"uplayerdante","Dante"},{"uplayernero","Nero"},{"uplayervergil","Vergil"},{"uplayertrish","Trish"},{"uplayerlady","Lady"}};
        for (auto& p : up) if (s.find(p[0]) != std::string::npos) { e.ch = p[1]; break; }
    }
    if (e.ch.empty()) {                              // no character -> rooms = BP, else Other
        if (s.find("\\room\\") != std::string::npos) { e.ch = "BP"; e.variant = 3; return; }
        e.ch = "Other"; e.variant = 0; return;
    }
    e.variant = (s.find("_ex00") != std::string::npos) ? 1 : (s.find("_ex01") != std::string::npos) ? 2 : 0;
}
// Scan the in-game MODS folder: each subfolder is a mod (drop one in -> it appears).
// Self-contained -- redirect targets are real paths inside the game folder.
static void loadModList() {
    g_mods.clear();
    char base[MAX_PATH]; buildAssetPath(base, "MODS");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((std::string(base) + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { logf("[mod] no MODS folder (%s)", base); return; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        ModEntry e; e.name = fd.cFileName; e.variant = 0; e.otherOn = false;
        walkArcs(std::string(base) + "\\" + fd.cFileName, e.arcs);
        if (e.arcs.empty()) continue;
        classifyMod(e);
        g_mods.push_back(e);                         // keep every folder that has arcs
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    logf("[mod] scanned %d mods from MODS folder", (int)g_mods.size());
}
// HDD override arcs: any file under MODS\HDD\ (mirroring the nativeDX10 layout) is
// redirected over its stock counterpart while Loose-file (HDD) loading is ON. This is
// how GUI / menu arcs get swapped -- their textures are bundled in the arc and aren't
// loose-readable, so we redirect the whole arc, keyed by its path tail.
static void walkHdd(const std::string& dir, size_t baseLen, std::vector<ModArc>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char* nm = fd.cFileName;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
        std::string full = dir + "\\" + nm;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { walkHdd(full, baseLen, out); continue; }
        ModArc a; a.key = lowerbs(full.substr(baseLen)); a.wine = full;   // key = path under MODS\HDD
        out.push_back(a);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static void loadHddOverrides() {
    g_hddArcs.clear();
    char base[MAX_PATH]; buildAssetPath(base, "MODS\\HDD");
    std::string b = base;
    walkHdd(b, b.size() + 1, g_hddArcs);             // +1 drops the separator after the base
    logf("[hdd] %d override file(s) under MODS\\HDD", (int)g_hddArcs.size());
}
static void saveActiveMods() {
    char path[MAX_PATH]; buildAssetPath(path, "MODS\\active.cfg");
    FILE* f = fopen(path, "wb"); if (!f) return;
    for (int slot = 0; slot < kNSlots; slot++) {
        int sel = g_modSel[slot];
        if (sel >= 0 && sel < (int)g_mods.size()) fprintf(f, "%d\t%s\n", slot, g_mods[sel].name.c_str());
    }
    for (auto& m : g_mods) if (m.ch == "Other" && m.otherOn) fprintf(f, "other\t%s\n", m.name.c_str());
    if (g_looseLoad) fprintf(f, "loose\t1\n");        // loose-file (HDD-cache) loading enabled
    fclose(f);
}
static void loadActiveMods() {
    char path[MAX_PATH]; buildAssetPath(path, "MODS\\active.cfg");
    FILE* f = fopen(path, "rb"); if (!f) { rebuildRedir(); return; }
    bool charUsed[5] = { false, false, false, false, false };
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t t = s.find('\t'); if (t == std::string::npos) continue;
        std::string col0 = s.substr(0, t); std::string nm = s.substr(t + 1);
        if (col0 == "other") {                       // an enabled "Other" mod
            for (auto& m : g_mods) if (m.ch == "Other" && m.name == nm) { m.otherOn = true; break; }
            continue;
        }
        if (col0 == "loose") { g_looseLoad = true; continue; }   // loose-file (HDD-cache) loading

        int slot = atoi(col0.c_str());
        if (slot < 0 || slot >= kNSlots) continue;
        int cc = kSlots[slot].costChar;
        if (cc >= 0 && charUsed[cc]) continue;       // one skin per character (avoid the mix crash)
        for (int i = 0; i < (int)g_mods.size(); i++)
            if (modEligible(g_mods[i], slot) && g_mods[i].name == nm) {
                g_modSel[slot] = i; if (cc >= 0) charUsed[cc] = true; break;
            }
    }
    fclose(f);
    rebuildRedir();
}
// Picking a skin in a variant slot: point the redirect AND set that character's
// costume to the matching variant, so the game actually loads the arc we redirect
// (an alt skin shows nothing if the costume is still on "original"). variant->index:
// 0->0 (base), 1->1 (ex00), 2->3 (ex01).
static void setCostume(int charId, int idxVal);    // fwd (defined above)
static void selectMod(int slot, int modIdx) {
    g_modSel[slot] = modIdx;
    int cc = kSlots[slot].costChar;
    // A character has ONE body (uPlayerXxx). Two of its skins active at once mixes a
    // modded body with another mod's costume -> mismatch -> crash. So a character's
    // skin slots are mutually exclusive: picking one clears its siblings.
    if (modIdx >= 0 && cc >= 0) {
        for (int s = 0; s < kNSlots; s++)
            if (s != slot && kSlots[s].costChar == cc) g_modSel[s] = -1;
    }
    rebuildRedir();
    saveActiveMods();
    if (modIdx >= 0 && cc >= 0) {
        int v = kSlots[slot].variant;
        setCostume(cc, v == 2 ? 3 : v);             // align costume so the skin's arc loads
    }
}

typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t oCreateFileA = nullptr;
static bool g_cfHook = false;
static HANDLE WINAPI hkCreateFileA(LPCSTR name, DWORD acc, DWORD shr, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disp, DWORD fl, HANDLE tmpl) {
    // Diagnostic: log each distinct .tex the engine opens + whether a loose file exists
    // there (deduped). Shows which textures load loose vs. from the arc.
    if (name && name[0]) {
        std::string ln = lowerbs(name);
        bool isTex = ln.size() > 4 && ln.compare(ln.size() - 4, 4, ".tex") == 0;
        if (isTex || ln.find("mm_d01") != std::string::npos) {
            static uint32_t seen[256]; static volatile int sn = 0;
            uint32_t h = 2166136261u; for (char c : ln) { h ^= (uint8_t)c; h *= 16777619u; }
            bool dup = false; for (int i = 0; i < sn; i++) if (seen[i] == h) { dup = true; break; }
            if (!dup && sn < 256) {
                seen[sn++] = h;
                DWORD att = GetFileAttributesA(name);
                logf("[hdd-open] [%s] %s", att != INVALID_FILE_ATTRIBUTES ? "LOOSE-EXISTS" : "miss", name);
            }
        }
    }
    if (g_redirOn && name && name[0]) {
        std::string n = lowerbs(name);
        for (auto& a : g_redir) {
            if (n.size() >= a.key.size() &&
                n.compare(n.size() - a.key.size(), a.key.size(), a.key) == 0) {
                HANDLE h = oCreateFileA(a.wine.c_str(), acc, shr, sa, disp, fl, tmpl);
                if (h != INVALID_HANDLE_VALUE) {
                    if (g_modHits < 200) logf("[mod] redirect %s", a.key.c_str());
                    g_modHits++;
                    return h;
                }
                break;                              // mod open failed -> fall back to original
            }
        }
    }
    return oCreateFileA(name, acc, shr, sa, disp, fl, tmpl);
}
static void installCreateFileHook() {
    if (g_cfHook) return;
    HMODULE k = GetModuleHandleA("kernel32.dll"); if (!k) return;
    void* tgt = (void*)GetProcAddress(k, "CreateFileA"); if (!tgt) return;
    MH_Initialize();
    if (MH_CreateHook(tgt, (void*)hkCreateFileA, (void**)&oCreateFileA) == MH_OK &&
        MH_EnableHook(tgt) == MH_OK) {
        g_cfHook = true; logf("[mod] CreateFileA redirect hook armed");
    }
}
static bool g_modInit = false;
static void modInitOnce() {
    if (g_modInit) return;
    g_modInit = true;
    for (int i = 0; i < kNSlots; i++) g_modSel[i] = -1;
    loadModList();
    loadHddOverrides();
    loadActiveMods();
    installCreateFileHook();
}
// Keep each character's costume aligned to its assigned skin's variant, every tick.
// A skin only renders if the game loads the costume arc it replaces -- so if Dante
// has a base skin assigned, force Dante's costume to base, etc. Without this, a skin
// picked at the menu (before the save loaded) silently never shows. Cheap field write.
static void modCostumeTick() {
    if (g_mods.empty()) return;
    uintptr_t p; if (!savePtr(p)) return;
    for (int ch = 0; ch < 5; ch++) {
        for (int s = 0; s < kNSlots; s++) {
            if (kSlots[s].costChar != ch || g_modSel[s] < 0) continue;
            int v = kSlots[s].variant; int val = (v == 2) ? 3 : v;   // variant -> costume index
            if (*(int*)(p + kCostFieldOff[ch]) != val) writeMem(p + kCostFieldOff[ch], &val, 4);
            break;
        }
    }
}

// ---- Loose-file (HDD) loading: make the engine read a loose file in nativeDX10\ over
// the .arc copy. LoadOne at RVA 0x6BC812 does `cmpb $0,0xC03E(%esi); je; or $0x4,%eax`
// -- the OR sets the loose-bit, gated by the (off-by-default) mForceHDDCache byte. We
// NOP the je so the bit is always set (what 4Hook does). Signature-checked, reversible
// (restore 74 0A), pure fallback (no loose file -> the .arc still loads).
static const uint32_t kHddCmpRVA   = 0x6BC812;                          // cmpb $0,0xC03E(%esi)
static const uint8_t  kHddCmpSig[7] = {0x80,0xBE,0x3E,0xC0,0x00,0x00,0x00};
static const uint32_t kHddJeRVA    = 0x6BC819;                          // the je right after
static const uint8_t  kHddJeOrig[2] = {0x74,0x0A};
static const uint8_t  kHddJeNop [2] = {0x90,0x90};
static void hddCacheTick() {
    if (!g_base) return;
    uintptr_t cmp = g_base + kHddCmpRVA;
    if (!inModule(cmp, 9) || memcmp((void*)cmp, kHddCmpSig, 7) != 0) {  // wrong build -> never touch code
        g_looseArmed = false; return;
    }
    uintptr_t je = g_base + kHddJeRVA;
    uint8_t* p = (uint8_t*)je;
    bool isNop = (p[0] == 0x90 && p[1] == 0x90);
    bool isJe  = (p[0] == 0x74 && p[1] == 0x0A);
    if (!isNop && !isJe) { g_looseArmed = false; return; }              // unexpected -> leave alone
    if (g_looseLoad && !isNop) { if (writeBytes(je, kHddJeNop,  2)) logf("[hdd] loose-file loading ON (je NOPed)"); }
    if (!g_looseLoad && !isJe) { if (writeBytes(je, kHddJeOrig, 2)) logf("[hdd] loose-file loading OFF (je restored)"); }
    g_looseArmed = g_looseLoad;
}

// ---- "Hold" registry: any per-character value you set gets re-applied every
// frame so it STAYS (the game can't reset/decay it). Only values you actually
// change are held; gated by g_holdVals. ----
struct Held { uint32_t o[4]; int n; int bytes; bool isF; float f; int i; };
static std::vector<Held> g_held;
static bool g_holdVals = true;
static void holdSet(const uint32_t* offs, int n, int bytes, bool isF, float f, int i) {
    for (auto& h : g_held)
        if (h.n == n && memcmp(h.o, offs, (size_t)n * sizeof(uint32_t)) == 0) {
            h.bytes = bytes; h.isF = isF; h.f = f; h.i = i; return;
        }
    Held h{}; for (int k = 0; k < n && k < 4; ++k) h.o[k] = offs[k];
    h.n = n; h.bytes = bytes; h.isF = isF; h.f = f; h.i = i;
    g_held.push_back(h);
}
static void holdClearAll() { g_held.clear(); }
static void holdClear(const uint32_t* offs, int n) {
    for (size_t k = 0; k < g_held.size(); ++k)
        if (g_held[k].n == n && memcmp(g_held[k].o, offs, (size_t)n * sizeof(uint32_t)) == 0) {
            g_held.erase(g_held.begin() + k); return;
        }
}
// Stop pinning a Majin chain and reset it to 0 (the "off" for an instant-max).
static void unpinMajin(const uint32_t* offs, int n, int bytes, bool isF) {
    holdClear(offs, n);
    uintptr_t a; if (majinChain(offs, n, a)) { if (isF) { float z=0.0f; writeMem(a,&z,4); } else { int z=0; writeMem(a,&z,bytes); } }
}
// Write a value to a Majin pointer chain and pin it (instant-max helpers).
static void pinMajinFloat(const uint32_t* offs, int n, float v) {
    uintptr_t a; if (majinChain(offs, n, a)) { writeMem(a, &v, 4); }
    holdSet(offs, n, 4, true, v, 0);
}
static void pinMajinInt(const uint32_t* offs, int n, int bytes, int v) {
    uintptr_t a; if (majinChain(offs, n, a)) { writeMem(a, &v, bytes); }
    holdSet(offs, n, bytes, false, 0, v);
}
static void applyHeld() {
    if (!g_holdVals || g_held.empty()) return;
    // Only pin while actually in active gameplay -- writing these during menus,
    // loading or transitions hit half-built objects and crashed the game.
    uintptr_t sp; if (!readPtr(g_base + 0xF59F18, sp) || !sp) return;
    for (auto& h : g_held) {
        uintptr_t a; if (!majinChain(h.o, h.n, a)) continue;
        if (h.isF) writeMem(a, &h.f, 4);
        else       writeMem(a, &h.i, h.bytes);
    }
}

// ---- value-driven UI rows (sliders are right-clickable to type a number) ----
// The control is ALWAYS drawn so the layout is identical at launch; it just
// greys out (disabled) until the value is reachable in a level as that character.
// Setting a value registers it with the hold registry so it stays put.
static void mjFloat(const char* label, const uint32_t* offs, int n, float lo, float hi) {
    uintptr_t a; bool ok = majinChain(offs, n, a);
    float v = ok ? *(float*)a : 0.0f;
    ImGui::BeginDisabled(!ok);
    ImGui::SetNextItemWidth(150);
    if (sliderRC(label, &v, lo, hi, "%.2f") && ok) { writeMem(a, &v, 4); holdSet(offs, n, 4, true, v, 0); }
    ImGui::EndDisabled();
}
static void mjInt(const char* label, const uint32_t* offs, int n, int lo, int hi, int bytes) {
    uintptr_t a; bool ok = majinChain(offs, n, a);
    int v = 0; if (ok) memcpy(&v, (void*)a, bytes);
    ImGui::BeginDisabled(!ok);
    ImGui::SetNextItemWidth(150);
    if (ImGui::SliderInt(label, &v, lo, hi) && ok) { writeMem(a, &v, bytes); holdSet(offs, n, bytes, false, 0, v); }
    ImGui::EndDisabled();
}
static void mjToggle(const char* label, const uint32_t* offs, int n, int bytes) {
    uintptr_t a; bool ok = majinChain(offs, n, a);
    int v = 0; if (ok) memcpy(&v, (void*)a, bytes); bool on = v != 0;
    ImGui::BeginDisabled(!ok);
    if (ImGui::Checkbox(label, &on) && ok) { int nv = on ? 1 : 0; writeMem(a, &nv, bytes); holdSet(offs, n, bytes, false, 0, nv); }
    ImGui::EndDisabled();
}

// Render an existing byte-patch cheat's checkbox (same behaviour as Section 2)
// wherever we want it -- lets us surface working cheats under a character group.
static void drawCheatByName(const char* name) {
    for (auto& c : g_cheats) {
        if (c.name != name) continue;
        ImGui::PushID((void*)&c);
        bool v = c.active;
        if (ImGui::Checkbox(c.name.c_str(), &v)) {
            if (v) { if (enableCheat(c)) c.active = true; } else disableCheat(c);
        }
        ImGui::PopID();
        return;
    }
    ImGui::TextDisabled("%s  (not found)", name);
}

// Toggle a generated cheat by name. Used so "Instant Trigger" can auto-engage
// the two DT byte patches the spam actually needs (no-cooldown only works when
// the gauge gate + activation cost are removed too).
static void setCheatByName(const char* name, bool on) {
    for (auto& c : g_cheats) {
        if (c.name != name) continue;
        if (on && !c.active)      { if (enableCheat(c)) c.active = true; }
        else if (!on && c.active) { disableCheat(c); }
        return;
    }
}

// Per-frame pins: keep a counter at 0 so the move never runs out.
static bool g_infTrickTp = false;     // Dante - Trickster teleport
static bool g_infAirCalibur = false;  // Nero  - Air Calibur
static bool g_dtSpam = false;          // Instant Trigger: pin the DT "able" flag on
static bool g_vergilJDC = false;       // Vergil - pin Yamato "Perfect Execute" flag (instant perfect JDC)
// (g_infConc removed -- Vergil concentration hack dropped by request)
static bool g_infBeowulf = false;      // Vergil - pin Beowulf charge ONLY while Beowulf equipped (checkbox toggle)
static bool g_infGilgamesh = false;    // Dante - pin Gilgamesh charge to max (checkbox toggle)
static bool g_disableDTStinger = false; // Dante - DT Stinger behaves like normal Stinger Lv2 (MistressDMC port)
static bool g_infLightningKick = false;// Trish - pin Lighting Kick charge to max (checkbox toggle)
static uint32_t g_btnMask = 0;         // live read of the button bitmask (Majin+0x192C) for macro mapping
// ---- Macro / autofire: 4 slots, each spams a chosen button bit ----
struct MacroBtn { const char* name; uint32_t bit; };
static const MacroBtn kMacroBtns[] = {
    {"(none)",0},{"Y / Triangle",0x1},{"RT / R2",0x2},{"A / Cross",0x4},{"RB / R1",0x10},
    {"X / Square",0x40},{"LB / L1",0x80},{"LT / L2",0x100},{"B / Circle",0x200},
};
static const int kNMacroBtns = (int)(sizeof(kMacroBtns)/sizeof(kMacroBtns[0]));
static const char* kMacroCombo = "(none)\0Y / Triangle\0RT / R2\0A / Cross\0RB / R1\0X / Square\0LB / L1\0LT / L2\0B / Circle\0";
static int  g_macroSel[4] = {0,0,0,0};      // index into kMacroBtns
static bool g_macroOn[4]  = {false,false,false,false};
static unsigned g_macroFrame = 0;
// Safely pin a small "uses-left" counter to 0. The crash was writing 0 into a
// field that, mid-transition / on the wrong character, holds a POINTER (the game
// then deref'd it as null). So: only in active gameplay, on a fully-resolved live
// player object, and only when the field currently looks like a small counter
// (<= 0x100) -- never a pointer-sized value.
static void pinCounterZero(const uint32_t* o, int n) {
    uintptr_t sp, mj;
    if (!(readPtr(g_base + 0xF59F18, sp) && sp && majinRoot(mj) && readPtr(mj, mj) && mj)) return;
    uintptr_t a;
    if (!majinChain(o, n, a) || !memReadable((void*)a, 4)) return;
    uint32_t cur = *(volatile uint32_t*)a;
    if (cur > 0 && cur <= 0x100) { int z = 0; writeMem(a, &z, 4); }
}
// Current-character ID off the live player object (Majin+0x19AC):
// 0=Dante 1=Nero 2=Vergil 3=Trish 4=Lady. Returns false if not reachable.
static bool currentCharId(int& id) {
    static const uint32_t o[] = {0x19AC};
    uintptr_t a;
    if (!majinChain(o, 1, a) || !memReadable((void*)a, 4)) return false;
    id = *(volatile int*)a;
    return true;
}
// ===== DEBUG diagnostics (kept) ==============================================
// Live move-state logger -> overlay.log. Toggle in the DEBUG tab. Logs, on every action
// change, the active actor's mActionNo (+0x1A00), mAtckId (+0x1A74, the movenames.cfg id),
// world Y (+0x44), a velocity candidate (+0x70), DT meter (+0x2504), and a byte window
// (+0x1A80..0x1AB0). General-purpose: used for RE'ing move ids, gates, grounded flags, etc.
static bool g_diagOn = false;    // off by default; turn on in DEBUG tab when investigating
static void diagTick() {
    if (!g_diagOn) return;
    char* a = (char*)activePlayer();
    if (!a || !memReadable(a, 0x2600)) return;
    static uint32_t lastAct = 0xFFFFFFFF;
    uint32_t act  = *(volatile uint32_t*)(a + 0x1A00);
    uint32_t atck = *(volatile uint32_t*)(a + 0x1A74);
    static unsigned fr = 0; fr++;
    if (act != lastAct || (fr % 20) == 0) {
        lastAct = act;
        float dt = *(volatile float*)(a + 0x2504);
        float py = *(volatile float*)(a + 0x44);
        float vy = *(volatile float*)(a + 0x70);
        char gb[160]; int gp = 0;
        for (int off = 0x1A80; off <= 0x1AB0 && gp < 140; off += 1)
            gp += snprintf(gb+gp, sizeof(gb)-gp, "%02X", *(volatile uint8_t*)(a + off));
        logf("[mv] act=0x%X atck=%u Y=%.1f vy=%.2f DT=%.0f  1A80:%s", act, atck, py, vy, dt, gb);
    }
}
// Horizontal "across the whole map" snatch was NOT solved -- the cap is the soft/auto
// target acquisition range (nearest enemy within ~120u; snatch ignores the hard-lock
// target, which reaches 2346u). Fixing it needs a code patch at the acquisition's range
// check, but that instruction can only be found with breakpoints, which CrossOver's
// x86->ARM translator blocks (so CE's "find what accesses" never fires). Per-frame data
// writes to force it either race the game thread (enemy+0xD0C distance) or freeze it
// (actor+0x4C4 list node). Left for a real-Windows session where CE breakpoints work.
// ===== MistressDMC: Random Mutator Mode (timed weighted-random gameplay mutators) =====
static void hint(const char* tip);   // fwd (defined with the GUI helpers below)

#include "coop.h"   // MistressDMC: local 2-player co-op (1.4)
static bool setRoomSpeed(float v) {
    uintptr_t base; if (!readPtr(g_base + 0xF59F18, base) || !base) return false;
    return writeMem(base + 0x34, &v, 4);
}
struct MoodMutator {
    std::string name, desc; bool enabled = true; int weight = 6; float duration = 20.0f;
    std::function<void()> onStart = []{}; std::function<void()> onEnd = []{}; std::function<void()> onFrame = []{};
    bool active = false; float timeLeft = 0.0f;
};
static std::vector<MoodMutator> g_muts;
static bool  g_mutRunning = false;
static int   g_mutModTime = 20, g_mutCoolTime = 12;
static float g_mutCool = 0.0f;
static int   g_mutActiveIdx = -1;
static bool mutInGameplay() { uintptr_t sp; return readPtr(g_base + 0xF59F18, sp) && sp; }
static float g_shPrev[3] = {0,0,0}; static bool g_shHave = false; static float g_shSmooth = 0.05f;
static void superhotFrame() {
    uintptr_t mj;
    if (!majinRoot(mj) || !readPtr(mj, mj) || !mj) { g_shHave = false; return; }
    if (!memReadable((void*)(mj + 0x48), 4)) { g_shHave = false; return; }
    float x = *(float*)(mj + 0x40), y = *(float*)(mj + 0x44), z = *(float*)(mj + 0x48);
    if (!g_shHave) { g_shPrev[0]=x; g_shPrev[1]=y; g_shPrev[2]=z; g_shHave=true; }
    float dx=x-g_shPrev[0], dy=y-g_shPrev[1], dz=z-g_shPrev[2];
    g_shPrev[0]=x; g_shPrev[1]=y; g_shPrev[2]=z;
    float speed = sqrtf(dx*dx+dy*dy+dz*dz);
    float t = speed * 1.4f; if (t > 1.0f) t = 1.0f;
    float target = t*t*(3.0f-2.0f*t);
    static float prev = 0.0f;
    float val = (target * g_shSmooth) + (1.0f - g_shSmooth) * prev;
    prev = val; if (val < 0.02f) val = 0.02f;
    setWorkRate(3, val); setRoomSpeed(val);
}
static bool g_dvdOn = false; static float g_dvdPos[2] = {300,300}; static float g_dvdVel[2] = {120,120}; static int g_dvdHue = 0;
static void dvdFrame() {
    if (!g_dvdOn) return;
    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime; if (dt <= 0.0f || dt > 0.1f) dt = 1.0f/60.0f;
    const ImVec2 sz(190.0f, 90.0f);
    g_dvdPos[0] += g_dvdVel[0]*dt; g_dvdPos[1] += g_dvdVel[1]*dt;
    bool hit=false;
    if (g_dvdPos[0] <= 0) { g_dvdPos[0]=0; g_dvdVel[0]= fabsf(g_dvdVel[0]); hit=true; }
    if (g_dvdPos[0]+sz.x >= io.DisplaySize.x){ g_dvdPos[0]=io.DisplaySize.x-sz.x; g_dvdVel[0]=-fabsf(g_dvdVel[0]); hit=true; }
    if (g_dvdPos[1] <= 0) { g_dvdPos[1]=0; g_dvdVel[1]= fabsf(g_dvdVel[1]); hit=true; }
    if (g_dvdPos[1]+sz.y >= io.DisplaySize.y){ g_dvdPos[1]=io.DisplaySize.y-sz.y; g_dvdVel[1]=-fabsf(g_dvdVel[1]); hit=true; }
    if (hit) g_dvdHue = (g_dvdHue + 47) % 360;
    ImColor col = ImColor::HSV(g_dvdHue/360.0f, 1.0f, 1.0f, 1.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(g_dvdPos[0],g_dvdPos[1]), ImVec2(g_dvdPos[0]+sz.x,g_dvdPos[1]+sz.y), IM_COL32(0,0,0,200), 6.0f);
    dl->AddRect(ImVec2(g_dvdPos[0],g_dvdPos[1]), ImVec2(g_dvdPos[0]+sz.x,g_dvdPos[1]+sz.y), (ImU32)col, 6.0f, 0, 2.0f);
    dl->AddText(ImVec2(g_dvdPos[0]+14, g_dvdPos[1]+34), (ImU32)col, "MistressDMC");
}
static void slowmoStart() { setWorkRate(0, 0.35f); }
static void slowmoEnd()   { setWorkRate(0, 1.0f);  }
static void mutatorsInit() {
    if (!g_muts.empty()) return;
    MoodMutator sh; sh.name="SUPERHOT"; sh.desc="Time only moves when you move"; sh.weight=6; sh.duration=20.0f;
    sh.onStart=[]{ g_shHave=false; }; sh.onFrame=[]{ superhotFrame(); }; sh.onEnd=[]{ setWorkRate(3,1.0f); setRoomSpeed(1.0f); };
    g_muts.push_back(sh);
    MoodMutator sm; sm.name="Slow-Mo Burst"; sm.desc="Bullet-time for the duration"; sm.weight=6; sm.duration=12.0f;
    sm.onStart=[]{ slowmoStart(); }; sm.onEnd=[]{ slowmoEnd(); };
    g_muts.push_back(sm);
}
static int mutatorPick() {
    int total = 0;
    for (auto& m : g_muts) if (m.enabled && m.weight > 0) total += m.weight;
    if (total <= 0) return -1;
    int r = rand() % total, acc = 0;
    for (int i = 0; i < (int)g_muts.size(); ++i) {
        if (!g_muts[i].enabled || g_muts[i].weight <= 0) continue;
        acc += g_muts[i].weight; if (r < acc) return i;
    }
    return -1;
}
static void mutatorStop() {
    if (g_mutActiveIdx >= 0) { g_muts[g_mutActiveIdx].onEnd(); g_muts[g_mutActiveIdx].active=false; }
    g_mutActiveIdx = -1; g_mutCool = 0.0f;
    setWorkRate(0,1.0f); setWorkRate(3,1.0f); setRoomSpeed(1.0f); g_dvdOn=false;
}
static void mutatorTick() {
    mutatorsInit();
    if (!g_mutRunning) return;
    float dt = ImGui::GetIO().DeltaTime; if (dt <= 0.0f || dt > 0.1f) dt = 1.0f/60.0f;
    if (!mutInGameplay()) return;
    if (g_mutActiveIdx >= 0) {
        MoodMutator& m = g_muts[g_mutActiveIdx];
        m.onFrame(); m.timeLeft -= dt;
        if (m.timeLeft <= 0.0f) { m.onEnd(); m.active=false; g_mutActiveIdx=-1; g_mutCool=(float)g_mutCoolTime; }
    } else {
        g_mutCool -= dt;
        if (g_mutCool <= 0.0f) {
            int idx = mutatorPick();
            if (idx >= 0) { g_mutActiveIdx = idx; g_muts[idx].active = true; g_muts[idx].timeLeft = (float)g_mutModTime;
                            g_muts[idx].onStart(); logf("[mutator] activating %s", g_muts[idx].name.c_str()); }
            else g_mutCool = 1.0f;
        }
    }
}
static void mutatorDrawTab() {
    mutatorsInit();
    ImGui::TextWrapped("Random timed gameplay mutators. Set the timers, enable the pool, then Start. [experimental]");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(150);
    if (ImGui::InputInt("Mod timer (seconds)", &g_mutModTime, 1, 5)) { if (g_mutModTime < 3) g_mutModTime = 3; }
    ImGui::SameLine(); hint("How long each random mutator stays active.");
    ImGui::SetNextItemWidth(150);
    if (ImGui::InputInt("Cooldown timer (seconds)", &g_mutCoolTime, 1, 5)) { if (g_mutCoolTime < 0) g_mutCoolTime = 0; }
    ImGui::SameLine(); hint("Gap between mutators.");
    ImGui::Spacing();
    if (!g_mutRunning) { if (ImGui::Button("Start Random Mutator Mode")) { g_mutRunning = true; g_mutCool = 0.0f; } }
    else {
        if (ImGui::Button("Stop Random Mutator Mode")) { g_mutRunning = false; mutatorStop(); }
        ImGui::SameLine();
        if (g_mutActiveIdx >= 0) ImGui::Text("Active: %s  (%.1fs)", g_muts[g_mutActiveIdx].name.c_str(), g_muts[g_mutActiveIdx].timeLeft);
        else                     ImGui::Text("Next mutator in %.1fs", g_mutCool);
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Mutator Pool", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("mutpool", 3, ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Enabled"); ImGui::TableSetupColumn("Weight");
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)g_muts.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s", g_muts[i].name.c_str());
                if (ImGui::IsItemHovered() && !g_muts[i].desc.empty()) ImGui::SetTooltip("%s", g_muts[i].desc.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::PushID(i); ImGui::Checkbox("##en", &g_muts[i].enabled);
                ImGui::TableSetColumnIndex(2); ImGui::SetNextItemWidth(120);
                ImGui::DragInt("##w", &g_muts[i].weight, 1, 0, 100, "%d", ImGuiSliderFlags_AlwaysClamp);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("SUPERHOT tuning")) {
        ImGui::SetNextItemWidth(150);
        ImGui::SliderFloat("Velocity smoothing", &g_shSmooth, 0.01f, 0.5f, "%.2f");
        hint("Lower = snappier reaction to your movement.");
    }
}

// ===== MistressDMC: Custom Camera Variables (additive on top of MOOD camera) =====
static bool  g_camVarsOn   = false;
static float g_cvHeight=0, g_cvDistance=0, g_cvDistLock=0, g_cvAngle=0, g_cvAngleLock=0, g_cvFov=0, g_cvFovBattle=0;
static void applyCamVars() {
    if (!g_camVarsOn) return;
    uint8_t* cd = getCameraData();
    if (!cd) return;
    if (g_cvHeight   != 0.0f) *(float*)(cd + 0xD0) += g_cvHeight;
    if (g_cvDistance != 0.0f) *(float*)(cd + 0xD8) += g_cvDistance;
    if (g_cvDistLock != 0.0f) *(float*)(cd + 0xDC) += g_cvDistLock;
    if (g_cvAngle    != 0.0f) *(float*)(cd + 0xD4) += g_cvAngle;
    if (g_cvFov       != 0.0f) *(float*)(cd + 0xE4) += g_cvFov;
    if (g_cvFovBattle != 0.0f) *(float*)(cd + 0xE8) += g_cvFovBattle;
}

// ===== MistressDMC: Selective Cancels (Dante) — make chosen moves cancellable =====
enum SelCancelBit {
    SC_ECSTASY=0x1, SC_ARGUMENT=0x2, SC_KICK13=0x4, SC_SLASH_DIM=0x8, SC_PROP=0x10,
    SC_SHOCK=0x20, SC_OMEN=0x40, SC_GUNSTINGER=0x80, SC_EPIDEMIC=0x100, SC_DT_PIN_UP=0x200,
    SC_DRAW=0x800, SC_ROLL=0x2000, SC_STINGER=0x4000, SC_REAL_IMPACT=0x8000, SC_FLUSH=0x10000,
};
static bool     g_scEnableDante = false;
static uint32_t g_scCancels     = 0;
static const int kSC_Melee = 0x3874, kSC_Style = 0x38F8, kSC_Jump = 0x3924;
static inline void scWrite(char* a, int slot, int v) { *(volatile int*)(a + slot) = v; }
static void applySelectiveCancels() {
    if (!g_scEnableDante || g_scCancels == 0) return;
    char* a = (char*)activePlayer();
    if (!a || !memReadable(a, 0x3A00)) return;
    if (*(uint8_t*)(a + 0x19AC) != 0) return;          // Dante only
    uint32_t mv = *(volatile uint32_t*)(a + 0x1A00);   // mActionNo
    #define SC_USUAL(bit) do { if (g_scCancels & (bit)) scWrite(a, kSC_Jump, 2); } while(0)
    switch (mv) {
        case 0x007: case 0x008: SC_USUAL(SC_ROLL); break;
        case 0x411: case 0x412: SC_USUAL(SC_ECSTASY); break;
        case 0x732: SC_USUAL(SC_ARGUMENT); break;
        case 0x900: SC_USUAL(SC_SLASH_DIM); break;
        case 0x232: SC_USUAL(SC_PROP); break;
        case 0x735: SC_USUAL(SC_OMEN); break;
        case 0x635: SC_USUAL(SC_GUNSTINGER); break;
        case 0x706: SC_USUAL(SC_EPIDEMIC); break;
        case 0x410: SC_USUAL(SC_DT_PIN_UP); break;
        case 0x310: SC_USUAL(SC_DRAW); break;
        case 0x20F: SC_USUAL(SC_STINGER); break;
        case 0x335: SC_USUAL(SC_REAL_IMPACT); break;
        case 0x30E: case 0x30F: SC_USUAL(SC_KICK13); break;
        case 0x333: SC_USUAL(SC_SHOCK); break;
        case 0x31E: case 0x31F: case 0x320:
            if (g_scCancels & SC_FLUSH) { scWrite(a, kSC_Melee, 2); scWrite(a, kSC_Style, 2); scWrite(a, kSC_Jump, 2); }
            break;
        default: break;
    }
    #undef SC_USUAL
}

// ===== MistressDMC: Easy Quick Drive — Prop cancellable into Quick Drive (~frame 7) =====
static bool  g_easyQuickDrive = false;
static float g_eqdFrameMax    = 7.0f;
static void applyEasyQuickDrive() {
    if (!g_easyQuickDrive) return;
    char* a = (char*)activePlayer();
    if (!a || !memReadable(a, 0x3A00)) return;
    if (*(uint8_t*)(a + 0x19AC) != 0) return;                 // Dante only
    uint32_t action = *(uint32_t*)(a + 0x1A00);
    uint32_t atck   = *(uint32_t*)(a + 0x1A74);
    uint32_t aen    = *(uint32_t*)(a + 0x1A78);
    if (action != 0xC || atck != 16 || aen == 0) return;      // not Prop
    if (*(float*)(a + 0x52C) >= g_eqdFrameMax) return;        // past early-frame window
    static const int slot[8] = {0x3874,0x38A0,0x38CC,0x38F8,0x3924,0x3950,0x397C,0x39A8};
    for (int s : slot) *(volatile int*)(a + s) = 2;
}

static void updateMajinPins() {
    int z = 0; (void)z;
    applyFpsLimit();        // pin the FPS cap (game restores it)
    applySuperCancel();     // pin cancel-table slots while Super Cancel is on
    applyEasyQuickDrive();  // MistressDMC: open Prop->Quick Drive window
    applySelectiveCancels(); // MistressDMC: pin selected move cancel slots
    // Infinite Concentration: pin Vergil's concentration float (activePlayer()+0x7B58) to Lv2
    // (200.0) so Judgement Cut End is always usable WITHOUT the super-speed a maxed value gives.
    if (g_infConc) {
        int cc = -1;
        if (currentCharId(cc) && cc == 2) {                 // Vergil only
            char* p = (char*)activePlayer();
            if (p && memReadable(p + 0x7B58, 4)) *(volatile float*)(p + 0x7B58) = 300.0f;  // Lv3
        }
    }
    // Doppelganger soft-collision: the clone has no body collision, so you can stand inside it
    // and "merge". Nudge it apart each frame if you overlap (position writes only -> safe, same
    // as the spawn does). Keeps min separation; outside that range the clone is left alone.
    if (g_doppNoMerge && g_doppActor) {
        char* d  = (char*)g_doppActor;
        char* pl = (char*)activePlayer();
        if (pl && d != pl && memReadable(d + OFF_POS_X, 12) && memReadable(pl + OFF_POS_X, 12)) {
            float dx = *(float*)(d+OFF_POS_X) - *(float*)(pl+OFF_POS_X);
            float dz = *(float*)(d+OFF_POS_Z) - *(float*)(pl+OFF_POS_Z);
            float d2 = dx*dx + dz*dz;
            const float SEP = 55.0f;
            if (d2 < SEP*SEP) {
                float dist = sqrtf(d2);
                if (dist < 0.01f) { dx = 1.0f; dz = 0.0f; dist = 1.0f; }   // exact overlap -> push +X
                *(float*)(d+OFF_POS_X) = *(float*)(pl+OFF_POS_X) + (dx/dist)*SEP;
                *(float*)(d+OFF_POS_Z) = *(float*)(pl+OFF_POS_Z) + (dz/dist)*SEP;
            }
        }
    }
    // ---- Triple Trouble caves: install the hook ONLY while in active gameplay as
    // the matching character, and tear it down otherwise. The hook can never be
    // live on the wrong character (which is what crashed it). apply/stop are
    // idempotent, so these only act on the actual install/remove transitions.
    {
        uintptr_t sp; bool live = readPtr(g_base + 0xF59F18, sp) && sp;
        int cid = -1; if (live) currentCharId(cid);
        // Charge Rate Increase: universal -> any character, in gameplay.
        if (g_crWant && live) { if (!g_crOn && applyChargeRate()) setChargeRate(g_crRate); }
        else if (g_crOn) stopChargeRate();
        // Lady Jump Cancel: install the CAVE (game-thread hook at exe+0xA1B18), exactly
        // like Revamped's shipping cheat -- it self-gates by Lady's gun-move IDs so it's
        // safe to run globally and needs no character check. (The per-frame force I tried
        // before raced the game thread and never stuck.)
        if (g_ljWant && live) { if (!g_ljOn) applyLadyJC(); }
        else if (g_ljOn) stopLadyJC();
        // Height restriction bypass: Vergil/Trish/Lady (2/3/4).
        if (g_hrWant && live && (cid == 2 || cid == 3 || cid == 4)) { if (!g_hrOn) applyHeightBypass(); }
        else if (g_hrOn) stopHeightBypass();
    }
    g_easyStepFlag = g_easyStepWant;                       // MistressDMC: Easy Enemy Step
    if (g_easyStepWant) { if (!g_esOn) applyEasyStep(); } else if (g_esOn) stopEasyStep();
    if (g_noHelmBreaker || g_noHelmSplit) { if (!g_helmHookOn) applyHelmHook(); }  // No Helmbreaker Knockdown
    else if (g_helmHookOn) stopHelmHook();
    if (g_infTrickTp)    { static const uint32_t o[] = {0x174C4};          pinCounterZero(o, 1); }
    if (g_infAirCalibur) { static const uint32_t o[] = {0xCC1C,0xA4,0x4C4}; pinCounterZero(o, 3); }
    // (Infinite Concentration removed by request.)
    if (g_infBeowulf) {   // Vergil: keep Beowulf fully charged. Weapon-gated so it
        // only writes the charge (Majin+0x7B7C) while Beowulf is equipped (id 0xB
        // at Majin+0x2398) -- the adjacent Force-Edge sword state is never touched.
        uintptr_t sp, mj;
        if (readPtr(g_base + 0xF59F18, sp) && sp && majinRoot(mj) && readPtr(mj, mj) && mj) {
            uintptr_t aw;
            static const uint32_t ow[] = {0x2398};   // equipped-weapon id
            if (majinChain(ow, 1, aw) && memReadable((void*)aw, 4) && *(volatile uint32_t*)aw == 0xB) {
                uintptr_t a;
                static const uint32_t oc[] = {0x7B7C};   // Beowulf Charge Value (float)
                if (majinChain(oc, 1, a) && memReadable((void*)a, 4)) { float v = 1000.0f; writeMem(a, &v, 4); }
            }
        }
    }
    if (g_infGilgamesh) {   // Dante (char 0): pin Gilgamesh charge to max. Gated to
        // Dante specifically + a sane-range value guard, so it can never write 1000
        // over a pointer field on the wrong character (which crashed the game).
        uintptr_t sp, mj; int cid;
        if (readPtr(g_base + 0xF59F18, sp) && sp && majinRoot(mj) && readPtr(mj, mj) && mj
            && currentCharId(cid) && cid == 0) {
            static const uint32_t o[] = {0x152AC,0xA4,0x4C4};
            uintptr_t a;
            if (majinChain(o, 3, a) && memReadable((void*)a, 4)) {
                float cur = *(volatile float*)a;
                if (cur >= 0.0f && cur <= 100000.0f) { float v = 1000.0f; writeMem(a, &v, 4); }
            }
        }
    }
    if (g_infLightningKick) {   // Trish (char 3): pin Lighting Kick charge to max (same gate).
        uintptr_t sp, mj; int cid;
        if (readPtr(g_base + 0xF59F18, sp) && sp && majinRoot(mj) && readPtr(mj, mj) && mj
            && currentCharId(cid) && cid == 3) {
            static const uint32_t o[] = {0x77DC,0xA4,0x4C4};
            uintptr_t a;
            if (majinChain(o, 3, a) && memReadable((void*)a, 4)) {
                float cur = *(volatile float*)a;
                if (cur >= 0.0f && cur <= 100000.0f) { float v = 1000.0f; writeMem(a, &v, 4); }
            }
        }
    }
    if (g_dtSpam) {   // keep "Devil Trigger Able Flag" = 1 -> activate DT anytime, no cooldown
        // Only force the flag during active, controllable gameplay. Writing it in
        // menus / loading / cutscene transitions (or for a character with no DT
        // object) let the game run DT activation on a half-built / null object and
        // crashed with a null vtable call. The in-level pointer + a live player
        // object + a sane current flag byte (0/1) gate that out.
        uintptr_t sp, mj;
        if (readPtr(g_base + 0xF59F18, sp) && sp && majinRoot(mj) && readPtr(mj, mj) && mj) {
            static const uint32_t o[] = {0x199D, 0x194};
            uintptr_t a;
            if (ceChain(g_base + 0xF240A4, o, 2, a)) {
                uint8_t cur = 0xFF;
                if (memReadable((void*)a, 1)) cur = *(volatile uint8_t*)a;
                if (cur <= 1) { uint8_t one = 1; writeMem(a, &one, 1); }  // only a sane flag byte
            }
        }
    }
    if (g_disableDTStinger) {   // Dante in DT: rewrite DT-Stinger atck id (0xA) -> Stinger Lv2 (0x9)
        char* pl = (char*)activePlayer();
        if (pl && memReadable(pl, 0x2800)) {
            int      ch  = (int)*(uint8_t*)(pl + 0x19AC);   // char id (0 = Dante)
            uint32_t aen = *(uint32_t*)(pl + 0x1A78);       // mAtckId_Enable
            uint8_t  dt8 = *(uint8_t*)(pl + 0x2773);        // in DT
            if (ch == 0 && dt8 && aen != 0) {
                uint32_t atck = *(uint32_t*)(pl + 0x1A74);  // mAtckId
                if (atck == 0xA) { uint32_t v = 0x9; writeMem((uintptr_t)(pl + 0x1A74), &v, 4); }
            }
        }
    }
    if (g_vergilJDC) {   // pin Yamato "Perfect Execute" byte (Majin+0x7A9B) = 1
        // Same gameplay/live-object gate as DT spam: only write a sane flag byte
        // (0/1) on a fully-resolved player object so a half-built / non-Vergil
        // actor never takes a forced perfect-execute on a null Yamato charge.
        uintptr_t sp, mj;
        if (readPtr(g_base + 0xF59F18, sp) && sp && majinRoot(mj) && readPtr(mj, mj) && mj) {
            static const uint32_t o[] = {0x7A9B};
            uintptr_t a;
            if (majinChain(o, 1, a)) {
                uint8_t cur = 0xFF;
                if (memReadable((void*)a, 1)) cur = *(volatile uint8_t*)a;
                if (cur <= 1) { uint8_t one = 1; writeMem(a, &one, 1); }
            }
        }
    }
    applyHeld();   // keep every value the user set pinned so it stays
    updateHud();   // HUD element editor: capture tree + hide selected/locked widgets
}

// Freeze the game (global work rate 0) while the menu is open, restoring the
// previous rate on close. Opt-in; no-ops out of a level.
static bool  g_paused = false;
static float g_pauseSavedWR = 1.0f;
static void updatePause() {
    bool want = g_pauseOnOpen && g_show;
    if (want && !g_paused) {
        if (!getWorkRate(0, g_pauseSavedWR)) g_pauseSavedWR = 1.0f;
        if (setWorkRate(0, 0.0f)) g_paused = true;
    } else if (!want && g_paused) {
        setWorkRate(0, g_pauseSavedWR);
        g_paused = false;
    }
}

// ===========================================================================
// Config save / load. Persists the whole menu state to a tiny text file beside
// dinput8.dll so users don't have to re-pick everything every launch. One file
// per named profile: DMC4SEMOOD_<profile>.cfg (default profile name "default").
// Level-dependent items (cheats, camera, move-speed, Lucifer/Full House) re-apply
// best-effort -- if you're not in a level yet they just no-op until you are.
// ===========================================================================
static bool g_autoLoad = false;   // auto-apply the "default" profile on launch

static void buildConfigPath(char* out, const char* profile) {
    char name[160];
    snprintf(name, sizeof(name), "DMC4SEMOOD_%s.cfg",
             (profile && *profile) ? profile : "default");
    buildAssetPath(out, name);
}

static bool saveConfig(const char* profile) {
    char path[MAX_PATH]; buildConfigPath(path, profile);
    FILE* f = fopen(path, "w");
    if (!f) { logf("[cfg] save failed: %s", path); return false; }
    fprintf(f, "# DMC4SEMOOD config\n");
    fprintf(f, "autoload=%d\n", g_autoLoad ? 1 : 0);
    fprintf(f, "hud=%d\n",      g_hudHidden ? 1 : 0);
    { uint32_t pieces = 0; for (int i = 0; i < kNHudPieces; ++i) if (g_hudPieces[i].on) pieces |= (1u << i);
      fprintf(f, "hudPieces=%X\n", pieces); }
    fprintf(f, "hudLockVtbl=%X\n", g_hudLockVtbl);
    fprintf(f, "hideHp=%d\n", g_hudHpOn ? 1 : 0);
    fprintf(f, "music=%d\n",    g_music ? 1 : 0);
    fprintf(f, "musicVol=%.4f\n", g_musicVol);
    fprintf(f, "track=%d\n",    g_curTrack);
    fprintf(f, "loopMode=%d\n", g_loopMode);
    fprintf(f, "bgMusic=%d\n",  g_bgMusic ? 1 : 0);
    fprintf(f, "shuffle=%d\n",  g_shuffle ? 1 : 0);
    fprintf(f, "dof=%d\n",      g_noDOF ? 1 : 0);
    fprintf(f, "mb=%d\n",       g_noMotionBlur ? 1 : 0);
    fprintf(f, "godrays=%d\n",  g_noGodRays ? 1 : 0);
    fprintf(f, "camOn=%d\n",    g_camOn ? 1 : 0);
    fprintf(f, "camDist=%.3f\n",   g_camDist);
    fprintf(f, "camHeight=%.3f\n", g_camHeight);
    fprintf(f, "camVarsOn=%d\n", g_camVarsOn ? 1 : 0);
    fprintf(f, "cvHeight=%.3f\n", g_cvHeight); fprintf(f, "cvDistance=%.3f\n", g_cvDistance);
    fprintf(f, "cvDistLock=%.3f\n", g_cvDistLock); fprintf(f, "cvAngle=%.3f\n", g_cvAngle);
    fprintf(f, "cvFov=%.3f\n", g_cvFov); fprintf(f, "cvFovBattle=%.3f\n", g_cvFovBattle);
    fprintf(f, "speedOn=%d\n",  g_speedOn ? 1 : 0);
    for (int k = 0; k < kNSpeed; ++k) fprintf(f, "speed%d=%.4f\n", k, g_speedVal[k]);
    fprintf(f, "luc=%d\n",      g_lucOn ? 1 : 0);
    fprintf(f, "snatch=%d\n",   g_snatchOn ? 1 : 0);
    fprintf(f, "rosePins=%d\n", g_rosePins ? 1 : 0);
    fprintf(f, "fh=%d\n",       g_fhOn ? 1 : 0);
    fprintf(f, "jc=%d\n",       g_jcOn ? 1 : 0);
    fprintf(f, "dtSpam=%d\n", g_dtSpam ? 1 : 0);
    fprintf(f, "vergilJDC=%d\n", g_vergilJDC ? 1 : 0);
    fprintf(f, "infBeowulf=%d\n", g_infBeowulf ? 1 : 0);
    fprintf(f, "infGilgamesh=%d\n", g_infGilgamesh ? 1 : 0);
    fprintf(f, "disableDTStinger=%d\n", g_disableDTStinger ? 1 : 0);
    fprintf(f, "forceLucifer=%d\n", g_forceLucifer ? 1 : 0);
    fprintf(f, "noHelmBreaker=%d\n", g_noHelmBreaker ? 1 : 0);
    fprintf(f, "noHelmSplit=%d\n", g_noHelmSplit ? 1 : 0);
    fprintf(f, "scEnable=%d\n", g_scEnableDante ? 1 : 0);
    fprintf(f, "scCancels=%u\n", g_scCancels);
    fprintf(f, "infLightningKick=%d\n", g_infLightningKick ? 1 : 0);
    fprintf(f, "infTrickTp=%d\n", g_infTrickTp ? 1 : 0);
    fprintf(f, "infAirCalibur=%d\n", g_infAirCalibur ? 1 : 0);
    fprintf(f, "airTrick=%d\n", g_atOn ? 1 : 0);
    fprintf(f, "swordTrick=%d\n", g_swOn ? 1 : 0);
    fprintf(f, "jcMult=%.4f\n", g_jcMult);
    fprintf(f, "chargeRate=%d\n",    g_crWant ? 1 : 0);
    fprintf(f, "chargeRateVal=%.4f\n", g_crRate);
    fprintf(f, "ladyJC=%d\n",        g_ljWant ? 1 : 0);
    fprintf(f, "heightBypass=%d\n",  g_hrWant ? 1 : 0);
    fprintf(f, "easyStep=%d\n", g_easyStepWant ? 1 : 0);
    fprintf(f, "skipShotgun=%d\n", g_skipShotgun);
    fprintf(f, "skipPandora=%d\n", g_skipPandora);
    fprintf(f, "skipGilgamesh=%d\n", g_skipGilgamesh);
    fprintf(f, "skipLucifer=%d\n", g_skipLucifer);
    fprintf(f, "skipYamato=%d\n", g_skipYamato);
    fprintf(f, "skipBeowulf=%d\n", g_skipBeowulf);
    fprintf(f, "skipForceEdge=%d\n", g_skipForceEdge);
    fprintf(f, "themeBlue=%d\n",     g_blue ? 1 : 0);
    fprintf(f, "zh=%d\n",            g_zh ? 1 : 0);
    fprintf(f, "showFps=%d\n",       g_showFps ? 1 : 0);
    fprintf(f, "guiScale=%.4f\n",    g_uiScale);
    fprintf(f, "guiAutoDPI=%d\n",    g_uiAutoDPI ? 1 : 0);
    fprintf(f, "skipBoot=%d\n",    g_skipBoot ? 1 : 0);
    fprintf(f, "forceFocus=%d\n",  g_forceFocus ? 1 : 0);
    fprintf(f, "borderless=%d\n",  g_borderless ? 1 : 0);
    fprintf(f, "pauseOnOpen=%d\n", g_pauseOnOpen ? 1 : 0);
    for (int h = 0; h < 16; h++) if (g_hkKey[h]) fprintf(f, "hk%d=%X\n", h, g_hkKey[h]);
    for (auto& c : g_cheats) if (c.active) fprintf(f, "cheat=%s\n", c.name.c_str());
    fclose(f);
    logf("[cfg] saved %s", path);
    return true;
}

static bool loadConfig(const char* profile) {
    char path[MAX_PATH]; buildConfigPath(path, profile);
    FILE* f = fopen(path, "r");
    if (!f) { logf("[cfg] load: not found %s", path); return false; }

    // Seed from current state so any missing key keeps its present value.
    bool  autoload = g_autoLoad, hud = g_hudHidden, music = g_music;
    uint32_t hudPieces = 0; for (int i = 0; i < kNHudPieces; ++i) if (g_hudPieces[i].on) hudPieces |= (1u << i);
    uint32_t hudLockVtbl = g_hudLockVtbl; bool hideHp = g_hudHpOn;
    bool  dof = g_noDOF, mb = g_noMotionBlur, gr = g_noGodRays;
    bool  camOn = g_camOn, speedOn = g_speedOn, luc = g_lucOn, fh = g_fhOn;
    bool  snatch = g_snatchOn, rosePins = g_rosePins != 0;
    bool  forceFocus = g_forceFocus, borderless = g_borderless, pauseOnOpen = g_pauseOnOpen;
    bool  skipBoot = g_skipBoot;
    bool  jc = g_jcOn; float jcMult = g_jcMult; bool dtSpam = g_dtSpam; bool airTrick = g_atOn; bool swordTrick = g_swOn;
    bool  vergilJDC = g_vergilJDC;
    bool  infBeowulf = g_infBeowulf;
    bool  infGilgamesh = g_infGilgamesh, infLightningKick = g_infLightningKick;
    bool  disableDTStinger = g_disableDTStinger, forceLuc = (g_forceLucifer != 0);
    bool  noHelmBreaker = g_noHelmBreaker, noHelmSplit = g_noHelmSplit;
    bool  scEnable = g_scEnableDante; unsigned scCancels = g_scCancels;
    bool  infTrickTp = g_infTrickTp, infAirCalibur = g_infAirCalibur;
    bool  crWant = g_crWant; float crRate = g_crRate; bool ljWant = g_ljWant, hrWant = g_hrWant;
    bool  easyStep = g_easyStepWant;
    bool  themeBlue = g_blue, zh = g_zh, showFps = g_showFps;
    float guiScale = g_uiScale; bool guiAutoDPI = g_uiAutoDPI;
    float musicVol = g_musicVol, camDist = g_camDist, camHeight = g_camHeight;
    int   track = g_curTrack, loopMode = g_loopMode; bool bgMusic = g_bgMusic, shuffle = g_shuffle;
    float speed[kNSpeed]; for (int k = 0; k < kNSpeed; ++k) speed[k] = g_speedVal[k];
    std::vector<std::string> wantCheats;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (!L || line[0] == '#') continue;
        char* eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; const char* key = line; const char* val = eq + 1;
        if      (!strcmp(key, "autoload")) autoload = atoi(val) != 0;
        else if (!strcmp(key, "hud"))      hud      = atoi(val) != 0;
        else if (!strcmp(key, "hudPieces")) hudPieces = (uint32_t)strtoul(val, nullptr, 16);
        else if (!strcmp(key, "hudLockVtbl")) hudLockVtbl = (uint32_t)strtoul(val, nullptr, 16);
        else if (!strcmp(key, "hideHp"))    hideHp   = atoi(val) != 0;
        else if (!strcmp(key, "music"))    music    = atoi(val) != 0;
        else if (!strcmp(key, "musicVol")) musicVol = (float)atof(val);
        else if (!strcmp(key, "track"))    track    = atoi(val);
        else if (!strcmp(key, "loopMode")) loopMode = atoi(val);
        else if (!strcmp(key, "bgMusic"))  bgMusic  = atoi(val) != 0;
        else if (!strcmp(key, "shuffle"))  shuffle  = atoi(val) != 0;
        else if (!strcmp(key, "dof"))      dof      = atoi(val) != 0;
        else if (!strcmp(key, "mb"))       mb       = atoi(val) != 0;
        else if (!strcmp(key, "godrays"))  gr       = atoi(val) != 0;
        else if (!strcmp(key, "camOn"))    camOn    = atoi(val) != 0;
        else if (!strcmp(key, "camDist"))  camDist  = (float)atof(val);
        else if (!strcmp(key, "camVarsOn")) g_camVarsOn = atoi(val) != 0;
        else if (!strcmp(key, "cvHeight"))   g_cvHeight   = (float)atof(val);
        else if (!strcmp(key, "cvDistance")) g_cvDistance = (float)atof(val);
        else if (!strcmp(key, "cvDistLock")) g_cvDistLock = (float)atof(val);
        else if (!strcmp(key, "cvAngle"))    g_cvAngle    = (float)atof(val);
        else if (!strcmp(key, "cvFov"))      g_cvFov      = (float)atof(val);
        else if (!strcmp(key, "cvFovBattle")) g_cvFovBattle = (float)atof(val);
        else if (!strcmp(key, "camHeight"))camHeight= (float)atof(val);
        else if (!strcmp(key, "speedOn"))  speedOn  = atoi(val) != 0;
        else if (!strcmp(key, "luc"))      luc      = atoi(val) != 0;
        else if (!strcmp(key, "snatch"))   snatch   = atoi(val) != 0;
        else if (!strcmp(key, "rosePins")) rosePins = atoi(val) != 0;
        else if (!strcmp(key, "fh"))       fh       = atoi(val) != 0;
        else if (!strcmp(key, "jc"))       jc       = atoi(val) != 0;
        else if (!strcmp(key, "dtSpam"))   dtSpam   = atoi(val) != 0;
        else if (!strcmp(key, "vergilJDC")) vergilJDC = atoi(val) != 0;
        else if (!strcmp(key, "infBeowulf")) infBeowulf = atoi(val) != 0;
        else if (!strcmp(key, "infGilgamesh")) infGilgamesh = atoi(val) != 0;
        else if (!strcmp(key, "disableDTStinger")) disableDTStinger = atoi(val) != 0;
        else if (!strcmp(key, "forceLucifer")) forceLuc = atoi(val) != 0;
        else if (!strcmp(key, "noHelmBreaker")) noHelmBreaker = atoi(val) != 0;
        else if (!strcmp(key, "noHelmSplit")) noHelmSplit = atoi(val) != 0;
        else if (!strcmp(key, "scEnable")) scEnable = atoi(val) != 0;
        else if (!strcmp(key, "scCancels")) scCancels = (unsigned)strtoul(val, nullptr, 10);
        else if (!strcmp(key, "infLightningKick")) infLightningKick = atoi(val) != 0;
        else if (!strcmp(key, "infTrickTp")) infTrickTp = atoi(val) != 0;
        else if (!strcmp(key, "infAirCalibur")) infAirCalibur = atoi(val) != 0;
        else if (!strcmp(key, "airTrick")) airTrick = atoi(val) != 0;
        else if (!strcmp(key, "swordTrick")) swordTrick = atoi(val) != 0;
        else if (!strcmp(key, "jcMult"))   jcMult   = (float)atof(val);
        else if (!strcmp(key, "chargeRate"))    crWant = atoi(val) != 0;
        else if (!strcmp(key, "chargeRateVal")) crRate = (float)atof(val);
        else if (!strcmp(key, "ladyJC"))        ljWant = atoi(val) != 0;
        else if (!strcmp(key, "heightBypass"))  hrWant = atoi(val) != 0;
        else if (!strcmp(key, "easyStep"))      easyStep = atoi(val) != 0;
        else if (!strcmp(key, "skipShotgun"))   g_skipShotgun   = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "skipPandora"))   g_skipPandora   = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "skipGilgamesh")) g_skipGilgamesh = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "skipLucifer"))   g_skipLucifer   = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "skipYamato"))    g_skipYamato    = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "skipBeowulf"))   g_skipBeowulf   = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "skipForceEdge")) g_skipForceEdge = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "themeBlue"))     themeBlue = atoi(val) != 0;
        else if (!strcmp(key, "zh"))            zh     = atoi(val) != 0;
        else if (!strcmp(key, "showFps"))       showFps = atoi(val) != 0;
        else if (!strcmp(key, "guiScale"))      guiScale = (float)atof(val);
        else if (!strcmp(key, "guiAutoDPI"))    guiAutoDPI = atoi(val) != 0;
        else if (!strcmp(key, "skipBoot"))    skipBoot    = atoi(val) != 0;
        else if (!strcmp(key, "forceFocus"))  forceFocus  = atoi(val) != 0;
        else if (!strcmp(key, "borderless"))  borderless  = atoi(val) != 0;
        else if (!strcmp(key, "pauseOnOpen")) pauseOnOpen = atoi(val) != 0;
        else if (!strncmp(key, "hk", 2) && key[2] >= '0' && key[2] <= '9') {
            int idx = atoi(key + 2);
            if (idx >= 0 && idx < 16) g_hkKey[idx] = (int)strtol(val, nullptr, 16);
        }
        else if (!strcmp(key, "cheat"))    wantCheats.push_back(val);
        else if (!strncmp(key, "speed", 5) && key[5] >= '0' && key[5] <= '9') {
            int k = atoi(key + 5); if (k >= 0 && k < kNSpeed) speed[k] = (float)atof(val);
        }
    }
    fclose(f);

    // --- cheats: clear everything, then enable exactly the saved set ---
    for (auto& c : g_cheats) if (c.active) disableCheat(c);
    int on = 0;
    for (auto& nm : wantCheats)
        for (auto& c : g_cheats)
            if (c.name == nm) { if (enableCheat(c)) { c.active = true; ++on; } break; }

    // --- simple toggles via their existing setters ---
    g_autoLoad = autoload;
    setHideHud(hud);
    (void)hudPieces; for (int i = 0; i < kNHudPieces; ++i) setHudPiece(i, false);   // feature removed: ensure all HUD draw calls restored
    g_hudLockVtbl = hudLockVtbl;
    if (hideHp && g_hudLockVtbl) setHideHp(true); else setHideHp(false);
    setDisableDOF(dof); setDisableMotionBlur(mb); setDisableGodRays(gr);
    g_music = music; g_musicVol = musicVol; g_loopMode = loopMode; g_bgMusic = bgMusic; g_shuffle = shuffle;
    if (track >= 0 && track < nTracks() && track != g_curTrack) selectTrack(track);
    applyMusicVolume();

    // --- move-speed cave ---
    for (int k = 0; k < kNSpeed; ++k) g_speedVal[k] = speed[k];
    if      (speedOn && !g_speedOn) enableSpeedCave();
    else if (!speedOn && g_speedOn) disableSpeedCave();
    else if (speedOn && g_speedOn && g_speedData)
        for (int k = 0; k < kNSpeed; ++k) g_speedData[k] = g_speedVal[k];

    // --- Lucifer / Full House restores ---
    if (luc && !g_lucOn) applyLuciferBug(); else if (!luc && g_lucOn) stopLuciferBug();
    if (snatch && !g_snatchOn) applySnatchRange(); else if (!snatch && g_snatchOn) stopSnatchRange();
    g_rosePins = rosePins ? 1 : 0;
    if (fh  && !g_fhOn)  applyFullHouseFix(); else if (!fh && g_fhOn) stopFullHouseFix();
    g_dtSpam = dtSpam;
    g_vergilJDC = vergilJDC;
    g_infBeowulf = infBeowulf;
    g_infGilgamesh = infGilgamesh; g_infLightningKick = infLightningKick;
    g_infTrickTp = infTrickTp; g_infAirCalibur = infAirCalibur;
    g_disableDTStinger = disableDTStinger;
    g_forceLucifer = forceLuc ? 1 : 0;
    if (forceLuc && !g_flOn) applyForceLucifer(); else if (!forceLuc && g_flOn) stopForceLucifer();
    g_noHelmBreaker = noHelmBreaker; g_noHelmSplit = noHelmSplit;
    g_scEnableDante = scEnable; g_scCancels = scCancels;

    // --- options: theme / language / fps / GUI scale (persist the GUI choices too) ---
    g_zh = zh; g_showFps = showFps;
    g_uiScale = guiScale; g_uiAutoDPI = guiAutoDPI;
    if (g_uiScale < 0.50f) g_uiScale = 0.50f; if (g_uiScale > 3.00f) g_uiScale = 3.00f;
    if (themeBlue != g_blue) { g_blue = themeBlue; applyTheme(g_blue); }
    else applyUiScale();   // re-stamp the restored scale (applyTheme already does this when the theme changed)

    // --- Section 2 "Triple Trouble": just restore intent; updateMajinPins installs
    //     the caves only while playing the matching character. ---
    g_crRate = crRate;
    g_crWant = crWant; g_ljWant = ljWant; g_hrWant = hrWant;
    g_easyStepWant = easyStep;
    skipSyncGun(); skipSyncSword();

    g_jcMult = jcMult;
    if (jc && !g_jcOn) applyJumpCancel(); else if (!jc && g_jcOn) stopJumpCancel();
    if (g_jcOn) setJumpCancelMult(jcMult);
    if (airTrick && !g_atOn) applyAirTrick(); else if (!airTrick && g_atOn) stopAirTrick();
    (void)swordTrick; if (g_swOn) stopSwordTrick();   // Swordless Air Trick removed -- never auto-apply

    g_forceFocus = forceFocus; g_pauseOnOpen = pauseOnOpen;
    setSkipBoot(skipBoot);   // sync the on-disk movie stub to the saved intent
    if (borderless != g_borderless) setBorderless(borderless);


    // --- camera (mirror the checkbox's save-orig / restore-orig behaviour) ---
    g_camDist = camDist; g_camHeight = camHeight;
    uint8_t* cd = getCameraData();
    if (camOn && !g_camOn) {
        if (cd) { g_camOrigH = *(float*)(cd + 0xD0); g_camOrigD = *(float*)(cd + 0xD8);
                  g_camOrigDL = *(float*)(cd + 0xDC); g_camSaved = true; }
        g_camOn = true;
    } else if (!camOn && g_camOn) {
        if (cd && g_camSaved) { *(float*)(cd + 0xD0) = g_camOrigH; *(float*)(cd + 0xD8) = g_camOrigD;
                                *(float*)(cd + 0xDC) = g_camOrigDL; g_camSaved = false; }
        g_camOn = false;
    }

    logf("[cfg] loaded %s: %d/%zu cheats", path, on, wantCheats.size());
    return true;
}

// Called once after the overlay is live; scans every saved profile and loads
// whichever one was saved with "Auto-load on launch" checked -- ANY profile, not
// just "default". If more than one is flagged, the most recently saved wins.
static void tryAutoLoad() {
    char glob[MAX_PATH]; buildAssetPath(glob, "DMC4SEMOOD_*.cfg");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string pre = "DMC4SEMOOD_", suf = ".cfg";
    std::string best; ULONGLONG bestTime = 0;
    do {
        std::string n = fd.cFileName;
        if (n.size() <= pre.size() + suf.size()) continue;
        if (n.compare(0, pre.size(), pre) != 0) continue;
        if (n.compare(n.size() - suf.size(), suf.size(), suf) != 0) continue;
        std::string name = n.substr(pre.size(), n.size() - pre.size() - suf.size());
        char path[MAX_PATH]; buildConfigPath(path, name.c_str());
        FILE* f = fopen(path, "r");
        if (!f) continue;
        bool wantAuto = false; char line[256];
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "autoload=", 9)) { wantAuto = atoi(line + 9) != 0; break; }
        fclose(f);
        if (!wantAuto) continue;
        ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (best.empty() || t >= bestTime) { best = name; bestTime = t; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    // Replay hotkeys default to F2 (record) / F3 (play) / F4 (auto-replay loop); a saved profile overrides below.
    if (!g_hkKey[kHkReplayRecord]) g_hkKey[kHkReplayRecord] = VK_F2;   // 0x71
    if (!g_hkKey[kHkReplayPlay])   g_hkKey[kHkReplayPlay]   = VK_F3;   // 0x72
    if (!g_hkKey[kHkReplayLoop])   g_hkKey[kHkReplayLoop]   = VK_F4;   // 0x73
    if (!best.empty()) { logf("[cfg] auto-loading profile '%s'", best.c_str()); loadConfig(best.c_str()); }
}

// Scan the dll's folder for saved profiles (DMC4SEMOOD_<name>.cfg) for the dropdown.
static std::vector<std::string> g_profiles;
static int g_profileSel = 0;
static void refreshProfiles() {
    g_profiles.clear();
    char glob[MAX_PATH]; buildAssetPath(glob, "DMC4SEMOOD_*.cfg");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        const std::string pre = "DMC4SEMOOD_", suf = ".cfg";
        do {
            std::string n = fd.cFileName;
            if (n.size() > pre.size() + suf.size() &&
                n.compare(0, pre.size(), pre) == 0 &&
                n.compare(n.size() - suf.size(), suf.size(), suf) == 0)
                g_profiles.push_back(n.substr(pre.size(), n.size() - pre.size() - suf.size()));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (g_profileSel >= (int)g_profiles.size()) g_profileSel = 0;
}

// Nero Blue Rose: jump the gun charge to a level (1/2/3) -- sets the current-charge
// value + full-charge flag and holds them. We deliberately do NOT touch the
// "Charge Power Graphic" (it's an index the game derefs -> null crash).
static void setGunCharge(int lv) {
    uintptr_t a;
    static const uint32_t oC[] = {0x3D8,0xA4,0x4C4};
    if (majinChain(oC,3,a)) { float v = (float)lv/3.0f*100.f; writeMem(a,&v,4); holdSet(oC,3,4,true,v,0); }
    static const uint32_t oF[] = {0x3D5,0xA4,0x4C4};
    if (majinChain(oF,3,a)) { int v = (lv>=3)?1:0;            writeMem(a,&v,1); holdSet(oF,3,1,false,0,v); }
}
// Nero Exceed: jump the Exceed GAUGE to a level (1/2/3) and hold it. We do NOT set
// Exceed Phase (the active rev state) -- forcing that crashes the rev processing.
static void setExceed(int lv) {
    uintptr_t a;
    static const uint32_t oG[] = {0xCAE4,0xA4,0x4C4};
    if (majinChain(oG,3,a)) { int v = lv; writeMem(a,&v,4); holdSet(oG,3,4,false,0,v); }
}

// ---- Gun charge Lv3 for ALL characters. The Nero charge chain {0x3D8,0xA4,
// 0x4C4} resolves through 0x4C4 = the *active character's* action/weapon object,
// so the same write works for Dante's E&I, Lady's handguns, Trish, Nero -- it
// charges whatever gun the current character is holding. (The earlier per-char
// "instant max" offsets were wrong: Concentration's 0x7930 is a POINTER, so
// pinning 3 into it crashed the game. Removed.)
static void setGunCharge(int lv);  // fwd (defined above)
static void setGunChargeAll() { setGunCharge(3); }
// Nero gun charge / Exceed off.
static void setGunChargeNeroOff() {
    static const uint32_t oC[] = {0x3D8,0xA4,0x4C4}; static const uint32_t oF[] = {0x3D5,0xA4,0x4C4};
    unpinMajin(oC, 3, 4, true); unpinMajin(oF, 3, 1, false);
}
static void setExceedOff() {
    static const uint32_t oG[] = {0xCAE4,0xA4,0x4C4}; unpinMajin(oG, 3, 4, false);
}

// Apply the full color theme. `blue` swaps the red accent family for a blue one;
// the window stays near-black in both. Called once at init and again whenever the
// Theme toggle flips, so the recolor is instant and total.
// ===== GUI scaling (Section: resolution independence) =========================
// The menu is authored at a base density (FontGlobalScale 0.75 + the tight padding
// below). On a 4K panel that base is microscopic; on 720p it's oversized. We apply
// ONE effective scale to both the font AND every style size each frame, so "bigger
// GUI -> bigger text" stays coherent and the menu is usable from 1280x720 up to 4K
// (and beyond) at any refresh rate. Two inputs:
//   g_uiScale   user multiplier (the slider) -- 1.0 = no manual change.
//   g_uiAutoDPI auto-fit the base to the render height so the menu keeps the same
//               on-screen proportion across resolutions (720p..4K).
// (State globals are declared earlier, near the applyTheme fwd decl, because
// save/loadConfig reference them.)

// Effective scale = manual slider * resolution DPI factor. The DPI factor maps the
// backbuffer height to 1080p (1.0); clamped so 720p stays readable and ultra-wide /
// 8K can't blow the menu off-screen.
static float effectiveUiScale() {
    float dpi = 1.0f;
    if (g_uiAutoDPI && g_bbH) {
        dpi = (float)g_bbH / 1080.0f;        // 720->0.667 1080->1 1440->1.333 2160->2
        if (dpi < 0.85f) dpi = 0.85f;        // floor: keep 720p and below legible
        if (dpi > 2.50f) dpi = 2.50f;        // ceil
    }
    float sc = g_uiScale * dpi;
    if (sc < 0.40f) sc = 0.40f;
    if (sc > 4.00f) sc = 4.00f;
    return sc;
}

// Re-derive the live style from the unscaled base and stamp the effective scale onto
// sizes (ScaleAllSizes) and the font (FontGlobalScale). Cheap; called whenever the
// effective scale changes (slider moved or resolution changed).
static void applyUiScale() {
    if (!g_styleCaptured) return;
    float sc = effectiveUiScale();
    ImGuiStyle st = g_styleBase;             // colors + unscaled sizes
    st.ScaleAllSizes(sc);                    // scales padding/spacing/scrollbar/grab...
    ImGui::GetStyle() = st;
    ImGui::GetIO().FontGlobalScale = kUiBaseFont * sc;
    g_uiScaleApplied = sc;
}

static void applyTheme(bool blue) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize  = 1.0f;            // accent outlines around controls
    // Compact, density: smaller text + tight spacing so everything
    // fits on one screen. The font scale is owned by applyUiScale() now (so the GUI
    // scale slider can drive it); these are the BASE (unscaled) size values.
    s.WindowPadding    = ImVec2(6, 3);
    s.FramePadding     = ImVec2(3, 1);
    s.ItemSpacing      = ImVec2(5, 1);
    s.ItemInnerSpacing = ImVec2(3, 1);
    s.IndentSpacing    = 12.0f;
    s.ScrollbarSize    = 11.0f;
    s.GrabMinSize      = 8.0f;
    ImVec4* c = s.Colors;
    const ImVec4 black = ImVec4(0.00f,0.00f,0.00f,1.00f);
    const ImVec4 bg    = ImVec4(0.02f,0.01f,0.01f,0.98f);   // near-black window (both themes)
    ImVec4 frame, accent, glow, dim, text, textDis, popup, scrollbg;
    if (!blue) {
        frame    = ImVec4(0.06f,0.01f,0.01f,1.00f);   // control fill (almost black)
        accent   = ImVec4(0.62f,0.05f,0.05f,1.00f);   // main red (softened)
        glow     = ImVec4(0.80f,0.12f,0.12f,1.00f);   // glowing red (hover/active)
        dim      = ImVec4(0.30f,0.02f,0.02f,1.00f);   // dim red fill
        text     = ImVec4(1.00f,1.00f,1.00f,1.00f);   // white text (all labels/toggles)
        textDis  = ImVec4(0.82f,0.82f,0.82f,1.00f);   // bright near-white hints (was dim grey)
        popup    = ImVec4(0.04f,0.00f,0.00f,0.98f);
        scrollbg = ImVec4(0.06f,0.005f,0.005f,0.90f);
        g_accent    = ImVec4(0.80f,0.12f,0.12f,1.0f);
        g_accentDim = ImVec4(0.62f,0.08f,0.08f,1.0f);
    } else {
        frame    = ImVec4(0.01f,0.02f,0.08f,1.00f);   // control fill (almost black-blue)
        accent   = ImVec4(0.10f,0.22f,0.70f,1.00f);   // main blue
        glow     = ImVec4(0.22f,0.45f,0.98f,1.00f);   // glowing blue (hover/active)
        dim      = ImVec4(0.04f,0.09f,0.36f,1.00f);   // dim blue fill
        text     = ImVec4(1.00f,1.00f,1.00f,1.00f);   // white text (all labels/toggles)
        textDis  = ImVec4(0.82f,0.82f,0.82f,1.00f);   // bright near-white hints (was dim grey)
        popup    = ImVec4(0.01f,0.02f,0.10f,0.98f);
        scrollbg = ImVec4(0.01f,0.02f,0.10f,0.90f);
        g_accent    = ImVec4(0.30f,0.58f,0.98f,1.0f);
        g_accentDim = ImVec4(0.18f,0.36f,0.82f,1.0f);
    }
    c[ImGuiCol_Text]             = text;
    c[ImGuiCol_TextDisabled]     = textDis;
    c[ImGuiCol_WindowBg]         = bg;
    c[ImGuiCol_ChildBg]          = ImVec4(0,0,0,0);
    c[ImGuiCol_PopupBg]          = popup;
    c[ImGuiCol_Border]           = accent;
    c[ImGuiCol_TitleBg]          = black;
    c[ImGuiCol_TitleBgActive]    = dim;
    c[ImGuiCol_Header]           = dim;
    c[ImGuiCol_HeaderHovered]    = accent;
    c[ImGuiCol_HeaderActive]     = glow;
    c[ImGuiCol_FrameBg]          = frame;
    c[ImGuiCol_FrameBgHovered]   = dim;
    c[ImGuiCol_FrameBgActive]    = accent;
    c[ImGuiCol_Button]           = dim;
    c[ImGuiCol_ButtonHovered]    = accent;
    c[ImGuiCol_ButtonActive]     = glow;
    c[ImGuiCol_CheckMark]        = glow;
    c[ImGuiCol_SliderGrab]       = accent;
    c[ImGuiCol_SliderGrabActive] = glow;
    c[ImGuiCol_Separator]        = accent;
    c[ImGuiCol_SeparatorHovered] = glow;
    c[ImGuiCol_SeparatorActive]  = glow;
    c[ImGuiCol_ResizeGrip]       = dim;
    c[ImGuiCol_ResizeGripHovered]= accent;
    c[ImGuiCol_ResizeGripActive] = glow;
    c[ImGuiCol_TextSelectedBg]   = dim;
    // Tabs are filled with the accent; their labels are drawn white (see beginBlackTab).
    c[ImGuiCol_Tab]              = dim;
    c[ImGuiCol_TabHovered]       = glow;
    c[ImGuiCol_TabSelected]      = accent;
    c[ImGuiCol_TabDimmed]        = dim;
    c[ImGuiCol_TabDimmedSelected]= accent;
    c[ImGuiCol_ScrollbarBg]          = scrollbg;
    c[ImGuiCol_ScrollbarGrab]        = accent;
    c[ImGuiCol_ScrollbarGrabHovered] = glow;
    c[ImGuiCol_ScrollbarGrabActive]  = glow;
    // Snapshot this freshly-built (UNSCALED) style as the base, then stamp the
    // current GUI/DPI scale onto the live style + font. Re-captures on every theme
    // toggle so colour changes carry through, and applyUiScale() reads from here.
    g_styleBase = s;
    g_styleCaptured = true;
    applyUiScale();
}

// Begin a tab whose label is drawn in WHITE (tabs are accent-filled), so the tab
// names stay readable. Pushes white text only around the label render.
static bool beginBlackTab(const char* name) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
    bool open = ImGui::BeginTabItem(name);
    ImGui::PopStyleColor();
    return open;
}

// Standalone FPS counter pinned to the screen's upper-left corner: dark-blue text
// on a black panel (matches the HUD look). Painted on the FOREGROUND draw list so
// it always sits on top of everything -- including the full-screen menu -- and is
// independent of window z-order, so it stays readable whether the menu is up or
// hidden during gameplay.
static void drawFpsOverlay() {
    // Only when enabled AND the menu is closed: the open menu already shows FPS in
    // its header, and hiding the corner copy then avoids overlapping the title.
    if (!g_showFps || g_show) return;
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    char buf[64];
    const float fps = io.Framerate;
    const float ms  = fps > 0.0f ? 1000.0f / fps : 0.0f;
    snprintf(buf, sizeof(buf), "%.0f FPS  (%.2f ms)", fps, ms);

    ImFont* font   = ImGui::GetFont();
    const float fsz = 19.0f;                       // bigger than menu text -> readable HUD
    const ImVec2 pad(9.0f, 6.0f);
    const ImVec2 ts = font->CalcTextSizeA(fsz, 99999.0f, 0.0f, buf);
    const ImVec2 p0(vp->Pos.x + 10.0f, vp->Pos.y + 10.0f);
    const ImVec2 p1(p0.x + ts.x + pad.x * 2.0f, p0.y + ts.y + pad.y * 2.0f);

    const ImU32 col = fpsColU32();                    // follows the red/blue theme
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 220), 5.0f);          // black panel
    dl->AddRect(p0, p1, col, 5.0f, 0, 1.5f);                          // themed border
    dl->AddText(font, fsz, ImVec2(p0.x + pad.x, p0.y + pad.y), col, buf);
}

// ===================== Combo move-name readout =====================
// White "Move + Move + Move ..." text painted on the foreground draw list (shows
// with the menu closed). The live move id is the player actor's mActionNo
// (+0x1A00); the specific attack is mAtckId (+0x1A74) with enable flag +0x1A78;
// DT is mIsSuperCharMode (+0x2773). The player pointer is the same chain the
// trainer already uses (activePlayer()). Names are NOT in the exe (they live in
// packed GMD message packs with a non-matching index), so we map the common
// (char, action, atck) ids to names via a hand-built table, with Learn mode to
// capture ids live. After 5 moves the line fades out and the next move starts a
// fresh line.
static bool  g_comboReadout = false;   // master toggle
static bool  g_comboLearn   = false;   // log (char,action,atck) ids to overlay.log
static float g_comboScale   = 1.0f;    // text size multiplier
static ImFont* g_comboFont  = nullptr; // bold-italic face (Arial Bold Italic), loaded at init

// A user-assigned name for a (character, move-id) pair. The game stores NO name
// text tied to mAtckId (confirmed at the disassembly level: mAtckId is a combat
// discriminator; the pretty names live in a separate menu-text namespace with no
// in-engine link to it). So names are ASSIGNED in-game via the namer and saved
// to MODS\movenames.cfg, persisting across launches.
struct NamedMove { int ch; uint32_t atck; std::string name; };
static std::vector<NamedMove> g_moveNames;

static char     g_comboName[5][40];      // move name per slot
static int      g_comboMult[5];          // repeat count per slot (shown as "xN")
static int      g_comboCount = 0;
static float    g_comboAlpha = 0.0f;
static float    g_comboIdle  = 0.0f;     // seconds since the last move was pushed
static uint32_t g_cLastAtck  = 0;        // last seen mAtckId (for edge detection)
static uint8_t  g_cLastDT    = 0;        // last mIsSuperCharMode
static uint32_t g_cLearnSeen[128];       // dedupe Learn-mode log keys
static int      g_cLearnN    = 0;
// Last attack event seen (for the in-game namer "name this move" button).
static int      g_cEvCh   = -1;
static uint32_t g_cEvAtck = 0;
static char     g_nameInput[40] = {0};

static const char* comboCharName(int ch) {
    // ctor-stamped id at player+0x19AC: 0=Dante 1=Nero 2=Vergil 3=Trish 4=Lady.
    static const char* n[5] = { "Dante","Nero","Vergil","Trish","Lady" };
    return (ch >= 0 && ch < 5) ? n[ch] : "?";
}
// Resolve a display name for (char, atck) from movenames.cfg ONLY. Unnamed moves
// return empty -- the readout never shows raw "id 0xNN" tags (only names on file).
static void comboLookup(int ch, uint32_t atck, char* out, size_t n) {
    for (auto& e : g_moveNames)
        if (e.ch == ch && e.atck == atck) { snprintf(out, n, "%s", e.name.c_str()); return; }
    if (n) out[0] = 0;
}
static bool comboHasName(int ch, uint32_t atck) {
    for (auto& e : g_moveNames) if (e.ch == ch && e.atck == atck) return true;
    return false;
}
static void saveMoveNames() {
    char path[MAX_PATH]; buildAssetPath(path, "MODS\\movenames.cfg");
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# DMC4SEMOOD move names: char<TAB>atckId<TAB>name\n");
    for (auto& e : g_moveNames) fprintf(f, "%d\t%u\t%s\n", e.ch, e.atck, e.name.c_str());
    fclose(f);
}
static void loadMoveNames() {
    char path[MAX_PATH]; buildAssetPath(path, "MODS\\movenames.cfg");
    FILE* f = fopen(path, "r");
    if (!f) return;
    g_moveNames.clear();
    char ln[256];
    while (fgets(ln, sizeof(ln), f)) {
        if (ln[0] == '#' || ln[0] == '\n') continue;
        int ch; unsigned atck; char nm[120];
        if (sscanf(ln, "%d\t%u\t%119[^\n]", &ch, &atck, nm) == 3)
            g_moveNames.push_back({ ch, atck, std::string(nm) });
    }
    fclose(f);
}
// Read the Windows clipboard (Unicode or ANSI) into out as UTF-8. Used by the
// "Paste" button so a name can be pasted without relying on Ctrl+V keystrokes
// reaching ImGui (unreliable under Wine / with a controller).
static bool readClipboardText(char* out, size_t n) {
    if (n == 0 || !OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (wchar_t* w = (wchar_t*)GlobalLock(h)) {
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, nullptr, nullptr);
            out[n - 1] = 0; GlobalUnlock(h); ok = true;
        }
    } else if (HANDLE h2 = GetClipboardData(CF_TEXT)) {
        if (char* c = (char*)GlobalLock(h2)) {
            strncpy(out, c, n - 1); out[n - 1] = 0; GlobalUnlock(h2); ok = true;
        }
    }
    CloseClipboard();
    // Trim a trailing newline (common when copying a line of text).
    size_t L = strlen(out);
    while (L && (out[L - 1] == '\n' || out[L - 1] == '\r')) out[--L] = 0;
    return ok;
}
static void setMoveName(int ch, uint32_t atck, const char* name) {
    if (ch < 0 || !name || !name[0]) return;
    for (auto& e : g_moveNames)
        if (e.ch == ch && e.atck == atck) { e.name = name; saveMoveNames(); return; }
    g_moveNames.push_back({ ch, atck, std::string(name) });
    saveMoveNames();
}
static void comboPush(const char* name) {
    // Same move again (and the line hasn't faded) -> bump its xN counter.
    if (g_comboCount > 0 && g_comboAlpha > 0.01f &&
        strcmp(g_comboName[g_comboCount - 1], name) == 0) {
        g_comboMult[g_comboCount - 1]++;
        g_comboAlpha = 1.0f; g_comboIdle = 0.0f;
        return;
    }
    if (g_comboCount >= 5 || g_comboAlpha <= 0.01f) g_comboCount = 0;  // start a fresh line
    if (g_comboCount < 5) {
        snprintf(g_comboName[g_comboCount], 40, "%s", name);
        g_comboMult[g_comboCount] = 1;
        g_comboCount++;
    }
    g_comboAlpha = 1.0f;
    g_comboIdle  = 0.0f;
}
// Per-frame: read the live move state, detect events, push names, manage fade.
static void comboReadoutTick() {
    if (!g_comboReadout) { g_comboCount = 0; g_comboAlpha = 0.0f; return; }
    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime; if (dt <= 0.0f) dt = 1.0f / 60.0f;

    char* pl = (char*)activePlayer();
    if (pl && memReadable(pl, 0x2800)) {
        int ch = (int)*(uint8_t*)(pl + 0x19AC);          // ctor-stamped char id (0=Dante..4=Lady)
        if (ch < 0 || ch > 4) ch = -1;
        uint32_t action = *(uint32_t*)(pl + 0x1A00);     // mActionNo
        uint32_t atck   = *(uint32_t*)(pl + 0x1A74);     // mAtckId
        uint32_t aen    = *(uint32_t*)(pl + 0x1A78);     // mAtckId_Enable
        uint8_t  dt8    = *(uint8_t*)(pl + 0x2773);      // mIsSuperCharMode

        // Attack event: a new enabled attack id (filters out idle/walk/run, which
        // keep mAtckId 0 / disabled).
        if (aen != 0 && atck != 0 && atck != g_cLastAtck) {
            g_cEvCh = ch; g_cEvAtck = atck;       // remember for the in-game namer
            if (comboHasName(ch, atck)) {         // ONLY show moves named in movenames.cfg -- never raw ids
                char nm[40]; comboLookup(ch, atck, nm, sizeof(nm));
                if (nm[0]) comboPush(nm);
            }
            if (g_comboLearn) {
                uint32_t key = (action << 8) ^ atck;
                bool seen = false;
                for (int i = 0; i < g_cLearnN; i++) if (g_cLearnSeen[i] == key) { seen = true; break; }
                if (!seen && g_cLearnN < 128) {
                    g_cLearnSeen[g_cLearnN++] = key;
                    logf("[combo] %s action=0x%X atck=0x%X\n",
                         comboCharName(ch), action, atck);
                }
            }
        }
        if (aen == 0) g_cLastAtck = 0; else g_cLastAtck = atck;

        // Devil Trigger rising edge.
        if (dt8 && !g_cLastDT) comboPush("Devil Trigger");
        g_cLastDT = dt8;
    }

    // Fade: hold then fade after 5 moves, or after a longer idle mid-combo.
    g_comboIdle += dt;
    if (g_comboCount > 0) {
        float fadeAfter = (g_comboCount >= 5) ? 0.8f : 3.0f;
        if (g_comboIdle > fadeAfter) {
            g_comboAlpha -= dt * 1.8f;
            if (g_comboAlpha <= 0.0f) { g_comboAlpha = 0.0f; g_comboCount = 0; }
        }
    }
    if (g_comboCount == 0 || g_comboAlpha <= 0.0f) return;

    // Build "Move + Move x3 + ..." and draw centered near the bottom.
    char line[320]; line[0] = 0;
    for (int i = 0; i < g_comboCount; i++) {
        if (i) strncat(line, "  +  ", sizeof(line) - strlen(line) - 1);
        char seg[56];
        if (g_comboMult[i] > 1) snprintf(seg, sizeof(seg), "%s x%d", g_comboName[i], g_comboMult[i]);
        else                    snprintf(seg, sizeof(seg), "%s", g_comboName[i]);
        strncat(line, seg, sizeof(line) - strlen(line) - 1);
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = g_comboFont ? g_comboFont : ImGui::GetFont();
    float fsz = 19.0f * g_comboScale;
    ImVec2 ts = font->CalcTextSizeA(fsz, 99999.0f, 0.0f, line);
    // Centered horizontally, near the BOTTOM of the screen (above the very edge).
    ImVec2 p(vp->Pos.x + (vp->Size.x - ts.x) * 0.5f, vp->Pos.y + vp->Size.y * 0.86f);
    int a = (int)(g_comboAlpha * 255.0f); if (a > 255) a = 255;
    int sh = (int)(g_comboAlpha * 210.0f);
    dl->AddText(font, fsz, ImVec2(p.x + 2, p.y + 2), IM_COL32(0, 0, 0, sh), line);   // shadow
    dl->AddText(font, fsz, p, IM_COL32(255, 255, 255, a), line);                      // white
}

// Compact hover help: a dim "(?)" after the previous widget; full text on hover.
static void hint(const char* tip) {
    ImGui::SameLine(0, 4);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", tip); }
}
// Jump the cursor to the right half of the window (for two-up checkbox rows).
// Overlap-safe: if the LEFT item already ran past the midpoint (long label, or a
// large GUI scale), don't SameLine -- let the next widget fall onto its own line so
// it can never land on top of the left label and steal its clicks.
static void rightCol() {
    float mid = ImGui::GetWindowWidth() * 0.5f;
    float leftEdge = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;  // right edge of last item, window-local
    if (leftEdge < mid - 6.0f) ImGui::SameLine(mid);   // else: stay on a fresh line
}

// On-screen snatch target-distance readout. Shows the live 3D distance from Nero to the
// soft/auto target (+0x4C4) and to the secondary target field (+0x13C8), plus the MAX each
// has reached. Purpose: read the EXACT distance at which snatch stops acquiring, so that
// value can be scanned for in Cheat Engine to find the acquisition-range parameter.
// "Disable All" -- turn OFF every toggle/patch the trainer can enable, restoring vanilla.
static void disableEverythingToggles() {
    for (auto& c : g_cheats) if (c.active) disableCheat(c);     // generated cheats
    stopCombatPatch(g_pFastTrick); stopCombatPatch(g_pKnockback); stopCombatPatch(g_pMovingTarget);
    stopCombatPatch(g_pTrickRange); stopCombatPatch(g_pEnemyAIMax); stopCombatPatch(g_pFreeSwords);
    stopCombatPatch(g_pInfTableHop); stopCombatPatch(g_pSuperTableHop);
    stopSnatchRange();
    g_superCancel = false;
    g_easyQuickDrive = false;
    g_scEnableDante = false; g_scCancels = 0;
    if (g_mutRunning) { g_mutRunning = false; mutatorStop(); }
    g_fpsLimit = 0;
    g_crWant = g_ljWant = g_hrWant = false;                     // live toggles (removed next frame)
    g_easyStepWant = false; if (g_esOn) stopEasyStep();
    g_skipShotgun = g_skipPandora = g_skipGilgamesh = g_skipLucifer = 0;
    g_skipYamato = g_skipBeowulf = g_skipForceEdge = 0; skipSyncGun(); skipSyncSword();
    g_disableDTStinger = false;
    g_noHelmBreaker = g_noHelmSplit = false; if (g_helmHookOn) stopHelmHook();
    g_infTrickTp = g_infAirCalibur = g_dtSpam = g_vergilJDC = false;
    g_infBeowulf = g_infGilgamesh = g_infLightningKick = false;
    g_diagOn = false;
    if (g_dmgOn)   stopDamageMod();
    if (g_speedOn) disableSpeedCave();
    if (g_diffMode != 0) { applyDifficulty(0); g_diffMode = 0; }
    setDisableDOF(false); setDisableMotionBlur(false); setDisableGodRays(false);
    g_hudHideAll = false; if (g_keepWeapons) { g_keepWeapons = false; setWeaponMode(0); }
    for (int i = 0; i < 4; i++) setWorkRate(i, 1.0f);           // normal speed
    logf("[disableall] all toggles off");
}
static void DrawUI() {
    drawFpsOverlay();      // corner FPS counter shows even with the menu hidden
    comboReadoutTick();    // live combo move-name readout (also shows menu-closed)
    if (!g_show) return;
    // Compact, window: small default size pinned to the top-left,
    // freely resizable afterwards. Sized to fit on one screen.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Default size follows the GUI scale so the window is proportioned for the
    // resolution at launch, but never larger than the screen (so it stays usable at
    // 1280x720). User can still resize freely afterwards.
    float uis = effectiveUiScale();
    float defW = 540.0f * uis, defH = 680.0f * uis;
    if (defW > vp->WorkSize.x * 0.96f) defW = vp->WorkSize.x * 0.96f;
    if (defH > vp->WorkSize.y * 0.96f) defH = vp->WorkSize.y * 0.96f;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 24, vp->Pos.y + 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(defW, defH), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MistressDMC  -  DMC4SE MOOD")) { ImGui::End(); return; }

    drawLogoWatermark();   // centered DMC4SE MOOD logo behind the controls (all sections)

    int activeCount = 0; for (auto& c : g_cheats) if (c.active) ++activeCount;
    ImGui::TextColored(g_accent, "MistressDMC");
    ImGui::SameLine(); ImGui::TextDisabled(tr("| %d/%zu on | 7 or L3+R3","| %d/%zu 开启 | 7 或 L3+R3"), activeCount, g_cheats.size());
    ImGui::SameLine(); ImGui::TextColored(fpsColVec(), "| %.0f FPS", ImGui::GetIO().Framerate);
    ImGui::TextColored(g_accentDim, tr("Join the discord @ discord.gg/aethergrid","加入 Discord @ discord.gg/aethergrid"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", tr("search cheats...","搜索作弊..."), g_filter, sizeof(g_filter));
    if (ImGui::Button(tr("Disable All","全部关闭"))) disableEverythingToggles();
    ImGui::SameLine(); if (ImGui::Button(tr("Hide (7)","隐藏 (7)"))) g_show = false;
    ImGui::SameLine(); if (ImGui::Button(tr(g_showFps ? "Hide FPS" : "Show FPS", g_showFps ? "隐藏 FPS" : "显示 FPS"))) g_showFps = !g_showFps;
    ImGui::Separator();

    // Each section's scroll child reserves this much at the bottom for the footer +
    // bottom-right options tab bar, so they stay pinned. (An earlier attempt wrapped
    // the whole tab bar in one extra child, which clipped the section content under
    // ImGui 1.92 -- categories below the first headers vanished. Per-section reserve
    // keeps the original single-child layout that renders everything.)
    const float footerH = ImGui::GetFrameHeightWithSpacing() * 3.2f + 10.0f;

    if (!ImGui::BeginTabBar("##sections", ImGuiTabBarFlags_FittingPolicyResizeDown)) { ImGui::End(); return; }

    // ===================== Section 1 =====================
    if (beginBlackTab(tr("System###s1","系统###s1"))) {
    ImGui::BeginChild("t0", ImVec2(0, -footerH));

    // --- Config: save / load named profiles (top of Section 1) ---
    if (ImGui::CollapsingHeader("Config (save / load)", ImGuiTreeNodeFlags_DefaultOpen)) {
        static char s_profile[64] = "default";
        static bool s_scanned = false;
        if (!s_scanned) { refreshProfiles(); s_scanned = true; }
        ImGui::Indent(8.0f);

        // Type a name to make a new save.
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("New profile name", s_profile, sizeof(s_profile));
        if (ImGui::Button("Save")) { saveConfig(s_profile); refreshProfiles(); }
        ImGui::SameLine();
        if (ImGui::Button("Load")) loadConfig(s_profile);

        // Dropdown of the saves we already made (pick one -> fills the name above).
        const char* cur = g_profiles.empty() ? "(no saves yet)"
                                             : g_profiles[g_profileSel].c_str();
        ImGui::SetNextItemWidth(180);
        if (ImGui::BeginCombo("Saved configs", cur)) {
            for (int i = 0; i < (int)g_profiles.size(); ++i) {
                bool sel = (i == g_profileSel);
                if (ImGui::Selectable(g_profiles[i].c_str(), sel)) {
                    g_profileSel = i;
                    strncpy(s_profile, g_profiles[i].c_str(), sizeof(s_profile) - 1);
                    s_profile[sizeof(s_profile) - 1] = 0;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (!g_profiles.empty() && ImGui::Button("Load selected"))
            loadConfig(g_profiles[g_profileSel].c_str());
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) refreshProfiles();

        if (ImGui::Checkbox("Auto-load on launch", &g_autoLoad)) { saveConfig("default"); refreshProfiles(); }
        ImGui::TextDisabled("Saves every cheat + toggle to DMC4SEMOOD_<name>.cfg beside the dll.");
        ImGui::TextDisabled("Auto-load snapshots your current setup as the launch default.");
        ImGui::Unindent(8.0f);
    }

    // --- Teleport & Bloody Palace (pointer-based; works while in a level) ---
    if (ImGui::CollapsingHeader("Teleport & Bloody Palace", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);
        static const char* roomNames[] = {
            "st000 Opera House", "st001 Opera House plaza",
            "st705 BP 1-19", "st503 BP 20", "st704 BP 21-39", "st504 BP 40",
            "st703 BP 41-59", "st505 BP 60", "st701 BP 61-79", "st506 BP 80",
            "st702 BP 81-99", "st507 BP 100", "st700 BP 101", "(manual Room I.D)"
        };
        static const int roomIds[] = { 0,1,705,503,704,504,703,505,701,506,702,507,700,-1 };
        static int mission = 1, roomSel = 0, manualRoom = 0, bpStage = 1;
        uint32_t cur = 0;
        if (readCurrentRoom(cur)) ImGui::Text("Current Room I.D: %u", cur);
        else ImGui::TextDisabled("Current Room I.D: (load a level first)");
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("Mission # (1-20)", &mission); if (mission < 0) mission = 0; if (mission > 20) mission = 20;
        ImGui::SetNextItemWidth(190);
        ImGui::Combo("Room", &roomSel, roomNames, IM_ARRAYSIZE(roomNames));
        int room = roomIds[roomSel];
        if (room < 0) { ImGui::SameLine(); ImGui::SetNextItemWidth(70); ImGui::InputInt("##manualroom", &manualRoom); if (manualRoom < 0) manualRoom = 0; room = manualRoom; }
        if (ImGui::Button("Initiate Jump")) { if (!areaJump(mission, room, -1)) logf("[aj] jump failed (no level?)"); }
        ImGui::Separator();
        ImGui::TextDisabled("Bloody Palace quick-jump (auto-picks room):");
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("BP Stage (1-101)", &bpStage); if (bpStage < 1) bpStage = 1; if (bpStage > 101) bpStage = 101;
        ImGui::SameLine();
        if (ImGui::Button("Jump to BP Stage")) { if (!areaJump(-1, bpRoomForStage(bpStage), bpStage)) logf("[bp] jump failed"); }
        ImGui::Separator();
        ImGui::Checkbox("Boss Rush (Bloody Palace)", &g_bossRush);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Skips all filler floors in Bloody Palace.\n"
                              "Start floor is forced to 20, then auto-jumps to the\n"
                              "boss floors only: 20 -> 40 -> 60 -> 80 -> 100.\n"
                              "Enable, then start (or continue) Bloody Palace.");
        if (g_bossRush) {
            uint32_t rr = 0;
            if (readCurrentRoom(rr)) {
                bool boss = (rr==503||rr==504||rr==505||rr==506||rr==507);
                if (boss) ImGui::TextColored(ImVec4(1.0f,0.35f,0.35f,1.0f), "Boss Rush ON \xE2\x80\x94 boss floor, fight!");
                else      ImGui::TextColored(ImVec4(0.40f,1.0f,0.45f,1.0f), "Boss Rush ON \xE2\x80\x94 skipping to next boss\xE2\x80\xA6");
            } else ImGui::TextColored(ImVec4(0.40f,1.0f,0.45f,1.0f), "Boss Rush ON (start Bloody Palace)");
        }
        ImGui::TextDisabled("Use in a level. Pick Mission + Room, hit Initiate Jump.");
        ImGui::Unindent(8.0f);
    }

    // --- Camera & Window ---
    if (ImGui::CollapsingHeader("Camera & Window", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);

        // MistressDMC: Custom Camera Variables (additive; 0 = vanilla). [untested in-game]
        ImGui::Checkbox("Custom Camera Variables", &g_camVarsOn);
        hint("Additive camera tweaks (0 = vanilla). Height/Distance/FOV verified; Angle & FOV-in-battle inferred. [experimental]");
        if (g_camVarsOn) {
            ImGui::Indent(8.0f); ImGui::PushItemWidth(160.0f);
            ImGui::InputFloat("Height",            &g_cvHeight,    10.0f, 20.0f,  "%.0f");
            ImGui::InputFloat("Distance",          &g_cvDistance,  100.0f, 200.0f, "%.0f");
            ImGui::InputFloat("Distance (Lockon)", &g_cvDistLock,  100.0f, 200.0f, "%.0f");
            ImGui::InputFloat("Angle",             &g_cvAngle,     0.1f,  0.2f,   "%.2f");
            ImGui::InputFloat("FOV",               &g_cvFov,       10.0f, 20.0f,  "%.0f");
            ImGui::InputFloat("FOV (In Battle)",   &g_cvFovBattle, 10.0f, 20.0f,  "%.0f");
            ImGui::PopItemWidth();
            if (ImGui::Button("Reset Camera Variables"))
                g_cvHeight=g_cvDistance=g_cvDistLock=g_cvAngle=g_cvAngleLock=g_cvFov=g_cvFovBattle=0.0f;
            ImGui::Unindent(8.0f);
        }
        ImGui::Separator();

        // Camera tool (reapplied each frame, like Slow-Mo). Distance = the
        // COD-style zoom-out; FOV just widens the lens.
        uint8_t* cd = getCameraData();
        if (cd && !g_camOn) {                                     // mirror live values when off
            g_camHeight = *(float*)(cd + 0xD0);
            g_camDist   = *(float*)(cd + 0xD8);
        }
        // Free Third Person Camera: one toggle that disables the auto-centre AND parks the
        // camera behind the character at a third-person distance/height (tune with the
        // sliders below). Combines the two working camera patches into a "behind-the-back" view.
        bool fc = g_noCenterCam;
        if (ImGui::Checkbox("Free Third Person Camera", &fc)) {
            setDisableCenterCamera(fc);                 // no auto-recenter (safe data/NOP)
            if (fc) {                                   // park the follow-cam behind via the camera struct
                if (cd) { g_camOrigH=*(float*)(cd+0xD0); g_camOrigD=*(float*)(cd+0xD8);
                          g_camOrigDL=*(float*)(cd+0xDC); g_camSaved=true; }
                if (g_camDist < 50.0f)  g_camDist  = 800.0f;
                if (g_camHeight == 0.0f) g_camHeight = 120.0f;
                g_camOn = true;
            } else {
                if (cd && g_camSaved) { *(float*)(cd+0xD0)=g_camOrigH; *(float*)(cd+0xD8)=g_camOrigD;
                                        *(float*)(cd+0xDC)=g_camOrigDL; g_camSaved=false; }
                g_camOn = false;
            }
        }
        hint("Turns off the auto-centre and holds the camera behind the character at the "
             "Distance/Height below. (The scripted fixed-camera code-cave was removed — it crashed "
             "on this build; this safe version uses the camera struct + centre patch instead.)");
        ImGui::SameLine();
        if (ImGui::Button("Reset camera")) {            // put EVERYTHING back to vanilla
            setFreeCamFixed(false);                     // restore the hook + NOP bytes
            setDisableCenterCamera(false);              // restore the auto-centre writes
            if (g_camOn && cd && g_camSaved) {          // restore the original distance/height
                *(float*)(cd + 0xD0) = g_camOrigH;
                *(float*)(cd + 0xD8) = g_camOrigD;
                *(float*)(cd + 0xDC) = g_camOrigDL;
                g_camSaved = false;
            }
            g_camOn = false;
            logf("[cam] reset to normal");
        }
        hint("One click puts the camera fully back to vanilla: turns off free-cam, re-enables "
             "centering, and restores the original distance/height. (Unchecking Free Third Person "
             "also restores its patches on its own.)");
        if (ImGui::Checkbox("Camera tool", &g_camOn)) {
            if (g_camOn && cd) {                 // remember defaults to restore later
                g_camOrigH  = *(float*)(cd + 0xD0);
                g_camOrigD  = *(float*)(cd + 0xD8);
                g_camOrigDL = *(float*)(cd + 0xDC);
                g_camSaved  = true;
            } else if (!g_camOn && cd && g_camSaved) {   // restore defaults on disable
                *(float*)(cd + 0xD0) = g_camOrigH;
                *(float*)(cd + 0xD8) = g_camOrigD;
                *(float*)(cd + 0xDC) = g_camOrigDL;
                g_camSaved = false;
            }
        }
        ImGui::BeginDisabled(!cd);
        ImGui::SetNextItemWidth(200);
        sliderRC("Distance (zoom out / pull back)", &g_camDist, 0.0f, 4000.0f, "%.0f", 0.0f, 50000.0f);
        ImGui::SetNextItemWidth(200);
        sliderRC("Height", &g_camHeight, -800.0f, 800.0f, "%.0f", -10000.0f, 10000.0f);
        ImGui::EndDisabled();
        if (!cd) ImGui::TextDisabled("Load a level to use the camera.");
        ImGui::Unindent(8.0f);
    }

    // --- Menu music (multi-track player) ---
    if (ImGui::CollapsingHeader("Menu", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);
        ImGui::Checkbox("Music on", &g_music);
        ImGui::SameLine();
        ImGui::Checkbox("Play in game (background)", &g_bgMusic);

        if (g_curTrack >= nTracks()) g_curTrack = 0;
        if (nTracks() == 0) {
            ImGui::TextDisabled("No music found. Drop .wav files into the 'music'");
            ImGui::TextDisabled("folder beside dinput8.dll, then click Rescan.");
            if (ImGui::Button("Rescan")) scanMusicFolder();
        } else {
            // track picker: < dropdown >
            if (ImGui::Button("<")) selectTrack(g_shuffle ? randTrack() : g_curTrack - 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(300);
            if (ImGui::BeginCombo("##track", g_trackNames[g_curTrack].c_str())) {
                for (int i = 0; i < nTracks(); ++i) {
                    bool sel = (i == g_curTrack);
                    if (ImGui::Selectable(g_trackNames[i].c_str(), sel)) selectTrack(i);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button(">")) selectTrack(g_shuffle ? randTrack() : g_curTrack + 1);
            ImGui::SameLine();
            if (ImGui::Button("Rescan")) { scanMusicFolder(); if (g_curTrack >= nTracks()) g_curTrack = 0; }

            static const char* kModes[] = { "Loop this song", "Playlist (loop all)", "Playlist (play through)" };
            ImGui::SetNextItemWidth(220);
            ImGui::Combo("Mode", &g_loopMode, kModes, IM_ARRAYSIZE(kModes));
            ImGui::SameLine(); ImGui::Checkbox("Shuffle", &g_shuffle);

            ImGui::SetNextItemWidth(160);
            sliderRC("Volume", &g_musicVol, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit()) applyMusicVolume();
        }
        ImGui::Unindent(8.0f);
    }

    // --- Slow-Mo / Work Rate ---
    if (ImGui::CollapsingHeader("Slow-Mo (Work Rate)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);
        static const char* labels[4] = { "Global Work Rate", "Game Work Rate",
                                         "Player Work Rate", "Enemy Work Rate" };
        // mirror the live game values into the sliders so they track external changes
        static float wr[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        uintptr_t probe;
        bool live = readPtr(g_base + 0xF59F18, probe) && probe;
        if (!live) ImGui::TextDisabled("Load a level to enable.");
        for (int i = 0; i < 4; ++i) {
            float cur; if (getWorkRate(i, cur)) wr[i] = cur;     // reflect actual value
            ImGui::BeginDisabled(!live);
            ImGui::SetNextItemWidth(200);
            if (sliderRC(labels[i], &wr[i], 0.05f, 3.0f, "%.2fx", 0.01f, 100.0f))
                setWorkRate(i, wr[i]);
            ImGui::EndDisabled();
        }
        if (ImGui::Button("Restore (all 1.0x)")) {
            for (int i = 0; i < 4; ++i) { wr[i] = 1.0f; setWorkRate(i, 1.0f); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Slow 0.3x")) {
            for (int i = 0; i < 4; ++i) { wr[i] = 0.3f; setWorkRate(i, 0.3f); }
        }
        ImGui::TextDisabled("1.0x = normal, lower = slow motion, higher = faster.");
        // FPS cap.
        ImGui::Separator();
        static const char* fpsNames[5] = { "Default (off)", "60", "120", "240", "Unlimited" };
        static const int   fpsVals[5]  = { 0, 60, 120, 240, 10000 };
        int fpsIdx = 0; for (int i=0;i<5;i++) if (fpsVals[i]==g_fpsLimit) fpsIdx=i;
        ImGui::SetNextItemWidth(160);
        if (ImGui::Combo("FPS limit", &fpsIdx, fpsNames, 5)) g_fpsLimit = fpsVals[fpsIdx];
        hint("Caps the frame rate. NOTE: DMC4's physics is locked to 60 -- anything above 60 makes the whole game run FASTER (not just smoother). 'Unlimited' = a very high cap.");
        ImGui::Unindent(8.0f);
    }

    // --- Graphics (string-rename patches; toggle freely) ---
    if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent(8.0f);
        bool dof = g_noDOF;
        if (ImGui::Checkbox("Disable Depth of Field", &dof)) setDisableDOF(dof);
        bool mb = g_noMotionBlur;
        if (ImGui::Checkbox("Disable Motion Blur", &mb)) setDisableMotionBlur(mb);
        bool gr = g_noGodRays;
        if (ImGui::Checkbox("Disable God Rays", &gr)) setDisableGodRays(gr);
        ImGui::TextDisabled("Cleaner image; takes effect as new effects are drawn.");
        ImGui::Unindent(8.0f);
    }

    ImGui::EndChild();
    ImGui::EndTabItem();
    }   // end System

    // ===================== Section 2 =====================
    if (beginBlackTab(tr("General###s2","常规###s2"))) {
        ImGui::BeginChild("t1", ImVec2(0, -footerH));
        if (ImGui::CollapsingHeader("Difficulty (Game Mode)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::TextDisabled("Harder-than-DMD modes. Load a level before switching.");
            const char* kDiffNames[3] = { "Default", "God Must Die", "Deicide Must Die" };
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("Mode##gamemode", kDiffNames[g_diffMode])) {
                for (int i = 0; i < 3; i++)
                    if (ImGui::Selectable(kDiffNames[i], g_diffMode == i)) applyDifficulty(i);
                ImGui::EndCombo();
            }
            hint("God Must Die: you take ~2x damage, enemies hit ~4x and stay in Devil Trigger. "
                 "Deicide Must Die: ~2.5x taken, enemies ~5x + permanent DT. Default restores vanilla scaling.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Devil Trigger & Vergil", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            if (ImGui::Checkbox("Instant Trigger", &g_dtSpam)) {
                setCheatByName("No Activation Cost", g_dtSpam);
                setCheatByName("Disable Devil Trigger Drain", g_dtSpam);
            }
            hint("Infinite Devil Trigger: removes the activation cost AND stops the gauge draining, plus pins the DT \"able\" flag so it re-fires instantly on every press.");
            rightCol();
            ImGui::Checkbox("Vergil: Perfect JDC", &g_vergilJDC);
            hint("Yamato Perfect Execute: forces every Yamato charge release to count as a \"perfect\" execute -> instant perfect Just Distortion.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Combo Display", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::Checkbox("Show combo move-names", &g_comboReadout);
            ImGui::SetNextItemWidth(180);
            ImGui::SliderFloat("Text size", &g_comboScale, 0.6f, 2.0f, "%.2fx");
            ImGui::TextDisabled("Only moves named in movenames.cfg are shown.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("No Limits", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            drawCategories(3);
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Window & System", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            if (ImGui::Checkbox("Borderless", &g_borderless)) setBorderless(g_borderless);
            ImGui::Checkbox("Force Focus (keep running when alt-tabbed)", &g_forceFocus);
            ImGui::Checkbox("Pause game while menu open", &g_pauseOnOpen);
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Air Tricks", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            bool at = g_atOn;
            if (ImGui::Checkbox("Allow Trick Down (Air) without DT", &at)) {
                if (at) { if (!applyAirTrick()) at = false; }
                else stopAirTrick();
            }
            ImGui::TextDisabled("Lock-On + hold Back + Jump in the air to trick");
            ImGui::TextDisabled("downward, no Devil Trigger required (Vergil).");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Enemy Spawn", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            // Swapper: turns Scarecrow spawns into the picked enemy via the game's
            // own spawn code (most reliable -- the game loads the enemy itself).
            ImGui::TextDisabled("Swapper: Scarecrow spawns become the picked enemy.");
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("Spawn as##swap", kSpawnPick[g_spawnPickSel])) {
                for (int i = 0; i < kNSpawnPick; i++) {
                    bool s = (i == g_spawnPickSel);
                    if (ImGui::Selectable(kSpawnPick[i], s)) applySpawnPick(i);
                    if (s) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Off##swap")) applySpawnPick(0);
            ImGui::Separator();
            // Direct spawn: calls the enemy's factory and drops it in front of you.
            ImGui::TextDisabled("Direct spawn (pick + Spawn, in active gameplay):");
            ImGui::SetNextItemWidth(200);
            if (ImGui::BeginCombo("Enemy##em", kEmTypes[g_emSel].name)) {
                for (int i = 0; i < kNEmTypes; i++) {
                    bool s = (i == g_emSel);
                    if (ImGui::Selectable(kEmTypes[i].name, s)) g_emSel = i;
                    if (s) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Spawn##em")) emSpawn(kEmTypes[g_emSel].createRVA);
            ImGui::SameLine();
            if (ImGui::Button("Despawn All##em")) despawnEnemies();
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Character Switch", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::TextDisabled("Forces the Bloody Palace character. Pick one, then");
            ImGui::TextDisabled("start a BP round -- you'll play as that character.");
            for (int i = 0; i < 5; i++) {
                if (i) ImGui::SameLine();
                bool on = (g_forceChar == i);
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, g_accent);
                if (ImGui::Button(kCharNames[i])) setCharacter(i);
                if (on) ImGui::PopStyleColor();
            }
            if (ImGui::Button("Off (use menu pick)")) setCharacter(-1);
            ImGui::TextDisabled("Turn Off before story missions.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Costume", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            int tc = costumeTargetChar();
            int cur = getCostume(tc);
            ImGui::TextDisabled("Costume for %s (the Character Switch pick).", kCharNames[tc]);
            struct { const char* label; int val; } opts[3] = {
                { "Default", 0 }, { "Alt 1 (ex00)", 1 }, { "Alt 2 (ex01)", 3 }
            };
            bool first = true;
            for (int i = 0; i < 3; i++) {
                if (opts[i].val == 1 && !charHasEx00(tc)) continue;  // Trish/Lady: no ex00
                if (!first) ImGui::SameLine();
                first = false;
                bool on = (cur == opts[i].val);
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, g_accent);
                if (ImGui::Button(opts[i].label)) setCostume(tc, opts[i].val);
                if (on) ImGui::PopStyleColor();
            }
            if (!charHasEx00(tc)) ImGui::TextDisabled("(%s only has the ex01 alt.)", kCharNames[tc]);
            ImGui::TextDisabled("Applies on the next level load -- not live.");
            ImGui::Unindent(8.0f);
        }
        drawCategories(0);   // general gameplay cheats (health, damage, items, ...)
        drawCategories(1);
        drawCategories(2);   // (was Environment tab) world/enemy cheats
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // ===================== Section 4 (live move tweaks) =====================
    if (beginBlackTab(tr("Character###s4","角色###s4"))) {
        ImGui::BeginChild("t3", ImVec2(0, -footerH));

        // --- Doppelganger (spawn a fighting clone of the active character) ---
        if (ImGui::CollapsingHeader("Doppelganger", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            void* dact = activePlayer();
            int dcid = dact ? *(int*)((char*)dact + OFF_CHARID) : -1;
            ImGui::Text("Active character: %s",
                        (dcid >= 0 && dcid < 5) ? kSpawn[dcid].name : "(none -- load a save first)");
            ImGui::Text("Clone: %s", g_doppActor ? "spawned (max 2 reached)" : "none");
            if (g_doppActor) {
                ImGui::TextDisabled("Spawn clone (already have one)");
            } else if (ImGui::Button("Spawn clone")) {
                doppSpawn();
            }
            ImGui::SameLine();
            if (ImGui::Button("Despawn")) doppDespawn();
            // Summon keybind row.
            char keyName[24];
            if (g_doppKey >= 0x21 && g_doppKey <= 0x5A) snprintf(keyName, sizeof keyName, "%c", g_doppKey);
            else snprintf(keyName, sizeof keyName, "VK 0x%02X", g_doppKey);
            ImGui::Text("Summon key:");
            ImGui::SameLine();
            if (ImGui::Button(g_doppKeyCapture ? "press any key..." : keyName)) g_doppKeyCapture = !g_doppKeyCapture;
            ImGui::SameLine();
            ImGui::TextDisabled("(click to rebind, Esc cancels)");
            ImGui::Checkbox("Keep clone from merging (push apart)", &g_doppNoMerge);
            hint("The clone has no body collision, so you can stand inside it and look like one "
                 "person. This pushes it apart each frame when you overlap so you stay separate. "
                 "Soft separation (position only) -- not full physics collision.");
            ImGui::Unindent(8.0f);
        }

        initRevDefaults();
        const ImVec4 grey(0.45f, 0.45f, 0.45f, 1.0f);

        // Per-character cheats. Play as the matching character to activate.
        ImGui::Separator();
        ImGui::Checkbox("Hold set values (keep them from resetting)", &g_holdVals);
        ImGui::SameLine();
        if (ImGui::Button("Clear all pins")) holdClearAll();
        if (ImGui::CollapsingHeader("Dante", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::Checkbox("Infinite Trickster Teleport", &g_infTrickTp);
            { static const uint32_t o[] = {0x152AC,0xA4,0x4C4}; mjFloat("Gilgamesh Charge", o, 3, 0.0f, 1000.0f); }
            ImGui::Checkbox("Infinite Gilgamesh Charge", &g_infGilgamesh);
            ImGui::TextDisabled("Play as Dante, tick on -> always max charge.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Nero", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::Checkbox("Infinite Air Calibur", &g_infAirCalibur);
            ImGui::Separator();
            ImGui::TextDisabled("Exceed");
            { static const uint32_t o[] = {0xCAE4,0xA4,0x4C4}; mjInt  ("Exceed Gauge (0-3)", o, 3, 0, 3, 4); }
            ImGui::TextDisabled("Instant Exceed:");
            ImGui::SameLine(); if (ImGui::Button("Lv 1##ex")) setExceed(1);
            ImGui::SameLine(); if (ImGui::Button("Lv 2##ex")) setExceed(2);
            ImGui::SameLine(); if (ImGui::Button("Lv 3##ex")) setExceed(3);
            ImGui::SameLine(); if (ImGui::Button("Off##ex"))  setExceedOff();
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Vergil", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            drawCheatByName("Infinite Trick Up");
            // (Infinite Concentration removed by request.)
            // Beowulf charge: a simple checkbox. Weapon-gated in updateMajinPins so
            // it only fills the charge while Beowulf is equipped (safe with Force Edge).
            ImGui::Checkbox("Infinite Beowulf Charge", &g_infBeowulf);
            ImGui::TextDisabled("Equip Beowulf, tick on -> always max charge.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Trish", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            { static const uint32_t o[] = {0x77DC,0xA4,0x4C4}; mjFloat("Fast Lighting Kick Charge", o, 3, 0.0f, 1000.0f); }
            ImGui::Checkbox("Infinite Lighting Kick Charge", &g_infLightningKick);
            ImGui::TextDisabled("Play as Trish, tick on -> always max charge.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Lady", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            drawCheatByName("Infinite Grenades");
            ImGui::Unindent(8.0f);
        }

        ImGui::Separator();
        ImGui::TextColored(g_accentDim, "MistressDMC Movesets");
        ImGui::Separator();

        if (ImGui::CollapsingHeader("Live toggles (Triple Trouble)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            // Intent flags only; updateMajinPins installs each hook just while playing
            // the matching character (Charge Rate = anyone).
            ImGui::Checkbox("Charge Rate Increase", &g_crWant);
            hint("Universal: faster charge for all weapons.");
            if (g_crWant) {
                ImGui::Indent(8.0f);
                ImGui::SetNextItemWidth(160);
                float r = g_crRate;
                if (sliderRC("Charge rate", &r, 0.1f, 10.0f, "%.2f", 0.05f, 50.0f)) { g_crRate = r; setChargeRate(r); }
                ImGui::Unindent(8.0f);
            }
            ImGui::Checkbox("Height restriction bypass", &g_hrWant);
            hint("Lady/Trish/Vergil only: removes the aerial cancel height limit.");
            ImGui::Checkbox("Lady Jump Cancel", &g_ljWant);
            hint("Lady: jump-cancel her gun moves (Shotgun stinger-knockback / Trigger Happy / Gun Throw). Installs while in gameplay; the cave self-gates to Lady's moves.");
            ImGui::Unindent(8.0f);
        }

        if (ImGui::CollapsingHeader("Combat tweaks", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::TextDisabled("In-place patches; default off. Load a level before toggling.");
            // One toggle per line: a few of these labels are long, so a 2-column
            // layout overlapped them and ate the clicks. Single column keeps every
            // checkbox fully clickable at any GUI scale.
            combatPatchToggle(g_pFastTrick);    hint("Vergil: skips the trick start-up gate (snappier tricks).");
            combatPatchToggle(g_pTrickRange);   hint("Vergil: removes the trick distance limit.");
            combatPatchToggle(g_pMovingTarget); hint("Forces the moving-target lock branch.");
            combatPatchToggle(g_pKnockback);    hint("Always release on knockback.");
            combatPatchToggle(g_pInfTableHop);  hint("Nero: removes the once-per-window limit on Table Hopper (the Devil Bringer evade) -- hop as often as you like.");
            combatPatchToggle(g_pSuperTableHop);hint("Nero: changes the Table Hopper count/variant (mov ecx,3 -> 0) for the 'super' behaviour.");
            { bool sr = g_snatchOn;
              if (ImGui::Checkbox("Increased Snatch Range", &sr)) { if (sr) applySnatchRange(); else stopSnatchRange(); }
              hint("Nero: triples Snatch / Devil Bringer reach (snatch-length forced to 2550.0f = 3x Lv3). Load a level before toggling. [experimental]"); }
            combatPatchToggle(g_pEnemyAIMax);   hint("Enemies fight at maximum aggression -- no passive/idle backoff. Flips the AI gate at 0x564C68 (jb->ja). Ported from the Non-JP cheat table.");
            combatPatchToggle(g_pFreeSwords);    hint("Vergil: Summoned Swords / Spiral / Storm / Blistering / Heavy Rain cost no Devil Trigger -- summon them freely. (NOPs the DT-debit at 0x4D004B.)");
            ImGui::Checkbox("Super Cancel (everything cancellable)", &g_superCancel);
            hint("Keeps every move cancellable -- any move's recovery/stiffness can be cancelled straight into the next action, so you can chain moves with no recovery (advanced combo tech). Pins the 8 cancel-table slots to 2.");
            ImGui::Checkbox("Easy Quick Drive", &g_easyQuickDrive);
            hint("Dante: Prop is cancellable into Quick Drive during its first frames (~frame 7). [experimental]");

            ImGui::Spacing();
            bool dm = g_dmgOn;
            if (ImGui::Checkbox("Player Damage Modifier", &dm)) { if (dm) applyDamageMod(); else stopDamageMod(); }
            hint("Scales damage you take. 0 = none, 1 = normal, >1 = more.");
            if (g_dmgOn) {
                ImGui::Indent(8.0f);
                ImGui::SetNextItemWidth(160);
                float m = g_dmgMult;
                if (sliderRC("Damage taken", &m, 0.0f, 3.0f, "%.2fx", 0.01f, 10.0f)) setDamageMult(m);
                ImGui::Unindent(8.0f);
            }
            ImGui::Unindent(8.0f);
        }

        if (ImGui::CollapsingHeader("Move Speed (animation rate)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            const ImVec4 grey(0.45f, 0.45f, 0.45f, 1.0f);
            bool avail = speedAvailable();
            ImGui::BeginDisabled(!avail);
            bool on = g_speedOn;
            if (ImGui::Checkbox("Enable move-speed overrides", &on)) {
                if (on) enableSpeedCave(); else disableSpeedCave();
            }
            ImGui::EndDisabled();
            if (!avail) { ImGui::SameLine(); ImGui::TextColored(grey, "(n/a)"); }
            ImGui::TextDisabled("1.0x = normal, higher = faster.");
            for (int k = 0; k < kNSpeed; ++k) {
                ImGui::SetNextItemWidth(180);
                if (sliderRC(kSpeedMoves[k].name, &g_speedVal[k], 0.25f, 8.0f, "%.2fx", 0.01f, 100.0f)) {
                    if (g_speedOn && g_speedData) g_speedData[k] = g_speedVal[k];   // live update
                }
            }
            if (ImGui::Button("Reset speeds")) {
                for (int k = 0; k < kNSpeed; ++k) {
                    g_speedVal[k] = kSpeedMoves[k].def;
                    if (g_speedOn && g_speedData) g_speedData[k] = g_speedVal[k];
                }
            }
            ImGui::Unindent(8.0f);
        }

        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // ===================== Section 5: DEBUG (experimental) =====================
    if (beginBlackTab("DEBUG###s5")) {
        ImGui::BeginChild("t5", ImVec2(0, -footerH));

        // --- Replay: Input Record / Playback -------------------------------
        if (ImGui::CollapsingHeader("Replay (Input Record / Playback)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            const int   st  = g_recState;                       // 0 idle 1 rec 2 play
            const float secs = (float)MACRO_MAX_FRAMES / 60.0f;  // 30 s @ 60 fps
            // status line
            if (st == 1) ImGui::TextColored(ImVec4(1.0f,0.35f,0.35f,1.0f),
                              "RECORDING  -  %d frames (%.1fs)", g_recLen, g_recLen / 60.0f);
            else if (st == 2) ImGui::TextColored(ImVec4(0.40f,1.0f,0.45f,1.0f),
                              "%s  -  %d / %d", g_replayLoop ? "LOOPING" : "PLAYING", g_playIdx, g_recLen);
            else ImGui::TextDisabled("Idle  -  %d frames buffered (%.1fs)", g_recLen, g_recLen / 60.0f);
            // buffer gauge
            ImGui::ProgressBar((float)g_recLen / (float)MACRO_MAX_FRAMES, ImVec2(-1, 6.0f), "");
            ImGui::TextDisabled("Buffer: up to %d frames (~%.0fs at 60 fps).", MACRO_MAX_FRAMES, secs);
            // transport buttons (mirror the F2/F3/F4 hotkeys)
            if (ImGui::Button(st == 1 ? "Stop Record" : "Record")) macroToggleRecord();
            ImGui::SameLine();
            ImGui::BeginDisabled(g_recLen == 0 && st != 2);
            if (ImGui::Button(st == 2 && !g_replayLoop ? "Stop Play" : "Play")) macroTogglePlay();
            ImGui::SameLine();
            if (ImGui::Button(st == 2 && g_replayLoop ? "Stop Loop" : "Auto-Replay")) macroToggleLoop();
            hint("Auto-Replay (F4) loops the recording continuously until you stop it. "
                 "Great for practising against a repeating input pattern.");
            ImGui::EndDisabled();
            ImGui::Separator();
            // overrides
            ImGui::Checkbox("Live Override", &g_liveOverride);
            hint("OFF (default) = faithful full replay: the recording drives every frame and your "
                 "live input is ignored -- keep this off to just play a take back. ON = the frames "
                 "YOU are actively driving use your live input instead (e.g. hold lock-on and it "
                 "won't fight you); handy for punch-in but it suppresses playback while you hold inputs.");
            ImGui::BeginDisabled(!g_liveOverride);
            ImGui::Indent(16.0f);
            ImGui::Checkbox("Record Override (punch-in)", &g_recordOverride);
            hint("While Live Override is steering a frame, write that live input back into the take "
                 "-- punch-in editing: re-record just the part you override, keep the rest.");
            ImGui::Unindent(16.0f);
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextDisabled("Hotkeys: Record F2 / Play F3 / Auto-Replay F4, rebindable in the");
            ImGui::TextDisabled("hotkey list -- they work even while this menu is hidden.");
            ImGui::TextDisabled("Captures BOTH the controller (XInput: Xbox, and DS4/DS5 via Steam");
            ImGui::TextDisabled("Input / DS4Windows) AND the keyboard (DirectInput), every frame.");
            ImGui::Separator();
            ImGui::Checkbox("Diagnose input coverage -> overlay.log", &g_inpDiag);
            hint("Fixing 'only some buttons replay': logs [inpdiag] lines comparing the game's merged "
                 "button state to what XInput carries. Turn on, then in gameplay press attack, gun, "
                 "style, and weapon-switch once each so I can see which inputs reach XInput.");
            ImGui::Unindent(8.0f);
        }

        // --- Move-state logger (RE diagnostics) --- DEV-only: omitted from public builds.
        // Build with -DMOOD_DEV to expose it locally; the shipped DLL does not include it.
#ifdef MOOD_DEV
        if (ImGui::CollapsingHeader("Move-state logger", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::Checkbox("Log move state to overlay.log", &g_diagOn);
            hint("Writes [mv] lines (action id, atck id, height, velocity, DT, byte window) to overlay.log on every action change. For reverse-engineering move ids / gates / flags. Leave off for normal play.");
            ImGui::Unindent(8.0f);
        }
#endif

        // --- Vergil: Infinite Concentration ---
        if (ImGui::CollapsingHeader("Vergil", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            if (ImGui::Checkbox("Infinite Concentration (Lv.3)", &g_infConc)) {
                if (g_infConc) applyCombatPatch(g_pConcFix);   // freeze the game's own write
                else { stopCombatPatch(g_pConcFix); }
            }
            hint("Pins Vergil's concentration at Lv.3 (the game's real max) -> full-power Judgement "
                 "Cut End anytime. From the Non-JP debug table. Vergil only.");
            ImGui::Unindent(8.0f);
        }

        // --- HUD (per-element show/hide, 4Hook-style) ---
        if (ImGui::CollapsingHeader("HUD", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            ImGui::TextDisabled("Check the pieces to KEEP -- everything else hides");
            ImGui::TextDisabled("automatically. (Or just 'Hide all' for a clean screen.)");
            ImGui::Checkbox("Hide all HUD", &g_hudHideAll);
            ImGui::Indent(16.0f);
            if (ImGui::Checkbox("Keep weapons showing", &g_keepWeapons))
                setWeaponMode(g_keepWeapons ? 1 : 0);
            ImGui::TextDisabled("Keep these HUD pieces:");
            for (int i = 0; i < kNKeep; i++) {
                ImGui::PushID(i);
                ImGui::Checkbox(g_keep[i].name, &g_keep[i].on);
                ImGui::PopID();
            }
            ImGui::Unindent(16.0f);
            ImGui::Separator();
            ImGui::TextDisabled("Weapon HUD (standalone):");
            int wm = g_wpnMode;
            if (ImGui::RadioButton("Normal##wpn", wm == 0)) setWeaponMode(0); ImGui::SameLine();
            if (ImGui::RadioButton("Always show##wpn", wm == 1)) setWeaponMode(1); ImGui::SameLine();
            if (ImGui::RadioButton("Always hide##wpn", wm == 2)) setWeaponMode(2);
            ImGui::TextDisabled("(Weapon control works on Dante's weapon HUD.)");
            ImGui::Unindent(8.0f);
        }

        if (ImGui::CollapsingHeader("Jump Cancel Window", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            bool jc = g_jcOn;
            if (ImGui::Checkbox("Increased jump cancel range", &jc)) {
                if (jc) applyJumpCancel(); else stopJumpCancel();
            }
            ImGui::BeginDisabled(!g_jcOn);
            float m = g_jcMult;
            ImGui::SetNextItemWidth(180);
            if (sliderRC("Window size (1.0 = vanilla)", &m, 1.0f, 5.0f, "%.2fx", 0.5f, 20.0f))
                setJumpCancelMult(m);
            ImGui::EndDisabled();
            ImGui::TextDisabled("Bigger window = easier jump cancels off enemies.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Unlock moves & weapons (no requirements)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            uintptr_t sp;
            bool save = savePtr(sp);
            ImGui::TextDisabled(save ? "Save data found. Click a character to unlock all."
                                     : "Load a save / level first.");
            ImGui::TextDisabled("Sets weapons, styles & abilities, and maxes Proud Souls");
            ImGui::TextDisabled("so any remaining move is free to learn in the menu.");
            ImGui::BeginDisabled(!save);
            if (ImGui::Button("Unlock Dante"))  unlockDante();  ImGui::SameLine();
            if (ImGui::Button("Unlock Nero"))   unlockNero();   ImGui::SameLine();
            if (ImGui::Button("Unlock Vergil")) unlockVergil();
            if (ImGui::Button("Unlock Trish"))  unlockTrish();  ImGui::SameLine();
            if (ImGui::Button("Unlock Lady"))   unlockLady();   ImGui::SameLine();
            if (ImGui::Button("Unlock EVERYONE")) {
                unlockDante(); unlockNero(); unlockVergil(); unlockTrish(); unlockLady();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("Tip: open Customize after unlocking to see the moves.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Gameplay restores", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            bool luc = g_lucOn;
            if (ImGui::Checkbox("Restore Lucifer Bug", &luc)) {
                if (luc) applyLuciferBug(); else stopLuciferBug();
            }
            ImGui::TextDisabled("Reverts Dante's Lucifer to the original 2008 DMC4 feel");
            ImGui::TextDisabled("(needle cap 128 -> 16, whole rose set detonates at once).");
            { bool rp = g_rosePins != 0;
              if (ImGui::Checkbox("Rose Removes Pins", &rp)) g_rosePins = rp ? 1 : 0;
              hint("Rose despawns pins instead of detonating them. [experimental]"); }
            { bool fl = (g_forceLucifer != 0);
              if (ImGui::Checkbox("Force Lucifer", &fl)) { g_forceLucifer = fl ? 1 : 0; if (fl) applyForceLucifer(); }
              hint("Lucifer's rose stays loaded across weapon changes (never force-despawned). Load a level first. [experimental]"); }
            bool fh = g_fhOn;
            if (ImGui::Checkbox("Fix Full House", &fh)) {
                if (fh) applyFullHouseFix(); else stopFullHouseFix();
            }
            ImGui::TextDisabled("Restores vanilla Full House inertia behaviour DMC4SE broke.");
            ImGui::TextDisabled("Both take effect immediately; load a level first.");
            ImGui::Separator();
            ImGui::TextDisabled("Skip weapons while cycling (Dante): [experimental]");
            { bool ss=g_skipShotgun, sp=g_skipPandora, sg=g_skipGilgamesh, sl=g_skipLucifer;
              if (ImGui::Checkbox("Skip Shotgun", &ss))   { g_skipShotgun=ss?1:0;   if(ss) g_skipPandora=0;   skipSyncGun(); }
              if (ImGui::Checkbox("Skip Pandora", &sp))   { g_skipPandora=sp?1:0;   if(sp) g_skipShotgun=0;   skipSyncGun(); }
              if (ImGui::Checkbox("Skip Gilgamesh", &sg)) { g_skipGilgamesh=sg?1:0; if(sg) g_skipLucifer=0;   skipSyncSword(); }
              if (ImGui::Checkbox("Skip Lucifer", &sl))   { g_skipLucifer=sl?1:0;   if(sl) g_skipGilgamesh=0; skipSyncSword(); }
              ImGui::TextDisabled("Shotgun<->Pandora and Gilgamesh<->Lucifer are mutually exclusive."); }
            ImGui::Separator();
            ImGui::TextDisabled("Skip weapons while cycling (Vergil): [experimental]");
            { bool sy=g_skipYamato, sb2=g_skipBeowulf, sf=g_skipForceEdge;
              int cnt = (sy?1:0) + (sb2?1:0) + (sf?1:0);   // at least one weapon must remain
              ImGui::BeginDisabled(!sy  && cnt >= 2);
              if (ImGui::Checkbox("Skip Yamato", &sy))     { g_skipYamato=sy?1:0;     skipSyncSword(); }
              ImGui::EndDisabled();
              ImGui::BeginDisabled(!sb2 && cnt >= 2);
              if (ImGui::Checkbox("Skip Beowulf", &sb2))   { g_skipBeowulf=sb2?1:0;   skipSyncSword(); }
              ImGui::EndDisabled();
              ImGui::BeginDisabled(!sf  && cnt >= 2);
              if (ImGui::Checkbox("Skip Force Edge", &sf)) { g_skipForceEdge=sf?1:0;  skipSyncSword(); }
              ImGui::EndDisabled();
              ImGui::TextDisabled("Skip up to 2 (at least one weapon must remain)."); }
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Boot / Opening Movie", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent(8.0f);
            bool sb = g_skipBoot;
            if (ImGui::Checkbox("Skip Boot Logos / Opening (→ title instantly)", &sb))
                setSkipBoot(sb);
            ImGui::TextDisabled("Stubs the boot opening cinematic (adv_for.wmv) so launch");
            ImGui::TextDisabled("drops straight to the title screen. Original kept as .orig;");
            ImGui::TextDisabled("uncheck to restore. Takes effect on the NEXT game launch.");
            ImGui::Checkbox("Skip warning screens (auto-confirm)", &g_skipWarnings);
            ImGui::TextDisabled("Auto-presses confirm a few times at boot to blow past the");
            ImGui::TextDisabled("display / autosave caution screens + 'Press Start' into the menu.");
            ImGui::Unindent(8.0f);
        }
        if (ImGui::CollapsingHeader("Hotkeys")) {
            ImGui::Indent(8.0f);
            ImGui::TextDisabled("Bind any action to a key (saved with the profile), or click Trigger.");
            for (int h = 0; h < kNHotkeys; h++) {
                ImGui::PushID(h);
                ImGui::Text("%s", kHotkeys[h].name);
                ImGui::SameLine(170);
                char kn[32];
                if (g_hkCap[h])                                   snprintf(kn, sizeof kn, "press key/button...");
                else if (g_hkKey[h] & 0x10000) {   // controller button
                    WORD b = g_hkKey[h] & 0xFFFF; const char* bn = "Pad";
                    switch (b) { case 0x1:bn="D-Up";break; case 0x2:bn="D-Down";break; case 0x4:bn="D-Left";break;
                        case 0x8:bn="D-Right";break; case 0x10:bn="Start";break; case 0x20:bn="Back";break;
                        case 0x40:bn="L3";break; case 0x80:bn="R3";break; case 0x100:bn="LB";break; case 0x200:bn="RB";break;
                        case 0x1000:bn="A";break; case 0x2000:bn="B";break; case 0x4000:bn="X";break; case 0x8000:bn="Y";break; }
                    snprintf(kn, sizeof kn, "[ Pad: %s ]", bn);
                }
                else if (g_hkKey[h] >= 0x30 && g_hkKey[h] <= 0x5A) snprintf(kn, sizeof kn, "[ %c ]", g_hkKey[h]);
                else if (g_hkKey[h])                              snprintf(kn, sizeof kn, "[ VK 0x%02X ]", g_hkKey[h]);
                else                                             snprintf(kn, sizeof kn, "[ set key ]");
                if (ImGui::Button(kn)) g_hkCap[h] = !g_hkCap[h];
                ImGui::SameLine();
                if (ImGui::Button("Trigger")) kHotkeys[h].fn();
                if (g_hkKey[h]) { ImGui::SameLine(); if (ImGui::SmallButton("clear")) g_hkKey[h] = 0; }
                ImGui::PopID();
            }
            ImGui::Unindent(8.0f);
        }

        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    // ===================== Section 6: Mods (skins + BP) =====================
    if (beginBlackTab("Mutators###s7")) {
        ImGui::BeginChild("t7", ImVec2(0, -footerH));
        mutatorDrawTab();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (beginBlackTab("Co-op###s8")) {
        ImGui::BeginChild("t8", ImVec2(0, -footerH));
        coopDrawTab();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (beginBlackTab("Mods###s6")) {
        ImGui::BeginChild("t6", ImVec2(0, -footerH));
        ImGui::TextDisabled("Put each mod in its own folder inside the MODS folder next to");
        ImGui::TextDisabled("dinput8.dll, then Reload. Pick a skin per character or a Bloody");
        ImGui::TextDisabled("Palace mod below (HDD priority -- your game files stay untouched).");
        ImGui::TextDisabled("Reload the level / BP round to apply.");
        ImGui::Spacing();

        // Loose-file (HDD) loading: read a loose file in nativeDX10\ over the .arc copy.
        if (ImGui::Checkbox("Loose-file loading (HDD)", &g_looseLoad)) {
            hddCacheTick();          // engine flag for loose standalone files
            rebuildRedir();          // swap in / out the MODS\HDD override arcs (GUI, etc.)
            saveActiveMods();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(g_looseArmed ? "(ON)" : (g_looseLoad ? "(arming...)" : "(off)"));
        hint("ON swaps the GUI / menu look (and any override arc in the MODS\\HDD folder) and "
             "lets loose standalone files under nativeDX10\\ load; OFF restores the stock GUI. "
             "It does NOT cover character skins or animations -- those load from the arc in "
             "memory, so for characters, costumes and movesets use the MODS folder (packaged "
             ".arc) instead. Reload the level / back out of the menu to apply.");
        ImGui::Spacing();

        if (g_mods.empty()) {
            ImGui::TextColored(g_accent, "No mods found. Add a folder in MODS and Reload.");
            if (ImGui::Button("Reload mod list")) { loadModList(); loadActiveMods(); }
        } else {
            const char* prevCh = "";
            for (int slot = 0; slot < kNSlots; slot++) {
                if (strcmp(kSlots[slot].ch, prevCh) != 0) {     // group header per character
                    prevCh = kSlots[slot].ch;
                    ImGui::Spacing();
                    ImGui::TextColored(g_accent, "%s", strcmp(prevCh,"BP")==0 ? "Bloody Palace" : prevCh);
                }
                int sel = g_modSel[slot];
                const char* cur = (sel >= 0 && sel < (int)g_mods.size()) ? g_mods[sel].name.c_str() : "(none)";
                char lbl[48]; snprintf(lbl, sizeof lbl, "%s##mods%d", kSlots[slot].label, slot);
                ImGui::SetNextItemWidth(330);
                if (ImGui::BeginCombo(lbl, cur)) {
                    if (ImGui::Selectable("(none)", sel < 0)) selectMod(slot, -1);
                    for (int i = 0; i < (int)g_mods.size(); i++) {
                        if (!modEligible(g_mods[i], slot)) continue;
                        ImGui::PushID(i);
                        bool s = (sel == i);
                        if (ImGui::Selectable(g_mods[i].name.c_str(), s)) selectMod(slot, i);
                        if (s) ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }
            // Other / unsorted mods (anything whose character couldn't be detected) --
            // checkbox each; enabling redirects all of its arcs.
            int nOther = 0; for (auto& m : g_mods) if (m.ch == "Other") nOther++;
            if (nOther) {
                ImGui::Spacing();
                ImGui::TextColored(g_accent, "Other / unsorted (%d)", nOther);
                for (int i = 0; i < (int)g_mods.size(); i++) {
                    if (g_mods[i].ch != "Other") continue;
                    ImGui::PushID(1000 + i);
                    bool on = g_mods[i].otherOn;
                    if (ImGui::Checkbox(g_mods[i].name.c_str(), &on)) { g_mods[i].otherOn = on; rebuildRedir(); saveActiveMods(); }
                    ImGui::PopID();
                }
            }
            ImGui::Separator();
            ImGui::Text("Active arc redirects: %d", (int)g_redir.size());
            ImGui::SameLine();
            ImGui::TextDisabled(g_cfHook ? "(HDD ON, fired %ux)" : "(installing...)", g_modHits);
            if (ImGui::Button("Reload mod list")) { loadModList(); loadActiveMods(); }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();

    ImGui::Separator();
    ImGui::TextColored(g_accentDim, tr("made by MistressDMC","由 MistressDMC 制作"));

    // ---- Bottom-right options panel: a little two-tab bar pinned to the corner.
    // Tab 1 = language (Chinese on/off), Tab 2 = theme (red <-> blue). Right-align
    // it on the footer row so it sits in the window's bottom-right corner.
    {
        const float panelW = 250.0f;
        ImGui::SameLine();
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > panelW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - panelW));
        ImGui::BeginGroup();
        if (ImGui::BeginTabBar("##opts", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
            if (beginBlackTab(tr("Language###optlang","语言###optlang"))) {
                ImGui::Checkbox(tr("Chinese (Simplified)###zh","中文（简体）###zh"), &g_zh);
                ImGui::EndTabItem();
            }
            if (beginBlackTab(tr("Theme###opttheme","主题###opttheme"))) {
                if (ImGui::Checkbox(tr("Blue GUI (instead of red)###blue","蓝色界面（替代红色）###blue"), &g_blue))
                    applyTheme(g_blue);
                ImGui::EndTabItem();
            }
            if (beginBlackTab(tr("Display###optdisp","显示###optdisp"))) {
                // GUI scale: drives font AND every style size together, so the whole
                // menu grows/shrinks as one. Auto-DPI fits the base to the screen
                // (720p..4K); the slider multiplies on top. Effective = shown below.
                ImGui::Checkbox(tr("Auto-fit to resolution###autodpi","自动适应分辨率###autodpi"), &g_uiAutoDPI);
                hint("Sizes the menu to the render resolution so it's readable from 1280x720 up to 4K and beyond. Turn off for a fixed size.");
                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat(tr("GUI Scale###guiscale","界面缩放###guiscale"), &g_uiScale, 0.50f, 3.00f, "%.2fx");
                hint("Bigger = bigger menu and bigger text. Applies live.");
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Reset###guiscalereset","重置###guiscalereset"))) g_uiScale = 1.00f;
                ImGui::TextDisabled(tr("render %dx%d  -  effective %.2fx","渲染 %dx%d  -  实际 %.2fx"),
                                    (int)g_bbW, (int)g_bbH, effectiveUiScale());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndGroup();
    }
    ImGui::End();
}

// DMC4SE reads the keyboard via DirectInput, so window key messages never reach
// our WndProc. We poll the physical key state each frame instead and feed ImGui
// directly. This is what actually makes the 7 toggle and the search box work.
static void feedKey(ImGuiKey k, int vk) {
    ImGui::GetIO().AddKeyEvent(k, (GetAsyncKeyState(vk) & 0x8000) != 0);
}
// Read controller buttons via XInput (the game already loads xinput1_3). wButtons
// is at offset 4 of XINPUT_STATE; LEFT_THUMB=0x40 (L3), RIGHT_THUMB=0x80 (R3).
typedef DWORD(WINAPI* XIGetState_t)(DWORD, void*);
static bool readPadButtons(WORD& out) {
    static XIGetState_t fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const char* dlls[] = {"xinput1_3.dll","xinput1_4.dll","xinput9_1_0.dll","xinput1_2.dll","xinput1_1.dll"};
        for (auto d : dlls) { HMODULE m = GetModuleHandleA(d); if (!m) m = LoadLibraryA(d);
                              if (m) { fn = (XIGetState_t)GetProcAddress(m, "XInputGetState"); if (fn) break; } }
    }
    if (!fn) return false;
    uint8_t buf[16] = {0};
    if (fn(0, buf) != 0) return false;           // ERROR_SUCCESS == 0
    out = *(WORD*)(buf + 4); return true;
}

static void pollInput() {
    ImGuiIO& io = ImGui::GetIO();
    bool fg = (GetForegroundWindow() == g_window);
    static bool oPrev = false;
    bool oNow = fg && (GetAsyncKeyState('7') & 0x8000) != 0;
    if (oNow && !oPrev && !io.WantTextInput) g_show = !g_show;   // edge-triggered toggle
    oPrev = oNow;
    // Doppelganger summon hotkey. When the menu arms a rebind, the next key press
    // becomes the new bind (Esc cancels); otherwise summon on the edge.
    if (g_doppKeyCapture) {
        for (int vk = 0x08; vk <= 0xFE; vk++) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL) continue;
            if (GetAsyncKeyState(vk) & 0x8000) {
                if (vk != VK_ESCAPE) g_doppKey = vk;
                g_doppKeyCapture = false;
                break;
            }
        }
    } else {
        static bool sumPrev = false;
        bool sumNow = fg && g_doppKey && (GetAsyncKeyState(g_doppKey) & 0x8000) != 0;
        if (sumNow && !sumPrev && !io.WantTextInput) {
            if (g_doppActor) doppDespawn();   // toggle: key spawns, then despawns, ...
            else             doppSpawn();
        }
        sumPrev = sumNow;
    }
    // Controller state, read once. Hotkeys can be bound to a CONTROLLER button too:
    // such binds are stored as 0x10000 | <XInput button bit> so they coexist with VK codes.
    WORD pad = 0; bool padOk = readPadButtons(pad);
    auto padFirstBit = [](WORD b)->WORD { for (WORD m = 1; m; m <<= 1) if (b & m) return m; return 0; };
    // Bindable hotkeys: arm-capture grabs the next KEY or CONTROLLER BUTTON; otherwise fire on rising edge.
    for (int h = 0; h < kNHotkeys; h++) {
        if (g_hkCap[h]) {
            bool bound = false;
            for (int vk = 0x08; vk <= 0xFE; vk++) {
                if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL) continue;
                if (GetAsyncKeyState(vk) & 0x8000) {
                    if (vk != VK_ESCAPE) g_hkKey[h] = vk;
                    g_hkCap[h] = false; bound = true; break;
                }
            }
            if (!bound && padOk && pad) { g_hkKey[h] = 0x10000 | padFirstBit(pad); g_hkCap[h] = false; }
            continue;
        }
        if (g_hkKey[h]) {
            bool down;
            if (g_hkKey[h] & 0x10000) down = fg && padOk && (pad & (g_hkKey[h] & 0xFFFF));  // controller button
            else                      down = fg && (GetAsyncKeyState(g_hkKey[h]) & 0x8000) != 0;
            if (down && !g_hkPrev[h] && !io.WantTextInput) kHotkeys[h].fn();
            g_hkPrev[h] = down;
        }
    }
    // L3 + R3 (click both sticks) also toggles the menu, like the 7 key.
    static bool l3r3Prev = false;
    bool l3r3 = fg && padOk && (pad & 0x40) && (pad & 0x80);
    if (l3r3 && !l3r3Prev) g_show = !g_show;
    l3r3Prev = l3r3;
    if (!fg) return;
    // edit/navigation keys (reflect held state so ImGui handles repeat + release)
    feedKey(ImGuiKey_Backspace, VK_BACK);   feedKey(ImGuiKey_Delete, VK_DELETE);
    feedKey(ImGuiKey_LeftArrow, VK_LEFT);   feedKey(ImGuiKey_RightArrow, VK_RIGHT);
    feedKey(ImGuiKey_Home, VK_HOME);        feedKey(ImGuiKey_End, VK_END);
    feedKey(ImGuiKey_Enter, VK_RETURN);     feedKey(ImGuiKey_Escape, VK_ESCAPE);
    // printable characters for the search box
    if (io.WantTextInput) {
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        static bool cprev[256] = {false};
        for (int vk = 0x08; vk < 256; ++vk) {
            bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            bool pressed = down && !cprev[vk];
            cprev[vk] = down;
            if (!pressed) continue;
            unsigned int ch = 0;
            if (vk >= 'A' && vk <= 'Z')      ch = shift ? (unsigned)vk : (unsigned)(vk + 32);
            else if (vk >= '0' && vk <= '9') ch = shift ? 0 : (unsigned)vk;
            else if (vk == VK_SPACE)         ch = ' ';
            // numeric entry for the right-click "type a value" slider popups
            else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ch = (unsigned)('0' + (vk - VK_NUMPAD0));
            else if (vk == VK_OEM_PERIOD || vk == VK_DECIMAL)  ch = '.';
            else if (vk == VK_OEM_MINUS  || vk == VK_SUBTRACT) ch = '-';
            if (ch) io.AddInputCharacter(ch);
        }
    }
}

// ---- Auto-skip the boot warning screens -------------------------------------
// After the (stubbed) logo movie, DMC4SE shows two A-NEXT caution screens (display
// warning + autosave warning) then the title's "Press Start". They poll the pad via
// XInput (the exe imports XINPUT1_3.dll), so OS synthetic input does nothing under Wine.
// Instead we HOOK XInputGetState and OR-in A+START for the game itself to read -- the
// reliable in-process route. We inject a few button "presses" (down edges) in the first
// seconds, capped so it lands at the menu and never bleeds into gameplay.
#define XI_GAMEPAD_A     0x1000
#define XI_GAMEPAD_START 0x0010
typedef DWORD (WINAPI* XIGetState_t)(DWORD, void*);
static XIGetState_t oXInputGetState = nullptr;
static DWORD WINAPI hkXInputGetState(DWORD idx, void* pState) {
    DWORD r = oXInputGetState(idx, pState);
    if (g_skipWarnings && idx == 0 && pState && r == 0 /*ERROR_SUCCESS*/) {
        static DWORD t0 = 0, lastPhase = 999; static int presses = 0;
        DWORD now = GetTickCount(); if (!t0) t0 = now;
        if (now - t0 < 12000 && presses <= 3) {            // first 12s, up to 3 confirms
            DWORD phase = (now - t0) % 750;                 // press ~160ms every 750ms
            if (phase < 160 && lastPhase >= 160) presses++; // count the rising edge
            if (phase < 160 && presses <= 3) {
                *(WORD*)((char*)pState + 4) |= (XI_GAMEPAD_A | XI_GAMEPAD_START); // wButtons@+4
                (*(DWORD*)pState)++;                         // bump dwPacketNumber -> state changed
            }
            lastPhase = phase;
        }
    }
    // ---- Replay: controller stream -- stage (record) or inject (playback) ------
    // No frame advance here: replayFrameTick() in hkPresent owns the clock so the
    // pad and keyboard streams stay in lockstep. We only read/write THIS frame's pad
    // at the current cursor. Only touch a connected controller (r == 0).
    if (idx == 0 && pState && r == 0 /*ERROR_SUCCESS*/) {
        PadFrame* livep = (PadFrame*)((char*)pState + 4);   // XINPUT_GAMEPAD @ +4
        int st = g_recState, i = g_playIdx;
        if (st == 1) {                                       // recording: stage live pad
            g_stagePad = *livep;
        } else if (st == 2 && i < g_recLen) {                // playing
            bool driving = g_liveOverride && (padActive(*livep) || keysActive(g_stageKeys));
            if (driving) { if (g_recordOverride) g_padRec[i] = *livep; }   // punch-in
            else { *livep = g_padRec[i]; (*(DWORD*)pState)++; }            // inject recorded pad
        }
        g_macroLive = livep->wButtons;                       // menu activity readout
        g_lastXiPad = *livep;                                // for the input diagnostic
    }
    return r;
}
// Input coverage diagnostic: compares the game's MERGED button state [actor+0x192c]
// (what the action system actually reacts to) against what the XInput layer carries.
// If pressing gun/style/switch changes merged192c but NOT xiBtn, those inputs don't
// reach XInput -> the macro (which records XInput) can't see them, and we must capture
// at the game's own input instead. Toggle in the Replay panel; logs to overlay.log.
static void inpDiagTick() {
    if (!g_inpDiag) return;
    char* a = (char*)activePlayer();
    if (!a || !memReadable(a, 0x1940)) return;
    static uint32_t lastMerged = 0xFFFFFFFF; static uint16_t lastXi = 0xFFFF;
    uint32_t merged = *(volatile uint32_t*)(a + 0x192c);
    uint16_t xi = g_lastXiPad.wButtons;
    if (merged != lastMerged || xi != lastXi) {
        lastMerged = merged; lastXi = xi;
        logf("[inpdiag] merged192c=0x%08X  xiBtn=0x%04X xiLT=%u xiRT=%u",
             merged, xi, g_lastXiPad.bLT, g_lastXiPad.bRT);
    }
}

// ---- Replay: keyboard stream (DirectInput) ---------------------------------
// DMC4SE reads the keyboard through DirectInput: it creates a keyboard device and
// calls IDirectInputDevice8::GetDeviceState(256, buf) once per frame (buf = 256 DIK
// scancodes, 0x80 = down). We hook that call to stage (record) / inject (playback)
// the keyboard, exactly like the pad -- so keyboard-only players are fully captured
// and replayed, and pad players just record an all-released keyboard (harmless).
// We reach the device by hooking IDirectInput8::CreateDevice (vtable[3]) and, when
// it hands back GUID_SysKeyboard, hooking that device's GetDeviceState (vtable[9]).
// COM vtables are shared per class, so each slot is patched once; GetDeviceState is
// filtered to cb==256 so mouse/joystick reads through the same slot pass straight
// through. Patching an .rdata vtable slot mirrors how the rest of MOOD patches code.
static const GUID kGUID_SysKeyboard =
    {0x6F1D2B61,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
typedef HRESULT (WINAPI *DIGetState_t)(void*, DWORD, void*);
typedef HRESULT (WINAPI *DICreateDev_t)(void*, const GUID*, void**, void*);
static DIGetState_t  oKbdGetState  = nullptr;
static DICreateDev_t oCreateDevice = nullptr;
// Swap one shared-vtable slot for our hook (once). Returns the original slot value.
static void* patchVtableSlot(void* obj, int idx, void* hook) {
    void** vt = *(void***)obj;
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    void* orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vt[idx], sizeof(void*));
    return orig;
}
static HRESULT WINAPI hkKbdGetState(void* dev, DWORD cb, void* buf) {
    HRESULT hr = oKbdGetState(dev, cb, buf);
    if (SUCCEEDED(hr) && buf && cb == 256) {                  // keyboard state only
        uint8_t* keys = (uint8_t*)buf;
        int st = g_recState, i = g_playIdx;
        if (st == 1) {                                        // recording: stage live keys
            memcpy(g_stageKeys, keys, 256);
        } else if (st == 2 && i < g_recLen) {                 // playing
            bool driving = g_liveOverride && (keysActive(keys) || padActive(g_stagePad));
            if (driving) { if (g_recordOverride) memcpy(g_keyRec[i], keys, 256); }  // punch-in
            else memcpy(keys, g_keyRec[i], 256);              // inject recorded keys
        }
    }
    return hr;
}
static HRESULT WINAPI hkCreateDevice(void* self, const GUID* rguid, void** dev, void* outer) {
    HRESULT hr = oCreateDevice(self, rguid, dev, outer);
    if (SUCCEEDED(hr) && dev && *dev && rguid &&
        memcmp(rguid, &kGUID_SysKeyboard, sizeof(GUID)) == 0 && !g_kbdHooked) {
        oKbdGetState = (DIGetState_t)patchVtableSlot(*dev, 9, (void*)&hkKbdGetState); // vtable[9]=GetDeviceState
        if (oKbdGetState) { g_kbdHooked = true; logf("[replay] keyboard GetDeviceState hooked"); }
    }
    return hr;
}
static void replayHookDInput(void* idi8) {
    if (oCreateDevice) return;                                // vtable is shared -> patch once
    oCreateDevice = (DICreateDev_t)patchVtableSlot(idi8, 3, (void*)&hkCreateDevice); // vtable[3]=CreateDevice
    if (oCreateDevice) logf("[replay] IDirectInput8::CreateDevice hooked");
}
// Frame clock: called once per rendered frame from hkPresent. Owns record-commit
// and play-advance so the pad + keyboard streams stay in perfect lockstep. Auto-
// replay (F4) restarts at the end instead of stopping.
static void replayFrameTick() {
    int st = g_recState;
    if (st == 1) {                                            // recording: commit staged frame
        int n = g_recLen;
        if (n < MACRO_MAX_FRAMES) {
            g_padRec[n] = g_stagePad;
            memcpy(g_keyRec[n], g_stageKeys, 256);
            g_recLen = n + 1;
        } else g_recState = 0;                                // buffer full -> stop
    } else if (st == 2) {                                     // playing: advance cursor
        int i = g_playIdx + 1;
        if (i >= g_recLen) {
            if (g_replayLoop && g_recLen > 0) i = 0;          // auto-replay: loop
            else { g_recState = 0; i = 0; }                   // one-shot: stop
        }
        g_playIdx = i;
    }
}
static void installXInputHook() {
    static bool done = false; if (done) return;
    HMODULE x = GetModuleHandleA("XINPUT1_3.dll");
    if (!x) x = LoadLibraryA("XINPUT1_3.dll");
    if (!x) { logf("[bootskip] XINPUT1_3 not present"); return; }
    void* tgt = (void*)GetProcAddress(x, "XInputGetState");
    if (!tgt) { logf("[bootskip] XInputGetState not found"); return; }
    MH_Initialize();
    if (MH_CreateHook(tgt, (void*)hkXInputGetState, (void**)&oXInputGetState) == MH_OK &&
        MH_EnableHook(tgt) == MH_OK) { done = true; logf("[bootskip] XInputGetState hooked"); }
    else logf("[bootskip] XInputGetState hook FAILED");
}
static void bootSkipTick() { installXInputHook(); }   // arm the hook once; injection happens in the hook

static HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    // Re-entrancy / cross-thread guard. ImGui has ONE global context and is not
    // thread-safe. During a resolution change DXGI can present from a second
    // thread (or re-enter this hook), which runs a fresh NewFrame/EndFrame while
    // a draw is still in flight -- leaving g.CurrentWindow null so the next
    // widget (e.g. a slider) writes through a null pointer and crashes. Let only
    // one present at a time drive the overlay; everything else passes through.
    if (InterlockedCompareExchange(&g_presentGuard, 1, 0) != 0)
        return oPresent(sc, sync, flags);
    struct GuardRelease { ~GuardRelease() { InterlockedExchange(&g_presentGuard, 0); } } guardRelease;

    // Detect a recreated D3D10 device (a resolution / fullscreen change can tear
    // the device down and build a new one). If the swapchain's device changed
    // under us, rebuild the backend against the new device before rendering --
    // otherwise we'd draw with a dangling device/RTV and crash.
    if (g_imguiInit) {
        ID3D10Device* cur = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D10Device), (void**)&cur)) && cur) {
            if (cur != g_device) {
                ImGui_ImplDX10_Shutdown();
                if (g_rtv)    { g_rtv->Release();    g_rtv = nullptr; }
                if (g_device) { g_device->Release(); }     // drop our ref on the old device
                g_device = cur;                            // keep this ref
                CreateRTV(sc);
                ImGui_ImplDX10_Init(g_device);
                logf("[present] device recreated -> backend rebuilt");
            } else {
                cur->Release();                            // unchanged: drop the extra ref
            }
        }
    }

    if (!g_imguiInit) {
        if (FAILED(sc->GetDevice(__uuidof(ID3D10Device), (void**)&g_device)))
            return oPresent(sc, sync, flags);
        DXGI_SWAP_CHAIN_DESC desc{}; sc->GetDesc(&desc);
        g_window = desc.OutputWindow;
        CreateRTV(sc);
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigDebugHighlightIdConflicts = false;
        // Fonts: keep the default (Latin) face, then MERGE Microsoft YaHei so the
        // Simplified-Chinese glyphs exist in the atlas for the localization toggle.
        // YaHei ships in the Wine bottle's Windows fonts; if it's missing we just
        // skip it (English stays fine, Chinese glyphs would show as boxes).
        {
            ImGuiIO& io = ImGui::GetIO();
            io.Fonts->AddFontDefault();
            const char* zhFont = "C:\\windows\\Fonts\\msyh.ttc";   // Microsoft YaHei
            if (GetFileAttributesA(zhFont) != INVALID_FILE_ATTRIBUTES) {
                ImFontConfig cfg; cfg.MergeMode = true; cfg.PixelSnapH = true;
                io.Fonts->AddFontFromFileTTF(zhFont, 16.0f, &cfg,
                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            }
            // Dedicated bold-italic face for the combo move-name readout (DMC
            // "Hybrid Theory" mission-title look). Arial Bold Italic ships in the
            // bottle; baked large so it stays crisp scaled to the on-screen size.
            const char* cbFont = "C:\\windows\\Fonts\\Arialbi.TTF";
            if (GetFileAttributesA(cbFont) != INVALID_FILE_ATTRIBUTES)
                g_comboFont = io.Fonts->AddFontFromFileTTF(cbFont, 48.0f);
            loadMoveNames();   // user-assigned combo move names (persisted)
        }
        // Black background, accent-everything theme (red by default; Theme toggle
        // flips it to blue via the same applyTheme() path).
        applyTheme(g_blue);
        ImGui_ImplWin32_Init(g_window);
        ImGui_ImplDX10_Init(g_device);
        if (g_window) g_oWndProc = (WNDPROC)SetWindowLongPtr(g_window, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
        initModule();
        populateCaves();   // core caves first
        populateCheats();  // then patches (spawn matrix sorts to bottom in UI)
        // Auto Royal Guard (block any hit) -- RE'd on the 2023 downgrade build.
        //  +4DD6CA: CanRoyalBlock() predicate: jne fail (75 50) -> jmp success (EB 4C)
        //  +4DD740: Royal Block timing-window gate: jb fail (72 02) -> nop nop (90 90)
        // Together: every incoming attack is treated as a valid Royal Block.
        addPatch("Royal Guard","Auto Royal Guard (block any hit)",
                 {{0x4DD6CA,{0xEB,0x4C},{0x75,0x50}},
                  {0x4DD740,{0x90,0x90},{0x72,0x02}}});
        g_skipBoot = bootSkipInstalled();  // reflect any stub already installed on disk
        applyTeleportCrashFix();   // guard the game's null-deref so area jumps can't crash
        applyTeleportCrashFix2();  // second null-deref site (refcount acquire on null stage obj)
        installRoseRemovesPins();   // MistressDMC: cave gated by g_rosePins flag (installed once)
        applyEmProbe();            // log every spawned actor's category + vtable (enemy-class hunt)
        verifyAvailability();
        // --- one-shot self-test: can an in-process code write even land in memory? ---
        // Patch a known code byte (air-hike site, verified to match this build),
        // read it back, then restore. The log tells us if writeBytes reaches memory
        // at all -- isolating "write failed" from "write landed but had no effect".
        {
            uintptr_t a = g_base + 0x4CD933;            // dec [ecx+2460] (Infinite Air Hike site)
            uint8_t orig[6]; memcpy(orig, (void*)a, 6);
            uint8_t nops[6] = {0x90,0x90,0x90,0x90,0x90,0x90};
            bool stuck = writeBytes(a, nops, 6);
            bool seen  = memcmp((void*)a, nops, 6) == 0;
            writeBytes(a, orig, 6);                      // restore immediately
            bool restored = memcmp((void*)a, orig, 6) == 0;
            logf("[selftest] in-process code write: stuck=%s readback=%s restored=%s",
                 stuck?"YES":"NO", seen?"YES":"NO", restored?"YES":"NO");
        }
        scanMusicFolder();          // discover music\*.wav tracks
        if (g_curTrack >= nTracks()) g_curTrack = 0;
        loadTrackPCM(g_curTrack);   // preload the selected song
        { char lp[MAX_PATH]; buildAssetPath(lp, "dmc4semood_logo.rgba"); loadLogo(lp); }
        logf("[Present] ready, %zu cheats", g_cheats.size());
        tryAutoLoad();   // apply the saved default profile if auto-load is enabled
        g_imguiInit = true;
    }
    pollInput();   // queue keyboard events BEFORE NewFrame processes them
    // Re-stamp the GUI scale if the slider moved or the resolution changed (auto-DPI).
    // Done before NewFrame so the whole frame uses the new style/font.
    if (g_imguiInit && effectiveUiScale() != g_uiScaleApplied) applyUiScale();
    updateMenuMusic();   // start/stop the looping menu track on open/close
    updateCamera();      // reapply the FOV target each frame while enabled
    applyCamVars();      // MistressDMC: additive custom camera deltas
    updateMajinPins();   // infinite-trickster-teleport / air-calibur upkeep
    bootSkipTick();      // auto-confirm the boot warning screens
    diagTick();          // DEBUG move-state logger (toggle in DEBUG tab) -> overlay.log
    inpDiagTick();       // input coverage diagnostic: merged game input vs XInput
    applyMacroHook();    // install the input record/playback cave (once; self-guards)
    serviceSpawn();      // BP enemy spawner: replay a pending spawn on the live director
    bossRushTick();      // BP boss-rush: bounce filler floors to the next boss floor
    danteProbeTick();    // diagnostic: log when/where pl006 (Dante) becomes loaded
    danteServiceTick();  // cold spawn: drop Dante in once his arcs finish mounting
    danteDiffTick();     // diff real vs ghost Dante to find his combat sub-objects
    danteSnapTick();     // snapshot real Dante's stats while on his floor
    danteGuardTick();    // keep the spawned Dante's collision lists from walking null
    updatePause();       // freeze the game while the menu is open (opt-in)
    ImGui_ImplDX10_NewFrame();
    ImGui_ImplWin32_NewFrame();   // sets DisplaySize + queues the cursor in WINDOW/client space
    // Re-base the overlay onto the BACKBUFFER instead of the window. In borderless
    // zoom the window is larger than the backbuffer, so the win32 backend's
    // window-sized DisplaySize would stretch/clip the UI and mis-map clicks. We draw
    // at backbuffer size and RE-QUEUE the cursor in backbuffer space as the LAST
    // input event -- it must come after the backend's event so it isn't overwritten
    // when ImGui::NewFrame() drains the queue (the earlier bug: poking io.MousePos
    // here got clobbered by NewFrame, leaving the wrong widget highlighted). No-op in
    // windowed mode where client == backbuffer, so it can't regress anything.
    if (g_bbW && g_bbH) {
        ImGuiIO& io = ImGui::GetIO();
        RECT cr{}; GetClientRect(g_window, &cr);
        int cw = cr.right - cr.left, ch = cr.bottom - cr.top;
        if (cw > 0 && ch > 0) {
            io.DisplaySize = ImVec2((float)g_bbW, (float)g_bbH);
            if (GetForegroundWindow() == g_window) {
                POINT p;
                if (GetCursorPos(&p) && ScreenToClient(g_window, &p))
                    io.AddMousePosEvent(p.x * (float)g_bbW / cw, p.y * (float)g_bbH / ch);
            }
        }
    }
    ImGui::NewFrame();
    mutatorTick();   // MistressDMC: drive Random Mutator Mode (needs valid DeltaTime/DisplaySize)
    coopFrame();     // MistressDMC: drive co-op (P2 input routing, camera, revive)
    DrawUI();
    ImGui::EndFrame();
    ImGui::Render();
    // Flip-model swapchains (DXVK / D3DMetal / Wine wined3d) rotate the back buffer
    // between presents, so a cached RTV (created once at init from GetBuffer(0)) only
    // composites onto the buffer that happens to be presented every Nth frame -- the
    // overlay strobes/flickers. Re-acquire the current back buffer's RTV each frame
    // (GetBuffer(0) always aliases the current back buffer) so the overlay lands on
    // whatever buffer is about to be flipped to the screen. Harmless on BLT-model
    // swapchains where buffer 0 never rotates.
    CreateRTV(sc);
    if (g_rtv) g_device->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());
    replayFrameTick();   // Replay: one commit/advance per rendered frame (single clock for pad+keyboard)
    ++g_frames;
    return oPresent(sc, sync, flags);
}
static HRESULT __stdcall hkResizeBuffers(IDXGISwapChain* sc, UINT bc, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    HRESULT hr = oResizeBuffers(sc, bc, w, h, f, fl);
    CreateRTV(sc);
    return hr;
}
// ERR09 fix: DMC4SE throws a fatal "ERR09: Unsupported function" when it tries to
// enter EXCLUSIVE fullscreen (the bug bites at non-1280x720 resolutions). Rather
// than overriding the player's choice, we honor whatever mode the game is set to:
// a fullscreen request becomes a borderless full-monitor window (looks identical to
// real fullscreen), and a windowed request restores the window. Either way the
// swapchain never actually goes EXCLUSIVE, so the broken ERR09 path never runs and
// resolution changes stop crashing -- the game just keeps the display mode you left
// it in (fullscreen stays fullscreen, windowed stays windowed).
static HRESULT __stdcall hkSetFullscreenState(IDXGISwapChain* sc, BOOL fs, IDXGIOutput* out) {
    if (fs) {
        logf("[err09] fullscreen requested -> borderless full-monitor (exclusive avoided)");
        setBorderless(true);
    } else {
        logf("[err09] windowed requested -> restoring window");
        setBorderless(false);
    }
    return oSetFullscreenState(sc, FALSE, nullptr);
}
// Bind our DXGI swapchain hooks once kiero has the methods table. Present/SetFullscreen/
// ResizeBuffers are IDXGISwapChain slots 8/10/13 -- identical whether the table was grabbed
// via a D3D10 or D3D11 dummy device (both swapchains use dxgi.dll's shared vtable), and the
// game's real D3D10 device is fetched from the swapchain inside hkPresent, so the render path
// is unaffected by which dummy we used.
static bool bindSwapchainHooks(const char* via) {
    logf("[InitThread] kiero %s SUCCESS", via);
    kiero::bind(8,  (void**)&oPresent,            (void*)hkPresent);
    kiero::bind(10, (void**)&oSetFullscreenState, (void*)hkSetFullscreenState);
    kiero::bind(13, (void**)&oResizeBuffers,      (void*)hkResizeBuffers);
    return true;
}
static DWORD WINAPI InitThread(LPVOID) {
    // Try D3D10 first (the game is DX10); fall back to a D3D11 dummy each round.
    // Some Windows 11 setups fail to create the D3D10 dummy device, which left the
    // overlay un-hooked (invisible). D3D11 hooks the same dxgi.dll Present, so it
    // shows the menu on those machines too.
    for (int t = 0; t < 120; ++t) {
        if (kiero::init(kiero::RenderType::D3D10) == kiero::Status::Success) return bindSwapchainHooks("D3D10"), 0;
        if (kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success) return bindSwapchainHooks("D3D11 (Win11 fallback)"), 0;
        Sleep(500);
    }
    logf("[InitThread] kiero FAILED (D3D10 and D3D11 dummies both unavailable)");
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hInst;
        DisableThreadLibraryCalls(hInst);
        g_log = fopen("C:\\overlay.log", "w");
        if (!g_log) g_log = fopen("overlay.log", "w");
        logf("[DllMain] MistressDMC attached.");
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
