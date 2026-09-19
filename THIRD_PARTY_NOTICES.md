# 第三方元件及授權說明

這是僅作用於遊戲本身的非官方相容與增強專案，並非 Capcom 或 SafeDiscShim 的官方發行版本。本專案不包含原版遊戲主程式、光碟映像檔、音訊、字型或可供遊戲使用的原版圖像資源。文件另收錄四張經挑選的功能示範截圖，並附非官方繪製的啟動器圖示；原作角色、遊戲畫面與相關商標的權利仍屬原權利人，本專案不授予這些原作內容的使用權。

本專案程式碼以 GPL-3.0-or-later 提供，並在 [LICENSE.md](LICENSE.md) 保留與 SafeDisc 連結的額外許可。各上游專案的著作權與授權聲明均保留原文。字幕翻譯是遊戲對白的非官方改作，程式碼授權不授予原版遊戲或對白的相關權利。

## 使用的第三方元件

| 元件 | 版本／修訂 | 授權 | 來源 |
| --- | --- | --- | --- |
| SafeDiscShim IOCTL 實作 | 759b10399e81f971faf11d87e805463ff5e413cf | GPL-3.0-or-later，含額外許可 | https://github.com/RibShark/SafeDiscShim/tree/759b10399e81f971faf11d87e805463ff5e413cf |
| MinHook | 1.3.4 | BSD-2-Clause；保留 HDE 聲明 | https://github.com/TsudaKageyu/minhook/tree/v1.3.4 |
| nlohmann/json | 3.11.3 | MIT | https://github.com/nlohmann/json/tree/v3.11.3 |
| Xidi | 5.0.0，官方 x86 發行版 | BSD-3-Clause | https://github.com/samuelgr/Xidi/releases/tag/v5.0.0 |
| LLVM libc++ 與 libc++abi | Zig 0.14.1 隨附版本 | Apache-2.0，含 LLVM 例外條款及保留聲明 | https://ziglang.org/download/0.14.1/release-notes.html |
| MinGW-w64 執行階段 | Zig 0.14.1 隨附版本 | 依 COPYING 中各元件的授權聲明 | https://www.mingw-w64.org/ |

相關上游原始碼及授權收錄於 `third_party`。Xidi 執行檔與 Zig 由 [Build.ps1](Build.ps1) 下載指定版本的官方封存檔，並以雜湊值驗證。編譯本專案不需要原版遊戲或專有 SDK。本補丁不附 Microsoft Windows 系統程式庫，也不附需另行安裝的 Microsoft Visual C++ 執行階段套件。

單一 EXE 會內嵌第三方授權原文，並展開到專用執行資料夾中的 `licenses`。玩家版封存檔也會附上本說明與 [LICENSE.md](LICENSE.md)。散布 EXE 時，必須向接收者提供完整的對應原始碼，包括隨附的第三方原始碼及建置腳本。若接收者沒有私人 GitHub 儲存庫的存取權，只提供網址並不足夠；請一併提供與玩家版相符的原始碼封存檔。

SafeDisc IOCTL 程式碼用於使用者模式的相容處理。本專案不安裝過時的驅動程式，不移除遊戲本身的光碟驗證，也不要求停用 Windows 安全防護。

本文件是繁體中文說明，不取代或修改各元件的正式授權條款；正式條款請以隨附的原始授權文件為準。