/*
 * starlight-osu :: injector/src/main.cpp
 *
 * Kernel-mode memory reader for osu! stable via BYOVD.
 * Reuses the same driver infrastructure as starlight-sys (Valorant).
 *
 * Features:
 *   - Relax (auto-click via Arduino HID)
 *   - Aim Assist (cursor correction via Arduino HID)
 *   - Debug overlay (optional)
 *
 * Usage:
 *   slhost.exe                         - Interactive mode
 *   slhost.exe --overlay               - Overlay mode (auto-detect osu!)
 *   slhost.exe --scan                  - List processes
 *   slhost.exe --preload               - Load driver, wait, then run
 *   slhost.exe --unload                - Unload driver and exit
 *   slhost.exe --driver-type <type>    - eneio64|lnv|ts|winring0|rtcore
 *   slhost.exe --quiet                 - Minimal output
 *   slhost.exe --skip-msr             - Skip MSR reads
 *
 * Must run as Administrator.
 */

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "byovd.h"
#include "physmem.h"
#include "kernel.h"
#include "../../shared/driver_backend.h"
#include "overlay.h"
#include "sig_scanner.h"
#include "osu_reader.h"
#include "renderer.h"
#include "relax.h"
#include "aim_assist.h"
#include "config.h"
#include "kernel_input.h"
#include "imgui.h"
#include <cmath>

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

enum class BackendKind { TS, LNV, WINRING0, RTCORE, ENEIO64 };

struct AppConfig {
    const wchar_t* driverPath  = nullptr;
    std::wstring   serviceName;
    const char*    targetName  = nullptr;
    BackendKind    backendKind = BackendKind::ENEIO64;
    uint32_t       intervalMs  = 16;
    bool           doScan      = false;
    bool           preload     = false;
    bool           unload      = false;
    bool           quiet       = false;
    bool           skipMSR     = false;
    bool           diag        = false;
    bool           overlay     = false;
};

static AppConfig g_cfg;

/* ------------------------------------------------------------------ */
/*  Logging                                                            */
/* ------------------------------------------------------------------ */

#define LOG(fmt, ...)  do { if (!g_cfg.quiet) { printf(fmt, ##__VA_ARGS__); fflush(stdout); } } while(0)
#define ERR(fmt, ...)  do { printf(fmt, ##__VA_ARGS__); fflush(stdout); } while(0)

/* ------------------------------------------------------------------ */
/*  Admin Check                                                        */
/* ------------------------------------------------------------------ */

static bool IsAdmin()
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

/* ------------------------------------------------------------------ */
/*  HVCI Check                                                         */
/* ------------------------------------------------------------------ */

static bool IsHVCIEnabled()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
        0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD enabled = 0;
        DWORD size = sizeof(enabled);
        if (RegQueryValueExW(hKey, L"Enabled", NULL, NULL,
            (LPBYTE)&enabled, &size) == ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return enabled == 1;
        }
        RegCloseKey(hKey);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Driver Path                                                        */
/* ------------------------------------------------------------------ */

static std::wstring GetDriverFullPath(IDriverBackend* backend)
{
    if (g_cfg.driverPath)
        return g_cfg.driverPath;

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    std::wstring dir(exePath);
    size_t lastSlash = dir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos)
        dir = dir.substr(0, lastSlash + 1);

    std::wstring path = dir + backend->DefaultDriverFile();

#ifdef _WIN64
    if (wcscmp(backend->DefaultDriverFile(), L"WinRing0.sys") == 0) {
        std::wstring path64 = dir + L"WinRing0x64.sys";
        if (GetFileAttributesW(path64.c_str()) != INVALID_FILE_ATTRIBUTES)
            path = path64;
    }
#endif

    return path;
}

/* ------------------------------------------------------------------ */
/*  Hex Dump                                                           */
/* ------------------------------------------------------------------ */

static void HexDump(const void* data, size_t size, uint64_t baseAddr)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i += 16) {
        printf("  %016llX  ", baseAddr + i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < size) printf("%02X ", p[i + j]);
            else printf("   ");
            if (j == 7) printf(" ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && (i + j) < size; j++) {
            uint8_t c = p[i + j];
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }
}

/* ------------------------------------------------------------------ */
/*  List Processes                                                     */
/* ------------------------------------------------------------------ */

