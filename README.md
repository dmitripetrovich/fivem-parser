# fivem-parser

A lightweight tool used to parse chat logs generated during a FiveM session.

Built with [GLFW](https://www.glfw.org/) and [ImGui](https://github.com/ocornut/imgui).

https://github.com/user-attachments/assets/e6b9e2f4-42dc-4b4a-9036-c37bbe060f99

https://github.com/user-attachments/assets/b9257680-8563-4eb3-851c-ae655ef83da2

## Building

Requires [premake](https://premake.github.io/) and a mingw-w64 toolchain.

```bash
premake5 gmake
make -C build config=release CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ AR=x86_64-w64-mingw32-ar
```
