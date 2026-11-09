# 调试器 handle 视图协议（desc / view）

适用范围：任何向脚本返回 `anyptr` 句柄的宿主类型 —— core 扩展（`fs` / `thd`）和 link（`cv` / `cip` / `net`）。
引擎只把 view 当作不透明 `variant` 转发；**payload 的 schema 由本仓库的 GUI 定义**，即本文。

## 注册

两种来源，注册时机不同，运行时查表顺序一致：

```cpp
// link：alexis_script_load 里注册，随模块加载
_fw.bind(anyptr_ex<T>::rid, "cip::conn", desc_fn, view_fn);

// core 扩展：install() 里注册
core::inspect::add(anyptr_ex<T>::rid, "net", desc_fn, view_fn);
```

- `desc_fn(const anyptr&) -> std::string`：变量树里显示的一行摘要；返回空串 = 无。
- `view_fn(const anyptr&) -> alx::variant`：返回 `varmap`（视图 payload）；返回 null/空 = 无视图。
- 类型名（如 `cip::conn`）同时是脚本 `type()` 的答案。
- 同一个 `rid` 只能注册一次；link 的注册在 load 返回后提交，重复注册返回 false。

## 调用链

```
VariablesModel / ViewDock
  → Session::request_view(ref)                    GUI 线程
  → proto::make_view_request(ref, thread, id)
  → axc-runner hook::serve_view                   脚本暂停时
      snapshot::resolve(ref) → 必须是 anyptr
      _info.wkview(anyptr, view)                  找 link 注册表
      失败再走 core::inspect::view(anyptr, view)   找 core 注册表
      → proto::make_view(ref, ok, error, view, thread, id)
  → Session::on_view → viewReady(ref, ok, error, QVariantMap)
  → ViewDock::show_view(ref, type, view)
```

- 请求只在**暂停**时发出，ref 是变量路径（`snapshot::resolve` 能解析的深链）。
- 值不是 anyptr → `ok=false`，错误 `view requires a handle: <ref>`。
- 两边都没有注册 → `ok=false`，错误 `no view registered for this handle`。
- 返回 id 用于丢弃过期的回答（切换焦点/线程后旧答案不渲染）。

## payload schema

`varmap` 至少三个字段：

| 键 | 类型 | 说明 |
|---|---|---|
| `kind` | string | `image` / `text` / `html`；其他值落到消息页 |
| `format` | string | image：`bmp` / `png`；text/html：`plain` / `html`（仅展示用） |
| `data` | bytes | 编码后的图像字节，或 UTF-8 文本/HTML |

### kind = "image"

`data` 是完整的 BMP 或 PNG 文件字节。可选元数据只用于标题栏：

| 键 | 类型 | 说明 |
|---|---|---|
| `width` / `height` / `channels` / `depth` | int/string | 显示图的几何与通道 |
| `srcWidth` / `srcHeight` / `srcChannels` | int | 原图几何 |
| `scaled` | bool | 显示前缩放过 |
| `normalized` | bool | 16/32 位深度归一化到 8 位 |

图像解码失败 → 消息页显示格式与字节数。

### kind = "text"

`data` 按 UTF-8 解码成纯文本，放进 `QPlainTextEdit`（只读，不换行）。适合键值列表。

### kind = "html"

`data` 按 UTF-8 解码成 HTML，放进只读 `QTextBrowser`：
- Qt 富文本子集（HTML 4 + CSS 2.1 的一部分）：`<h3>`、`<p>`、`<table>`、`<code>`、inline `style` 可用；flex/grid 不可用。
- 外链不打开（`setOpenLinks(false)` / `setOpenExternalLinks(false)`）；view 里不要放需要联网的资源。
- Qt5/Qt6 同接口，与 `LangRefDock` 的渲染方式一致。

## 契约

- **view 在脚本暂停时由 runner 调用**，运行在引擎线程；不要调用回脚本、不要长时间持锁。拿不到锁时宁可少显示（`try_lock`），不要卡住调试器。
- payload 要小：点表这类按需展开的数据直接给全量；大数组/图像由 link 自己决定采样或缩放。
- 文本必须转义：HTML 里的点名、设备返回的错误串都要做 HTML escape。
- `desc` 每次快照都会调；保持 O(1) 且不加锁（只读不可变字段）。
- `view` 返回 null 与“未注册”在 GUI 上都是“无视图”，但前者由类型自己决定，后者是缺注册。

## 示例：cip

`links/cip/src/cip.cpp` 的 `conn_desc` / `conn_view`：

- desc：`client 10.0.0.5:44818` / `server 0.0.0.0:44818`
- view：`kind=html`，内容是角色、alive、session、点表 `<table>`（名字 + 类型形状）、last error；点表在 `try_lock` 下读取，锁忙时只显示 `points: busy`。

## 示例：net

`links/net/src/net.cpp` 的 `net_desc` / `net_view`：

- desc：`tcp_server 0.0.0.0:9000`
- view：`kind=text`，把 `status()` 的键值表逐行渲染。
