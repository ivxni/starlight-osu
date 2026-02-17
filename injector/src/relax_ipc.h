#pragma once
/*
 * relax_ipc.h - Named pipe IPC for Relax key injection.
 *
 * slhost (elevated) cannot SendInput to osu! (normal) due to UIPI.
 * A helper process (relax_helper.exe) runs as normal user and performs
 * SendInput. slhost sends key commands via a named pipe.
 */

#include <Windows.h>
#include <cstdint>

class RelaxIPC {
public:
    RelaxIPC();
    ~RelaxIPC();

    /* Start pipe server (non-blocking). Call once at startup. */
    void Start();

    /* Send key press: vk = virtual key code (e.g. 'A' = 0x41) */
    bool SendPress(uint16_t vk);

    /* Send key release */
    bool SendRelease(uint16_t vk);

    /* Is a helper client connected? */
    bool IsConnected() const { return m_clientPipe != INVALID_HANDLE_VALUE; }

private:
    static DWORD WINAPI AcceptThread(LPVOID param);
    void AcceptLoop();

    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    HANDLE m_clientPipe = INVALID_HANDLE_VALUE;
    HANDLE m_acceptThread = nullptr;
    CRITICAL_SECTION m_cs;
    bool m_running = false;
};
