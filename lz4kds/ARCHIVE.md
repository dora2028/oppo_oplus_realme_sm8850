# LZ4KDS 移植存档

记录本特性改了哪些文件、改在什么位置、为什么这样改，供下次接手时快速
恢复上下文。

- 目标内核：AOSP common `android16-6.12`（6.12.69 GKI）
- 目标算法：在 zram 上新增 `lz4kds` = LZ4KD 单页压缩 + SDDC 相似页/差分
- 集成方式：**仓库上游文件零改动**，全部通过 `lz4kds/` 目录 + 注入脚本

---

## 1. 改动清单

### 1.1 新增文件（全部在 `lz4kds/` 下）

| 文件 | 作用 |
|---|---|
| `src/lz4kds_sddc.c` | SDDC 引用表、alias/delta 数据通路（本项目核心） |
| `src/lz4kds_backend.c` | zram 后端，注册 `"lz4kds"` 算法 |
| `src/lz4kd_encode.c` | LZ4KD 编码（华为 HiSilicon） |
| `src/lz4kd_encode_delta.c` | LZ4KD 差分编码 |
| `src/lz4kd_decode.c` | LZ4KD 解码 |
| `src/lz4kd_decode_delta.c` | LZ4KD 差分解码 |
| `src/lz4kd_private.h` | 编解码私有定义 |
| `src/lz4kd_encode_private.h` | 编码私有定义 |
| `include/lz4kd.h` | 编解码公共头，装到 `include/linux/` |
| `include/lz4kds_sddc.h` | SDDC 对外接口 |
| `include/lz4kds_zcomp.h` | zcomp 接口视图（独立审阅用） |
| `scripts/lz4kds_inject.py` | 注入 / 回滚 / 检查脚本 |
| `test/sddc_real_test.c` | 链接真实 `lz4kds_sddc.c` 的往返 + 压测 |
| `test/exhaust_test.c` | 引用表耗尽后仍正确回退 |
| `test/random_test.c` | 不可压缩页必须报 PAGE_SIZE 且不带标记 |
| `test/kshim.h` + `test/linux/` | 用户态编译垫片 |
| `ci/build-lz4kds.yml` | 独立 CI 配置 |
| `README.md` | 使用与设计说明 |

### 1.2 注入脚本会修改内核树的 3 个文件

以下改动**只发生在 CI 工作区 checkout 出的内核源码副本里**，仓库中这些
文件保持上游原样。每处都带 `LZ4KDS-BEGIN/END` 标记。

| 内核文件 | 改动位置 | 改了什么 |
|---|---|---|
| `drivers/block/zram/Makefile` | 文件末尾追加 | 新对象的编译规则 + `-DLZ4K_DELTA` |
| `drivers/block/zram/zcomp.c` | ① `#include "backend_842.h"` 之后<br>② `backends[]` 的 `NULL` 哨兵之前 | ① 引入 backend 声明头<br>② 注册 `&backend_lz4kds` |
| `drivers/block/zram/Kconfig` | `config ZRAM_BACKEND_ZSTD` 之前 | 新增 `ZRAM_BACKEND_LZ4KDS` |

早期版本还会改 `zcomp.h`（给 `zcomp_ops` 加 `lz4kds_release` 回调）和
`zram_drv.c`（在 `zram_free_page()` 里调用它）。该钩子只维护一个无人读取的
计数器，见 §3.2 第 1 条，已整体移除。

**若内核树已有 `lib/lz4kd/`**（即应用了上游 `lz4kd.patch`），注入脚本会
自动跳过自带的编解码源码与 `select LZ4KD_COMPRESS`，改用上游对象，避免
`lz4kd_encode()` 等符号重复定义导致链接失败。

### 1.3 新增的独立 CI

`.github/workflows/build-lz4kds.yml`（源文件 `lz4kds/ci/build-lz4kds.yml`）

两个 job：
- `host-test`：编译并运行三个用户态测试（均链接真实 `lz4kds_sddc.c`）
- `build`：clone AOSP common → 注入 → 配置 → 编译 zram 对象 → 校验符号

不依赖、不修改上游任何 workflow。

**关于独立 CI 之前那次 `failure`**：失败原因不是注入或 AOSP 树参数，而是
工具链缺失——日志里是

```
scripts/Kconfig.include:41: linker 'ld.lld' not found
make[2]: *** [scripts/kconfig/Makefile:102: gki_defconfig] Error 1
```

