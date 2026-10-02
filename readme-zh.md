# SumatraPDF Sidecar

一个 SumatraPDF 修改版：批注不写进 PDF，存成旁边的 JSON 文件。

上游仓库：[sumatrapdfreader/SumatraPDF](https://github.com/sumatrapdfreader/SumatraPDF)。本仓库 fork 自官方 master，改动集中在 `src/Sidecar.cpp`（新文件）和 6 个带 `// SIDECAR:` 注释的挂载点。

PDF 批注的 Sidecar（边车文件）是指批注数据不写进 PDF 本体，而是保存为同目录下同名的独立文件（如 `book.pdf` → `book.json`）。打开 PDF 时自动加载旁边的批注文件，修改批注时只重写这个几 KB 的小文件，PDF 原封不动——这对存放在 OneDrive 等同步盘上的大型 PDF 尤其有用，避免改一个批注就触发几百 MB 的全量重传。代价是其他阅读器打开该 PDF 时看不到批注，因为数据不在 PDF 里。

## 为什么

> **SumatraPDF** 原本支持将注释存为同名 `.smx` 纯文本文件，最后支持版本为 **3.2**；
> **Okular** 早期将注释存为隐藏的 XML 文件，最后支持版本为 **1.2**。
> 两家在 2018 年前后主动砍掉该功能且明确表示不会恢复。放弃的核心原因有三：文件重命名或另存后 sidecar 容易失联，引发“数据丢失”错觉；纯文本格式无法承载复杂注释类型；跨格式坐标单位不统一导致维护成本高。最终标准 PDF 内嵌注释的成熟，使这条独立存储路线彻底退出历史舞台。

行业共识是批注理应跟着文件走，但对某些场景，这恰恰是灾难：我的 PDF 书库里有几百兆的单文件扫描件，放在 OneDrive 上同步。SumatraPDF 的批注功能一旦启用，每次画条线都改写整个 PDF——600MB 的文件，改一个字节，OneDrive 就得整个重传；关掉批注，等于放弃阅读器一半的价值。

因此我喜欢sidecar：打开带批注的 PDF，PDF 文件本身的修改时间是零。所有批注数据在旁边的 `.json` 文件里。

## 工作方式

- **导入**：打开 PDF 时，如果同目录存在同名 `.json`（例如 `book.pdf` → `book.json`），批注自动载入，PDF 本体不动。
- **自动保存**：批注变更后 2 秒防抖写盘，只写 JSON。
- **Ctrl+S**：启用 sidecar 后，Ctrl+S 保存的是 JSON，不再弹出"另存副本"。
- **中央文件夹模式**（可选）：设置 `Annotations.centralFolder` 后，所有 sidecar 集中到一处，按 `父文件夹名/文件名.json` 存放。适合书库分布在多个文件夹、或想单独同步批注库的场景。

设置项（AdvancedSettings）：

| 设置 | 默认 | 说明 |
|---|---|---|
| `Annotations.separateSave` | false | 打开 sidecar 功能 |
| `Annotations.centralFolder` | 空 | 批注集中存放目录 |
| `Annotations.separateSaveAsMd` | false | sidecar 存为 Markdown 而非 JSON（**实验性**，见下） |

```ini
Annotations [
    ....
	SeparateSave = true
	CentralFolder = E:\Downloads\Claw
    SeparateSaveAsMd = true
]
```

## 支持的批注类型

16 种，属性完整往返：

Text（便签）、FreeText（文本框）、Highlight、Underline、Squiggly、StrikeOut、Line、Square、Circle、Polygon、PolyLine、Ink、Caret、Redact、Stamp（图章）、FileAttachment（附件）

FreeText 支持字体、字号、文字颜色、对齐方式、加粗、斜体、下划线、透明背景。所有类型的颜色（含无色/透明）、透明度、边框、作者、时间戳都会保留。

图章和附件带有二进制载荷（图片、嵌入文件），JSON 不放二进制：载荷写入 JSON 旁边的 `assets/` 目录，按内容哈希命名（`assets/<hash>.<ext>`）并以相对路径引用。相同内容自动去重，保存时会清理不再被任何 sidecar 引用的资产文件。旧格式（无 asset 字段）的 sidecar 仍可正常导入。

## 已知限制

- **没有冲突合并。** 两台机器同时改同一个 sidecar，后保存的覆盖先保存的。单人使用没问题。
- 大批注有防御性上限：多边形/折线 512 个顶点，墨迹 64 笔、每笔最多 2048 个点。超出的部分截断。
- 别的阅读器打开这个 PDF 看不到批注——数据在 JSON 里，不在 PDF 里。这是设计使然。

## Markdown sidecar（实验性 🧪）

设置 `SeparateSaveAsMd = true` 后，批注存成 `book.md` 而不是 `book.json`：每条批注是一个 Obsidian callout，可用md编辑器编辑。

```markdown
---
sumatrapdf_sidecar: 2
generator: SumatraPDF-sidecar/2-md
file: 教材.pdf
---
# 我的读书笔记（随便写，保存时逐字保留）

> [!Note]
> type: highlight
> page: 11
> rect: [58.4,695.2,299.6,708.4]
> quads: [[58.4,708.4,299.6,695.2,58.4,695.2,299.6,708.4]]
> text: highlighted words
> contents: highlighted words
> author: AlexShy
```

规则：

- **callout 外的一切都是你的**。标题、散文、普通引用块——SumatraPDF 读写时逐字保留，只动 callout 里 `key: value` 形式的机器行。批注数据和批注笔记从此住同一个文件。
- **contents 行恒存在**。没有批注文字时用高亮原文填充；两者都空时是 `contents: ""`，在 Obsidian 里手填，重开文档生效。
- **双向同步，以重开为界**。关闭文档后删掉一个 callout，重开时该批注消失；手写一个合法 callout（不推荐，难以精确计算坐标），重开时变成真批注。文档开着时改 md，保存会被会话数据覆盖——外部编辑请关掉文档再动。
- **自动迁移**。开启后读取优先 `.md`，没有则回退 `.json`；下一次保存写 `.md`，旧 `.json` 原样保留不再更新。
- **原子写盘**。md 混着你的笔记，保存走临时文件 + 替换，中途崩溃不会毁文件。
- **查询**：front matter 是 YAML 字段；callout 内 `key:: value` 双冒号兼容。

⚠️ **实验性说明**：格式仍可能调整（带版本号字段，保证旧文件可读）；单人场景验证有限。

## 许可证

GPLv3，沿用上游。
