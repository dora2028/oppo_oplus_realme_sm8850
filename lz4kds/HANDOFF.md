# LZ4KDS 交接文档

> **用途**：在新对话里贴这份文档，让 AI 立刻理解项目背景、当前进度、方向和流程，
> 无需从头解释。
>
> **最后更新**：2026-11-18，对应 commit `1a88ae6`

---

## 0. 一句话背景

给 ColorOS/AOSP 6.12.69 GKI 内核的 zram **新增 `lz4kds` 压缩算法**
（华为 LZ4KD 编解码 + SDDC 相似页/差分压缩），集成在一个**独立目录**里，
上游文件零改动，通过注入脚本接入内核树。

---

## 1. 关键坐标

| 项目 | 值 |
|---|---|
| 仓库 | `https://github.com/dora2028/oppo_oplus_realme_sm8850`（fork 自 `cctv18/oppo_oplus_realme_sm8850`） |
| Token | `/storage/emulated/0/dora_2028/.gh_token`（本地工作副本在 `workspace/dora_2028/.gh_token`） |
| 本地仓库 | `/workspace/push`（= Android 侧 `.../terminal-user/workspace/push`） |
| 目标内核 | AOSP common `android16-6.12`（6.12.69 GKI） |
| 参考仓库 | `https://github.com/whitewhale0612/android_kernel_common_oneplus_sm8750-C16` |
| 当前 HEAD | `1a88ae6` |
| 上游基线 | `a19d43f`（`fastbuild_6.12.69_gki.yml` 与它逐字节一致） |

---

## 2. 项目当前状态

### 已完成

- ✅ LZ4KDS 算法层（SDDC 引用表、alias/delta/普通三条通路）
- ✅ 独立为 `lz4kds/` 单目录，**上游文件零改动**
- ✅ 注入脚本三模式：`inject` / `check` / `revert`，幂等 + 逐字节精确回滚
- ✅ 用户态测试 3 个，全部链接真实 `src/lz4kds_sddc.c`
- ✅ 独立 CI：`.github/workflows/build-lz4kds.yml`
- ✅ 两轮性能修正（见 §5）

### 未完成 / 待验证

- ⬜ **未在真机长测**。只验证到：编译通过、算法往返正确、符号入镜像。
- ⬜ **独立 CI 的 `build` job 尚未绿**。目前卡在 `gendwarfksyms` 缺 `dwarf.h`
  （已补 `libdw-dev`，待下次运行确认）。
- ⬜ 统计接口 `lz4kds_sddc_get_stats()` 已实现但**未接到 sysfs/debugfs**。
- ⬜ 16 KiB 页内核不支持（delta 编解码器按 4 KiB 块工作）。

### 刷机前必读

> 代码只验证到编译 + 用户态算法正确，**没在真机上跑过**。
> 刷入前请保留原内核镜像；刷后用
> `cat /sys/block/zram0/comp_algorithm` 确认列表里出现 `[lz4kds]`。

---

## 3. 目录结构

```
lz4kds/
├── src/
│   ├── lz4kds_sddc.c          534 行  ★核心：引用表、alias/delta 数据通路
│   ├── lz4kds_backend.c       112 行  zram 后端，注册 "lz4kds"
│   ├── lz4kd_encode.c         418 行  华为 LZ4KD 编码（勿改）
│   ├── lz4kd_encode_delta.c   259 行  差分编码（勿改）
│   ├── lz4kd_decode.c         243 行  解码（勿改）
│   ├── lz4kd_decode_delta.c   225 行  差分解码（勿改）
│   └── lz4kd_*_private.h             编解码私有定义（勿改）
├── include/
│   ├── lz4kd.h                      编解码公共头 → 装到 include/linux/
│   ├── lz4kds_sddc.h                SDDC 对外接口
│   └── lz4kds_zcomp.h               zcomp 接口视图（__KERNEL__ 判定 in-tree）
├── scripts/
│   └── lz4kds_inject.py       388 行  ★注入/检查/回滚
├── test/
│   ├── sddc_real_test.c              往返 + 压测 + 回放（链接真实实现）
│   ├── exhaust_test.c                引用表耗尽后正确回退
│   ├── random_test.c                 不可压缩页必须报 PAGE_SIZE
│   ├── kshim.h                       用户态编译垫片
│   └── linux/*.h                     内核头垫片
├── ci/build-lz4kds.yml               独立 CI（与 .github/ 下同步）
├── README.md                         用法
├── ARCHIVE.md                        ★移植存档（改动清单/设计决定/踩坑）
└── .gitignore
```

