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
| `test/sddc_test.c` | 用户态算法测试 |
| `test/kshim.h` | 用户态编译垫片 |
| `ci/build-lz4kds.yml` | 独立 CI 配置 |
| `README.md` | 使用与设计说明 |

### 1.2 注入脚本会修改内核树的 5 个文件

以下改动**只发生在 CI 工作区 checkout 出的内核源码副本里**，仓库中这些
文件保持上游原样。每处都带 `LZ4KDS-BEGIN/END` 标记。

| 内核文件 | 改动位置 | 改了什么 |
|---|---|---|
| `drivers/block/zram/Makefile` | 文件末尾追加 | 新对象的编译规则 + `-DLZ4K_DELTA` |
| `drivers/block/zram/zcomp.c` | ① `#include "backend_842.h"` 之后<br>② `backends[]` 的 `NULL` 哨兵之前 | ① 引入 backend 声明头<br>② 注册 `&backend_lz4kds` |
| `drivers/block/zram/zcomp.h` | `zcomp_ops` 的 `release_params` 之后 | 新增 `lz4kds_release` 回调 |
| `drivers/block/zram/zram_drv.c` | `zram_free_page()` 内，`zs_free()` 之前 | 调用 `lz4kds_release`（仅引用计数） |
| `drivers/block/zram/Kconfig` | `config ZRAM_BACKEND_ZSTD` 之前 | 新增 `ZRAM_BACKEND_LZ4KDS` |

### 1.3 新增的独立 CI

`.github/workflows/build-lz4kds.yml`（源文件 `lz4kds/ci/build-lz4kds.yml`）

两个 job：
- `host-test`：编译并运行用户态算法测试，必须输出 `RESULT: PASS`
- `build`：clone AOSP common → 注入 → 配置 → 编译 zram 对象 → 校验符号

不依赖、不修改上游任何 workflow。

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

### 3.2 性能（这一版的重点）

第一版实测切换后**严重卡顿**，根因是三处实现缺陷：

| 问题 | 影响 | 修正 |
|---|---|---|
| 解压时扫描整张引用表（1024 bucket） | zram 读是最热路径，每读一页全表扫描 | 引用数组按 id 直接寻址，解压 O(1) |
| 单把全局 spinlock | 所有 CPU 的 zram 读写串行化 | 64 个锁分段 |
| 持锁执行 8×4KB memcpy + 编码 | 写路径长时间占锁 | 候选先快照，编码无锁完成 |

页查找另用哈希索引（`LZ4KDS_HASH_BITS=12`），平均 O(1)。

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
gcc -O2 -include lz4kds/test/kshim.h \
    -I lz4kds/include -I lz4kds/src \
    -o /tmp/sddc_test \
    lz4kds/test/sddc_test.c \
    lz4kds/src/lz4kd_encode.c lz4kds/src/lz4kd_encode_delta.c \
    lz4kds/src/lz4kd_decode.c lz4kds/src/lz4kd_decode_delta.c
/tmp/sddc_test        # 期望 RESULT: PASS
```

覆盖 alias / delta / 普通 / 原始四类往返 + 2000 轮随机压测。

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
  （`LZ4KDS_REF_BUDGET`、`LZ4KDS_LOCK_STRIPES`、`LZ4KDS_DELTA_TRIES`）。
- 若需统计：`lz4kds_sddc_get_stats()` 已提供 alias/delta/saved_bytes 等
  计数，可接 sysfs 或 debugfs 暴露（当前未接）。
