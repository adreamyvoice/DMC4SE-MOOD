// ============================================================================
//  coop.h -- local 2-player co-op (DMC4SE-MOOD 1.4)
//
//  Player 2 drives the doppelganger with its own controller, keyboard set, or
//  keyboard+mouse, completely independently of Player 1.
//
//  WHY THIS IS NEEDED
//  Every player actor in DMC4SE copies its input from ONE shared global pad
//  object, through a single function, into its own input block:
//
//      8d4858: lea 0x1884(%esi),%eax   ; &actor->inputBlock
//      8d485f: call 0x8d0dc0           ; fill it from the global pad
//
//      8d0dc0: push %ebp; mov %esp,%ebp; and $-8,%esp; sub $0x54,%esp
//      8d0dc9: mov 0x132432c,%eax      ; <-- the one global pad
//      8d0dce: movq 0xbd4(%eax),%xmm0  ; buttons
//      8d13e1: ret $0x4                ; stdcall, one argument
//
//  The clone is a normal player actor, so the game fed it P1's input and both
//  characters always moved together.
//
//  THE FIX
//  Hook that function. Its argument says WHICH player's block is being filled,
//  so for each player we can point the global pad at a structure of our own
//  carrying that player's device input, run the original, and put the real pad
//  back. The game never knows, so every device type works identically.
//  All five characters funnel through this one function.
// ============================================================================
#pragma once

// ---- game addresses (2015 build, RVAs from g_base) -------------------------
static const uint32_t RVA_INPUTSNAP        = 0x4D0DC0;  // VA 0x8d0dc0
static const uint32_t RVA_INPUTSNAP_RESUME = 0x4D0DC9;
static const uint8_t  INPUTSNAP_PROLOGUE[9] =
    { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x54 };

static const uint32_t RVA_PADPTR     = 0xF2432C;   // the shared pad object*
static const uint32_t RVA_INPUTSTATE = 0xF242E4;   // shared input state* (keyboard)
static const uint32_t PAD_OBJ_SIZE    = 0xFE8;
static const uint32_t INPUTSTATE_SIZE = 0xA40;
static const uint32_t KBD_BITMAP_LO   = 0x120;     // keyboard bitmaps inside it
static const uint32_t KBD_BITMAP_HI   = 0x1A0;

static const uint32_t OFF_INPUTBLOCK = 0x1884;     // actor+0x1884
static const uint32_t OFF_STICK_XY   = 0x18D0;     // int16 LX, LY (+-127)
static const uint32_t OFF_ALIVE      = 0x1B74;     // nonzero = alive
static const uint32_t OFF_HP_CUR     = 0x1B00;
static const uint32_t OFF_HP_MAX     = 0x1B04;
static const uint32_t OFF_YAW        = 0x1C48;
static const uint32_t OFF_STATE_ID   = 0x1A00;     // 0x15 = downed
static const uint32_t OFF_REVIVE     = 0x2772;     // write 1 -> game revives
// pad payload
static const uint32_t PAD_BTN_HELD0 = 0xBD4, PAD_BTN_HELD1 = 0xBD8;
static const uint32_t PAD_BTN_REL   = 0xBDC, PAD_BTN_HELD2 = 0xBE0;
static const uint32_t PAD_LX = 0xBE4, PAD_LY = 0xBE8, PAD_RX = 0xBEC, PAD_RY = 0xBF0;
static const uint32_t PAD_LT = 0xC18, PAD_RT = 0xC20;
static const uint32_t PAD_FLOATS = 0xBE4;          // 0x50 bytes cleared per fill
// camera parameter object (getCameraData())
static const uint32_t CAM_DIST = 0xD8, CAM_DIST2 = 0xDC;

// ---- devices ---------------------------------------------------------------
enum CoopDev {
    DEV_AUTO = -1,
    DEV_GAME = 0,                                   // untouched (P1 default)
    DEV_PAD1, DEV_PAD2, DEV_PAD3, DEV_PAD4,
    DEV_KB1, DEV_KB2, DEV_KB1_MOUSE, DEV_KB2_MOUSE,
    DEV_COUNT
};
static const char* kCoopDevNames[DEV_COUNT] = {
    "Game default", "Controller 1", "Controller 2", "Controller 3", "Controller 4",
    "Keyboard: WASD set", "Keyboard: Arrows/Numpad set",
    "Keyboard WASD + Mouse", "Keyboard Arrows + Mouse",
};

