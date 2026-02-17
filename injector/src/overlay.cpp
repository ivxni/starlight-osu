/*
 * starlight-sys :: injector/src/overlay.cpp
 *
 * External DX11 + ImGui overlay implementation.
 */

#include "overlay.h"

#include <timeapi.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

Overlay::Overlay() {}

Overlay::~Overlay()
{
    if (m_imguiInit) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_imguiInit = false;
    }
    CleanupRenderTarget();
    CleanupD3D11();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        UnregisterClassW(L"StarlightOverlay", GetModuleHandle(nullptr));
    }
}

/* ------------------------------------------------------------------ */
/*  Window Creation                                                    */
/* ------------------------------------------------------------------ */

bool Overlay::CreateOverlayWindow(const wchar_t* targetTitle)
{
    m_targetHwnd = FindWindowW(nullptr, targetTitle);
    if (!m_targetHwnd) {
        /* Try class name for Valorant */
        m_targetHwnd = FindWindowW(L"UnrealWindow", nullptr);
    }

    if (m_targetHwnd) {
        RECT rc{};
        GetClientRect(m_targetHwnd, &rc);
        m_width  = rc.right - rc.left;
        m_height = rc.bottom - rc.top;
        if (m_width <= 0)  m_width  = 1920;
        if (m_height <= 0) m_height = 1080;
    }

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = L"StarlightOverlay";
    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TRANSPARENT |
                    WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    POINT pos{0, 0};
    if (m_targetHwnd) {
        RECT rc{};
        GetWindowRect(m_targetHwnd, &rc);
        pos.x = rc.left;
        pos.y = rc.top;
    }

    m_hwnd = CreateWindowExW(
        exStyle,
        L"StarlightOverlay",
        L"",
        WS_POPUP,
        pos.x, pos.y, m_width, m_height,
        nullptr, nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    if (!m_hwnd) return false;

    /* Make the window fully transparent to the compositor */
    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    /* Extend frame into client area for DWM transparency */
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_hwnd);

    return true;
}

/* ------------------------------------------------------------------ */
/*  DirectX 11 Setup                                                   */
/* ------------------------------------------------------------------ */

bool Overlay::CreateD3D11Device()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = m_width;
    sd.BufferDesc.Height                  = m_height;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = m_hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.SampleDesc.Quality                 = 0;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd,
        &m_swapChain, &m_device, &featureLevel, &m_deviceContext
    );

    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void Overlay::CreateRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        m_device->CreateRenderTargetView(backBuffer, nullptr, &m_renderTarget);
        backBuffer->Release();
    }
}

void Overlay::CleanupRenderTarget()
{
    if (m_renderTarget) { m_renderTarget->Release(); m_renderTarget = nullptr; }
}

void Overlay::CleanupD3D11()
{
    if (m_swapChain)     { m_swapChain->Release();     m_swapChain = nullptr; }
    if (m_deviceContext)  { m_deviceContext->Release();  m_deviceContext = nullptr; }
    if (m_device)         { m_device->Release();         m_device = nullptr; }
}

/* ------------------------------------------------------------------ */
/*  ImGui Init                                                         */
/* ------------------------------------------------------------------ */

bool Overlay::InitImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding  = 2.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.85f;

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_device, m_deviceContext);

    m_imguiInit = true;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Window Position Tracking                                           */
/* ------------------------------------------------------------------ */

