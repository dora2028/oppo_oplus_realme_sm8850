# LZ4KDS for zram (6.12)

为 AOSP 6.12 zram 增加 `lz4kds` 压缩算法：LZ4KD 单页压缩 + SDDC 相似页/差分。

- 完全相同的页 → 5 字节 alias
- 只有少量差异的页 → 对该页的 LZ4KD 差分
- 其余页 → 普通 LZ4KD 单页压缩

> **改动清单与移植记录见 [ARCHIVE.md](ARCHIVE.md)**（改了哪些文件、
> 改在什么位置、踩过哪些坑）。

## 设计原则

**仓库里的上游文件一行不改。** 所有代码都是新增文件，与上游
`lz4kd.patch` 等补丁没有文件级冲突，可独立启停。

接入内核树由 `scripts/lz4kds_inject.py` 完成，它只修改 CI 工作区里
checkout 出来的内核源码副本。脚本支持注入 / 检查 / 回滚三种模式，
回滚后与原文件逐字节一致。

## 目录

```
lz4kds/
├── src/            新增 C 源码
│   ├── lz4kds_sddc.c      SDDC 引用表与 alias/delta 通路（核心）
│   ├── lz4kds_backend.c   zram 后端，注册 "lz4kds"
│   └── lz4kd_*.c          LZ4KD 编解码（华为，BSD-3-Clause/GPL-2.0）
├── include/        新增头文件
├── scripts/        lz4kds_inject.py
├── test/           用户态算法测试
├── ci/             独立 CI 配置
├── README.md
└── ARCHIVE.md      移植存档
```

## 用法

```sh
# 注入到内核树（幂等）
python3 lz4kds/scripts/lz4kds_inject.py <内核目录> <lz4kds目录>

# 查看状态
python3 lz4kds/scripts/lz4kds_inject.py <内核目录> <lz4kds目录> --mode check

# 回滚（逐字节还原）
python3 lz4kds/scripts/lz4kds_inject.py <内核目录> <lz4kds目录> --mode revert
```

注入脚本会在 5 个上游文件里加入带 `LZ4KDS-BEGIN/END` 标记的代码块，
并在修改前把原文件备份到 `<内核目录>/.lz4kds_backup/`。

## 验证

用户态（无需内核树）：

```sh
gcc -O2 -include lz4kds/test/kshim.h \
    -I lz4kds/include -I lz4kds/src \
    -o /tmp/sddc_test lz4kds/test/sddc_test.c \
    lz4kds/src/lz4kd_encode.c lz4kds/src/lz4kd_encode_delta.c \
    lz4kds/src/lz4kd_decode.c lz4kds/src/lz4kd_decode_delta.c
/tmp/sddc_test      # 期望 RESULT: PASS
```

设备上：

```sh
cat /sys/block/zram0/comp_algorithm     # 应包含 [lz4kds]
```

## 关键实现要点

**引用不可回收**：alias/delta payload 只带数字 id，且可能已落盘。因此引用
在设备生命周期内永不回收，内存由 16 MiB 预算封顶；预算用尽后新页不再晋升
（仍正常压缩），已存在 id 始终有效。用固定内存上限换可证明的正确性。

**性能**：引用数组按 id 直接寻址（解压 O(1)）、64 个锁分段、编码不在锁内
执行、页查找走哈希索引。

**编码器状态**：`lz4kd_encode_state_bytes_min()` 在未定义 `LZ4K_DELTA` 时
返回 8192，而 delta 编码器需要 16448 字节，会越界。注入脚本加
`-DLZ4K_DELTA` 以取得正确值。

## 已知限制

- 需要 4 KiB 页（delta 编解码按 4 KiB 块工作）
- 未做真机长时间压测，建议保留原内核镜像
- 与上游 `002-zstd.patch` 同时使用时 zstd 后端编译失败（该补丁裁剪了
  `zstd.h` 中 12 个接口）；本方案本身不碰 zstd

## 许可

`src/lz4kd_*.c` 与 `include/lz4kd.h` 来自华为 HiSilicon，按
BSD-3-Clause / GPL-2.0 双许可发布，版权归原作者。
`lz4kds_*` 与注入脚本为本项目新增，GPL-2.0-or-later。