struct CoopKeys { int up, down, left, right, jump, square, triangle, circle,
                      lockon, l2, r1, l1, start, select; };
static const CoopKeys kCoopKeysL = { 'W','S','A','D', VK_SPACE,'J','K','L',
                                     VK_LSHIFT,'Q','E','R', VK_TAB,'C' };
static const CoopKeys kCoopKeysR = { VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,
                                     VK_NUMPAD0,VK_NUMPAD1,VK_NUMPAD2,VK_NUMPAD3,
                                     VK_RSHIFT,VK_NUMPAD4,VK_NUMPAD5,VK_NUMPAD6,
                                     VK_ADD,VK_SUBTRACT };

// ---- state -----------------------------------------------------------------
static bool  g_coopOn        = false;   // the menu checkbox
static bool  g_coopCam       = true;
static bool  g_coopRevive    = true;
static bool  g_coopKbdGuard  = true;    // keep a keyboard player off the other player
static bool  g_coopMouseAim  = true;
static int   g_coopP1Dev     = DEV_GAME;
static int   g_coopP2Dev     = DEV_AUTO;
static bool  g_coopEngaged   = false;
static bool  g_coopPadSeen[4] = { false, false, false, false };
static bool  g_coopWarpReq   = false;
static uint32_t g_coopMask   = 0;
static float g_coopLX = 0.f, g_coopLY = 0.f;
static volatile LONG g_coopSnaps = 0, g_coopSnapsP2 = 0;

static uint8_t* g_coopFakePad[2] = { nullptr, nullptr };   // [0]=P1 [1]=P2
static uint8_t* g_coopFakeState  = nullptr;
static uint8_t* g_coopRealPad    = nullptr;
static uint8_t* g_coopRealState  = nullptr;
static uint8_t* g_coopTruePad    = nullptr;   // last pointer known to be the game's own
static uint8_t* g_coopTrueState  = nullptr;
static volatile LONG g_coopInSwap = 0;        // reentrancy guard

typedef DWORD (WINAPI *CoopXIGetState)(DWORD, void*);
static CoopXIGetState g_coopXI = nullptr;

struct CoopPad {   // minimal XInput gamepad mirror
    WORD  buttons; BYTE lt, rt; SHORT lx, ly, rx, ry;
};

// ---- helpers ---------------------------------------------------------------
static inline bool coopDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void coopResolveXInput() {
    if (g_coopXI) return;
    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !g_coopXI; ++i) {
        HMODULE h = GetModuleHandleA(dlls[i]);
        if (!h) h = LoadLibraryA(dlls[i]);
        if (h) g_coopXI = (CoopXIGetState)GetProcAddress(h, "XInputGetState");
    }
}

static bool coopReadPadIdx(DWORD idx, CoopPad& out) {
    coopResolveXInput();
    if (!g_coopXI) return false;
    struct { DWORD packet; CoopPad gp; } st;
    ZeroMemory(&st, sizeof(st));
    if (g_coopXI(idx, &st) != ERROR_SUCCESS) return false;
    out = st.gp;
    return true;
}

static void coopScanPads() {
    CoopPad p;
    for (DWORD i = 0; i < 4; ++i) g_coopPadSeen[i] = coopReadPadIdx(i, p);
}

// "Game default" means the player is on the game's own pad, i.e. the first
// connected controller -- auto must never hand that same one to P2.
static int coopOccupiedPad(int dev) {
    if (dev == DEV_GAME) { for (int i = 0; i < 4; ++i) if (g_coopPadSeen[i]) return i; return -1; }
    if (dev >= DEV_PAD1 && dev <= DEV_PAD4) return dev - DEV_PAD1;
    return -1;
}
static int coopResolveDev(int dev, int otherDev) {
    if (dev != DEV_AUTO) return dev;
    int taken = coopOccupiedPad(otherDev);
    for (int i = 0; i < 4; ++i) if (g_coopPadSeen[i] && i != taken) return DEV_PAD1 + i;
    return (otherDev == DEV_KB1 || otherDev == DEV_KB1_MOUSE) ? DEV_KB2 : DEV_KB1;
}
static bool coopIsOurPad(const uint8_t* p) {
    return p && (p == g_coopFakePad[0] || p == g_coopFakePad[1]);
}

