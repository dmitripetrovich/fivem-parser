local cwd = os.getcwd()
local vendor_dir = ("%s/vendor"):format(cwd)

local V_GLFW = ("%s/glfw"):format(vendor_dir)
local V_IMGUI = ("%s/imgui"):format(vendor_dir)
local V_STB = ("%s/stb"):format(vendor_dir)

local Parser = {}

function Parser.projects()
        project "glfw"
                kind "StaticLib"
                language "C"
                cdialect "C11"
                toolset "gcc"
                targetdir "build"
                files {
                        ("%s/src/context.c"):format(V_GLFW),
                        ("%s/src/egl_context.c"):format(V_GLFW),
                        ("%s/src/init.c"):format(V_GLFW),
                        ("%s/src/input.c"):format(V_GLFW),
                        ("%s/src/monitor.c"):format(V_GLFW),
                        ("%s/src/null_init.c"):format(V_GLFW),
                        ("%s/src/null_joystick.c"):format(V_GLFW),
                        ("%s/src/null_monitor.c"):format(V_GLFW),
                        ("%s/src/null_window.c"):format(V_GLFW),
                        ("%s/src/osmesa_context.c"):format(V_GLFW),
                        ("%s/src/platform.c"):format(V_GLFW),
                        ("%s/src/vulkan.c"):format(V_GLFW),
                        ("%s/src/wgl_context.c"):format(V_GLFW),
                        ("%s/src/win32_init.c"):format(V_GLFW),
                        ("%s/src/win32_joystick.c"):format(V_GLFW),
                        ("%s/src/win32_module.c"):format(V_GLFW),
                        ("%s/src/win32_monitor.c"):format(V_GLFW),
                        ("%s/src/win32_thread.c"):format(V_GLFW),
                        ("%s/src/win32_time.c"):format(V_GLFW),
                        ("%s/src/win32_window.c"):format(V_GLFW),
                        ("%s/src/window.c"):format(V_GLFW),
                }
                defines { "_GLFW_WIN32" }
                includedirs {
                        ("%s/include"):format(V_GLFW),
                        ("%s/src"):format(V_GLFW),
                }

        project "imgui"
                kind "StaticLib"
                language "C++"
                cppdialect "C++17"
                toolset "gcc"
                targetdir "build"
                files {
                        ("%s/imgui.cpp"):format(V_IMGUI),
                        ("%s/imgui_draw.cpp"):format(V_IMGUI),
                        ("%s/imgui_tables.cpp"):format(V_IMGUI),
                        ("%s/imgui_widgets.cpp"):format(V_IMGUI),
                        ("%s/backends/imgui_impl_glfw.cpp"):format(V_IMGUI),
                        ("%s/backends/imgui_impl_opengl3.cpp"):format(V_IMGUI),
                }
                includedirs {
                        V_IMGUI,
                        ("%s/backends"):format(V_IMGUI),
                        ("%s/include"):format(V_GLFW),
                }
end

function Parser.link()
        filter {}
        includedirs {
                ("%s/include"):format(V_GLFW),
                V_IMGUI,
                ("%s/backends"):format(V_IMGUI),
                V_STB,
        }
        links { "glfw", "imgui" }
end

return Parser
