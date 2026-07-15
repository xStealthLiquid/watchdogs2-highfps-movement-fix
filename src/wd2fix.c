/*
 * Watch Dogs 2 - High FPS Movement Fix
 * ------------------------------------------------------------------
 * Fixes the framerate-dependent sideways "sliding" of the player
 * character above 60 FPS (very noticeable during fast A/D strafing).
 *
 * Cause: The Havok character controller is stepped once per rendered
 * frame. At e.g. 120 FPS it runs twice as often as at 60, so the
 * per-frame velocity limit "over-charges" (the classic Quake/Source
 * strafe bug).
 *
 * Fix: A trampoline hook on the character-step function re-times the
 * physics ONLY for the player, via an accumulator, to a fixed 60 Hz,
 * independent of the render FPS. NPCs, animals and vehicles are left
 * untouched. The engine's own interpolation keeps the picture smooth.
 *
 * Singleplayer only (launch with -eac_launcher). Do NOT use online.
 *
 * Build:  zig cc -shared -target x86_64-windows-gnu -O2 -o wd2fix.asi wd2fix.c -lpsapi -luser32
 * Load:   as an .asi via Ultimate ASI Loader (dinput8.dll).
 */

#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ------------------------------------------------------------------ Logging */
/* Log file sits next to the DLL (portable); status/errors only - no spam. */
static char g_logPath[MAX_PATH] = {0};
static CRITICAL_SECTION g_logLock;

static void log_line(const char *fmt, ...) {
    if (!g_logPath[0]) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf[n++] = '\r'; buf[n++] = '\n';
    EnterCriticalSection(&g_logLock);
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        DWORD w; WriteFile(h, buf, (DWORD)n, &w, NULL);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logLock);
}

/* -------------------------------------------------------------- AOB scanner */
static uint8_t *scan_module(HMODULE mod, const unsigned char *pat, size_t len, int *out_count) {
    MODULEINFO mi;
    if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi))) { *out_count = -1; return NULL; }
    uint8_t *base = (uint8_t *)mi.lpBaseOfDll;
    uint8_t *end  = base + mi.SizeOfImage;
    uint8_t *found = NULL; int count = 0;
    uint8_t *p = base;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint8_t *regEnd = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        int readable = (mbi.State == MEM_COMMIT) &&
                       (mbi.Protect & (PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_READONLY|PAGE_READWRITE|PAGE_EXECUTE_WRITECOPY|PAGE_WRITECOPY)) &&
                       !(mbi.Protect & (PAGE_GUARD|PAGE_NOACCESS));
        if (readable) {
            uint8_t *s = (uint8_t *)mbi.BaseAddress; if (s < base) s = base;
            uint8_t *e = regEnd; if (e > end) e = end;
            if ((size_t)(e - s) >= len) {
                for (uint8_t *q = s; q <= e - len; ++q) {
                    if (q[0] == pat[0] && memcmp(q, pat, len) == 0) { if (!found) found = q; ++count; }
                }
            }
        }
        p = regEnd;
        if (regEnd <= (uint8_t *)mbi.BaseAddress) break;
    }
    *out_count = count;
    return found;
}

/* ------------------------------------------------ Near allocation (for E9) */
static void *alloc_near(void *target) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t t = (uintptr_t)target;
    for (int dir = 0; dir < 2; ++dir) {
        for (uintptr_t off = gran; off < 0x78000000ULL; off += gran) {
            uintptr_t addr = dir ? (t + off) : (t - off);
            addr &= ~(gran - 1);
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void *)addr, &mbi, sizeof(mbi))) continue;
            if (mbi.State != MEM_FREE) continue;
            void *p = VirtualAlloc((void *)addr, 0x1000, MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return NULL;
}

/* ------------------------------------------ Component hook (player position) */
/* Anchor: mov rax,[rdi+8]; movss xmm0,[rax+4C]; movss xmm1,[rax+50]; movss xmm2,[rax+54]
   rax = player transform component; position at [rax+0x4C/0x50/0x54]. We grab the
   component pointer to identify the player character. */
static const unsigned char CPAT[] = {
    0x48,0x8B,0x47,0x08, 0xF3,0x0F,0x10,0x40,0x4C, 0xF3,0x0F,0x10,0x48,0x50, 0xF3,0x0F,0x10,0x50,0x54
};
static volatile uint64_t *g_pComp = NULL;

