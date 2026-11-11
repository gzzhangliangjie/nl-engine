// imgui_backend.cpp — 编辑器极简 ImGui 后端（GLFW + OpenGL 2.0 固定管线）
#include "imgui_backend.hpp"
#include <imgui.h>
#include <GL/gl.h>
#include <cstdio>
#include <string>

namespace nl::editor::ui {

static GLuint g_FontTexture = 0;
static GLFWwindow* g_Window = nullptr;
static double g_Wheel = 0.0;
static int g_MouseJustPressed[3] = {0, 0, 0};

static const char* clipboardGet(void* /*user*/) {
    static std::string buf;
    if (g_Window) {
        const char* s = ::glfwGetClipboardString(g_Window);
        if (s) buf = s;
    }
    return buf.c_str();
}
static void clipboardSet(void* /*user*/, const char* text) {
    if (g_Window) ::glfwSetClipboardString(g_Window, text);
}
static void scrollCallback(GLFWwindow* /*w*/, double /*x*/, double y) { g_Wheel += y; }
static void mouseButtonCallback(GLFWwindow* /*w*/, int button, int action, int /*mods*/) {
    if (button >= 0 && button < 3) g_MouseJustPressed[button] = (action == GLFW_PRESS);
}

static void createFontsTexture() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h, nullptr);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    io.Fonts->TexID = (ImTextureID)tex;   // 本构建 ImTextureID = ImU64
    g_FontTexture = tex;
}

bool imguiInit(GLFWwindow* window) {
    g_Window = window;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "imgui_impl_gl2_nl";
    // 注意：GL2 固定管线不支持 VtxOffset（官方 imgui_impl_opengl2 不设此位）；
    // 16 位索引超 64K 时 core 会自动拆分 cmd。
    io.SetClipboardTextFn = clipboardSet;
    io.GetClipboardTextFn = clipboardGet;
    io.ClipboardUserData = nullptr;
    ::glfwSetScrollCallback(window, scrollCallback);
    ::glfwSetMouseButtonCallback(window, mouseButtonCallback);
    createFontsTexture();
    return true;
}

void imguiShutdown() {
    if (g_FontTexture) {
        glDeleteTextures(1, &g_FontTexture);
        ImGui::GetIO().Fonts->TexID = (ImTextureID)0;
        g_FontTexture = 0;
    }
}

void imguiNewFrame(GLFWwindow* window) {
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(g_FontTexture != 0 && "imguiInit 必须先调用");

    int w = 0, h = 0, displayW = 0, displayH = 0;
    ::glfwGetWindowSize(window, &w, &h);
    ::glfwGetFramebufferSize(window, &displayW, &displayH);
    io.DisplaySize = ImVec2((float)w, (float)h);
    if (w > 0 && h > 0)
        io.DisplayFramebufferScale = ImVec2((float)displayW / (float)w, (float)displayH / (float)h);

    // 鼠标
    double mx = 0, my = 0;
    ::glfwGetCursorPos(window, &mx, &my);
    io.MousePos = ImVec2((float)mx, (float)my);
    for (int i = 0; i < 3; ++i) {
        io.MouseDown[i] = g_MouseJustPressed[i] || ::glfwGetMouseButton(window, i) != 0;
        g_MouseJustPressed[i] = 0;
    }
    io.MouseWheel += (float)g_Wheel;
    g_Wheel = 0.0;

    ImGui::NewFrame();
}

void imguiRenderDrawData() {
    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw || !draw->Valid) return;

    int fbW = (int)(draw->DisplaySize.x * draw->FramebufferScale.x);
    int fbH = (int)(draw->DisplaySize.y * draw->FramebufferScale.y);
    if (fbW <= 0 || fbH <= 0) return;
    if (draw->CmdListsCount <= 0) return;

    glViewport(0, 0, fbW, fbH);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_SCISSOR_TEST);

    glMatrixMode(GL_PROJECTION); glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, draw->DisplaySize.x, draw->DisplaySize.y, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix();
    glLoadIdentity();

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

#define OFFSETOF(T, U) ((size_t)&(((T*)0)->U))
    for (int n = 0; n < draw->CmdListsCount; ++n) {
        const ImDrawList* list = draw->CmdLists[n];
        const ImDrawVert* vtx = list->VtxBuffer.Data;
        const ImDrawIdx* idx = list->IdxBuffer.Data;
        // client arrays 模式：第 4 参数是「内存指针」，必须传 buffer 基址 + 偏移
        //（照官方 imgui_impl_opengl2 口径；只传纯偏移 = 驱动从近零地址读顶点 → 段错误）
        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert), (const GLvoid*)vtx);
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                          (const GLvoid*)((const char*)vtx + OFFSETOF(ImDrawVert, uv)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert),
                       (const GLvoid*)((const char*)vtx + OFFSETOF(ImDrawVert, col)));

        for (int cmd_i = 0; cmd_i < list->CmdBuffer.Size; ++cmd_i) {
            const ImDrawCmd* pcmd = &list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback) { pcmd->UserCallback(list, pcmd); continue; }
            glBindTexture(GL_TEXTURE_2D, (GLuint)pcmd->TextureId);
            glScissor((int)pcmd->ClipRect.x,
                      (int)(fbH - pcmd->ClipRect.w),
                      (int)(pcmd->ClipRect.z - pcmd->ClipRect.x),
                      (int)(pcmd->ClipRect.w - pcmd->ClipRect.y));
            glDrawElements(GL_TRIANGLES, (GLsizei)pcmd->ElemCount,
                           sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, idx);
            idx += pcmd->ElemCount;
        }
    }
#undef OFFSETOF

    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_SCISSOR_TEST);
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
}

} // namespace nl::editor::ui
