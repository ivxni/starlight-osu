/*
 * starlight-sys :: injector/src/arduino_serial.cpp
 *
 * Shared Arduino Starlink serial interface.
 * Manages a single COM port connection used by triggerbot and aimbot.
 */

#include "arduino_serial.h"
#include <cstdio>
#include <cstring>
#include <mutex>

static HANDLE   g_serial     = INVALID_HANDLE_VALUE;
static bool     g_tried      = false;
static DWORD    g_lastTryMs  = 0;
static char     g_port[16]   = {};
static std::mutex g_mtx;

bool ArduinoOpen(const char* comPort)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_serial != INVALID_HANDLE_VALUE) return true;
    /* Retry every 5 seconds if previous attempt failed */
    DWORD now = GetTickCount();
    if (g_tried && (now - g_lastTryMs) < 5000) return false;
    g_tried = true;
    g_lastTryMs = now;

    char portToTry[16] = {};
    if (comPort && comPort[0]) {
        strncpy(portToTry, comPort, sizeof(portToTry) - 1);
    } else {
        printf("[*] Arduino: scanning COM ports for Starlink...\n");
        fflush(stdout);

        for (int i = 1; i <= 32; i++) {
            char path[32];
            snprintf(path, sizeof(path), "\\\\.\\COM%d", i);

            HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                                   0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;

            DCB dcb{};
            dcb.DCBlength = sizeof(DCB);
            GetCommState(h, &dcb);
            dcb.BaudRate = 115200;
            dcb.ByteSize = 8;
            dcb.Parity   = NOPARITY;
            dcb.StopBits = ONESTOPBIT;
            SetCommState(h, &dcb);

            COMMTIMEOUTS to{};
            to.ReadIntervalTimeout        = 50;
            to.ReadTotalTimeoutConstant   = 300;
            to.ReadTotalTimeoutMultiplier = 0;
            to.WriteTotalTimeoutConstant  = 100;
            SetCommTimeouts(h, &to);

            PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
            Sleep(50);

            const char* ping = "?\n";
            DWORD written = 0;
            WriteFile(h, ping, 2, &written, nullptr);
            Sleep(200);

            char buf[64] = {};
            DWORD bytesRead = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &bytesRead, nullptr);

            if (bytesRead > 0 && strstr(buf, "OK")) {
                snprintf(portToTry, sizeof(portToTry), "COM%d", i);
                printf("[+] Arduino: Starlink found on COM%d\n", i);
                fflush(stdout);
                CloseHandle(h);
                break;
            }
            CloseHandle(h);
        }

        if (!portToTry[0]) {
            printf("[!] Arduino: Starlink not found on any COM port\n");
            fflush(stdout);
            return false;
        }
    }

    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\%s", portToTry);

    g_serial = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_serial == INVALID_HANDLE_VALUE) {
        printf("[!] Arduino: failed to open %s (error %lu)\n",
               portToTry, GetLastError());
        fflush(stdout);
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    GetCommState(g_serial, &dcb);
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(g_serial, &dcb);

    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout        = 50;
    to.ReadTotalTimeoutConstant   = 100;
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant  = 50;
    SetCommTimeouts(g_serial, &to);

    PurgeComm(g_serial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    strncpy(g_port, portToTry, sizeof(g_port) - 1);
    printf("[+] Arduino: Starlink connected on %s (HID mouse)\n", g_port);
    fflush(stdout);
    return true;
}

void ArduinoClose()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_serial != INVALID_HANDLE_VALUE) {
        CloseHandle(g_serial);
        g_serial = INVALID_HANDLE_VALUE;
    }
    g_tried = false;
    g_port[0] = '\0';
}

bool ArduinoSend(const char* cmd)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_serial == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    DWORD len = (DWORD)strlen(cmd);
    return WriteFile(g_serial, cmd, len, &written, nullptr) && written == len;
}

bool ArduinoIsOpen()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_serial != INVALID_HANDLE_VALUE;
}

const char* ArduinoGetPort()
{
    return g_port;
}
