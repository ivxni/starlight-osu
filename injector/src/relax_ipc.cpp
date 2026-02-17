/*
 * relax_ipc.cpp - Named pipe server for Relax key commands.
 */

#include "relax_ipc.h"
#include <cstdio>
#include <cstring>

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\starlight_osu_relax";

RelaxIPC::RelaxIPC()
{
    InitializeCriticalSection(&m_cs);
}

RelaxIPC::~RelaxIPC()
{
    m_running = false;
    if (m_acceptThread) {
        WaitForSingleObject(m_acceptThread, 2000);
        CloseHandle(m_acceptThread);
    }
    EnterCriticalSection(&m_cs);
    if (m_clientPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_clientPipe);
        m_clientPipe = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&m_cs);
    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    DeleteCriticalSection(&m_cs);
}

void RelaxIPC::Start()
{
    if (m_acceptThread) return;

    m_running = true;
    m_acceptThread = CreateThread(nullptr, 0, AcceptThread, this, 0, nullptr);
    if (m_acceptThread) {
        printf("[+] Relax: pipe server started. Run relax_helper.exe for input.\n");
        fflush(stdout);
    }
}

DWORD WINAPI RelaxIPC::AcceptThread(LPVOID param)
{
    RelaxIPC* self = static_cast<RelaxIPC*>(param);
    self->AcceptLoop();
    return 0;
}

void RelaxIPC::AcceptLoop()
{
    while (m_running) {
        m_pipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 64, 64, 0, nullptr);

        if (m_pipe == INVALID_HANDLE_VALUE) {
            Sleep(2000);
            continue;
        }

        BOOL connected = ConnectNamedPipe(m_pipe, nullptr);
        if (connected || GetLastError() == ERROR_PIPE_CONNECTED) {
            EnterCriticalSection(&m_cs);
            if (m_clientPipe != INVALID_HANDLE_VALUE)
                CloseHandle(m_clientPipe);
            m_clientPipe = m_pipe;
            m_pipe = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&m_cs);
            printf("[+] Relax: helper connected.\n");
            fflush(stdout);

            /* Wait for client to disconnect before accepting again */
            WaitForSingleObject(m_clientPipe, INFINITE);
            EnterCriticalSection(&m_cs);
            CloseHandle(m_clientPipe);
            m_clientPipe = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&m_cs);
            printf("[*] Relax: helper disconnected.\n");
            fflush(stdout);
        } else {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }

        Sleep(500);
    }
}

bool RelaxIPC::SendPress(uint16_t vk)
{
    EnterCriticalSection(&m_cs);
    HANDLE h = m_clientPipe;
    LeaveCriticalSection(&m_cs);

    if (h == INVALID_HANDLE_VALUE) return false;

    char buf[8];
    int len = sprintf_s(buf, "P %u\n", vk);
    DWORD written = 0;
    BOOL ok = WriteFile(h, buf, len, &written, nullptr);
    if (!ok) {
        EnterCriticalSection(&m_cs);
        CloseHandle(m_clientPipe);
        m_clientPipe = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&m_cs);
        return false;
    }
    return true;
}

bool RelaxIPC::SendRelease(uint16_t vk)
{
    EnterCriticalSection(&m_cs);
    HANDLE h = m_clientPipe;
    LeaveCriticalSection(&m_cs);

    if (h == INVALID_HANDLE_VALUE) return false;

    char buf[8];
    int len = sprintf_s(buf, "R %u\n", vk);
    DWORD written = 0;
    BOOL ok = WriteFile(h, buf, len, &written, nullptr);
    if (!ok) {
        EnterCriticalSection(&m_cs);
        CloseHandle(m_clientPipe);
        m_clientPipe = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&m_cs);
        return false;
    }
    return true;
}
