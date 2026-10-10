# napi-host-launcher

最小 N-API 宿主启动器：用几十 KB 的 mingw 交叉编译 win32 程序，直接加载 `.node`（x86 N-API 插件）并调用其签名引擎，绕开完整 node.exe 运行时。

## 背景

某些商业签名引擎（如汽水音乐的 `metasecml.dll`）只提供 **x86-64 Windows PE** 版本，无 ARM 原生版本。在 ARM 云主机（Oracle Ampere / Apple Silicon）上完整跑 `node.exe + bdms.node` 开销大且依赖重。本宿主将运行栈压缩为：

```
wine/Hangover (x86 模拟) → napi_host.exe (~264KB) → bdms.node → metasecml.dll
```

去掉完整 node.exe，签名能力不变，热态吞吐与 node 常驻服务同级别（实测 ~77ms/次、双 worker 绑核池 22 QPS）。

## 原理

1. 宿主导出 27 个 `napi_*` 函数（N-API ABI），让 Windows 版 `.node` 能正常加载
2. patch `bdms.node` 的延迟导入表，把 `napi_register_module_v1` 等符号指向宿主实现
3. patch 引擎 setter 回调（`metasecml.dll` 导出函数表的写入回调），加载时自动注册真实引擎全局指针
4. 调用 `generateHttpSignatureHeaders(url, headers)` 走真实引擎出签名（X-Helios / X-Medusa）

## 编译

需要 mingw-w64 交叉编译器：

```bash
x86_64-w64-mingw32-gcc -O2 -o napi_host.exe napi_mini_host.c -Wl,--export-all-symbols
```

在 Linux ARM 主机上运行需要 wine（Hangover 支持 ARM64EC 模拟）：

```bash
WINEPREFIX=/path/to/prefix /usr/bin/wine napi_host.exe \
  "Z:\\path\\to\\bdms.node" \
  "https://api.example.com/path?query=1" \
  $'user-agent\r\nMyApp/1.0\r\ncontent-type\r\napplication/json'
# 输出: X-Helios + X-Medusa（与 node 版签名服务同族）
```

## 运行模式

| 模式 | 触发方式 | 说明 |
|---|---|---|
| CLI 单次 | `napi_host.exe <bdms> <url> <headers> [deviceId] [loops]` | 单次签名；`loops=N` 进程内基准 |
| server 常驻 | `... "1" server` | stdin 行协议：`b64url\tb64headers\n` → `b64signature\n` |
| dumpmem | `... "1" dumpmem <path>` | 运行时 dump 加壳 DLL 的解壳镜像（逆向用） |

## HTTP 服务前端

`napi_http_server.py` 是可选的多 worker HTTP 前端：

- 监听 `0.0.0.0:8900`，`POST /sign` → `{"ok":true,"headers":{...}}`，`GET /healthz`
- 双 wine worker 池（独立 WINEPREFIX + `taskset` 绑核），round-robin 分配，崩溃自动重启
- 实测并发收益：单 worker 自由调度 12 QPS → 双 worker 绑核 0/1 引擎层 15.3 QPS、HTTP 8 并发 22.1 QPS

```bash
NAPI_TOKEN=your_token python3 napi_http_server.py
```

⚠️ `NAPI_TOKEN` 必须通过环境变量提供（默认值为占位符，未设置会拒绝所有请求）。

## 注意事项

- headers 必须以**真实 CRLF** 传入（bash 里用 `$'...\r\n...'`）；字面 `\\r\\n` 会导致引擎崩溃
- 引擎函数表（`o0o0` 导出的 8 个函数指针）**不能直调**（会崩）；正确路径是 setter 回调注册
- `metasecml.dll` 为 x86-only + 自定义加壳（.text raw=0，代码在 .xs2 节），运行时解壳；无 ARM 版本，跨架构只能靠模拟器

## License

MIT