即 `make gki_defconfig` 在 kconfig 阶段就退出了，根本没走到编译。workflow
设了 `LLVM=1 LLVM_IAS=1`，但依赖只装了 `build-essential`，没有 clang/lld。
上游 `fastbuild` 之所以能编，是因为它另外下载了
`cctv18/oneplus_sm8650_toolchain` 的 clang19 + rust + build-tools 并拼了
PATH。现已改为 `apt-get install clang lld llvm`，并在编译前显式检查
`clang` / `ld.lld` 是否存在，缺失时给出可读报错而不是 kconfig abort。

---

## 2. 注入脚本用法

```sh
# 注入（幂等，可重复执行）
python3 lz4kds/scripts/lz4kds_inject.py <内核目录> <lz4kds目录>

# 查看当前状态
python3 lz4kds/scripts/lz4kds_inject.py <内核目录> <lz4kds目录> --mode check

# 完整回滚（逐字节还原，已验证）
python3 lz4kds/scripts/lz4kds_inject.py <内核目录> <lz4kds目录> --mode revert
```

回滚机制：注入前把 5 个待改文件备份到 `<内核目录>/.lz4kds_backup/`，
回滚时直接还原备份，因此能保证逐字节精确；备份不存在时退回按标记删除。

实测结论（本地 6.12 头文件环境）：

| 场景 | 结果 |
|---|---|
| 注入 | 5 个文件被修改，11 个源文件被拷贝 |
| 重复注入 | 0 个文件被修改（幂等） |
| 回滚 | 5 个文件与原始**逐字节一致** |
| 注入后编译 | `zram.o` 链接成功 |

---

## 3. 关键设计决定

### 3.1 引用不可回收（正确性）

alias/delta payload 只携带一个数字引用 id，而该 payload 可能已写入 swap
或后端设备。若引用被回收，旧 payload 会解出错误数据。

**决定**：引用在设备生命周期内永不回收，内存由 `LZ4KDS_REF_BUDGET`
（16 MiB）封顶。预算用尽后新页不再晋升为引用（仍正常压缩），
但所有已存在 id 始终有效。

**代价**：固定内存占用上限，换来可证明的正确性。

### 3.2 性能

**第一轮修正**（10:02 那次）解决的是锁与查表：

| 问题 | 影响 | 修正 |
|---|---|---|
| 解压时扫描整张引用表（1024 bucket） | zram 读是最热路径，每读一页全表扫描 | 引用数组按 id 直接寻址，解压 O(1) |
| 单把全局 spinlock | 所有 CPU 的 zram 读写串行化 | 64 个锁分段 |
| 持锁执行 8×4KB memcpy + 编码 | 写路径长时间占锁 | 候选先快照，编码无锁完成 |

页查找另用哈希索引（`LZ4KDS_HASH_BITS=12`），平均 O(1)。

**第二轮修正**（本次）。第一轮只修了"怎么访问"，没修"访问了没用的东西"。
逐条比对迁移前后的代码后发现三处仍在热路径上空转：

1. **`zram_free_page()` 里的释放钩子完全是空转。**
   钩子映射每个待释放对象，只为把 `ref->users` 减一；而 `users` 在整个
   代码库里**没有任何地方被读取**（引用本就不可回收）。它却跑在释放热路径
   上、在 `zram_slot_lock` 之内、且处于 atomic 上下文
   （`zram_slot_free_notify` 由 bio 完成路径调用），每释放一页多一次
   `zcomp_stream_get()` + `zs_map_object()` 往返。
   **修正**：删除该钩子，同时删掉 `zcomp_ops` 的 `lz4kds_release` 扩展和
   `lz4kds_sddc_put_payload()`。注入脚本改动的内核文件从 5 个降到 3 个。

2. **delta 候选永远找不到。**
   候选只从"整页 4 KiB jhash"的同一 bucket 取。但整页哈希对单字节差异
   完全不鲁棒——实测 2000 个仅改 1 字节的相似页，**0 个**与原页同 bucket。
   于是 delta 路径形同虚设，却每次写入都要付一次整页哈希 + bucket 遍历。
   **修正**：改用 4 个跨页均匀分布的 64 字节窗口指纹，任一窗口未被改动
   即可命中。实测同负载下 delta 从 0 提升到 617 次（`sddc_real_test`）。

3. **引用表满后每次发布都线性扫表尾。**
   `for (id = next_id; id < REF_MAX; id++)` 在 `next_id` 触顶后每次都从
   4096 扫到末尾才失败。
   **修正**：引用不可回收 ⇒ `next_id` 本身就是首个空槽，直接取用，O(1)。