static bool coopIsKeyboard(int dev) {
    return dev == DEV_KB1 || dev == DEV_KB2 || dev == DEV_KB1_MOUSE || dev == DEV_KB2_MOUSE;
}

static void coopKeysToPad(CoopPad& p, const CoopKeys& k) {
    SHORT x = 0, y = 0;
    if (coopDown(k.left))  x -= 32767;
    if (coopDown(k.right)) x += 32767;
    if (coopDown(k.down))  y -= 32767;
    if (coopDown(k.up))    y += 32767;
    p.lx = x; p.ly = y;
    WORD b = 0;
    if (coopDown(k.jump))     b |= 0x1000;   // A
    if (coopDown(k.square))   b |= 0x4000;   // X
    if (coopDown(k.triangle)) b |= 0x8000;   // Y
    if (coopDown(k.circle))   b |= 0x2000;   // B
    if (coopDown(k.lockon))   b |= 0x0200;   // RB
    if (coopDown(k.l2))       b |= 0x0100;   // LB
    if (coopDown(k.start))    b |= 0x0010;
    if (coopDown(k.select))   b |= 0x0020;
    p.buttons = b;
    if (coopDown(k.r1)) p.rt = 255;
    if (coopDown(k.l1)) p.lt = 255;
}

static void coopAddMouse(CoopPad& p) {
    static POINT last = { 0, 0 };
    static bool have = false;
    POINT c;
    if (!GetCursorPos(&c)) return;
    if (have) {
        long rx = (c.x - last.x) * 900, ry = -(c.y - last.y) * 900;
        if (rx >  32767) rx =  32767;  if (rx < -32767) rx = -32767;
        if (ry >  32767) ry =  32767;  if (ry < -32767) ry = -32767;
        p.rx = (SHORT)rx; p.ry = (SHORT)ry;
    }
    last = c; have = true;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) p.buttons |= 0x4000;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) p.buttons |= 0x0200;
}

static bool coopReadDevice(int dev, CoopPad& p) {
    ZeroMemory(&p, sizeof(p));
    switch (dev) {
    case DEV_GAME: return false;
    case DEV_PAD1: case DEV_PAD2: case DEV_PAD3: case DEV_PAD4: {
        DWORD i = (DWORD)(dev - DEV_PAD1);
        bool ok = coopReadPadIdx(i, p);
        g_coopPadSeen[i] = ok;
        return ok;                       // unplugged -> leave that player alone
    }
    case DEV_KB1:       coopKeysToPad(p, kCoopKeysL); return true;
    case DEV_KB2:       coopKeysToPad(p, kCoopKeysR); return true;
    case DEV_KB1_MOUSE: coopKeysToPad(p, kCoopKeysL); if (g_coopMouseAim) coopAddMouse(p); return true;
    case DEV_KB2_MOUSE: coopKeysToPad(p, kCoopKeysR); if (g_coopMouseAim) coopAddMouse(p); return true;
    }
    return false;
}

// XInput -> the game's own pad format (PS2-style bit order, +-127 sticks)
static uint32_t coopMapButtons(const CoopPad& g) {
    uint32_t m = 0; WORD b = g.buttons;
    if (b & 0x0020) m |= 0x000001;   // SELECT
    if (b & 0x0040) m |= 0x000002;   // L3
    if (b & 0x0080) m |= 0x000004;   // R3
    if (b & 0x0010) m |= 0x000008;   // START
    if (b & 0x0001) m |= 0x000010;   // UP
    if (b & 0x0008) m |= 0x000020;   // RIGHT
    if (b & 0x0002) m |= 0x000040;   // DOWN
    if (b & 0x0004) m |= 0x000080;   // LEFT
    if (b & 0x0100) m |= 0x000100;   // L2
    if (b & 0x0200) m |= 0x000200;   // R2
    if (g.lt > 0x1E) m |= 0x000400;  // L1
    if (g.rt > 0x1E) m |= 0x000800;  // R1
    if (b & 0x8000) m |= 0x001000;   // TRIANGLE
    if (b & 0x2000) m |= 0x002000;   // CIRCLE
    if (b & 0x1000) m |= 0x004000;   // CROSS
    if (b & 0x4000) m |= 0x008000;   // SQUARE
    if (g.ly >  16000) m |= 0x010000;
    if (g.lx >  16000) m |= 0x020000;
    if (g.ly < -16000) m |= 0x040000;
    if (g.lx < -16000) m |= 0x080000;
    if (g.ry >  16000) m |= 0x100000;
    if (g.rx >  16000) m |= 0x200000;
    if (g.ry < -16000) m |= 0x400000;
    if (g.rx < -16000) m |= 0x800000;
    return m;
}
static SHORT coopDeadzone(SHORT v, SHORT dz) {
    if (v > -dz && v < dz) return 0;
    return (SHORT)((int)v * 127 / 32767);
}