static int install_comp_hook(HMODULE mod) {
    int cnt = 0;
    uint8_t *base = scan_module(mod, CPAT, sizeof(CPAT), &cnt);
    if (!base) { log_line("[comp] anchor not found (%d)", cnt); return 0; }
    uint8_t *site = base + 4;              /* movss xmm0,[rax+0x4C] -> rax = component */
    uint8_t *stub = (uint8_t*)alloc_near(site);
    if (!stub) { log_line("[comp] alloc_near failed"); return 0; }
    uint8_t s[34]; memset(s, 0x90, sizeof(s));
    s[0]=0x48; s[1]=0x89; s[2]=0x05; *(int32_t*)(s+3)=19;          /* mov [rip+19], rax */
    s[7]=0xF3; s[8]=0x0F; s[9]=0x10; s[10]=0x40; s[11]=0x4C;       /* movss xmm0,[rax+4C] (displaced) */
    s[12]=0xFF; s[13]=0x25; *(int32_t*)(s+14)=0;                   /* jmp [rip+0] */
    *(uint64_t*)(s+18) = (uint64_t)(site+5);
    *(uint64_t*)(s+26) = 0;
    memcpy(stub, s, sizeof(s));
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(s));
    g_pComp = (volatile uint64_t*)(stub+26);
    int64_t rel = (int64_t)stub - (int64_t)(site+5);
    if (rel > 0x7fffffffLL || rel < -0x80000000LL) { log_line("[comp] stub out of range"); return 0; }
    uint8_t patch[5]; patch[0]=0xE9; *(int32_t*)(patch+1)=(int32_t)rel;
    DWORD oldp;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldp)) { log_line("[comp] VProtect failed"); return 0; }
    memcpy(site, patch, 5);
    VirtualProtect(site, 5, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}

/* =================== Character-step substepping fix =================== */
/* Hook anchor: prologue of the character-step function (void Step(void* character, float dt)).
   Unique within Disrupt_64.dll. */
static const unsigned char CHPAT[] = {
    0x53,0x48,0x83,0xEC,0x30, 0x48,0x8B,0x01, 0x0F,0x29,0x74,0x24,0x20,
    0x48,0x89,0xCB, 0x0F,0x28,0xF1, 0xFF,0x90,0x98,0x00,0x00,0x00
};

static const float FIXED_DT = 1.0f/60.0f;      /* physics reference rate */
static volatile int g_substepOn = 1;           /* on from start */
static void (*oCharStep)(void*, float) = NULL; /* trampoline = original function */

/* Auto-discovery of the path from a character object to its world position:
   look for the player's component position (g_pComp+0x4C) inside the character,
   either directly as a float pair or via one pointer indirection. */
static volatile int g_navFound = 0;
static volatile int g_navK = -1, g_navOff = -1;      /* position path char[+navK]->+navOff */
#define DIAGN 64
static uint64_t g_diagChecked[DIAGN];
static volatile int g_diagCount = 0;

/* Accumulator slots (player bodies only, 2-4) */
#define CSLOTS 8
static struct { uint64_t ptr; float accum; DWORD lastTick; } g_charAcc[CSLOTS];

static uint64_t rdptr(uint64_t a) {
    if (IsBadReadPtr((void*)a,8)) return 0;
    uint64_t v; memcpy(&v,(void*)a,8); return v;
}
/* World position (X,Y) of this character via the discovered path. 0 = unreadable. */
static int char_pos(uint64_t ch, float *x, float *y) {
    uint64_t base = (g_navK>=0) ? rdptr(ch+g_navK) : ch;
    if (!base || IsBadReadPtr((void*)(base+g_navOff),8)) return 0;
    memcpy(x,(void*)(base+g_navOff),4); memcpy(y,(void*)(base+g_navOff+4),4);
    return 1;
}

static void cs_discover(uint64_t ch) {
    if (g_navFound || !g_pComp || !*g_pComp) return;
    uint64_t comp = *g_pComp;
    if (IsBadReadPtr((void*)(comp+0x54),4)) return;
    float px,py; memcpy(&px,(void*)(comp+0x4C),4); memcpy(&py,(void*)(comp+0x50),4);
    if (px*px+py*py < 1.0f) return;                 /* player position not valid yet */
    for (int i=0;i<g_diagCount;i++) if (g_diagChecked[i]==ch) return;
    if (g_diagCount>=DIAGN) { g_diagCount=0; return; }
    g_diagChecked[g_diagCount++]=ch;
    if (!IsBadReadPtr((void*)ch, 0x208)) {          /* 1) directly inside the character */
        for (int off=0; off<=0x200; off+=4) {
            float fx,fy; memcpy(&fx,(void*)(ch+off),4); memcpy(&fy,(void*)(ch+off+4),4);
            float dx=fx-px,dy=fy-py; if(dx<0)dx=-dx; if(dy<0)dy=-dy;
            if (dx<0.6f && dy<0.6f) { g_navK=-1; g_navOff=off; g_navFound=1;
                log_line("[fix] player path: char+0x%X (direct)", off); return; }
        }
    }
    for (int k=0; k<=0x100; k+=8) {                 /* 2) via a pointer inside the character */
        uint64_t p = rdptr(ch+k);
        if (p<=0x10000 || IsBadReadPtr((void*)p, 0x128)) continue;
        for (int off=0; off<=0x120; off+=4) {
            float fx,fy; memcpy(&fx,(void*)(p+off),4); memcpy(&fy,(void*)(p+off+4),4);
            float dx=fx-px,dy=fy-py; if(dx<0)dx=-dx; if(dy<0)dy=-dy;
            if (dx<0.6f && dy<0.6f) { g_navK=k; g_navOff=off; g_navFound=1;
                log_line("[fix] player path: char+0x%X -> +0x%X", k, off); return; }
        }
    }
}

