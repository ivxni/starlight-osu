add_executable(starlight-osu
    src/main.cpp
    src/byovd.cpp
    src/physmem.cpp
    src/kernel.cpp
    src/arduino_serial.cpp
    src/config.cpp
    src/sig_scanner.cpp
    src/osu_reader.cpp
    src/relax.cpp
    src/kernel_input.cpp
    src/aim_assist.cpp
)

target_include_directories(starlight-osu PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/shared
)

# Link Windows libraries
target_link_libraries(starlight-osu PRIVATE
    advapi32   # SCM (Service Control Manager)
    ntdll      # Low-level NT functions
    psapi      # Process Status API (EnumDeviceDrivers)
    winmm      # timeBeginPeriod / timeEndPeriod (1ms timer resolution)
    winhttp    # HTTP client for config polling
)

# Output name: slhost.exe (generic-looking)
set_target_properties(starlight-osu PROPERTIES
    OUTPUT_NAME "slhost"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