static void coopFillPad(uint8_t* pad, uint8_t* actor, const CoopPad& g, int slot) {
    static uint32_t prevMask[2] = { 0, 0 };
    uint32_t mask = coopMapButtons(g);
    uint32_t released = prevMask[slot] & ~mask;
    prevMask[slot] = mask;

    SHORT lx = coopDeadzone(g.lx, 7849), ly = coopDeadzone(g.ly, 7849);
    SHORT rx = coopDeadzone(g.rx, 8689), ry = coopDeadzone(g.ry, 8689);
    g_coopMask = mask; g_coopLX = (float)lx; g_coopLY = (float)ly;

    memset(pad + PAD_FLOATS, 0, 0x50);
    *(uint32_t*)(pad + PAD_BTN_HELD0) = mask;
    *(uint32_t*)(pad + PAD_BTN_HELD1) = mask;
    *(uint32_t*)(pad + PAD_BTN_REL)   = released;
    *(uint32_t*)(pad + PAD_BTN_HELD2) = mask;
    *(float*)(pad + PAD_LX) = (float)lx;
    *(float*)(pad + PAD_LY) = (float)ly;
    *(float*)(pad + PAD_RX) = (float)rx;
    *(float*)(pad + PAD_RY) = (float)ry;
    *(float*)(pad + PAD_LT) = (g.lt > 0x1E) ? g.lt / 255.0f : 0.0f;
    *(float*)(pad + PAD_RT) = (g.rt > 0x1E) ? g.rt / 255.0f : 0.0f;
    if (actor) {
        *(int16_t*)(actor + OFF_STICK_XY)     = lx;
        *(int16_t*)(actor + OFF_STICK_XY + 2) = ly;
    }
}

// ---- player pointers -------------------------------------------------------
static uint8_t* coopP1() {
    uint8_t* p = (uint8_t*)g_doppOrigPlayer;
    if (!p) p = (uint8_t*)activePlayer();
    return (p && memReadable(p, 0x4000)) ? p : nullptr;
}
static uint8_t* coopP2() {
    uint8_t* p = (uint8_t*)g_doppActor;
    return (p && memReadable(p, 0x4000)) ? p : nullptr;
}
static uint8_t** coopPadSlot()   { return g_base ? (uint8_t**)(g_base + RVA_PADPTR)     : nullptr; }
static uint8_t** coopStateSlot() { return g_base ? (uint8_t**)(g_base + RVA_INPUTSTATE) : nullptr; }

// Keep the pad we hand the game structurally identical to the real one, then
// stamp this player's device input over the payload. Runs on the game thread
// inside the hook, so it can never tear.
static bool coopRefreshPad(int slot, int dev) {
    uint8_t** ps = coopPadSlot();
    if (!ps) return false;
    uint8_t* real = *ps;
    if (coopIsOurPad(real)) real = g_coopTruePad;      // never copy from ourselves
    if (!real || !memReadable(real, PAD_OBJ_SIZE)) return false;
    g_coopTruePad = real;

    CoopPad g;
    if (!coopReadDevice(dev, g)) return false;

    if (!g_coopFakePad[slot]) {
        g_coopFakePad[slot] = (uint8_t*)VirtualAlloc(nullptr, PAD_OBJ_SIZE,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_coopFakePad[slot]) return false;
        logf("[coop] pad buffer %d @ %p", slot, g_coopFakePad[slot]);
    }
    memcpy(g_coopFakePad[slot], real, PAD_OBJ_SIZE);
    coopFillPad(g_coopFakePad[slot], slot ? coopP2() : coopP1(), g, slot);
    return true;
}

