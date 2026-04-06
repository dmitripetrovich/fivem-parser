set_project("fivem-parser")
set_version("1.0.3")

set_plat("mingw")
set_arch("x86_64")

add_rules("mode.debug", "mode.release")
set_policy("build.fence", true)
set_languages("c11", "cxx17")
set_warnings("none")
set_runtimes("static")

if is_mode("debug") then
        set_optimize("none")
        set_symbols("debug")
else
        set_optimize("fastest")
        set_strip("all")
end

local ROOT = os.projectdir()
local V_GLFW = ("%s/vendor/glfw"):format(ROOT)
local V_IMGUI = ("%s/vendor/imgui"):format(ROOT)

target("glfw")
        set_kind("static")
        set_group("vendor")
        add_files(("%s/src/*.c|glx_context.c|linux_joystick.c|macos_time.c|posix_module.c|posix_poll.c|posix_thread.c|posix_time.c|wl_init.c|wl_monitor.c|wl_window.c|x11_init.c|x11_monitor.c|x11_window.c|xkb_unicode.c"):format(V_GLFW))
        add_defines("_GLFW_WIN32")
        add_includedirs(("%s/include"):format(V_GLFW), ("%s/src"):format(V_GLFW))

target("imgui")
        set_kind("static")
        set_group("vendor")
        add_files(("%s/*.cpp|imgui_demo.cpp"):format(V_IMGUI))
        add_files(("%s/backends/imgui_impl_glfw.cpp"):format(V_IMGUI), ("%s/backends/imgui_impl_opengl3.cpp"):format(V_IMGUI))
        add_includedirs(V_IMGUI, ("%s/include"):format(V_GLFW))

target("Parser")
        set_kind("binary")
        set_group("app")
        add_files("*.c", "*.cpp")
        add_includedirs(("%s/include"):format(V_GLFW), V_IMGUI, ("%s/backends"):format(V_IMGUI))
        add_deps("glfw", "imgui")
        add_ldflags("-mwindows", "-static", {force = true})
        add_syslinks("opengl32", "gdi32", "user32", "kernel32", "comdlg32", "shell32", "dwmapi")
        set_filename("Parser.exe")
        set_targetdir("$(projectdir)")
