#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "parser.h"

void ui_init(GLFWwindow *window);
void ui_render(GLFWwindow *window);
void ui_shutdown(void);
bool ui_get_show_timestamps(void);

#define MUTEX_NAME "ChatParser_SingleInstance"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int nShow) {
        (void)hInst; (void)hPrev; (void)cmd; (void)nShow;
        HANDLE mutex = CreateMutexA(NULL, TRUE, MUTEX_NAME);
        if (!mutex) {
                return 1;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
                MessageBoxA(NULL, "fivem-parser is already running.", "fivem-parser", MB_ICONINFORMATION);
                CloseHandle(mutex);
                return 1;
        }
        if (!glfwInit()) {
                MessageBoxA(NULL, "Failed to initialize GLFW.", "fivem-parser", MB_ICONERROR);
                CloseHandle(mutex);
                return 1;
        }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        GLFWwindow *window = glfwCreateWindow(800, 500, "fivem-parser v1.1.2", NULL, NULL);
        if (!window) {
                glfwTerminate();
                CloseHandle(mutex);
                return 1;
        }
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);
        glfwSetWindowSizeLimits(window, 600, 400, GLFW_DONT_CARE, GLFW_DONT_CARE);
        config_load();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = NULL;
        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 130");
        ui_init(window);
        while (!glfwWindowShouldClose(window)) {
                glfwWaitEventsTimeout(0.5);
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                ui_render(window);
                ImGui::Render();
                int dw, dh;
                glfwGetFramebufferSize(window, &dw, &dh);
                glViewport(0, 0, dw, dh);
                glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glfwSwapBuffers(window);
        }
        g_config.show_timestamps = ui_get_show_timestamps() ? 1 : 0;
        config_save();
        ui_shutdown();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        CloseHandle(mutex);
        return 0;
}
