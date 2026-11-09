# Ollama Link

```ascript
link "bin/links/ollama.so" as olm;
```

补全元数据在 `ollama.json`（与 `ollama.so` 同目录）：`olm.` 提示全局函数，`olm.<key>.` 提示 `connect` 创建的实例方法。

## 全局函数

### `olm.list(host, [timeout]) → vec | string`

列出远端实例可用模型。`timeout` 秒，默认 5。**连接失败或响应无效时返回 string 错误信息**（不是 null）。

```ascript
var mods = olm.list("192.168.1.100:11434");
if (type(mods) == "string") { $io_print("%1\n", mods); return; }
for (var m : mods) $io_print("%1\n", m.name);
```

### `olm.show(addr, model, [timeout]) → string`

查看模型详情。`timeout` 秒，默认 5。连接失败返回 null；成功返回 Ollama 原始响应字符串（JSON 文本）。

```ascript
var info = olm.show("192.168.1.100:11434", "gemma3:4b");
```

### `olm.connect(key, host, model, [timeout]) → bool`

连接并创建命名句柄。`timeout` 秒，默认 10。连接失败返回 false。

```ascript
if (!olm.connect("llm", "192.168.1.100:11434", "gemma3:4b", 60))
    $io_print("连接失败\n");
```

## Area 函数

### `olm.<key>.chat(input, [options]) → string`

对话模式，自动维护历史。请求无响应返回 null；响应无法解析返回原文。

- `input`：`string`（用户消息）、`map`（`{role, content}`）、`vec`（批量）
- `options`：可选，Ollama 参数 `{temperature, top_p, ...}`

```ascript
var r = olm.llm.chat("你好");
olm.llm.chat(map{"role": "system", "content": "你是助手"});
olm.llm.chat("更精确", map{"temperature": 0.2});
```

### `olm.<key>.generate(prompt, [options]) → string`

单次生成，无历史。请求无响应返回 null；响应无法解析返回原文。

```ascript
var t = olm.llm.generate("写一首诗");
```

### `olm.<key>.show() → string`

查看当前模型信息。成功返回原始响应字符串；未连接抛错（见「注意」）。

```ascript
var info = olm.llm.show();
```

### `olm.<key>.clear()`

清空对话历史，无返回值。

```ascript
olm.llm.clear();
```

### `olm.<key>.alive() → bool`

检查连接是否有效（unwrap 成功且 TCP 连接正常）。

```ascript
if (!olm.llm.alive()) $io_print("连接已断开\n");
```

## 注意

- host 格式：`ip:port`，不带 `http://`
- `chat` 有状态，`generate` 无状态
- 参数类型错误抛 ArgError/TypeError
- area 函数（`olm.<key>.*`）要求 key 已由 `connect` 创建；key 不存在时访问即抛错，不会返回 null
