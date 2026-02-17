cmake_minimum_required(VERSION 3.20)
project(starlight-osu LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(MSVC)
    add_compile_options(/W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE)
    add_compile_options(/DWIN32_LEAN_AND_MEAN /DNOMINMAX)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ---- Injector / Memory Reader ----
add_subdirectory(injector)
