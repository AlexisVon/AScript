# AlxLib — 未来工作（future）

本文记录**已定规格、尚未实现**的条目（与 issue 的"行为不对"、feature 的"能力还缺"分层：这里放的是已经拍完口径、只等实现的）。做完即删，编号随之重排。
**代码注释不引用本文编号**——注释只描述行为本身。

---

（F-1 `''` 码点字面量已实现，2026-10-09：结论折进 `design.md` §6.5 #44、`lang.md` §3.4。）

---

（F-2 宿主侧类型解释与 view 已实现，2026-11-03：结论折进 `design.md` §4.9g、`api.md` §4.2 / §11.5、`lang.md` §5.3。）

---

## 已关闭的评审：AScript GUI 提案（2026-10-22）

> 全部条目已有结论，没有遗留的待实现项，因此本文不再为它们保留编号。设计背景见 AScript 仓
> `doc/gui-rebuild.md` / `doc/gui-impl-plan.md`。

**已实现**（结论折进 `doc/core/design.md` §11.3 / §11.5、`doc/scpt/design.md` §4.9b / §4.9d、
`doc/comm/design.md` §2.1）：

- 进程树 kill：`process_ctrl` 的 POSIX 进程组 / Windows 常驻 Job Object。
- `hook_info` 标量位置 `wkdt_row()` / `wkdt_col()` 与完整路径 `wken_path()`。
- `comm_ex` 的队列上限构造参数。

**评审后不做**（2026-10-22，原因逐条记录在此，避免重复提案）：

- **异常事件**（`hook_event::error`）：脚本层已有明显的 `try` / `catch`，hook 不再加同类事件。
- **行表 / 语句起始行**：不命中就是不命中——与 C++ 开优化同理，位置是"尽最大努力"而非精确；
  宿主用行号 diff 自行判断即可。
- **暂停求值 / 改变量**：不做引擎内修改，避免改崩。
- **每引擎 hook 上下文**：引擎本就是单线程，不需要。
