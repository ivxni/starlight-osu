/*
 * relax_helper.exe - De-elevated SendInput helper for osu! Relax.
 *
 * This process runs at medium integrity (same as osu!) so SendInput
 * reaches osu! without UIPI blocking it. Auto-launched by slhost.
 *
 * Reads key commands from a named pipe and calls SendInput.
 */

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\sl_osu_rx";

int wmain()
{
    HANDLE pipe = CreateFileW(
        PIPE_NAME, GENERIC_READ,
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        /* Silent exit if pipe not available */
        return 1;
    }

    char buf[128];
    DWORD bytesRead = 0;

    while (ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buf[bytesRead] = '\0';

        for (char* p = buf; *p; ) {
            if ((p[0] == 'P' || p[0] == 'R') && p[1] == ' ') {
                int vk = atoi(p + 2);
                if (vk > 0 && vk < 256) {
                    INPUT input = {};
                    input.type = INPUT_KEYBOARD;
                    input.ki.wVk = static_cast<WORD>(vk);
                    input.ki.wScan = static_cast<WORD>(
                        MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
                    input.ki.dwFlags = (p[0] == 'R') ? KEYEVENTF_KEYUP : 0;
                    SendInput(1, &input, sizeof(INPUT));
                }
            }
            char* nl = strchr(p, '\n');
            p = nl ? nl + 1 : p + strlen(p);
        }
    }

    CloseHandle(pipe);
    return 0;
}