static void ListProcesses(KernelContext& kernel, PhysicalMemory& phys)
{
    PhysicalMemory::Session session(phys);
    if (!session) { ERR("[!] Cannot open device\n"); return; }

    uint64_t systemCR3  = kernel.GetSystemCR3();
    uint64_t systemEpVA = kernel.GetSystemEprocessVA();
    uint32_t imgNameOff = kernel.GetImageNameOffset();

    printf("\n  %-6s  %-18s  %-30s\n", "PID", "DTB", "ImageName");
    printf("  %-6s  %-18s  %-30s\n", "------", "------------------",
           "------------------------------");

    if (systemEpVA == 0) {
        ERR("[!] System EPROCESS VA unknown\n");
        return;
    }

    uint64_t first_flink = phys.ReadVirt<uint64_t>(systemCR3,
        systemEpVA + EPROCESS_LINKS);

    if (first_flink == 0 || first_flink < KERNEL_VA_MIN) {
        ERR("[!] Invalid Flink: 0x%llX\n", first_flink);
        return;
    }

    uint64_t current = first_flink;
    int count = 0;

    while (count < 4096) {
        uint64_t eprocess_va = current - EPROCESS_LINKS;
        uint64_t pid = phys.ReadVirt<uint64_t>(systemCR3, eprocess_va + EPROCESS_PID);
        uint64_t dtb = phys.ReadVirt<uint64_t>(systemCR3, eprocess_va + EPROCESS_DTB);
        char name[16] = {};
        phys.ReadVirtual(systemCR3, eprocess_va + imgNameOff, name, 15);

        if (name[0] != '\0')
            printf("  %-6u  0x%016llX  %s\n",
                   static_cast<uint32_t>(pid), dtb, name);

        uint64_t next = phys.ReadVirt<uint64_t>(systemCR3, current);
        if (next == 0 || next == first_flink || next < KERNEL_VA_MIN)
            break;
        current = next;
        count++;
    }
    printf("\n  Total: %d processes\n", count);
}

/* ------------------------------------------------------------------ */
/*  Interactive Mode                                                   */
/* ------------------------------------------------------------------ */

