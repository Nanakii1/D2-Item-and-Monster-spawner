// ItemDropD2.cpp - ".item <code> [quality] [ilvl]" for Diablo II LoD 1.13c (single player only, via PlugY)
//
// Build (x86 developer prompt):  cl /LD /O2 /MT ItemDropD2.cpp user32.lib
// Load:  PlugY.ini -> DllToLoad=ItemDropD2.dll
//
// Offline / single player only. Do not load on Battle.net. Keep a backup of your characters.
//
// How it works:
//  1. A hook on the client's chat input (D2Client+0x70B75, chained to BH.dll) catches ".item ...".
//  2. The request is queued.
//  3. A detour at the start of D2Game's packet dispatcher (D2Game+0xCC550) runs on the server
//     thread; there the queue is drained and D2Game's own ground-drop wrapper (D2Game+0x11880)
//     creates the item at the player.
// Every address is verified against the expected bytes before it is patched or called.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <string>
#include <sstream>
#include <queue>
#include <vector>
#include <mutex>
#include <cwchar>
#include <cstdio>
#include <cstdarg>
#include <cctype>

#pragma comment(lib, "user32.lib")

// ---------------- settings ----------------
static const bool DEBUG_LOG = false;   // true: writes ItemDrop.log next to Game.exe

static void Log(const char* fmt, ...)
{
    if (!DEBUG_LOG) return;
    FILE* f = nullptr;
    if (fopen_s(&f, "ItemDrop.log", "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------- shared queue ----------------
struct SpawnRequest {
    int kind;         // 0 = item, 1 = monster
    std::string code;
    int classIndex;   // item: index into the item table; monster: monster class ID
    int quality;      // items only: 2 normal, 3 superior, 4 magic, 5 set, 6 rare, 7 unique
    int ilvl;         // items only: 0 = game decides, 1-99 = forced item level
    int count;        // monsters only: how many (capped in HandleMon)
    int sockets;      // items only: 0 = leave as the game made it, 1-6 = request that many (clamped per item)
    std::vector<int> gems;   // items only: item-table indexes of gems/runes to insert, in order
};

static std::queue<SpawnRequest> g_spawnQueue;
static std::mutex g_spawnLock;

// ---------------- offsets (verified against 1.13c files; bytes checked at runtime) ----------------
static const DWORD PATCH_SITE    = 0x70B75;  // D2Client: call that GameInput_Intercept replaces
static const DWORD PRINTGAME_RVA = 0x7D850;  // D2Client: PrintGameString

typedef void (__stdcall *PrintGameString_t)(wchar_t* msg, int color);
static PrintGameString_t PrintGameString = nullptr;
static DWORD g_inputCall = 0;

// ---------------- item code -> index ----------------
typedef void* (__stdcall *GetItemText_t)(DWORD);

// D2Common ordinal 10695, record size 0x1A8, code at +0x80.
static int FindItemIndex(const char* code)
{
    static GetItemText_t fn = nullptr;
    if (!fn)
    {
        HMODULE c = GetModuleHandleA("D2Common.dll");
        if (!c) return -1;
        fn = (GetItemText_t)GetProcAddress(c, MAKEINTRESOURCEA(10695));
        if (!fn) return -1;
    }

    char want[4] = {0, 0, 0, 0};
    size_t n = 0;
    for (; n < 4 && code[n]; ++n)
        want[n] = (char)tolower((unsigned char)code[n]);

    __try
    {
        for (DWORD i = 0; i < 5000; ++i)
        {
            const char* rec = (const char*)fn(i);
            if (!rec) break;
            const char* c = rec + 0x80;
            bool match = true;
            for (size_t k = 0; k < 4 && match; ++k)
            {
                char have = c[k];
                if (k < n) match = (have == want[k]);
                else       match = (have == ' ' || have == 0);
            }
            if (match) return (int)i;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}

static int ParseQuality(const std::wstring& q)
{
    if (q == L"low")    return 1;
    if (q == L"normal") return 2;
    if (q == L"super")  return 3;
    if (q == L"magic")  return 4;
    if (q == L"set")    return 5;
    if (q == L"rare")   return 6;
    if (q == L"unique") return 7;
    return 2;   // default: normal
}

// ---------------- chat hook (client thread) ----------------
static bool g_monAvailable = false;   // set by InstallThread when D2Game+0xEDDB0 matches

static const int MON_MAX_COUNT = 5;   // hard cap per command
static const int MON_MAX_CLASS = 1000;

// ".mon <classId> [count]"  (numeric monster class ID from the monster table)
static DWORD HandleMon(const wchar_t* args)
{
    std::wistringstream in(args);
    int cls = -1, count = 1;
    in >> cls;
    bool parsedClass = !in.fail();
    if (parsedClass) { in >> count; if (in.fail()) count = 1; }

    wchar_t buf[128];
    if (!g_monAvailable)
        wcscpy_s(buf, L"Monster spawn unavailable (D2Game bytes did not match)");
    else if (!parsedClass)
        wcscpy_s(buf, L"Usage: .mon <classId> [count]");
    else if (cls < 0 || cls > MON_MAX_CLASS)
        wcscpy_s(buf, L"Monster class ID out of range");
    else
    {
        if (count < 1) count = 1;
        if (count > MON_MAX_COUNT) count = MON_MAX_COUNT;
        SpawnRequest req;
        req.kind = 1;
        req.classIndex = cls;
        req.quality = 0;
        req.ilvl = 0;
        req.sockets = 0;
        req.count = count;
        {
            std::lock_guard<std::mutex> lk(g_spawnLock);
            g_spawnQueue.push(req);
        }
        swprintf_s(buf, L"Queued monster: class %d x%d", cls, count);
    }
    if (PrintGameString) PrintGameString(buf, 0);
    return (DWORD)-1;
}

// Returns -1 if handled (swallow the message), 0 otherwise.
DWORD __fastcall HandleInput(wchar_t* wMsg)
{
    if (_wcsnicmp(wMsg, L".mon ", 5) == 0)
        return HandleMon(wMsg + 5);

    if (_wcsnicmp(wMsg, L".item ", 6) != 0)
        return 0;

    std::wistringstream in(wMsg + 6);
    std::wstring wcode, wqual;
    int ilvl = 0;                       // 0 = let the game derive it from the player's level
    int sockets = 0;                    // 0 = leave as the game made it
    in >> wcode >> wqual >> ilvl >> sockets;
    std::vector<std::wstring> gemCodes;
    {
        std::wstring g;
        while (gemCodes.size() < 7 && (in >> g)) gemCodes.push_back(g);
    }

    wchar_t buf[128];
    if (wcode.empty() || wcode.size() > 4 || ilvl < 0 || ilvl > 99 || sockets < 0 || sockets > 6 ||
        gemCodes.size() > 6)
    {
        wcscpy_s(buf, L"Usage: .item <code> [quality] [ilvl 0-99] [sockets 0-6] [gem/rune codes...]");
    }
    else
    {
        std::string code(wcode.begin(), wcode.end());
        int idx = FindItemIndex(code.c_str());
        if (idx < 0)
        {
            swprintf_s(buf, L"Unknown item code: %s", wcode.c_str());
        }
        else
        {
            SpawnRequest req;
            req.kind = 0;
            req.code = code;
            req.classIndex = idx;
            req.quality = ParseQuality(wqual);
            req.ilvl = ilvl;
            req.sockets = sockets;
            req.count = 1;

            const wchar_t* badGem = nullptr;
            for (size_t i = 0; i < gemCodes.size() && !badGem; ++i)
            {
                if (gemCodes[i].size() > 4) { badGem = gemCodes[i].c_str(); break; }
                std::string gc(gemCodes[i].begin(), gemCodes[i].end());
                int gi = FindItemIndex(gc.c_str());
                if (gi < 0) badGem = gemCodes[i].c_str();
                else req.gems.push_back(gi);
            }
            if (badGem)
            {
                swprintf_s(buf, L"Unknown gem/rune code: %s", badGem);
            }
            else
            {
                if (req.sockets < (int)req.gems.size()) req.sockets = (int)req.gems.size();
                {
                    std::lock_guard<std::mutex> lk(g_spawnLock);
                    g_spawnQueue.push(req);
                }
                swprintf_s(buf, L"Queued item: %s (index %d)", wcode.c_str(), idx);
            }
        }
    }

    if (PrintGameString) PrintGameString(buf, 0);
    return (DWORD)-1;
}

void __declspec(naked) GameInput_Intercept()
{
    __asm {
        pushad
        mov ecx, ebx
        call HandleInput
        cmp eax, -1
        popad
        je BlockIt
        call g_inputCall
        ret
BlockIt:
        xor eax, eax
        ret
    }
}

static bool PatchCall(DWORD site, void* target)
{
    BYTE* p = (BYTE*)site;
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    p[0] = 0xE8;
    *(DWORD*)(p + 1) = (DWORD)target - (site + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

// ---------------- server side (game thread) ----------------
static BYTE*  g_dropFn = nullptr;      // D2Game + 0x11880, set only if its bytes match
static DWORD  g_dispatchRet = 0;       // D2Game + 0xCC555

// Wrapper convention (read from the disassembly): ebx = source unit, five stack arguments
// (pGame, item index, quality, 2 unknown = 0); the wrapper cleans the stack itself (ret 0x14).
// Kept in its own function without C++ objects so the caller may use __try.
static DWORD __cdecl CallDrop(BYTE* fn, void* game, void* unit, int cls, int quality)
{
    DWORD result;
    __asm {
        push 0
        push 0
        push quality
        push cls
        push game
        mov  ebx, unit
        call fn
        mov  result, eax
    }
    return result;
}

// Item level override. The drop wrapper derives the item level from the unit's level stat by
// calling the helper at D2Game+0xE5D0 (eax = unit, one stack argument, ret 4). We detour that
// helper; while g_ilvlOverride is non-zero (set only around our own spawn call, on the server
// thread) it returns that value instead. Otherwise it runs the original instructions.
static volatile LONG g_ilvlOverride = 0;
static DWORD g_ilvlRet = 0;            // D2Game + 0xE5D5
static bool  g_ilvlHooked = false;

void __declspec(naked) IlvlDetour()
{
    __asm {
        cmp g_ilvlOverride, 0
        jne Override
        push esi                    // the three instructions overwritten by the jump
        mov  esi, eax
        test esi, esi
        jmp  dword ptr [g_ilvlRet]
Override:
        mov  eax, g_ilvlOverride
        ret  4
    }
}

// Gem / rune insertion. We reuse the game's own two steps for a player socketing an item:
//  1) D2Game+0xD7160  "pick up ground item to the cursor" (the 0xD6E30 variant puts it into the
//     inventory instead, which the log showed: mode 0). Needs an empty cursor.
//  2) D2Game+0xD4A70  "insert cursor item into socket" (the core of packet 0x28): eax = gem ID,
//     stdcall(pGame, pPlayer, targetItemID, &err, 1, 1, 0, 1), ret 0x20. The 1,1,1,1 are what the
//     packet handler passes; the third flag (0) skips its "target must be stored/equipped" check,
//     because our freshly made target item is still lying on the ground.
static BYTE* g_pickupFn = nullptr;
static BYTE* g_socketFn = nullptr;

// Cursor pickup: eax = player, ebx = game, stack (itemId, &err), ret 8. The return value is not
// meaningful, success is judged by the item's mode afterwards (4 = on the cursor).
static void CallPickup(BYTE* fn, void* game, void* player, DWORD itemId, DWORD* err)
{
    __asm {
        push err
        push itemId
        mov  ebx, game
        mov  eax, player
        call fn
    }
}

static int CallSocket(BYTE* fn, void* game, void* player, DWORD gemId, DWORD targetId, DWORD* err)
{
    int r;
    __asm {
        push 1
        push 0
        push 1
        push 1
        push err
        push targetId
        push player
        push game
        mov  eax, gemId
        call fn
        mov  r, eax
    }
    return r;
}

static BYTE* g_monFn = nullptr;        // D2Game + 0xEDDB0, set only if its bytes match

// Monster wrapper convention (read from the disassembly, values copied from the game's own
// callers): eax = monster class ID, ecx = 1, edx = reference unit (the monster appears next to
// it), stack = pGame, 4, 0x40; the wrapper cleans the stack itself (ret 0xC).
static DWORD __cdecl CallMonster(BYTE* fn, void* game, void* unit, int cls)
{
    DWORD result;
    __asm {
        mov  edx, unit
        mov  eax, cls
        mov  ecx, 1
        push 0x40
        push 4
        push game
        call fn
        mov  result, eax
    }
    return result;
}

static void SpawnMonsters(const SpawnRequest& req, void* game, void* player)
{
    if (!g_monFn) return;
    for (int i = 0; i < req.count && i < MON_MAX_COUNT; ++i)
    {
        DWORD mon = 0;
        __try { mon = CallMonster(g_monFn, game, player, req.classIndex); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("monster: EXCEPTION 0x%08X", GetExceptionCode());
            return;
        }
        Log("monster: class=%d returned unit=%08X", req.classIndex, mon);
        if (!mon) return;   // invalid class or no room: stop instead of repeating
    }
}

static void TrySpawn(void* game, void* player)
{
    if ((!g_dropFn && !g_monFn) || !game || !player) return;

    SpawnRequest req;
    {
        std::lock_guard<std::mutex> lk(g_spawnLock);
        if (g_spawnQueue.empty()) return;
        req = g_spawnQueue.front();
        g_spawnQueue.pop();
    }

    if (req.kind == 1) { SpawnMonsters(req, game, player); return; }
    if (!g_dropFn) return;

    Log("spawn: code=%s index=%d quality=%d", req.code.c_str(), req.classIndex, req.quality);

    DWORD item = 0;
    if (g_ilvlHooked && req.ilvl >= 1 && req.ilvl <= 99)
        InterlockedExchange(&g_ilvlOverride, req.ilvl);
    __try { item = CallDrop(g_dropFn, game, player, req.classIndex, req.quality); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        InterlockedExchange(&g_ilvlOverride, 0);
        Log("spawn: EXCEPTION 0x%08X", GetExceptionCode());
        return;
    }
    InterlockedExchange(&g_ilvlOverride, 0);
    Log("spawn: returned item unit=%08X", item);

    if (item && req.sockets > 0)
    {
        // D2Common ordinal 10992 = SetSockets(item, n), stdcall, ret 8. Read from the disassembly:
        // it checks the unit is an item, clamps n to the item's maximum (size and type based, 6 at
        // most), sets the "socketed" flag (0x800) in the item data and writes stat 0xC2.
        static void (__stdcall *SetSockets)(DWORD, int) = nullptr;
        static bool tried = false;
        if (!tried)
        {
            tried = true;
            HMODULE c = GetModuleHandleA("D2Common.dll");
            BYTE* fn = c ? (BYTE*)GetProcAddress(c, MAKEINTRESOURCEA(10992)) : nullptr;
            // first instructions: push edi ; mov edi,[esp+8] ; test edi,edi
            if (fn && fn[0] == 0x57 && fn[1] == 0x8B && fn[2] == 0x7C && fn[3] == 0x24 && fn[4] == 0x08)
                SetSockets = (void (__stdcall *)(DWORD, int))fn;
            else
                Log("SetSockets bytes MISMATCH, socket option disabled");
        }
        if (SetSockets)
        {
            __try { SetSockets(item, req.sockets); Log("spawn: requested %d sockets", req.sockets); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("SetSockets EXCEPTION 0x%08X", GetExceptionCode()); }
        }
    }

    if (item && !req.gems.empty())
    {
        if (!g_pickupFn || !g_socketFn) { Log("gems requested but socket functions unavailable"); return; }
        DWORD targetId = *(DWORD*)(item + 0x0C);           // unit ID

        // The game's socket routine refuses targets that are not identified (item flag 0x10 in
        // ItemData+0x18; it tests that flag on both target and gem). Items made by the drop
        // function are unidentified when magic/rare/set/unique, so identify the target first.
        __try
        {
            DWORD* data = *(DWORD**)(item + 0x14);          // unit -> ItemData
            if (data)
            {
                Log("target flags before: %08X", data[6]);
                data[6] |= 0x10;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("identify EXCEPTION"); }
        for (size_t i = 0; i < req.gems.size(); ++i)
        {
            DWORD gem = 0;
            __try
            {
                gem = CallDrop(g_dropFn, game, player, req.gems[i], 2);
                if (!gem) { Log("gem %u: create failed", (unsigned)i); break; }
                DWORD gemId = *(DWORD*)(gem + 0x0C);

                DWORD err = 0;
                CallPickup(g_pickupFn, game, player, gemId, &err);
                int ok = (*(DWORD*)(gem + 0x10) == 4);       // mode 4 = on the cursor
                Log("gem %u: pickup err=%u mode=%u", (unsigned)i, err, *(DWORD*)(gem + 0x10));
                if (!ok) break;                              // e.g. cursor not empty

                {
                    DWORD* gd = *(DWORD**)(gem + 0x14);
                    DWORD* td = *(DWORD**)(item + 0x14);
                    Log("gem %u: gem mode=%u flags=%08X, target mode=%u flags=%08X", (unsigned)i,
                        *(DWORD*)(gem + 0x10), gd ? gd[6] : 0, *(DWORD*)(item + 0x10), td ? td[6] : 0);
                }
                err = 0;
                ok = CallSocket(g_socketFn, game, player, gemId, targetId, &err);
                Log("gem %u: socket ok=%d err=%u", (unsigned)i, ok, err);
                if (!ok) break;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                Log("gem %u: EXCEPTION 0x%08X", (unsigned)i, GetExceptionCode());
                break;
            }
        }
    }
}

static void __cdecl OnServerPacket(void* game, void* player)
{
    TrySpawn(game, player);
}

void __declspec(naked) DispatchDetour()
{
    __asm {
        pushad
        pushfd
        mov eax, [esp + 40]        // pGame: first stack argument of the dispatcher
        push esi                   // player unit
        push eax                   // pGame
        call OnServerPacket
        add esp, 8
        popfd
        popad
        mov al, [ebx]              // the three instructions overwritten by the jump
        test al, al
        push ebp
        jmp dword ptr [g_dispatchRet]
    }
}

// ---------------- installation ----------------
static DWORD WINAPI InstallThread(LPVOID)
{
    HMODULE h = NULL;
    while (!(h = GetModuleHandleA("D2Client.dll")))
        Sleep(100);

    PrintGameString = (PrintGameString_t)((BYTE*)h + PRINTGAME_RVA);

    HMODULE g = NULL;
    while (!(g = GetModuleHandleA("D2Game.dll")))
        Sleep(200);

    // Server side: verify bytes, then install.
    {
        BYTE* fn = (BYTE*)g + 0x11880;
        static const BYTE expect[12] = {0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x81,0xEC,0x98,0x00,0x00,0x00};
        if (memcmp(fn, expect, 12) == 0) { g_dropFn = fn; Log("drop wrapper bytes OK"); }
        else Log("drop wrapper bytes MISMATCH, spawn disabled");
    }
    {
        BYTE* fn = (BYTE*)g + 0xD7160;
        static const BYTE expect[7] = {0x85,0xDB,0x57,0x8B,0xF8,0x75,0x07};
        if (memcmp(fn, expect, 7) == 0) { g_pickupFn = fn; Log("pickup function bytes OK"); }
        else Log("pickup function bytes MISMATCH, gems disabled");
        BYTE* fn2 = (BYTE*)g + 0xD4A70;
        static const BYTE expect2[11] = {0x53,0x55,0x8B,0x6C,0x24,0x14,0x56,0x8B,0x74,0x24,0x10};
        if (memcmp(fn2, expect2, 11) == 0) { g_socketFn = fn2; Log("socket function bytes OK"); }
        else Log("socket function bytes MISMATCH, gems disabled");
    }
    if (g_dropFn)
    {
        // push esi ; mov esi,eax ; test esi,esi   (verified in the file: 56 8B F0 85 F6)
        BYTE* fn = (BYTE*)g + 0xE5D0;
        static const BYTE expect[5] = {0x56, 0x8B, 0xF0, 0x85, 0xF6};
        DWORD old;
        if (memcmp(fn, expect, 5) == 0 && VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old))
        {
            g_ilvlRet = (DWORD)fn + 5;
            fn[0] = 0xE9;
            *(DWORD*)(fn + 1) = (DWORD)IlvlDetour - ((DWORD)fn + 5);
            VirtualProtect(fn, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), fn, 5);
            g_ilvlHooked = true;
            Log("ilvl detour installed");
        }
        else Log("ilvl helper bytes MISMATCH, ilvl override disabled");
    }
    {
        // sub esp,0x28 ; push esi ; mov esi,edx ; push esi  (derived from the disassembly)
        BYTE* fn = (BYTE*)g + 0xEDDB0;
        static const BYTE expect[7] = {0x83,0xEC,0x28,0x56,0x8B,0xF2,0x56};
        if (memcmp(fn, expect, 7) == 0) { g_monFn = fn; g_monAvailable = true; Log("monster wrapper bytes OK"); }
        else Log("monster wrapper bytes MISMATCH, .mon disabled");
    }
    {
        BYTE* disp = (BYTE*)g + 0xCC550;
        static const BYTE expect[5] = {0x8A, 0x03, 0x84, 0xC0, 0x55};
        if ((g_dropFn || g_monFn) && memcmp(disp, expect, 5) == 0)
        {
            g_dispatchRet = (DWORD)disp + 5;
            DWORD old;
            if (VirtualProtect(disp, 5, PAGE_EXECUTE_READWRITE, &old))
            {
                disp[0] = 0xE9;
                *(DWORD*)(disp + 1) = (DWORD)DispatchDetour - ((DWORD)disp + 5);
                VirtualProtect(disp, 5, old, &old);
                FlushInstructionCache(GetCurrentProcess(), disp, 5);
                Log("dispatcher detour installed");
            }
        }
        else Log("dispatcher bytes mismatch or no drop wrapper, not patching");
    }

    // Client side: keep our hook in front, chained to whoever else (BH.dll) patched the call.
    BYTE* site = (BYTE*)h + PATCH_SITE;
    for (;;)
    {
        Sleep(500);

        if (site[0] != 0xE8) continue;
        DWORD target = (DWORD)site + 5 + *(int*)(site + 1);
        if (target == (DWORD)GameInput_Intercept) continue;

        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)target, &mbi, sizeof(mbi)) ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;

        g_inputCall = target;
        if (PatchCall((DWORD)site, GameInput_Intercept))
            Log("patched site, chaining to 0x%08X", g_inputCall);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
        CreateThread(NULL, 0, InstallThread, NULL, 0, NULL);
    }
    return TRUE;
}