/* --- Player-body detection via movement consistency ---
   Your 2 bodies are near the component every frame AND move together with you.
   An animal/NPC is only occasionally near; while petting you stand still (no
   confusion). Bodies that stay consistently near WHILE moving are remembered
   by pointer -> afterwards immune to nearby dogs. */
#define PTRACK 64
static struct { uint64_t ch; short score; DWORD tick, sTick; float pX,pY,pCX,pCY; } g_ptrack[PTRACK];
#define PBODIES 6
static struct { uint64_t ch; DWORD tick; } g_pbody[PBODIES];
/* ADOPT: a character only becomes a body via movement synchrony with you
   (excludes still-standing / differently-moving animals+NPCs, incl. petting).
   HOLD: afterwards purely by proximity -> body B (slightly offset) stays stable
   without per-frame sync jitter. A pushed dog is released as soon as you move
   away (proximity drops). */
#define NEAR_ADD2  (1.5f*1.5f)
#define NEAR_KEEP2 (3.0f*3.0f)

static int cs_is_player(uint64_t ch) {
    if (!g_navFound || !g_pComp || !*g_pComp) return 0;
    uint64_t comp = *g_pComp;
    DWORD now = GetTickCount();
    float fx,fy,cx,cy;
    if (!char_pos(ch,&fx,&fy)) return 0;
    if (IsBadReadPtr((void*)(comp+0x54),4)) return 0;
    memcpy(&cx,(void*)(comp+0x4C),4); memcpy(&cy,(void*)(comp+0x50),4);
    float ddx=fx-cx, ddy=fy-cy;
    float dist2 = ddx*ddx+ddy*ddy;
    /* 1) already a confirmed body -> hold while near (stable, no sync jitter) */
    for (int i=0;i<PBODIES;i++) if (g_pbody[i].ch==ch) {
        if (dist2 < NEAR_KEEP2) { g_pbody[i].tick=now; return 1; }
        g_pbody[i].ch=0; return 0;                   /* far away -> release */
    }
    /* 2) not yet confirmed: adopt only via movement synchrony */
    int isNear = dist2 < NEAR_ADD2;
    int slot=-1, free=-1;
    for (int i=0;i<PTRACK;i++){ if(g_ptrack[i].ch==ch){slot=i;break;} if(free<0&&(g_ptrack[i].ch==0||(now-g_ptrack[i].tick)>1500))free=i; }
    if (slot<0) slot=free;
    if (slot<0) return 0;
    struct { uint64_t ch; short score; DWORD tick, sTick; float pX,pY,pCX,pCY; } *t = &g_ptrack[slot];
    if (t->ch!=ch){ t->ch=ch; t->score=0; t->sTick=now; t->pX=fx; t->pY=fy; t->pCX=cx; t->pCY=cy; }
    t->tick=now;
    if (now - t->sTick >= 80) {                      /* compare movement every ~80ms */
        float chdx=fx-t->pX, chdy=fy-t->pY;
        float cmdx=cx-t->pCX, cmdy=cy-t->pCY;
        float cmMag2=cmdx*cmdx+cmdy*cmdy;
        if (cmMag2 > 0.08f*0.08f) {                  /* only judge when YOU move noticeably */
            float diffx=chdx-cmdx, diffy=chdy-cmdy;
            int synced = isNear && (diffx*diffx+diffy*diffy < 0.08f*0.08f);
            if (synced) { if (t->score < 10) t->score++; }
            else        { t->score -= 2; if (t->score < 0) t->score = 0; }
        }
        t->sTick=now; t->pX=fx; t->pY=fy; t->pCX=cx; t->pCY=cy;
        if (t->score >= 3) {                         /* synchronous -> adopt as a body */
            for (int i=0;i<PBODIES;i++) if (g_pbody[i].ch==0 || (now-g_pbody[i].tick)>2000) { g_pbody[i].ch=ch; g_pbody[i].tick=now; break; }
            return 1;
        }
    }
    return 0;
}