static void InteractiveMode(KernelContext& kernel, PhysicalMemory& phys)
{
    printf("\n=== starlight-osu Interactive Mode ===\n");
    printf("Commands:\n");
    printf("  list                  - List all processes\n");
    printf("  find <name>           - Find process by name\n");
    printf("  read <va_hex> <size>  - Read process memory\n");
    printf("  scan                  - Scan osu! signatures\n");
    printf("  status                - Read osu! game state\n");
    printf("  quit                  - Exit\n\n");

    ProcessInfo activeProc{};
    bool hasProc = false;
    uint64_t activeModuleBase = 0;

    char line[256];
    while (true) {
        printf("osu> ");
        if (!fgets(line, sizeof(line), stdin)) break;

        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0)
            break;

        if (strcmp(line, "list") == 0) {
            ListProcesses(kernel, phys);
            continue;
        }

        if (strncmp(line, "find ", 5) == 0) {
            const char* name = line + 5;
            PhysicalMemory::Session session(phys);
            if (!session) { ERR("[!] Device open failed\n"); continue; }

            ProcessInfo proc{};
            if (kernel.FindProcess(name, proc)) {
                activeProc = proc;
                hasProc = true;
                activeModuleBase = kernel.GetProcessModuleBase(proc);
                printf("[+] Active: %s (PID %u, DTB 0x%llX",
                       proc.name, proc.pid, proc.dtb);
                if (activeModuleBase)
                    printf(", Base 0x%llX", activeModuleBase);
                printf(")\n");
            } else {
                printf("[!] Process '%s' not found\n", name);
            }
            continue;
        }

        if (strncmp(line, "read ", 5) == 0) {
            uint64_t va = 0;
            uint32_t sz = 0;
            if (sscanf(line + 5, "%llx %u", &va, &sz) == 2) {
                if (!hasProc) {
                    printf("[!] No active process. Use 'find <name>' first.\n");
                    continue;
                }
                if (sz > 4096) sz = 4096;

                PhysicalMemory::Session session(phys);
                if (!session) { ERR("[!] Device open failed\n"); continue; }

                uint8_t* buf = new uint8_t[sz];
                if (kernel.ReadProcessMemory(activeProc, va, buf, sz)) {
                    printf("[+] Read %u bytes from VA 0x%llX:\n", sz, va);
                    HexDump(buf, sz, va);
                } else {
                    printf("[!] Read failed\n");
                }
                delete[] buf;
            } else {
                printf("Usage: read <va_hex> <size_dec>\n");
            }
            continue;
        }

        if (strcmp(line, "scan") == 0) {
            if (!hasProc) {
                /* Auto-find osu! */
                PhysicalMemory::Session session(phys);
                if (!session) { ERR("[!] Device open failed\n"); continue; }

                if (kernel.FindProcess("osu!.exe", activeProc)) {
                    hasProc = true;
                    activeModuleBase = kernel.GetProcessModuleBase(activeProc);
                    printf("[+] Found osu!: PID %u, Base 0x%llX\n",
                           activeProc.pid, activeModuleBase);
                } else {
                    printf("[!] osu! not found. Is it running?\n");
                    continue;
                }
            }

            PhysicalMemory::Session session(phys);
            if (!session) { ERR("[!] Device open failed\n"); continue; }

            SigScanner scanner(kernel, activeProc);
            if (scanner.ScanAll()) {
                printf("[+] Signature scan complete!\n");
                printf("    Status:    0x%llX\n", scanner.GetStatusPtr());
                printf("    AudioTime: 0x%llX\n", scanner.GetAudioTimePtr());
                printf("    Ruleset:   0x%llX\n", scanner.GetRulesetPtr());
                printf("    Mods:      0x%llX\n", scanner.GetModsPtr());
                printf("    Playing:   0x%llX\n", scanner.GetPlayingPtr());
            }
            continue;
        }

        if (strcmp(line, "status") == 0) {
            if (!hasProc) {
                printf("[!] No osu! process. Use 'find osu!' or 'scan' first.\n");
                continue;
            }

            PhysicalMemory::Session session(phys);
            if (!session) { ERR("[!] Device open failed\n"); continue; }

            SigScanner scanner(kernel, activeProc);
            if (!scanner.ScanAll()) {
                printf("[!] Signature scan failed\n");
                continue;
            }

            /* Read a few values */
            uint64_t statusPtr = scanner.GetStatusPtr();
            if (statusPtr) {
                int32_t status = kernel.ReadProcess<int32_t>(activeProc, statusPtr);
                const char* statusStr = "Unknown";
                switch (status) {
                    case 0: statusStr = "Menu"; break;
                    case 1: statusStr = "Editing"; break;
                    case 2: statusStr = "Playing"; break;
                    case 5: statusStr = "Song Select"; break;
                    case 7: statusStr = "Ranking"; break;
                    case 11: statusStr = "Multiplayer"; break;
                }
                printf("[+] Game Status: %d (%s)\n", status, statusStr);
            }

            uint64_t modsPtr = scanner.GetModsPtr();
            if (modsPtr) {
                int32_t mods = kernel.ReadProcess<int32_t>(activeProc, modsPtr);
                printf("[+] Mods: 0x%X", mods);
                if (mods & 8)   printf(" HD");
                if (mods & 16)  printf(" HR");
                if (mods & 64)  printf(" DT");
                if (mods & 128) printf(" RX");
                if (mods & 256) printf(" HT");
                printf("\n");
            }
            continue;
        }

        printf("[?] Unknown: %s\n", line);
    }
}

/* ------------------------------------------------------------------ */
/*  Parse Arguments                                                    */
/* ------------------------------------------------------------------ */

