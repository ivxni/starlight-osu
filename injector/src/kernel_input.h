#pragma once
/*
 * kernel_input.h - De-elevated input relay for osu!
 *
 * Auto-launches a hidden copy of slhost.exe at medium integrity
 * that performs SendInput on our behalf (bypasses UIPI).
 *
 * From Relax's perspective, just call PressKey / ReleaseKey.
 */

#include "physmem.h"
#include <cstdint>
#include <Windows.h>

class KernelInput {
public:
    explicit KernelInput(PhysicalMemory& phys);
    ~KernelInput();

    bool Init();
    bool IsReady() const { return m_ready; }

    void PressKey(uint16_t vk);
    void ReleaseKey(uint16_t vk);

    void MousePress(uint8_t buttonMask);
    void MouseRelease(uint8_t buttonMask);

private:
    bool LaunchHelper();
    bool PipeSend(const char* msg, int len);

    PhysicalMemory& m_phys;
    bool m_ready = false;

    HANDLE m_pipe          = INVALID_HANDLE_VALUE;
    HANDLE m_helperProcess = nullptr;
    CRITICAL_SECTION m_cs;
};
