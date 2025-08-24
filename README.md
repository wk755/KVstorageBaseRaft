# KVstorageBaseRaft

> 基于 **C++20 / Muduo / Protobuf / 自研 RPC / Fiber** 的 **Raft 分布式 KV 存储**

- ✅ Raft：Leader 选举、日志复制、任期与角色切换、快照（InstallSnapshot）与持久化（Persister）  
- ✅ KV 状态机：`Get / Put / Append`，**ClientId + RequestId** 幂等防重  
- ✅ RPC 通道：Muduo + Protobuf 实现的**长连接** RPC（`MprpcChannel` / `RpcProvider`）  
- ✅ 客户端 Clerk：**记忆最近 Leader**、错 Leader **自动重试**  
- ✅ Fiber（实验）：轻量协程/调度/IO 管理（可选示例）

> 说明：面向学习与工程实践，不直接用于生产。

---

## 🚀 TL;DR（两分钟跑起来）

```bash
# 依赖（Ubuntu）
sudo apt-get update
sudo apt-get install -y build-essential cmake   libmuduo-dev libprotobuf-dev protobuf-compiler libboost-all-dev

# 构建
unzip KVstorageBaseRaft-cpp-main.zip
cd KVstorageBaseRaft-cpp-main
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# 启 3 节点（会生成 ./bin/test.conf 并前台运行）
./bin/raftCoreRun -n 3 -f ./bin/test.conf
```

另开终端，运行客户端：
```bash
cd KVstorageBaseRaft-cpp-main/build
./bin/callerMain     # 读取同目录 test.conf，循环 Put/Get 并自动容错
```

---

## ✨ 特性 Highlights

### Raft 共识（`src/raftCore`）
- 选举/心跳 `AppendEntries`，**任期**维护与角色切换
- 日志复制，提交索引 `commitIndex`、应用进度 `lastApplied`
- **快照**：`InstallSnapshot` + **Persister**（Raft 状态与快照落盘）
- `ApplyMsg` 通道把已提交日志发送给状态机

### KV 状态机（`KvServer`）
- `Get / Put / Append` 三操作
- **幂等防重**：`ClientId + RequestId` 去重表
- 存储实现可替换（默认 `SkipList` + `unordered_map`）

### 客户端（`src/raftClerk`）
- 读取节点 `ip:port` 列表
- **记忆最近 Leader**，`ErrWrongLeader` 时自动轮询重试

### RPC 通道（`src/rpc` + `src/raftRpcPro`）
- Muduo + Protobuf 的长连接 RPC
- 协议 `.proto`：Raft RPC 与 KvServer RPC

### Fiber 运行时（实验，`src/fiber`）
- 协程/调度器/IO 管理 + hook，示例见 `example/fiberExample`

---

## 🧱 架构 Overview

```text
Client (Clerk)
  └── Protobuf RPC (Get/Put/Append)；记忆最近 Leader + 错 Leader 重试
        ▼
      KvServer (状态机)
        ▲ ApplyMsg（已提交日志）
        │
      Raft Core  ── peer-to-peer ── Raft Core ── ...
        │ persist(snapshot/state)
        ▼
     Persister
```

**热点路径策略**
- Clerk 缓存 Leader → 降低错 Leader 重试成本  
- 状态机防重 → 消除重放副作用  
- Raft 先比任期再决定回复/切角色 → 避免无效复制  
- 快照触发与安装 → 降低日志回放时间

---

## 📁 项目结构

```
src/
  common/          config.h（调参/Debug）、util.h
  fiber/           协程/调度/IO 管理（示例见 example/fiberExample）
  rpc/             MprpcChannel / RpcProvider / Controller …
  raftRpcPro/      raftRPC.proto、kvServerRPC.proto（及生成的 pb.h/cpp）
  raftCore/        Raft、KvServer、Persister、ApplyMsg、LockQueue …
  raftClerk/       Clerk 客户端、raftServerRpcUtil（封装 Kv RPC）

example/
  raftCoreExample/ raftCoreRun（集群启动器）、callerMain（示例客户端）
  rpcExample/      其他 RPC demo

bin/
  test.conf        节点配置（运行时生成/覆盖）
```

---

## 🔧 构建与运行（详细）

### 依赖
| 组件 | 版本建议 | 说明 |
|---|---|---|
| GCC/Clang | C++20 | 编译器 |
| CMake | ≥ 3.22 | 构建系统 |
| Muduo | 系统包 | 网络库 |
| Protobuf | ≥ 3.x | 协议与生成器 |
| Boost | 系统包 | 持久化/序列化等 |

macOS（可选）：
```bash
brew install cmake protobuf boost
# muduo 需自行编译或使用替代（如 asio + 自研）
```

### 运行 3 节点
```bash
./bin/raftCoreRun -n 3 -f ./bin/test.conf
# test.conf 例：
# node0ip=127.0.0.1
# node0port=29016
# node1ip=127.0.0.1
# node1port=29017
# node2ip=127.0.0.1
# node2port=29018
```

### 客户端演示
```bash
./bin/callerMain   # 读取 test.conf；循环 Put/Get；错 Leader 自动恢复
```

---

## 🧪 功能演示 & 验证

### 1) 正常读写
- `callerMain` 输出 `Put/Get` 成功日志，值单调递增或按预期变化

### 2) 容错：Leader 切换
- 在 `raftCoreRun` 的窗口杀掉 Leader 子进程（或 Ctrl+C 稍等再拉起）  
- 观察其余节点选举，新 Leader 产生  
- 客户端短暂失败后**自动恢复**（重试到新 Leader）

### 3) 快照/持久化
- `Persister.*` 负责落盘 Raft 状态与 Snapshot  
- 触发/间隔参数见 `config.h`，可自行调大/调小验证恢复时长

---

## 🔭 可观测性 Observability

- **日志**：`DPrintf(...)`（见 `util.h`），开关在 `config.h::Debug`  
  压测/长跑时建议重定向到文件，降噪对尾延迟的影响
- **调参**：`config.h` 包含选举超时、心跳间隔、应用频率、Fiber 配置等
- **扩展建议**：  
  - 在 `RpcProvider`/`KvServer` 关键路径埋点  
  - 以 **Prometheus 文本**（/metrics）导出：如 QPS、重试、选举次数、日志长度等  
  - Grafana 面板：`rate(raft_append_entries[1m])`、`rate(kv_put_total[1m])`、`raft_term` 等

---

## 🗺️ Roadmap（可扩展方向）

- 日志压缩策略：更细的快照触发、**增量快照**
- 线性一致读：**Leader lease / ReadIndex**
- 分片/路由：在 KvServer 之上做 **Sharding**（Clerk 端路由）
- 落盘引擎：替换为 LSM / mmap 文件 / 自定义 WAL
- 更强可观测：Prometheus + Grafana + Trace（OpenTelemetry）
- 安全：RPC 鉴权 / TLS（OpenSSL）

---

## ❓FAQ（常见问题）

- **`Connection refused`**  
  没有节点在监听或端口不对。确认 `./bin/raftCoreRun` 正在前台运行，`ss -ltnp | grep 2901`。
- **一直 `ErrWrongLeader`**  
  选举未稳定或配置端口错误；等心跳稳定或检查 `test.conf`。
- **编译找不到 muduo/protobuf**  
  确认系统包已安装，或在 CMake 里指定 `CMAKE_PREFIX_PATH`。

---

## 📎 关键可执行文件

- `bin/raftCoreRun` —— 启动/管理多节点集群（例：3 节点）  
- `bin/callerMain` —— 示例客户端（读取 `test.conf`，循环 Put/Get）
