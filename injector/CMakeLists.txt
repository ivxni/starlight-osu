# ---- ImGui vendor sources ----
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/imgui_impl_win32.cpp
)

add_executable(starlight-osu
    src/main.cpp
    src/byovd.cpp
    src/physmem.cpp
    src/kernel.cpp
    src/overlay.cpp
    src/arduino_serial.cpp
    src/config.cpp
    src/sig_scanner.cpp
    src/osu_reader.cpp
    src/relax.cpp
    src/kernel_input.cpp
    src/aim_assist.cpp
    src/renderer.cpp
    ${IMGUI_SOURCES}
)


target_include_directories(starlight-osu PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/shared
    ${IMGUI_DIR}
)

# Link Windows libraries + DirectX 11
target_link_libraries(starlight-osu PRIVATE
    advapi32   # SCM (Service Control Manager)
    ntdll      # Low-level NT functions
    psapi      # Process Status API (EnumDeviceDrivers)
    d3d11      # DirectX 11
    dxgi       # DXGI
    dwmapi     # Desktop Window Manager (transparency)
)

# Output name: slhost.exe (generic-looking)
set_target_properties(starlight-osu PROPERTIES
    OUTPUT_NAME "slhost"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