static void ParseArgs(int argc, wchar_t* argv[])
{
    static char targetBuf[256];

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--scan") == 0)
            g_cfg.doScan = true;
        else if (wcscmp(argv[i], L"--preload") == 0)
            g_cfg.preload = true;
        else if (wcscmp(argv[i], L"--unload") == 0)
            g_cfg.unload = true;
        else if (wcscmp(argv[i], L"--quiet") == 0 || wcscmp(argv[i], L"-q") == 0)
            g_cfg.quiet = true;
        else if (wcscmp(argv[i], L"--skip-msr") == 0)
            g_cfg.skipMSR = true;
        else if (wcscmp(argv[i], L"--diag") == 0)
            g_cfg.diag = true;
        else if (wcscmp(argv[i], L"--overlay") == 0)
            g_cfg.overlay = true;
        else if (wcscmp(argv[i], L"--driver") == 0 && i + 1 < argc)
            g_cfg.driverPath = argv[++i];
        else if (wcscmp(argv[i], L"--service") == 0 && i + 1 < argc) {
            i++;
            g_cfg.serviceName = argv[i];
        }
        else if (wcscmp(argv[i], L"--driver-type") == 0 && i + 1 < argc) {
            i++;
            if (wcscmp(argv[i], L"lnv") == 0)
                g_cfg.backendKind = BackendKind::LNV;
            else if (wcscmp(argv[i], L"ts") == 0)
                g_cfg.backendKind = BackendKind::TS;
            else if (wcscmp(argv[i], L"winring0") == 0)
                g_cfg.backendKind = BackendKind::WINRING0;
            else if (wcscmp(argv[i], L"rtcore") == 0)
                g_cfg.backendKind = BackendKind::RTCORE;
            else if (wcscmp(argv[i], L"eneio64") == 0)
                g_cfg.backendKind = BackendKind::ENEIO64;
            else
                printf("[!] Unknown driver type: %ls\n", argv[i]);
        }
        else if (wcscmp(argv[i], L"--target") == 0 && i + 1 < argc) {
            i++;
            WideCharToMultiByte(CP_ACP, 0, argv[i], -1,
                                targetBuf, 256, NULL, NULL);
            g_cfg.targetName = targetBuf;
        }
        else if (wcscmp(argv[i], L"--interval") == 0 && i + 1 < argc) {
            i++;
            g_cfg.intervalMs = static_cast<uint32_t>(_wtoi(argv[i]));
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Unload Mode                                                        */
/* ------------------------------------------------------------------ */

static int DoUnload(const std::wstring& serviceName)
{
    LOG("[*] Unloading driver service '%ls'...\n", serviceName.c_str());

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        ERR("[!] OpenSCManager failed (error %lu)\n", GetLastError());
        return 1;
    }

    SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_ALL_ACCESS);
    if (svc) {
        SERVICE_STATUS status{};
        ControlService(svc, SERVICE_CONTROL_STOP, &status);
        Sleep(500);
        DeleteService(svc);
        CloseServiceHandle(svc);
        LOG("[+] Service stopped and deleted\n");
    } else {
        LOG("[*] Service not found (already unloaded?)\n");
    }

    CloseServiceHandle(scm);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  De-elevated Input Helper Mode                                      */
/*                                                                     */
/*  When launched with --svc, slhost runs as a silent input relay.     */
/*  It connects to the named pipe and calls SendInput at medium        */
/*  integrity (same as osu!). No console, no window, no driver.        */
/* ------------------------------------------------------------------ */

static const wchar_t* SVC_PIPE_NAME = L"\\\\.\\pipe\\sl_d3d_sync";

static int RunInputHelper()
{
    HANDLE pipe = CreateFileW(
        SVC_PIPE_NAME, GENERIC_READ,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return 1;

    DWORD attachedTid = 0;  /* osu! thread we're attached to */

    char buf[128];
    DWORD bytesRead = 0;
    while (ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        for (char* p = buf; *p; ) {
            if ((p[0] == 'P' || p[0] == 'R') && p[1] == ' ') {
                int vk = atoi(p + 2);
                if (vk > 0 && vk < 256) {
                    /* Attach to osu!'s input thread so our keybd_event
                     * goes to the same queue osu! reads from */
                    HWND osuHwnd = FindWindowW(nullptr, L"osu!");
                    DWORD osuTid = osuHwnd ? GetWindowThreadProcessId(osuHwnd, nullptr) : 0;
                    if (osuTid && osuTid != GetCurrentThreadId() && osuTid != attachedTid) {
                        if (attachedTid)
                            AttachThreadInput(GetCurrentThreadId(), attachedTid, FALSE);
                        if (AttachThreadInput(GetCurrentThreadId(), osuTid, TRUE))
                            attachedTid = osuTid;
                    }

                    DWORD flags = (p[0] == 'R') ? KEYEVENTF_KEYUP : 0;
                    keybd_event(static_cast<BYTE>(vk), 0, flags, 0);
                }
            }
            char* nl = strchr(p, '\n');
            p = nl ? nl + 1 : p + strlen(p);
        }
    }

    if (attachedTid)
        AttachThreadInput(GetCurrentThreadId(), attachedTid, FALSE);
    CloseHandle(pipe);
    return 0;
}

/* ------------------------------------------------------------------ */

int wmain(int argc, wchar_t* argv[])
{
    /* Silent input helper mode (de-elevated, no admin, no output) */
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--svc") == 0)
            return RunInputHelper();
    }

    printf("[*] starlight-osu init\n");
    fflush(stdout);

    ParseArgs(argc, argv);

    if (!IsAdmin()) {
        ERR("[!] Administrator privileges required.\n");
        return 1;
    }

    if (IsHVCIEnabled()) {
        LOG("\n[*] HVCI is ENABLED. Using eneio64 by default.\n");
    }

    /* ---- Select driver backend ---- */
    LnvMSRIOBackend     lnvBackend;
    ThrottleStopBackend  tsBackend;
    WinRing0Backend      winring0Backend;
    RTCore64Backend      rtcoreBackend;
    Eneio64Backend       eneio64Backend;

    IDriverBackend* backend = nullptr;
    switch (g_cfg.backendKind) {
        case BackendKind::LNV:      backend = &lnvBackend;      break;
        case BackendKind::TS:       backend = &tsBackend;       break;
        case BackendKind::WINRING0: backend = &winring0Backend; break;
        case BackendKind::RTCORE:   backend = &rtcoreBackend;   break;
        case BackendKind::ENEIO64:  backend = &eneio64Backend;  break;
    }

    std::wstring serviceName = g_cfg.serviceName.empty()
        ? backend->DefaultServiceName()
        : g_cfg.serviceName;

    LOG("[*] Backend: %s\n", backend->BackendName());

    if (!backend->SupportsMSR())
        g_cfg.skipMSR = true;

    if (g_cfg.unload)
        return DoUnload(serviceName);

    /* ---- Phase 0: PA Safety Filter ---- */
    PhysicalMemory phys(backend);
    KernelContext kernel(phys);

    LOG("[1/6] Loading PA safety filter...\n");
    if (!kernel.PreloadSafetyFilter()) {
        ERR("[!] Cannot load memory map.\n");
        return 1;
    }

    /* ---- Phase 1: Load Driver ---- */
    ByovdLoader byovd;

    {
        PhysicalMemory::Session trySession(phys);
        if (trySession) {
            LOG("[2/6] Device already available. Skipping load.\n");
        } else {
            std::wstring driverPath = GetDriverFullPath(backend);
            LOG("[2/6] Loading driver: %ls\n", driverPath.c_str());

            if (!byovd.LoadDriver(driverPath, serviceName)) {
                ERR("[!] Driver load failed.\n");
                return 1;
            }
            LOG("[+] Driver loaded.\n");
        }
    }

    if (g_cfg.preload) {
        LOG("[+] Driver pre-loaded. Start osu! now.\n");
        LOG("[*] Press ENTER when ready...\n");
        getchar();
    }

    /* ---- Phase 2: Verify Physical Memory ---- */
    LOG("[3/6] Testing device access...\n");
    {
        PhysicalMemory::Session session(phys);
        if (!session) {
            ERR("[!] Cannot open device handle.\n");
            byovd.UnloadDriver();
            return 1;
        }

        uint8_t buf[32] = {};
        bool ok = backend->ReadPhysicalChunk(phys.GetHandle(), 0x1000, buf, 32);
        bool allZero = true;
        for (int i = 0; i < 32; i++)
            if (buf[i] != 0) { allZero = false; break; }

        if (ok && !allZero) {
            LOG("[+] Physical memory reads verified.\n");
        } else {
            ERR("[!] Physical memory reads may not be working.\n");
        }

        if (!g_cfg.skipMSR) {
            uint64_t lstar = phys.ReadMSR(MSR_LSTAR);
            if (lstar > 0)
                LOG("[+] Kernel entry: 0x%llX\n", lstar);
        }
    }

    /* ---- Phase 3: Kernel Context ---- */
    LOG("[4/6] Scanning for kernel context...\n");
    {
        PhysicalMemory::Session session(phys);
        if (!session) {
            ERR("[!] Cannot open device for kernel scan.\n");
            byovd.UnloadDriver();
            return 1;
        }

        if (!kernel.Initialize(g_cfg.skipMSR)) {
            ERR("[!] Kernel context init failed.\n");
            byovd.UnloadDriver();
            return 1;
        }
        LOG("[+] Kernel context ready.\n");
    }

    /* ---- Phase 4: Action ---- */

    if (g_cfg.overlay) {
        LOG("[5/6] Starting overlay mode...\n");
        LOG("[*] Waiting for osu! to start...\n");

        /* Poll for osu! process */
        ProcessInfo proc{};
        uint64_t moduleBase = 0;
        while (true) {
            {
                PhysicalMemory::Session session(phys);
                if (session && kernel.FindProcess("osu!.exe", proc)) {
                    moduleBase = kernel.GetProcessModuleBase(proc);
                    if (moduleBase) break;
                }
            }
            Sleep(2000);
            printf(".");
            fflush(stdout);
        }

        LOG("\n[+] osu!: PID %u, Base 0x%llX\n", proc.pid, moduleBase);

        /* Signature scan */
        LOG("[*] Scanning osu! signatures...\n");
        SigScanner sigs(kernel, proc, moduleBase);
        {
            PhysicalMemory::Session session(phys);
            if (!session || !sigs.ScanAll()) {
                ERR("[!] Signature scan failed. osu! may have updated.\n");
                ERR("[!] Run in interactive mode ('scan' command) to debug.\n");
                /* Continue anyway - some features may still work */
            }
        }

        /* Wait for osu! window */
        LOG("[*] Waiting for osu! window...\n");
        for (int w = 0; w < 15; w++) {
            HWND hw = FindWindowW(nullptr, L"osu!");
            if (hw) break;
            Sleep(2000);
        }

        /* Create overlay */
        Overlay overlay;
        if (!overlay.Initialize(L"osu!")) {
            ERR("[!] Overlay initialization failed.\n");
            byovd.UnloadDriver();
            return 1;
        }

        /* Initialize input relay (de-elevated SendInput helper) */
        KernelInput kernelInput(phys);
        if (!kernelInput.Init()) {
            LOG("[!] KernelInput: input relay init failed.\n");
        }

        /* Start osu! reader thread */
        OsuReader reader(kernel, phys, proc, sigs);
        reader.Start();

        /* Create assist modules */
        Relax relax(kernelInput);
        AimAssist aimAssist;
        OsuDebugRenderer renderer;

        ConfigLoad(&relax, &aimAssist);
        ConfigPollStart(&relax, &aimAssist);

        /* Run overlay render loop */
        overlay.Run([&]() {
            OsuSnapshot snap = reader.GetSnapshot();

            /* Update assists */
            relax.Update(snap);
            aimAssist.Update(snap, overlay.GetWidth(), overlay.GetHeight());

            /* Render debug overlay */
            renderer.Render(snap, overlay.GetWidth(), overlay.GetHeight(),
                            relax, aimAssist);

            GameStatePush(snap);
        });

        /* Cleanup */
        ConfigPollStop();
        ConfigSave(relax, aimAssist);
        reader.Stop();
        LOG("[+] Overlay closed.\n");
    }
    else if (g_cfg.doScan) {
        LOG("[5/6] Listing processes...\n");
        ListProcesses(kernel, phys);
    }
    else if (g_cfg.targetName) {
        LOG("[5/6] Target: %s\n", g_cfg.targetName);

        PhysicalMemory::Session session(phys);
        if (!session) {
            ERR("[!] Device open failed\n");
            byovd.UnloadDriver();
            return 1;
        }

        ProcessInfo proc{};
        if (kernel.FindProcess(g_cfg.targetName, proc))
            LOG("[+] Found: PID %u, DTB 0x%llX\n", proc.pid, proc.dtb);

        InteractiveMode(kernel, phys);
    }
    else {
        LOG("[5/6] Interactive mode...\n");
        InteractiveMode(kernel, phys);
    }

    /* ---- Phase 5: Cleanup ---- */
    LOG("[6/6] Cleaning up...\n");
    byovd.UnloadDriver();
    LOG("[+] Done.\n");

    return 0;
}