static bool coopRefreshState() {
    uint8_t** ss = coopStateSlot();
    if (!ss) return false;
    uint8_t* real = *ss;
    if (real == g_coopFakeState) real = g_coopTrueState;
    if (!real || !memReadable(real, INPUTSTATE_SIZE)) return false;
    g_coopTrueState = real;
    if (!g_coopFakeState) {
        g_coopFakeState = (uint8_t*)VirtualAlloc(nullptr, INPUTSTATE_SIZE,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_coopFakeState) return false;
    }
    memcpy(g_coopFakeState, real, INPUTSTATE_SIZE);
    memset(g_coopFakeState + KBD_BITMAP_LO, 0, 0x20);   // DIK key bitmap only
    return true;
}

// ---- the hook --------------------------------------------------------------
extern "C" {
    volatile uint8_t* g_coopP2Block = nullptr;
    volatile uint8_t* g_coopP1Block = nullptr;
    void* g_coopTramp  = nullptr;    // stolen prologue + jmp resume
    void* g_coopResume = nullptr;

    void __cdecl coopCountSnap(void) { InterlockedIncrement(&g_coopSnaps); }

    int __cdecl coopSwapIn(uint8_t* block) {
        if (!g_coopEngaged) return 0;
        if (InterlockedCompareExchange(&g_coopInSwap, 1, 0) != 0) return 0;  // already inside one

        int slot = -1, dev = DEV_GAME;
        if (g_coopP2Block && block == (uint8_t*)g_coopP2Block) {
            slot = 1; dev = coopResolveDev(g_coopP2Dev, g_coopP1Dev);
            InterlockedIncrement(&g_coopSnapsP2);
        } else if (g_coopP1Block && block == (uint8_t*)g_coopP1Block) {
            slot = 0; dev = g_coopP1Dev;
        } else { InterlockedExchange(&g_coopInSwap, 0); return 0; }

        int swapped = 0;
        if (dev != DEV_GAME && coopRefreshPad(slot, dev)) {
            uint8_t** ps = coopPadSlot();
            if (ps) { g_coopRealPad = *ps; *ps = g_coopFakePad[slot]; swapped = 1; }
        }
        // if either player is on a keyboard, strip the shared keyboard state
        // from the other one so the same keys can't drive both characters
        if (g_coopKbdGuard && !coopIsKeyboard(dev) &&
            (coopIsKeyboard(g_coopP1Dev) || coopIsKeyboard(coopResolveDev(g_coopP2Dev, g_coopP1Dev))) &&
            coopRefreshState()) {
            uint8_t** ss = coopStateSlot();
            if (ss) { g_coopRealState = *ss; *ss = g_coopFakeState; swapped = 1; }
        }
        if (!swapped) InterlockedExchange(&g_coopInSwap, 0);
        return swapped;
    }

    void __cdecl coopSwapOut(void) {
        uint8_t** ps = coopPadSlot();
        if (ps) {
            uint8_t* want = g_coopRealPad ? g_coopRealPad : g_coopTruePad;
            if (want && coopIsOurPad(*ps)) *ps = want;    // only ever put the real one back
        }
        uint8_t** ss = coopStateSlot();
        if (ss) {
            uint8_t* want = g_coopRealState ? g_coopRealState : g_coopTrueState;
            if (want && *ss == g_coopFakeState) *ss = want;
        }
        g_coopRealPad = nullptr; g_coopRealState = nullptr;
        InterlockedExchange(&g_coopInSwap, 0);
    }
}

// stdcall(1 arg) wrapper around the game's input-snapshot function
__attribute__((naked)) static void coopSnapStub() {
    asm volatile(
        "pushl %eax\n\t"
        "pushl %ecx\n\t"
        "pushl %edx\n\t"
        "call  _coopCountSnap\n\t"
        "popl  %edx\n\t"
        "popl  %ecx\n\t"
        "popl  %eax\n\t"
        "movl  4(%esp), %eax\n\t"
        "cmpl  _g_coopP2Block, %eax\n\t"
        "je    2f\n\t"
        "cmpl  _g_coopP1Block, %eax\n\t"
        "je    2f\n\t"
        "1:\n\t"                       // passthrough: replay prologue, resume
        "pushl %ebp\n\t"
        "movl  %esp, %ebp\n\t"
        "andl  $-8, %esp\n\t"
        "subl  $0x54, %esp\n\t"
        "jmpl  *_g_coopResume\n\t"
        "2:\n\t"
        "pushl %eax\n\t"
        "call  _coopSwapIn\n\t"
        "addl  $4, %esp\n\t"
        "testl %eax, %eax\n\t"
        "je    1b\n\t"
        "pushl 4(%esp)\n\t"            // forward the argument
        "call  *_g_coopTramp\n\t"      // original (stdcall, cleans its own arg)
        "call  _coopSwapOut\n\t"
        "ret   $4\n\t"
    );
}

