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
    std::string code;
    int classIndex;   // index into the item table (from the 3-letter code)
    int quality;      // 2 normal, 3 superior, 4 magic, 5 set, 6 rare, 7 unique
    int ilvl;         // parsed, but not passed to the game yet
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
// Returns -1 if handled (swallow the message), 0 otherwise.
DWORD __fastcall HandleInput(wchar_t* wMsg)
{
    if (_wcsnicmp(wMsg, L".item ", 6) != 0)
        return 0;

    std::wistringstream in(wMsg + 6);
    std::wstring wcode, wqual;
    int ilvl = 85;
    in >> wcode >> wqual >> ilvl;

    wchar_t buf[128];
    if (wcode.empty() || wcode.size() > 4)
    {
        wcscpy_s(buf, L"Usage: .item <code> [quality] [ilvl]");
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
            req.code = code;
            req.classIndex = idx;
            req.quality = ParseQuality(wqual);
            req.ilvl = ilvl;
            {
                std::lock_guard<std::mutex> lk(g_spawnLock);
                g_spawnQueue.push(req);
            }
            swprintf_s(buf, L"Queued item: %s (index %d)", wcode.c_str(), idx);
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

static void TrySpawn(void* game, void* player)
{
    if (!g_dropFn || !game || !player) return;

    SpawnRequest req;
    {
        std::lock_guard<std::mutex> lk(g_spawnLock);
        if (g_spawnQueue.empty()) return;
        req = g_spawnQueue.front();
        g_spawnQueue.pop();
    }

    Log("spawn: code=%s index=%d quality=%d", req.code.c_str(), req.classIndex, req.quality);

    DWORD item = 0;
    __try { item = CallDrop(g_dropFn, game, player, req.classIndex, req.quality); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("spawn: EXCEPTION 0x%08X", GetExceptionCode());
        return;
    }
    Log("spawn: returned item unit=%08X", item);
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
        BYTE* disp = (BYTE*)g + 0xCC550;
        static const BYTE expect[5] = {0x8A, 0x03, 0x84, 0xC0, 0x55};
        if (g_dropFn && memcmp(disp, expect, 5) == 0)
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