void Overlay::UpdateWindowPosition()
{
    if (!m_targetHwnd || !IsWindow(m_targetHwnd)) return;

    RECT rc{};
    GetWindowRect(m_targetHwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    /* Only move if changed */
    RECT myRc{};
    GetWindowRect(m_hwnd, &myRc);
    if (myRc.left != rc.left || myRc.top != rc.top ||
        (myRc.right - myRc.left) != w || (myRc.bottom - myRc.top) != h)
    {
        MoveWindow(m_hwnd, rc.left, rc.top, w, h, FALSE);
        m_width  = w;
        m_height = h;
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

bool Overlay::Initialize(const wchar_t* targetWindowTitle)
{
    if (!CreateOverlayWindow(targetWindowTitle)) {
        printf("[!] Overlay: failed to create window\n");
        return false;
    }
    if (!CreateD3D11Device()) {
        printf("[!] Overlay: failed to create DX11 device\n");
        return false;
    }
    if (!InitImGui()) {
        printf("[!] Overlay: failed to init ImGui\n");
        return false;
    }
    printf("[+] Overlay: initialized (%dx%d)\n", m_width, m_height);
    return true;
}

static bool s_menuActive = false;

void Overlay::Run(std::function<void()> renderCallback)
{
    m_running = true;
    MSG msg{};

    /* Clear stale key states -- GetAsyncKeyState can return
     * leftover presses from before the overlay started */
    GetAsyncKeyState(VK_INSERT);
    GetAsyncKeyState(VK_END);
    Sleep(500);
    GetAsyncKeyState(VK_INSERT);
    GetAsyncKeyState(VK_END);

    /* High-resolution frame limiter (target ~144 fps) */
    timeBeginPeriod(1);
    LARGE_INTEGER qpcFreq, lastFrame;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&lastFrame);
    const double targetFrameTime = 1.0 / 144.0;

    printf("[+] Overlay: render loop started (144 fps target)\n"); fflush(stdout);

    while (m_running) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                printf("[*] Overlay: WM_QUIT received\n"); fflush(stdout);
                m_running = false;
            }
        }
        if (!m_running) break;

        /* Toggle menu visibility with INSERT key */
        if (GetAsyncKeyState(VK_INSERT) & 1) {
            m_menuVisible = !m_menuVisible;
            LONG exStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
            if (m_menuVisible) {
                /* Make overlay clickable and bring to front for input */
                exStyle &= ~WS_EX_TRANSPARENT;
                exStyle &= ~WS_EX_NOACTIVATE;
                SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);
                SetForegroundWindow(m_hwnd);
                SetFocus(m_hwnd);
            } else {
                /* Click-through again, give focus back to game */
                exStyle |= WS_EX_TRANSPARENT;
                exStyle |= WS_EX_NOACTIVATE;
                SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);
                if (m_targetHwnd && IsWindow(m_targetHwnd))
                    SetForegroundWindow(m_targetHwnd);
            }
            s_menuActive = m_menuVisible;
            printf("[*] Menu: %s\n", m_menuVisible ? "visible" : "hidden");
            fflush(stdout);
        }

        /* Exit on END key (require two consecutive checks to avoid stale presses) */
        static int endKeyCount = 0;
        if (GetAsyncKeyState(VK_END) & 0x8000) {
            endKeyCount++;
            if (endKeyCount > 5) {
                printf("[*] Overlay: END key pressed, exiting\n"); fflush(stdout);
                m_running = false;
                break;
            }
        } else {
            endKeyCount = 0;
        }

        /* Only show overlay when game window is active/foreground */
        if (m_targetHwnd && IsWindow(m_targetHwnd)) {
            HWND fg = GetForegroundWindow();
            bool gameActive = (fg == m_targetHwnd || fg == m_hwnd);

            if (!gameActive) {
                /* Hide overlay when game is not focused (alt-tabbed) */
                if (IsWindowVisible(m_hwnd))
                    ShowWindow(m_hwnd, SW_HIDE);
                Sleep(50);
                continue;
            } else if (!IsWindowVisible(m_hwnd)) {
                ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            }
        }

        /* Track game window position + keep on top (throttled, not every frame) */
        static DWORD lastPosUpdate = 0;
        DWORD now = GetTickCount();
        if (now - lastPosUpdate > 500) {
            UpdateWindowPosition();
            SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            lastPosUpdate = now;
        }

        /* Start ImGui frame */
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        /* Call the user's render callback (ESP, bones, etc.) */
        if (renderCallback)
            renderCallback();

        /* Render */
        ImGui::Render();

        const float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_deviceContext->OMSetRenderTargets(1, &m_renderTarget, nullptr);
        m_deviceContext->ClearRenderTargetView(m_renderTarget, clearColor);

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = m_swapChain->Present(0, 0);
        if (FAILED(hr)) {
            printf("[!] Overlay: Present() failed (0x%08lX)\n", hr);
            fflush(stdout);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                m_running = false;
                break;
            }
        }

        /* Frame limiter: spin-wait for precise timing */
        for (;;) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsed = (double)(now.QuadPart - lastFrame.QuadPart) / qpcFreq.QuadPart;
            if (elapsed >= targetFrameTime) {
                lastFrame = now;
                break;
            }
            if (targetFrameTime - elapsed > 0.002)
                Sleep(1);
        }
    }

    timeEndPeriod(1);
    printf("[+] Overlay: render loop ended\n"); fflush(stdout);
}

void Overlay::Shutdown()
{
    m_running = false;
    if (m_hwnd)
        PostMessage(m_hwnd, WM_CLOSE, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  WndProc                                                            */
/* ------------------------------------------------------------------ */

LRESULT CALLBACK Overlay::WndProc(HWND hWnd, UINT msg,
                                   WPARAM wParam, LPARAM lParam)
{
    /* When menu is hidden, block activation to stay click-through.
     * When menu is visible, allow normal window behavior for ImGui input. */
    if (!s_menuActive) {
        if (msg == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (msg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE)
            return 0;
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