**设计原则**：仓库里的上游文件一行不改。所有代码都是新增文件，与上游
`other_patch/lz4kd.patch` 等补丁**没有文件级冲突**，可独立启停。

---

## 4. 工作流程

### 4.1 注入 / 检查 / 回滚

```sh
# 注入（幂等，可重复执行）
python3 lz4kds/scripts/lz4kds_inject.py <内核目录> <lz4kds目录>

# 只看状态，不改文件
python3 lz4kds/scripts/lz4kds_inject.py <内核目录> <lz4kds目录> --mode check

# 回滚（逐字节还原）
python3 lz4kds/scripts/lz4kds_inject.py <内核目录> <lz4kds目录> --mode revert
```

回滚精确性靠 `<内核>/.lz4kds_backup/` 备份目录：注入前把待改文件备份，
回滚时直接还原。备份不存在时退回按 `LZ4KDS-BEGIN/END` 标记删除。

### 4.2 注入脚本改哪些文件（当前 3 个）

| 内核文件 | 改动位置 | 改了什么 |
|---|---|---|
| `drivers/block/zram/Makefile` | 文件末尾追加 | 新对象编译规则 + `-DLZ4K_DELTA` |
| `drivers/block/zram/zcomp.c` | ① `#include "backend_842.h"` 之后<br>② `backends[]` 的 `NULL` 哨兵之前 | ① 引入 backend 声明头<br>② 注册 `&backend_lz4kds` |
| `drivers/block/zram/Kconfig` | `config ZRAM_BACKEND_ZSTD` 之前 | 新增 `ZRAM_BACKEND_LZ4KDS` |

> 早期版本还改 `zcomp.h` 和 `zram_drv.c`（加释放钩子），已移除，见 §5 第 1 条。

**若内核树已有 `lib/lz4kd/`**（应用了上游 `lz4kd.patch`），脚本会自动跳过
自带编解码源码与 `select LZ4KD_COMPRESS`，改用上游对象，避免
`lz4kd_encode()` 等符号重复定义。

### 4.3 本地跑测试

```sh
cd /workspace/push
for t in sddc_real_test exhaust_test random_test; do
  gcc -O2 -include lz4kds/test/kshim.h \
      -I lz4kds/test -I lz4kds/include -I lz4kds/src \
      -o /tmp/$t lz4kds/test/$t.c \
      lz4kds/src/lz4kds_sddc.c \
      lz4kds/src/lz4kd_encode.c lz4kds/src/lz4kd_encode_delta.c \
      lz4kds/src/lz4kd_decode.c lz4kds/src/lz4kd_decode_delta.c
  /tmp/$t | tail -1        # 期望 RESULT: PASS
done
```

### 4.4 提交推送

```sh
cd /workspace/push
git add -A && git commit -F <消息文件>
TOKEN=$(cat /workspace/dora_2028/.gh_token)
git push -q "https://x-access-token:$TOKEN@github.com/dora2028/oppo_oplus_realme_sm8850.git" HEAD:main
```

### 4.5 触发独立 CI

```sh
curl -s -X POST -H "Authorization: token $TOKEN" \
  -H "Accept: application/vnd.github+json" \
  "https://api.github.com/repos/dora2028/oppo_oplus_realme_sm8850/actions/workflows/build-lz4kds.yml/dispatches" \
  -d '{"ref":"main","inputs":{"kernel_version":"android16-6.12","kernel_dir_name":"common","run_host_tests":true}}'
```

> **用户明确要求：CI 只查一次最终结果，不要反复轮询**，避免浪费 token。

---

## 5. 踩过的坑（重要，别重复踩）

### 坑 1：释放钩子完全是空转（第一轮卡顿主因）

