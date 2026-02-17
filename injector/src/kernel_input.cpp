/*
 * kernel_input.cpp - De-elevated input relay for osu!
 *
 * Launches a hidden copy of slhost.exe (with --svc) at medium integrity
 * using explorer.exe's token. The de-elevated instance reads commands
 * from a named pipe and calls SendInput(). Because it runs at the same
 * integrity level as osu!, input is delivered normally (no UIPI block).
 *
 * From the outside:
 *   - No extra executable (same binary)
 *   - No console window
 *   - Pipe name looks like a generic D3D helper
 */

#include "kernel_input.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstring>

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\sl_d3d_sync";

/* ------------------------------------------------------------------ */
/*  Constructor / Destructor                                           */
/* ------------------------------------------------------------------ */

KernelInput::KernelInput(PhysicalMemory& phys)
    : m_phys(phys)
{
    InitializeCriticalSection(&m_cs);
}

KernelInput::~KernelInput()
{
    if (m_helperProcess) {
        TerminateProcess(m_helperProcess, 0);
        CloseHandle(m_helperProcess);
    }
    if (m_pipe != INVALID_HANDLE_VALUE)
        CloseHandle(m_pipe);
    DeleteCriticalSection(&m_cs);
}

/* ------------------------------------------------------------------ */
/*  Init: launch de-elevated helper                                    */
/* ------------------------------------------------------------------ */

bool KernelInput::Init()
{
    if (LaunchHelper()) {
        m_ready = true;
        printf("[+] KernelInput: input relay active\n");
        fflush(stdout);
        return true;
    }

    printf("[!] KernelInput: input relay failed to start\n");
    fflush(stdout);
    return false;
}

/* ------------------------------------------------------------------ */
/*  Find explorer.exe PID                                              */
/* ------------------------------------------------------------------ */

static DWORD FindExplorerPid()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

/* ------------------------------------------------------------------ */
/*  Launch de-elevated copy of slhost.exe --svc                        */
/* ------------------------------------------------------------------ */

bool KernelInput::LaunchHelper()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    /* Create named pipe (write-only, we send commands to helper) */
    m_pipe = CreateNamedPipeW(
        PIPE_NAME,
        PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_WAIT,
        1, 256, 0, 0, nullptr);

    if (m_pipe == INVALID_HANDLE_VALUE) {
        printf("[!] KernelInput: CreateNamedPipe failed (%lu)\n", GetLastError());
        fflush(stdout);
        return false;
    }

    /* Get explorer.exe's medium-integrity token */
    DWORD explorerPid = FindExplorerPid();
    if (!explorerPid) {
        printf("[!] KernelInput: explorer.exe not found\n");
        fflush(stdout);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    HANDLE hExplorer = OpenProcess(
        PROCESS_QUERY_INFORMATION, FALSE, explorerPid);
    if (!hExplorer) {
        printf("[!] KernelInput: cannot open explorer.exe (%lu)\n", GetLastError());
        fflush(stdout);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    HANDLE hToken = nullptr;
    OpenProcessToken(hExplorer,
        TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY, &hToken);
    CloseHandle(hExplorer);

    if (!hToken) {
        printf("[!] KernelInput: cannot get explorer token (%lu)\n", GetLastError());
        fflush(stdout);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    HANDLE hNewToken = nullptr;
    DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr,
                     SecurityImpersonation, TokenPrimary, &hNewToken);
    CloseHandle(hToken);

    if (!hNewToken) {
        printf("[!] KernelInput: DuplicateToken failed (%lu)\n", GetLastError());
        fflush(stdout);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    wchar_t cmdLine[MAX_PATH + 32] = {};
    swprintf_s(cmdLine, L"\"%s\" --svc", exePath);

    static wchar_t desktop[] = L"winsta0\\default";
    STARTUPINFOW si = { sizeof(si) };
    si.lpDesktop = desktop;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessWithTokenW(
        hNewToken, 0,
        exePath, cmdLine,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);
    CloseHandle(hNewToken);

    if (!ok) {
        printf("[!] KernelInput: launch failed (%lu)\n", GetLastError());
        fflush(stdout);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    m_helperProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    printf("[*] KernelInput: helper PID %lu, waiting for connect...\n",
           pi.dwProcessId);
    fflush(stdout);

    BOOL connected = ConnectNamedPipe(m_pipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        printf("[!] KernelInput: helper did not connect (%lu)\n", GetLastError());
        fflush(stdout);
        TerminateProcess(m_helperProcess, 0);
        CloseHandle(m_helperProcess);
        m_helperProcess = nullptr;
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        return false;
    }

    printf("[+] KernelInput: relay connected\n");
    fflush(stdout);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Pipe Communication                                                 */
/* ------------------------------------------------------------------ */

bool KernelInput::PipeSend(const char* msg, int len)
{
    EnterCriticalSection(&m_cs);
    HANDLE h = m_pipe;
    LeaveCriticalSection(&m_cs);

    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(h, msg, len, &written, nullptr);
    if (!ok) {
        EnterCriticalSection(&m_cs);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&m_cs);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void KernelInput::PressKey(uint16_t vk)
{
    if (!m_ready) return;
    char buf[16];
    int len = sprintf_s(buf, "P %u\n", vk);
    PipeSend(buf, len);
}

void KernelInput::ReleaseKey(uint16_t vk)
{
    if (!m_ready) return;
    char buf[16];
    int len = sprintf_s(buf, "R %u\n", vk);
    PipeSend(buf, len);
}

void KernelInput::MousePress(uint8_t /*mask*/)  { /* unused */ }
void KernelInput::MouseRelease(uint8_t /*mask*/) { /* unused */ }