另修一处潜在的**编译期隐患**：`lz4kds_zcomp.h` 的 in-tree 分支原本以
`LZ4KDS_IN_TREE` 为条件，但注入脚本从未定义该宏，backend 实际编译时用的是
本地复制的结构体。当前两者布局一致所以没出事，但上游一旦给 `zcomp_ops`
加字段，就会得到一个**静默错位的 vtable**而不是编译错误。
**修正**：改用 `__KERNEL__` 判定，in-tree 一律包含真实 `zcomp.h`。

实测（x86 主机，200k 次迭代）：

| 路径 | 修正前 | 修正后 |
|---|---|---|
| 解压 alias 页 | 2.36 µs/页 | 1.95 µs/页 |
| 写路径（表已满） | 4.83 µs/页 | 6.64 µs/页 |

写路径变慢是**预期且正确**的：修正前 delta 根本没生效，等于白跑；修正后
delta 真正参与编码，用 CPU 换压缩率。`LZ4KDS_DELTA_TRIES` 取 2 是实测折中
（取 1 为 8.02 µs、取 4 为 10.06 µs）。

### 3.3 编码器状态大小（踩过的坑）

`lz4kd_encode_state_bytes_min()` 在**未定义** `LZ4K_DELTA` 时返回 8192，
但 delta 编码器实际需要 `(1<<13 + 1<<5) * 2 = 16448` 字节，会越界写。

用户态实测：按 8192 分配 → 段错误。

**解决**：注入脚本加 `-DLZ4K_DELTA`，此时由 `lz4kd_encode_delta.c`
提供正确值 16448。

### 3.4 与上游 lz4kd.patch 的关系

上游 `other_patch/lz4kd.patch` 自带 `backend_lz4kd.c`，且其 `lz4kd`
后端只是调整 LZ4 的 acceleration 参数，**并未使用真正的华为 LZ4KD 编解码**。

本方案因此：
- 不复用上游 backend，自带 `lz4kds_backend.c`
- 直接使用 `lib/lz4kd/` 的真实编解码
- 与上游补丁**没有文件级冲突**，可独立启停

---

## 4. 已知限制

1. **需要 4 KiB 页**：delta 编解码器按 4 KiB 块工作，16 KiB 页内核不支持。
2. **未真机长测**：只验证到「编译通过 + 算法往返正确 + 符号入镜像」。
   首次使用建议保留原内核镜像。
3. **zstd 不可用**：若同时应用上游 `002-zstd.patch`，该补丁对
   `include/linux/zstd.h` 做了 +77/−166 的裁剪，删除 `backend_zstd.c`
   依赖的 12 个接口（已实测确认全部 GONE），导致 zstd 后端编译失败。
   本独立方案本身不碰 zstd。
4. **SUSFS 不兼容**：开启 SUSFS 时 `fs/proc/task_mmu.c` 的补丁与该内核
   版本不匹配，会编译失败（与 LZ4KDS 无关）。

---

## 5. 验证方法

### 用户态（无需内核树）

```sh
cd <repo>
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

注意这里链接的是 `src/lz4kds_sddc.c` 本身。早先的 `test/sddc_test.c`
在测试文件里**另写了一份** SDDC 逻辑，测的是那份副本，所以它通过并不能
证明生产代码正确——已删除。

覆盖：alias / delta / 普通 / 原始四类往返、2000 轮压测、引用表耗尽、
不可压缩页。

### 设备上

```sh
cat /sys/block/zram0/comp_algorithm     # 应包含 [lz4kds]
echo lz4kds > /sys/block/zram0/comp_algorithm
```

---

## 6. 后续接手建议

- 若需 zstd 与 LZ4KDS 共存：跳过 `002-zstd.patch`，改用 AOSP 原生
  zstd（其 `zstd.h` 完整提供所需 12 个接口，已核实）。
- 若需调参：`lz4kds_sddc.c` 顶部常量
  （`LZ4KDS_REF_BUDGET`、`LZ4KDS_LOCK_STRIPES`、`LZ4KDS_DELTA_TRIES`、
  `LZ4KDS_WINDOWS`、`LZ4KDS_SAMPLE_WAYS`）。
  `LZ4KDS_DELTA_TRIES` 直接决定写路径开销与压缩率的取舍，实测取 2 最优；
  调大前请重跑 `test/sddc_real_test.c` 确认 delta 命中率确有提升。
- 若需统计：`lz4kds_sddc_get_stats()` 已提供 alias/delta/saved_bytes 等
  计数，可接 sysfs 或 debugfs 暴露（当前未接）。
