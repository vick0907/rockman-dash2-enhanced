# Licensing and Third-Party Notices

This is an unofficial, game-local compatibility and enhancement project. It is not an official Capcom or SafeDiscShim release. It does not include or license the original game, disc images, audio, graphics, fonts, or trademarks.

The project code is provided under GPL-3.0-or-later, with the SafeDisc linking permission retained in LICENSE.md. Original upstream copyright and license notices are preserved. Subtitle translations are unofficial adaptations of game dialogue; the code license does not grant rights to the underlying game or dialogue.

## Incorporated Components

| Component | Version / Revision | License | Source |
| --- | --- | --- | --- |
| SafeDiscShim IOCTL implementation | 759b10399e81f971faf11d87e805463ff5e413cf | GPL-3.0-or-later with additional permission | https://github.com/RibShark/SafeDiscShim/tree/759b10399e81f971faf11d87e805463ff5e413cf |
| MinHook | 1.3.4 | BSD-2-Clause; retained HDE notices | https://github.com/TsudaKageyu/minhook/tree/v1.3.4 |
| nlohmann/json | 3.11.3 | MIT | https://github.com/nlohmann/json/tree/v3.11.3 |
| Xidi | 5.0.0, official x86 release | BSD-3-Clause | https://github.com/samuelgr/Xidi/releases/tag/v5.0.0 |
| LLVM libc++ and libc++abi | As shipped with Zig 0.14.1 | Apache-2.0 with LLVM exceptions and retained notices | https://ziglang.org/download/0.14.1/release-notes.html |
| MinGW-w64 runtime | As shipped with Zig 0.14.1 | Component-specific notices in COPYING | https://www.mingw-w64.org/ |

Relevant upstream sources and licenses are in third_party. Xidi binaries and Zig are fetched from pinned official archives by Build.ps1; neither the original game nor a proprietary SDK is needed to compile this project. Microsoft Windows system libraries and the separately installed Microsoft Visual C++ runtime are not bundled.

The single EXE embeds the third-party license texts and extracts them into its private runtime's licenses directory. The player archive also carries these notices and LICENSE.md. Distributing the EXE requires providing recipients with its complete corresponding source, including the vendored sources and build scripts. A private GitHub URL is not sufficient for recipients who cannot access it; provide the matching source archive with the player package.

The SafeDisc IOCTL code is used for user-mode compatibility. This project does not install the obsolete driver, remove the game's own disc checks, or require disabling Windows security.