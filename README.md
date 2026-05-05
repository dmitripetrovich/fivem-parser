# fivem-parser

A lightweight tool used to parse chat logs generated during a FiveM session.

Built with [GLFW](https://www.glfw.org/) and [ImGui](https://github.com/ocornut/imgui).

https://github.com/user-attachments/assets/e6b9e2f4-42dc-4b4a-9036-c37bbe060f99

https://github.com/user-attachments/assets/b9257680-8563-4eb3-851c-ae655ef83da2

## Building

Requires [xmake](https://xmake.io/) and a mingw-w64 toolchain.

```bash
xmake
```

## Setup

To be able to parse a session's chat messages, the server must be running the included `fivem-parser` resource. This is addon that simply logs chat messages to the client's log file, which the parser then reads.

1. Download or clone the repository with `git clone https://github.com/bd53/fivem-parser`.
2. Copy `resources/fivem-parser` folder into the `resources/` directory.
3. Add `ensure fivem-parser` to where resources are being loaded (after chat resource).

This removes the need for any modifications to your chat resource.

## License

A complete copy of the license is included in the [fivem-parser/LICENSE](./LICENSE) file.
