#pragma once
/*
 * starlight-sys :: injector/src/overlay.h
 *
 * External DirectX 11 overlay window with ImGui rendering.
 * Creates a transparent, topmost, click-through window that sits
 * on top of the game and renders ESP / bone data.
 */

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <functional>

class Overlay {
public:
    Overlay();
    ~Overlay();

    /* Initialize the overlay: create window, DX11 device, ImGui context.
     * targetWindowTitle: the title of the game window to overlay on.
     * Returns true on success. */
    bool Initialize(const wchar_t* targetWindowTitle);

    /* Run the render loop. Blocks until the overlay is closed.
     * renderCallback is called every frame between ImGui::NewFrame() and
     * ImGui::Render(), allowing the caller to draw ESP, bones, etc. */
    void Run(std::function<void()> renderCallback);

    /* Signal the overlay to shut down (can be called from another thread). */
    void Shutdown();

    /* Get the overlay window dimensions */
    int GetWidth()  const { return m_width; }
    int GetHeight() const { return m_height; }
    bool IsRunning() const { return m_running; }
    bool IsMenuVisible() const { return m_menuVisible; }

private:
    bool CreateOverlayWindow(const wchar_t* targetTitle);
    bool CreateD3D11Device();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void CleanupD3D11();
    bool InitImGui();
    void UpdateWindowPosition();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam);

    HWND                    m_hwnd          = nullptr;
    HWND                    m_targetHwnd    = nullptr;
    ID3D11Device*           m_device        = nullptr;
    ID3D11DeviceContext*    m_deviceContext  = nullptr;
    IDXGISwapChain*         m_swapChain     = nullptr;
    ID3D11RenderTargetView* m_renderTarget  = nullptr;

    int  m_width       = 1920;
    int  m_height      = 1080;
    bool m_running     = false;
    bool m_imguiInit   = false;
    bool m_menuVisible = false;
};