static void myCharStep(void* character, float dt) {
    void (*orig)(void*, float) = oCharStep;
    if (!orig) return;
    uint64_t cp = (uint64_t)character;
    if (g_substepOn) cs_discover(cp);
    if (!g_substepOn || !cs_is_player(cp)) { orig(character, dt); return; }  /* substep player only */
    DWORD now = GetTickCount();
    int slot=-1, free=-1;
    for (int i=0;i<CSLOTS;i++) {
        if (g_charAcc[i].ptr==cp) { slot=i; break; }
        if (free<0 && (g_charAcc[i].ptr==0 || (now - g_charAcc[i].lastTick) > 1000)) free=i;
    }
    if (slot<0) slot=free;
    if (slot<0) { orig(character, dt); return; }
    if (g_charAcc[slot].ptr != cp) { g_charAcc[slot].ptr=cp; g_charAcc[slot].accum=0.0f; }
    g_charAcc[slot].lastTick = now;
    float rdt = dt; if (rdt > 1.0f/30.0f) rdt = 1.0f/30.0f; if (rdt < 0.0f) rdt = 0.0f;
    g_charAcc[slot].accum += rdt;
    int steps = 0;
    while (g_charAcc[slot].accum >= FIXED_DT && steps < 3) {
        orig(character, FIXED_DT);
        g_charAcc[slot].accum -= FIXED_DT;
        steps++;
    }
    if (steps >= 3) g_charAcc[slot].accum = 0.0f;   /* spike guard */
}

static int install_charstep_hook(HMODULE mod) {
    int cnt = 0;
    uint8_t *fn = scan_module(mod, CHPAT, sizeof(CHPAT), &cnt);
    if (!fn) { log_line("[fix] character-step anchor not found (%d)", cnt); return 0; }
    uint8_t *blk = (uint8_t*)alloc_near(fn);
    if (!blk) { log_line("[fix] alloc_near failed"); return 0; }
    /* blk[0..18]  trampoline: displaced 5B + jmp [rip] -> fn+5
       blk[19..32] jump stub:  jmp [rip] -> myCharStep */
    uint8_t s[40]; memset(s,0x90,sizeof(s));
    memcpy(s, fn, 5);
    s[5]=0xFF; s[6]=0x25; *(int32_t*)(s+7)=0; *(uint64_t*)(s+11) = (uint64_t)(fn+5);
    s[19]=0xFF; s[20]=0x25; *(int32_t*)(s+21)=0; *(uint64_t*)(s+25) = (uint64_t)myCharStep;
    memcpy(blk, s, sizeof(s));
    FlushInstructionCache(GetCurrentProcess(), blk, sizeof(s));
    oCharStep = (void(*)(void*,float))blk;
    int64_t rel = (int64_t)(blk+19) - (int64_t)(fn+5);
    if (rel > 0x7fffffffLL || rel < -0x80000000LL) { log_line("[fix] jump stub out of range"); return 0; }
    uint8_t patch[5]; patch[0]=0xE9; *(int32_t*)(patch+1)=(int32_t)rel;
    DWORD op;
    if (!VirtualProtect(fn,5,PAGE_EXECUTE_READWRITE,&op)) { log_line("[fix] VProtect failed"); return 0; }
    memcpy(fn, patch, 5);
    VirtualProtect(fn,5,op,&op);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    return 1;
}

/* --------------------------------------------------------------- Worker */
static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    HMODULE mod = NULL;
    for (int i = 0; i < 120 && !mod; ++i) { mod = GetModuleHandleA("Disrupt_64.dll"); if (!mod) Sleep(500); }
    if (!mod) { log_line("Disrupt_64.dll not found - aborting"); return 0; }
    int okc = install_comp_hook(mod);
    int okf = install_charstep_hook(mod);
    log_line("Watch Dogs 2 High FPS Movement Fix loaded. Component-Hook=%s  Fix-Hook=%s  (F1 = toggle)",
             okc?"ok":"FAILED", okf?"ok":"FAILED");
    return 0;
}

/* -------------------------------------------------------------- Hotkeys */
static DWORD WINAPI hotkeys(LPVOID unused) {
    (void)unused;
    for (;;) {
        if (GetAsyncKeyState(VK_F1) & 1) {
            g_substepOn = !g_substepOn;
            for (int i=0;i<CSLOTS;i++){ g_charAcc[i].ptr=0; g_charAcc[i].accum=0; }
            log_line("F1: High FPS Fix %s", g_substepOn ? "ON" : "off");
        }
        Sleep(30);
    }
    return 0;
}

/* --------------------------------------------------------------- DllMain */
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        InitializeCriticalSection(&g_logLock);
        /* Put the log next to our own DLL */
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(hMod, path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            char *slash = strrchr(path, '\\');
            if (slash) { *(slash+1) = 0; snprintf(g_logPath, sizeof(g_logPath), "%swd2fix.log", path); }
        }
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
        CreateThread(NULL, 0, hotkeys, NULL, 0, NULL);
    }
    return TRUE;
}