`zram_free_page()` 里的钩子会映射每个待释放对象，只为把 `ref->users` 减一。
但 `users` 在整个代码库里**没有任何地方被读取**——引用本就不可回收。

它却跑在释放热路径上、在 `zram_slot_lock` 之内、且处于 **atomic 上下文**
（`zram_slot_free_notify` 由 bio 完成路径调用），每释放一页多一次
`zcomp_stream_get()` + `zs_map_object()` 往返。

**已删除**该钩子，连带删掉 `zcomp_ops` 的 `lz4kds_release` 扩展和
`lz4kds_sddc_put_payload()`。

### 坑 2：delta 候选永远找不到

候选原本只从「整页 4 KiB jhash」的同一 bucket 取。但整页哈希对单字节差异
完全不鲁棒——实测 2000 个仅改 1 字节的相似页，**0 个**与原页同 bucket。
于是 delta 路径形同虚设，却每次写入都要付一次整页哈希 + bucket 遍历。

**已改为** 4 个跨页均匀分布的 64 字节窗口指纹，任一窗口未被改动即可命中。
同负载下 delta 命中从 0 提升到 617 次。

### 坑 3：引用表满后线性扫表尾

`for (id = next_id; id < REF_MAX; id++)` 在 `next_id` 触顶后每次都从 4096
扫到末尾才失败。引用不可回收 ⇒ `next_id` 本身就是首个空槽，**已改为直接取用**，O(1)。

### 坑 4：`LZ4KDS_IN_TREE` 从未定义

`lz4kds_zcomp.h` 的 in-tree 分支原本以 `LZ4KDS_IN_TREE` 为条件，但注入脚本
从未定义该宏，backend 实际编译时用的是**本地复制的结构体**。当前两者布局
一致所以没出事，但上游一旦给 `zcomp_ops` 加字段，就会得到一个**静默错位的
vtable** 而不是编译错误。**已改用 `__KERNEL__` 判定**。

### 坑 5：测试是假的

原 `test/sddc_test.c` 在测试文件里**另写了一份** SDDC 逻辑（注释写着
`Mirrors lz4kds_sddc_compress()`），所以它通过只证明那份副本正确。
**已删除**，换成三个链接真实 `src/lz4kds_sddc.c` 的测试。

### 坑 6：编码器状态大小

`lz4kd_encode_state_bytes_min()` 在未定义 `LZ4K_DELTA` 时返回 8192，但 delta
编码器实际需要 `(1<<13 + 1<<5) * 2 = 16448` 字节，会越界写（用户态实测段错误）。
注入脚本加 `-DLZ4K_DELTA`，代码里按 32 KiB 分配。

### 坑 7：CI 工具链

独立 CI 报 `exit code 2` 的原因**不是**注入或 AOSP 树参数，而是：

```
scripts/Kconfig.include:41: linker 'ld.lld' not found
```

连 `make gki_defconfig` 都没跑起来，卡在 kconfig 阶段。workflow 设了
`LLVM=1 LLVM_IAS=1` 但只装了 `build-essential`。上游 fastbuild 能编是因为它
另外下载了 clang19 + rust + build-tools 三件套。**已补 `clang lld llvm`**。

后续又暴露 `gendwarfksyms` 缺 `dwarf.h`，**已补 `libdw-dev` + `pahole`**。

---

## 6. 性能数据

实测（x86 主机，200k 次迭代）：

| 路径 | 第一轮修正后 | 第二轮修正后 |
|---|---|---|
| 解压 alias 页 | 2.36 µs/页 | **1.95 µs/页** |
| 写路径（表已满） | 4.83 µs/页 | 6.64 µs/页 |

写路径变慢是**预期且正确**的：第一轮修正后 delta 根本没生效，等于白跑；
第二轮修正后 delta 真正参与编码，用 CPU 换压缩率。

`LZ4KDS_DELTA_TRIES` 实测折中：

| 取值 | 写路径 | 说明 |
|---|---|---|
| 1 | 8.02 µs | delta 命中少 |
| **2** | **6.64 µs** | **当前值，最优折中** |
| 4 | 10.06 µs | 收益递减 |

