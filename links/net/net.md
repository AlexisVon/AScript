# net Link

```ascript
link "bin/links/net.so" as net;
```

TCP/UDP 原始传输，建在 alxcomm 的 `transmit` 之上。数据与连接事件以**投递**形式在脚本自己的 `yield()` 点执行（§9.6），C++ 接收线程永远不直接跑脚本。补全元数据在 `net.json`（与 `net.so` 同目录）。

从 core 的 `$net_*` 迁出，语义不变；句柄与 `cip` / `cv` 一样是普通值，拷贝共享同一个连接（`make_ref`），最后一个持有者关闭传输。

## 用法

```js
var s = net.open("tcp_server", "0.0.0.0:9000");
net.on_recv(s, "on_data");      // def on_data(addr, data)
net.on_event(s, "on_conn");     // def on_conn(addr, connected)
def on_data(addr, data) { net.send(s, data, addr); }
while (true) { yield(100); }
```

调试器里显示类型 `net`、desc（`tcp_server 0.0.0.0:9000`）和文本视图（status 的键值表）；协议见 `doc/view-protocol.md`。

## 接口

| 接口 | 说明 |
|---|---|
| `net.open(type, addr[, opts])` | `tcp_server` / `tcp_client` / `udp_trans`；opts：`window`（TCP 每连接在途投递，默认 1）、`expiry`（UDP 排队等待 ms，默认 100）、`max_inflight`（UDP 在途上限，默认 64） |
| `net.on_recv(h, "def")` | 收信回调 `def(addr, data)`，data 为 bytes；addr 为 `"a.b.c.d:port"` |
| `net.on_event(h, "def")` | 连接事件 `def(addr, connected)`：tcp_server 报告每个 peer 的连接/断开，tcp_client 报告断开，udp 无此事件 |
| `net.send(h, data[, addr])` | 定向发送；省略 addr = 所有 peer（TCP）；UDP 必须带 addr；返回 bool |
| `net.disconnect(h, addr)` | server 句柄踢掉一个 peer；client/UDP 抛 `ArgError` |
| `net.status(h)` | `{type, addr, closed, peers, rx, delivered, expired, full, errors, last_error}` |
| `net.close(h)` | 停止传输、join 线程；可重复调用，句柄对象仍在 |

## 语义

- **TCP 背压**：接收线程提交投递后等待其 future（`window` 控制同时在途数）；脚本处理不过来时接收缓冲填满、TCP 窗口收缩，不丢数据。
- **UDP 语义**：提交后不等；排队超过 `expiry` 由后台心跳结算为 `expired`，引擎队列满（`yield full`）或单 socket 在途超 `max_inflight` 计 `full`——即 UDP 的丢包。
- 投递只在脚本 `yield()` 时执行；脚本不 yield 时 TCP 连接停住（背压），UDP 按上述规则丢弃。
- handler 内 `net.close` / `delete` 句柄是安全的；close 后已入队的投递仍可能执行一次（每连接最多 `window` 条）。
- 计数由接收线程在处理流量时更新（UDP 的 `expired` 在下一次收包时结算），`last_error` 记录传输层的最近错误。
- 句柄拷贝共享连接：`var b = a` 后 `net.close(b)` 会关掉 `a` 看到的那条连接。

## 测试

```bash
bash test/cli/net/run.sh
```
