# LZ4KDS for zram (6.12)

为 AOSP 6.12 zram 增加 `lz4kds` 压缩算法：在 LZ4KD 单页压缩之上加入
SDDC（相似页/差分）压缩。

- 完全相同的页 → 存成 5 字节 alias
- 只有少量差异的页 → 存成对该页的 LZ4KD 差分
- 其余页 → 普通 LZ4KD 单页压缩

## 设计原则

**本目录之外的上游文件保持原样。** 所有代码都是新增文件，与上游
`lz4kd.patch` 等补丁没有文件级冲突，可以独立启停。

接入内核树由 `scripts/lz4kds_inject.py` 完成：它只修改 CI 工作区里
checkout 出来的内核源码副本，仓库本身的上游文件一行不动。脚本是幂等的，
重复执行结果一致。

## 目录结构

```
lz4kds/
├── src/                        新增的 C 源码
│   ├── lz4kds_sddc.c           SDDC 引用表与 alias/delta 数据通路
│   ├── lz4kds_backend.c        zram 后端，注册 "lz4kds" 算法
│   ├── lz4kd_encode.c          LZ4KD 编解码（华为，BSD-3-Clause/GPL-2.0）
│   ├── lz4kd_encode_delta.c
│   ├── lz4kd_decode.c
│   └── lz4kd_decode_delta.c
├── include/                    新增的头文件
│   ├── lz4kds_sddc.h
│   ├── lz4kds_zcomp.h          自包含的 zcomp 接口视图
│   └── lz4kd.h                 编解码公共头（装到 include/linux/）
├── scripts/
│   └── lz4kds_inject.py        幂等注入脚本
├── test/                       用户态验证
│   ├── sddc_test.c             算法往返与压力测试
│   └── kshim.h
└── ci/
    └── build-lz4kds.yml        独立的 GitHub Actions 配置
```

## 注入脚本做了什么

`scripts/lz4kds_inject.py <内核 common 目录> <本目录>`：

1. 拷贝 `src/` 与 `include/` 的文件到 `drivers/block/zram/` 与 `include/linux/`
2. 在 `drivers/block/zram/Makefile` 追加新对象
3. 在 `zcomp.c` 的后端表加入 `&backend_lz4kds`
4. 在 `zcomp.h` 的 `zcomp_ops` 增加 `lz4kds_release` 回调，
   并在 `zram_drv.c` 的释放路径调用它（仅用于引用使用计数）
5. 在 `Kconfig` 注册 `CONFIG_ZRAM_BACKEND_LZ4KDS`

以上改动全部带 `lz4kds` 标记，便于识别与回滚。

## 关键实现说明

### 引用不可回收

alias/delta payload 只携带一个数字引用 id，而它可能已经被写入 swap 或
后端设备。如果它指向的引用被释放，该 payload 就会解出错误数据。
因此引用在设备生命周期内**永不回收**，内存由 `LZ4KDS_REF_BUDGET`
（16 MiB）封顶：预算用尽后新页不再晋升为引用（仍正常压缩），
但所有已存在的 id 始终有效。这是用固定的内存上限换取可证明的正确性。

### 性能

第一版实现对每次解压都要在全局 spinlock 下扫描整张引用表，导致 zram
读路径严重变慢（实测切换后卡顿明显）。本版做了三处修正：

| 问题 | 修正 |
|---|---|
| 解压 O(n) 全表扫描 | 引用数组按 id 直接寻址，解压 O(1) |
| 单把全局锁 | 64 个锁分段，互不相关引用不互相阻塞 |
| 持锁跑编码器 | 候选先快照，编码在无锁状态完成 |

### 编码器状态大小

`lz4kd_encode_state_bytes_min()` 在未定义 `LZ4K_DELTA` 时返回 8192，
但 delta 编码器需要 `(1<<13 + 1<<5) * 2 = 16448` 字节，会越界写。
定义 `LZ4K_DELTA` 后由 `lz4kd_encode_delta.c` 提供正确值，注入脚本
已通过 `-DLZ4K_DELTA` 保证这一点。

## 验证

用户态（无需内核树）：

```sh
gcc -O2 -include test/kshim.h -I include -I src -o /tmp/sddc_test \
    test/sddc_test.c src/lz4kd_encode.c src/lz4kd_encode_delta.c \
    src/lz4kd_decode.c src/lz4kd_decode_delta.c
/tmp/sddc_test
```

覆盖 alias、delta、普通、原始四类往返，以及 2000 轮随机压测。

设备上确认：

```sh
cat /sys/block/zram0/comp_algorithm    # 应包含 [lz4kds]
```

## 已知限制

- 需要 4 KiB 页（delta 编解码器按 4 KiB 块工作），不支持 16 KiB 页内核。
- 仅与 zram 的 `zcomp_ops` 接口耦合，未验证其他内核版本。
- 未做真机长时间压测，首次使用建议保留原内核镜像以便回滚。

## 许可

`src/lz4kd_*.c` 与 `include/lz4kd.h` 来自华为 HiSilicon 的 LZ4KD 实现，
按 BSD-3-Clause / GPL-2.0 双许可发布，版权归原作者。
`lz4kds_*` 与注入脚本为本项目新增，GPL-2.0-or-later。
