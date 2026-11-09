# cip Link

```ascript
link "bin/links/cip.so" as cip;
```

EtherNet/IP（CIP over TCP，显式消息 UCMM）的客户端与服务端。补全元数据在 `cip.json`（与 `cip.so` 同目录），GUI 按别名提示 `cip.` 下的函数。

移植自 AlxCip 的 cip 模块（同作者，Apache-2.0）：`$cip_*(handle, ...)` 变成 `cip.*(handle, ...)`，语义一一对应。

## 模型

```js
var d = cip.open("10.0.0.5:44818", 3000);      // 客户端
cip.define_real(d, "Line2_Speed");
cip.set(d, "Line2_Speed", 2.5);
var v = cip.get(d, "Tag_Array[1,3]");
cip.wait(d, "Cmd_Start", is_one, 5000);        // 谓词是脚本函数（或函数名）
cip.close(d);

var s = cip.listen("127.0.0.1:44818");          // 服务端
cip.define_dint(s, "Count");
cip.get(s, "Count");                            // 本地表直读
```

- **句柄是连接**。`open` / `listen` 返回 `anyptr<cip_conn>`；句柄拷贝共享同一个连接（`make_ref`），最后一个持有者释放时关闭。`close()` 只关传输，对象仍在；`cip.close` 可重复调用。
- 句柄是普通变量：可以传参、放进容器、从工厂函数返回。
- 调试器里显示类型 `cip::conn`、desc（`client 10.0.0.5:44818` / `server ...`）和 HTML 视图（alive、session、点表、last error）；协议见 `doc/view-protocol.md`。
- 服务端的表由 define 行填充，每个点以零值起步；服务端句柄上的 `get` / `set` / `wait` 直接读写本地表（不走网络），远端写入会唤醒服务端的 `wait`。
- `ASCRIPT_CIP_DEBUG=1` 把线上字节活动写到 stderr；默认完全安静。

## 失败的两条通道

- **脚本错误抛异常**：标签语法错、值的类型不对、重复声明、连接/监听失败。
- **设备语义的空结果不抛**：`get` → `null`，`set` → `false`，`wait` 超时 → `false`，原因留在 `error(h)`（一次成功的操作后清空）。批量 `get` 逐项继续（缺失项为 `null`）；批量 `set` 在第一个拒绝处停止（之前的已写入）。

## 点与类型

- 原子类型：`define_bool` / `define_sint` / `define_int` / `define_dint` / `define_lint` / `define_real` / `define_lreal`，可选形状：元素数或 `[rows, columns]`（行主序，最多两维）。
- `define_string` / `define_sstring`：长度随值走，不声明容量（上限是线缆长度字段 65535 / 255）。
- `define_byte`：一个元素一个字节。单个元素按 int `0..255`；连续段读为 `bytes`，写接受 `bytes`（长度必须相等）、字符串（补零到段长）或 int 向量。
- 标签：`Tag`、`Tag[i]`、`Tag[a,b)`（b 排除，即脚本自己的切片规则）、`Tag[r][c]`、`Tag[r]`（整行）、`Tag[r][a,b)`。点名的前面部分按原样匹配（声明名里带 `[0]` 也可以）；范围只允许出现在最后一个索引。
- **写必须有声明**（请求要带类型）；**读不要求声明**（回包自带类型）。声明过的名字读回时会做类型校验，表写错会当场报出来。

## 接口

### `cip.open(addr, [timeout=3000]) → handle`
`addr` 省略端口时为 44818；`timeout` 是单次回复的等待毫秒数。

### `cip.listen(addr) → handle`
绑定并立即开始监听；端口占用或绑定失败当场抛异常。

### `cip.define_bool(h, name, [shape])` / `define_sint` / `define_int` / `define_dint` / `define_lint` / `define_real` / `define_lreal`
`shape`：元素数，或 `[rows, columns]`。重复声明同名点抛异常；改名先 `remove`。

### `cip.define_string(h, name)` / `cip.define_sstring(h, name)` / `cip.define_byte(h, name, [shape])`

### `cip.remove(h, name)`
删除声明及其种子值（只动本地记账，不碰设备）。

### `cip.points(h) → map`
当前声明表：`{"Tag_Array": "INT[10]", "Grid": "SINT[2][3]"}`。

### `cip.get(h, tag) → any` / `cip.get(h, tags: vec) → vec`
单个标签返回标量、向量（数组/行/范围）或 `bytes`（BYTE 段）；批量按给定顺序返回。

### `cip.set(h, tag, v) → bool` / `cip.set(h, points: map) → bool`
整点写入接受声明的形状：嵌套二维或扁平向量均可。

### `cip.wait(h, key, cond, timeout, [period=20]) → bool` / `cip.wait(h, keys: vec, cond, timeout, [period=20]) → bool`
`cond` 接收 `get` 会返回的值（多键时是一个 `vec`）。客户端按 `period` 采样；服务端不采样，只在远端写入事件上唤醒，外加开头一次。超时返回 `false`，最后看到的值写进 `error(h)`。

> 服务端 `wait` 的谓词在表的互斥锁内运行：谓词里不要再调用同一个句柄的方法（会重入同一把锁），并保持短小——远端写入要等它。

### `cip.error(h) → string`
最近一次 get/set/wait 失败的原因；成功的操作会清空。

### `cip.close(h)`
关闭传输（客户端会先发 UnRegisterSession）。句柄对象仍在，释放最后一个副本时析构。

### `cip.alive(h) → bool`
客户端：已连接；服务端：监听仍打开。

## 限制

- 仅 UCMM 显式消息、符号标签；无 Class 1/3、无连接管理、无跨网路由。
- 单次请求的回复上限 480 字节；超出的读按名字拒绝（分片未实现，用 `Tag[a,b)` 切片）。
- 元素索引/二维索引 ≤ 255；CIP 回包头按模拟器实测的 4 字节解析；`STRING` / `SSTRING` 按长度前缀处理，真实 Logix 的 STRING UDT 未覆盖。

## 测试

```bash
bash test/cli/cip/run.sh
```

无硬件即可跑：错误用例、同进程回环（server + client）、2-D 相机握手、真实客户端抓包回放（`msp_frames.txt`）。