static bool    g_coopHooked = false;
static uint8_t g_coopSaved[9];

static bool coopInstallHook() {
    if (g_coopHooked) return true;
    if (!g_base) return false;
    uint8_t* site = (uint8_t*)(g_base + RVA_INPUTSNAP);
    if (!memReadable(site, 16) || memcmp(site, INPUTSNAP_PROLOGUE, 9) != 0) {
        logf("[coop] input hook: prologue mismatch at %p (%02x %02x %02x) - not installed",
             site, site[0], site[1], site[2]);
        return false;
    }
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(tramp, INPUTSNAP_PROLOGUE, 9);
    tramp[9] = 0xE9;
    intptr_t trel = (intptr_t)(g_base + RVA_INPUTSNAP_RESUME) - (intptr_t)(tramp + 14);
    memcpy(tramp + 10, &trel, 4);
    g_coopTramp  = tramp;
    g_coopResume = (void*)(g_base + RVA_INPUTSNAP_RESUME);

    DWORD old;
    if (!VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(g_coopSaved, site, 9);
    intptr_t rel = (intptr_t)(uint8_t*)&coopSnapStub - (intptr_t)(site + 5);
    site[0] = 0xE9;
    memcpy(site + 1, &rel, 4);
    for (int i = 5; i < 9; ++i) site[i] = 0x90;
    VirtualProtect(site, 9, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 9);
    g_coopHooked = true;
    logf("[coop] input hook installed @ %p - P2 has independent input", site);
    return true;
}

static void coopRemoveHook() {
    if (!g_coopHooked) return;
    g_coopP1Block = nullptr;
    g_coopP2Block = nullptr;
    uint8_t* site = (uint8_t*)(g_base + RVA_INPUTSNAP);
    DWORD old;
    if (VirtualProtect(site, 9, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(site, g_coopSaved, 9);
        VirtualProtect(site, 9, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 9);
    }
    g_coopHooked = false;
    logf("[coop] input hook removed");
}

// ---- support systems -------------------------------------------------------
// After the clone registers, the game puts it in the active-player slot (that
// is the trainer's normal "control the clone" behaviour). For co-op we hand
// the slot straight back so pad 0 keeps driving P1.
static void coopRestoreP1Slot() {
    if (!g_base) return;
    uint8_t* mgr = *(uint8_t**)(g_base + RVA_PLAYERMGR);
    uint8_t* p1  = (uint8_t*)g_doppOrigPlayer;
    uint8_t* p2  = coopP2();
    if (mgr && memReadable(mgr, 0x50) && p1 && p2 && *(uint8_t**)(mgr + 0x24) == p2) {
        *(uint8_t**)(mgr + 0x24) = p1;
        logf("[coop] pad 0 returned to P1");
    }
}

static void coopWarpP2() {
    uint8_t* p1 = coopP1(); uint8_t* p2 = coopP2();
    if (!p1 || !p2) return;
    float yaw = *(float*)(p1 + OFF_YAW);
    *(float*)(p2 + OFF_POS_X) = *(float*)(p1 + OFF_POS_X) - sinf(yaw) * 120.0f;
    *(float*)(p2 + OFF_POS_Y) = *(float*)(p1 + OFF_POS_Y);
    *(float*)(p2 + OFF_POS_Z) = *(float*)(p1 + OFF_POS_Z) + cosf(yaw) * 120.0f;
    logf("[coop] warped P2 to P1");
}

// Zoom the camera out as the players separate so both stay in frame.
static float g_coopCamOrig = 0.f, g_coopCamBase = 0.f, g_coopCamCur = 0.f;
static bool  g_coopCamHeld = false;

static void coopCameraRestore() {
    if (!g_coopCamHeld) return;
    uint8_t* cam = getCameraData();
    if (cam && memReadable(cam, 0x100)) {
        *(float*)(cam + CAM_DIST)  = g_coopCamOrig;
        *(float*)(cam + CAM_DIST2) = g_coopCamOrig - 30.0f;
    }
    g_coopCamHeld = false;
}

static void coopCameraTick() {
    if (!g_coopCam) { coopCameraRestore(); return; }
    uint8_t* cam = getCameraData();
    uint8_t* p1 = coopP1(); uint8_t* p2 = coopP2();
    if (!cam || !memReadable(cam, 0x400) || !p1 || !p2) return;

    if (!g_coopCamHeld) {
        g_coopCamOrig = *(float*)(cam + CAM_DIST);
        g_coopCamBase = (g_coopCamOrig <= 50.0f) ? 200.0f : g_coopCamOrig;
        g_coopCamCur  = g_coopCamBase;
        g_coopCamHeld = true;
    }
    float dx = *(float*)(p1 + OFF_POS_X) - *(float*)(p2 + OFF_POS_X);
    float dy = *(float*)(p1 + OFF_POS_Y) - *(float*)(p2 + OFF_POS_Y);
    float dz = *(float*)(p1 + OFF_POS_Z) - *(float*)(p2 + OFF_POS_Z);
    float d  = sqrtf(dx*dx + dy*dy + dz*dz);

    float b = g_coopCamBase, target = b + 0.6f * d;
    if (target > 4.0f * b) target = 4.0f * b;
    if (target < b)        target = b;
    g_coopCamCur += (target - g_coopCamCur) * 0.2f;
    *(float*)(cam + CAM_DIST)  = g_coopCamCur;
    *(float*)(cam + CAM_DIST2) = g_coopCamCur - 30.0f;
}

static void coopReviveTick() {
    if (!g_coopRevive) return;
    static ULONGLONG downSince = 0;
    uint8_t* p2 = coopP2();
    if (!p2) { downSince = 0; return; }

    bool alive  = *(uint8_t*)(p2 + OFF_ALIVE) != 0;
    bool downed = !alive || *(uint32_t*)(p2 + OFF_STATE_ID) == 0x15;
    if (!downed) { downSince = 0; return; }

    ULONGLONG now = GetTickCount64();
    if (!downSince) { downSince = now; return; }
    if (now - downSince < 5000) return;

    *(uint8_t*)(p2 + OFF_REVIVE) = 1;
    float maxHp = *(float*)(p2 + OFF_HP_MAX);
    if (maxHp > 1.0f) {
        float hp = maxHp * 0.5f;
        *(float*)(p2 + OFF_HP_CUR) = (hp < 1.0f) ? 1.0f : hp;
    }
    downSince = 0;
    logf("[coop] P2 revived");
}

// ---- per-frame -------------------------------------------------------------
static void coopFrame() {
    static int scan = 0;
    if (--scan <= 0) { scan = 120; coopScanPads(); }

    uint8_t* p2 = coopP2();
    bool want = g_coopOn && p2 != nullptr;

    if (want && !g_coopEngaged) {
        coopScanPads();
        logf("[coop] engaged  P1=%s  P2=%s  (pads %d%d%d%d)",
             kCoopDevNames[g_coopP1Dev],
             kCoopDevNames[coopResolveDev(g_coopP2Dev, g_coopP1Dev)],
             g_coopPadSeen[0], g_coopPadSeen[1], g_coopPadSeen[2], g_coopPadSeen[3]);
        g_coopEngaged = true;
        coopRestoreP1Slot();
        coopInstallHook();
    } else if (!want && g_coopEngaged) {
        g_coopEngaged = false;
        if (g_coopHooked) coopRemoveHook();
        coopSwapOut();            // make sure nothing is left pointing at us
        coopCameraRestore();
        logf("[coop] disengaged");
    }
    if (!g_coopEngaged) return;

    // Watchdog: if the shared pointers were left on one of our buffers (a
    // device change mid-swap, or a stray thread), put the real ones back --
    // otherwise that player reads frozen input and stops moving.
    uint8_t** ps = coopPadSlot();
    if (ps && coopIsOurPad(*ps) && g_coopTruePad) {
        *ps = g_coopTruePad;
        g_coopRealPad = nullptr;
        InterlockedExchange(&g_coopInSwap, 0);
        logf("[coop] pad pointer was stuck on a co-op buffer - restored");
    }
    uint8_t** ss = coopStateSlot();
    if (ss && *ss == g_coopFakeState && g_coopTrueState) {
        *ss = g_coopTrueState;
        g_coopRealState = nullptr;
        logf("[coop] input-state pointer was stuck - restored");
    }

    coopRestoreP1Slot();
    if (g_coopWarpReq) { g_coopWarpReq = false; coopWarpP2(); }
    if (GetAsyncKeyState(VK_HOME) & 0x8000) coopWarpP2();

    if (coopInstallHook()) {
        uint8_t* p1 = coopP1();
        g_coopP2Block = p2 + OFF_INPUTBLOCK;
        g_coopP1Block = p1 ? p1 + OFF_INPUTBLOCK : nullptr;
    }
    coopCameraTick();
    coopReviveTick();
}

// ---- menu ------------------------------------------------------------------
static void coopDrawTab() {
    ImGui::TextWrapped("Local 2-player co-op. Player 2 controls the doppelganger "
                       "with its own controller or keyboard, independently of Player 1.");
    ImGui::Spacing();

    ImGui::Checkbox("Enable co-op", &g_coopOn);
    ImGui::SameLine();
    hint("Turns the doppelganger into a second player. Off by default - the "
         "doppelganger otherwise behaves exactly as it always has.");

    if (!g_coopOn) return;

    int p2res = coopResolveDev(g_coopP2Dev, g_coopP1Dev);
    ImGui::Separator();
    ImGui::Text("Player 1 device: %s", kCoopDevNames[g_coopP1Dev]);
    ImGui::SameLine();
    if (ImGui::Button("Change P1")) {
        do { g_coopP1Dev = (g_coopP1Dev + 1) % DEV_COUNT; }
        while (g_coopP1Dev != DEV_GAME && g_coopP1Dev == g_coopP2Dev);
    }
    ImGui::Text("Player 2 device: %s%s", g_coopP2Dev == DEV_AUTO ? "Auto -> " : "",
                kCoopDevNames[p2res]);
    ImGui::SameLine();
    if (ImGui::Button("Change P2")) {
        g_coopP2Dev = (g_coopP2Dev == DEV_AUTO) ? DEV_PAD1 : g_coopP2Dev + 1;
        if (g_coopP2Dev >= DEV_COUNT) g_coopP2Dev = DEV_AUTO;
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto")) g_coopP2Dev = DEV_AUTO;

    ImGui::Text("Controllers: 1:%s  2:%s  3:%s  4:%s",
                g_coopPadSeen[0] ? "yes" : "-", g_coopPadSeen[1] ? "yes" : "-",
                g_coopPadSeen[2] ? "yes" : "-", g_coopPadSeen[3] ? "yes" : "-");

    ImGui::Spacing();
    ImGui::Checkbox("Co-op camera", &g_coopCam);
    ImGui::SameLine(); hint("Zooms out as the players separate so both stay in frame.");
    ImGui::Checkbox("Auto-revive P2", &g_coopRevive);
    ImGui::SameLine(); hint("Revives Player 2 about five seconds after going down, at half health.");
    ImGui::Checkbox("Keep a keyboard player's keys off the other player", &g_coopKbdGuard);
    ImGui::Checkbox("Mouse aims the camera", &g_coopMouseAim);
    if (ImGui::Button("Warp P2 to P1")) g_coopWarpReq = true;
    ImGui::SameLine(); hint("Also on the HOME key.");

    ImGui::Separator();
    if (g_coopEngaged) {
        if (g_coopHooked)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Engaged - P2 has independent input");
        else
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Engaged - input hook FAILED (check the build)");
        ImGui::Text("P2 input: %08X   stick %.0f, %.0f", g_coopMask, g_coopLX, g_coopLY);
        ImGui::Text("input snapshots: %ld total / %ld routed to P2", g_coopSnaps, g_coopSnapsP2);
    } else {
        ImGui::TextDisabled("Summon the doppelganger to start co-op.");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("P2 is a clone of Player 1's character.");
    ImGui::TextDisabled("WASD set:  WASD move, Space jump, J/K/L attack, LShift lock-on, Q/E/R shoulders");
    ImGui::TextDisabled("Arrow set: arrows move, Num0 jump, Num1/2/3 attack, RShift lock-on, Num4/5/6 shoulders");
}
