local component = dofile("component.lua")

workspace "fivem-parser"
        configurations { "Debug", "Release" }
        language "C++"
        cppdialect "C++17"
        cdialect "C11"
        staticruntime "on"
        warnings "Off"
        location "build"
        filter "configurations:Debug"
                symbols "On"
                optimize "Off"
        filter "configurations:Release"
                optimize "Speed"
        filter {}
        targetdir "."
        objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

component.projects()

project "Parser"
        kind "WindowedApp"
        language "C++"
        cppdialect "C++17"
        toolset "gcc"
        files {
                "*.c",
                "*.cpp",
        }
        component.link()
        linkoptions { "-mwindows", "-static", "-s" }
        links { "stdc++", "opengl32", "gdi32", "user32", "kernel32", "comdlg32", "shell32", "dwmapi" }
        targetname "Parser"