压缩率（模拟相似页负载 20000 页）：平均存储 53 字节 = 原始大小的 **1.3%**。

---

## 7. 可调常量

全在 `lz4kds/src/lz4kds_sddc.c` 顶部：

| 常量 | 当前值 | 作用 |
|---|---|---|
| `LZ4KDS_REF_MAX` | 4096 | 引用表槽位数 |
| `LZ4KDS_REF_BUDGET` | 16 MiB | 引用页内存上限 |
| `LZ4KDS_LOCK_STRIPES` | 64 | 分段锁数量 |
| `LZ4KDS_HASH_BITS` | 12 | alias 桶数（4096） |
| `LZ4KDS_DELTA_TRIES` | 2 | 每次写入最多尝试的 delta 次数 |
| `LZ4KDS_WINDOWS` | 4 | 相似度窗口数 |
| `LZ4KDS_SAMPLE_WAYS` | 2 | 每窗口保留候选数 |
| `LZ4KDS_ENCODE_STATE_BYTES` | 32 KiB | 编码器状态大小（勿低于 16448） |

---

## 8. 设计决定

### 引用为什么不可回收（正确性）

alias/delta payload 只携带一个数字引用 id，而该 payload 可能已写入 swap 或
后端设备。若引用被回收，旧 payload 会解出错误数据。

**决定**：引用在设备生命周期内永不回收，内存由 `LZ4KDS_REF_BUDGET` 封顶。
预算用尽后新页不再晋升为引用（仍正常压缩），但所有已存在 id 始终有效。

### 与上游 lz4kd.patch 的关系

上游 `other_patch/lz4kd.patch` 自带 `backend_lz4kd.c`，且其 `lz4kd` 后端
**只是调整 LZ4 的 acceleration 参数，并未使用真正的华为 LZ4KD 编解码**。

本方案因此：不复用上游 backend，自带 `lz4kds_backend.c`；直接使用真实
LZ4KD 编解码；与上游补丁没有文件级冲突。

### 已知不兼容

- **zstd**：上游 `002-zstd.patch` 裁剪了 `include/linux/zstd.h`（+77/−166），
  删掉 `backend_zstd.c` 依赖的 12 个接口，导致 zstd 后端编译失败。
  本方案本身不碰 zstd。
- **SUSFS**：开启时 `fs/proc/task_mmu.c` 的补丁与该内核版本不匹配，
  会编译失败（与 LZ4KDS 无关）。

---

## 9. 给下一个 AI 的建议

1. **先读 `lz4kds/ARCHIVE.md`**，那是完整移植存档，比这份文档更细。
2. **不要改动上游文件**。这是用户明确要求，也是这个项目的核心设计约束。
3. **改代码后必须跑 §4.3 的三个测试**，尤其 `sddc_real_test` 里的 delta 命中
   断言——delta 命中为 0 说明相似度索引又退化了。
4. **不要反复轮询 CI**，用户明确要求只查一次最终结果。
5. **不要在没跑测试的情况下说"完成了"**。这个项目已经出现过两次「测试通过
   但测的是假副本」和「代码能编译但热路径空转」的情况。
6. 用户偏好**独立最小实现**，不喜欢改上游 Xposed/内核模块（不利后续更新）。
7. 回复用中文，简洁直接，先给结论再给依据。

---

## 10. 快速自检清单

接手后想知道"现在到底行不行"，按顺序跑：

```sh
cd /workspace/push

# 1. 上游文件是否仍然零改动
git diff --stat a19d43f -- .github/workflows/fastbuild_6.12.69_gki.yml \
    scripts lib local other_patch zram_patch droidspaces_patch
# 期望：输出为空

# 2. 注入脚本语法
python3 -m py_compile lz4kds/scripts/lz4kds_inject.py

# 3. ci 与 workflow 是否同步
diff -q lz4kds/ci/build-lz4kds.yml .github/workflows/build-lz4kds.yml

# 4. 三个测试
# 见 §4.3

# 5. 注入 / 幂等 / 回滚（在真实 AOSP zram 源上）
# 见 §4.1
```

全部通过 ⇒ 项目处于健康状态。
