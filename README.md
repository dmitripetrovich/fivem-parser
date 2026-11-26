# fivem-parser

https://github.com/user-attachments/assets/e6b9e2f4-42dc-4b4a-9036-c37bbe060f99

## Building

Requires [premake](https://premake.github.io/) and a mingw-w64 toolchain.

```bash
premake5 gmake
make -C build config=release CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ AR=x86_64-w64-mingw32-ar
```
