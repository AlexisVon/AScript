# 库目录与 axp 搜索路径

适用：`link` / `import` 的路径解析、axp 携带的搜索路径、设置页的「库」、语言参考的「库」文档页。

## 三层目录

| 层 | 内容 | 来源 | 可编辑 |
|---|---|---|---|
| 系统 | `<appdir>`、`<appdir>/links`、`<appdir>/imports` | 宿主进程（axc-cli / axc-runner / axc-gui） | 否，设置页只读 |
| 用户 | 任意目录表达式 | 设置 → 库（QSettings `lib/userDirs`） | 是 |
| 工程/产物 | `.ax` 的 `path` / `docpath`；axp hint 的 `search_paths` | 项目文件 / 编译产物 | 随文件 |

`<appdir>` 是可执行文件所在目录。CLI 与 runner 每次建引擎时自动并入系统三目录；设置页只读展示同一份列表。

## 路径表达式

- **绝对路径**：原样。
- **相对路径**：相对脚本/产物目录（见下）。
- **`{script}`**：运行中的脚本/产物目录。
  - `.ax` 运行：`.ax` 所在目录；axp 运行：axp 所在目录；内联文本：home（`-e` 时 CWD）。
  - 编译期：`.ax` 所在目录（裸 `.axc` 编译时是当前目录）。
- **`{exe}`**：宿主可执行文件目录（与系统 `appdir` 相同）。

`{script}/lib`、`{exe}/links` 是合法表达式；相对路径等价于 `{script}/<path>`。

## 运行时搜索顺序

引擎自身的顺序：模块 `env()`（默认 `["."]`）→ `set_search_paths` 列表 → CWD 兜底。宿主构造的列表：

- **`.ax`**：`path`（解析后）→ 用户库目录 → 系统三目录。
- **`.axp`**（runner/CLI）：hint 的 `search_paths`（展开）→ 用户库目录 → 系统三目录。
- **裸 `.axc` / 内联文本**：CLI 为 `-p` → 系统三目录；GUI runner 为用户库目录 → 系统三目录。

## axp 携带的搜索路径

编译 `.ax` 或 `.axc` 时，产物 hint 写入保留键 `search_paths`：

```json
{"search_paths": ["<.ax path 原样>", "<用户目录原样>", "{exe}", "{exe}/links", "{exe}/imports"]}
```

- 已有 hint JSON 的其它键保留；同名 `search_paths` 被替换。
- 用户目录与 `.ax path` 保留原样表达式（绝对/相对/宏），所以产物可搬移。
- 运行时由 runner/CLI 解包并展开；**相对路径相对 axp 所在目录**。
- `search_paths` 是 AScript 的保留键，不要在用户 hint 里另作他用。

## 设置页（设置 → 库）

- 系统库目录：三行只读。
- 用户库目录：可添加、双击编辑、移除；支持上面的表达式语法。

## 文档页（语言参考 → 库）

树根「库」列出上述目录（系统 + 用户 + 工程 `path`/`docpath` + `<project>/link|import`）下的：

- `*.so` / `*.dll` / `*.dylib`，且同目录存在同名 `.json`；
- `*.axp`。

点击才读取：link 读 JSON，axp 读编译期 hint（无 hint 显示提示）。文档与代码提示共用 HintProvider 的 LRU 缓存；`工具 → 提示缓存 → 重载/清空` 会同时刷新该页。
