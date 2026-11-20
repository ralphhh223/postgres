# PostgreSQL CPU PMC 大作业

基于 PostgreSQL 16.14。在非并行 Hash Join 的 `MultiExecPrivateHash()` 中采集当前后端进程的用户态 CPU 性能计数。默认关闭，打开后在执行 SQL 的会话输出 INFO。

## 编译和初始化

在 Linux 上构建并安装，使用普通系统账号运行数据库。以下命令从源码根目录执行，`$PWD/pg-install` 作为演示安装目录。

```sh
./configure --prefix="$PWD/pg-install" --without-icu --without-readline --without-zlib
make -j8
make install
pg="$PWD/pg-install/bin"
"$pg/initdb" -D "$PWD/pg-data"
"$pg/pg_ctl" -D "$PWD/pg-data" -l "$PWD/pg-server.log" -w start
"$pg/psql" -X -v ON_ERROR_STOP=1 -d postgres -f contrib/halo_pmc/demo.sql
```

从 Git 源码构建需要 Bison、Flex、C 编译器和 GNU Make。`demo.sql` 先调用 `init.sql`。观察其他数据库时，在该库单独执行一次 `init.sql`。初始化脚本由数据库超级用户执行，在事务中创建 `pg_catalog.pmc_name`，并写入 Linux perf 通用事件。创建表所需的 `allow_system_table_mods` 只在初始化事务内生效。表只向普通用户开放查询，不开放修改。事件由管理员按名称和 Linux ABI 的 `type/config/config1/config2` 扩充。Intel 原始 stores 事件示例在脚本末尾，编码需按实际处理器型号验证。

```sql
SET halo_pmc_list = 'Instructions,cycles,PERF_COUNT_HW_CACHE_L1D_RD,PERF_COUNT_HW_CACHE_L1D_MISS_RD';
SET halo_pmc_rdpmc = off;
-- 运行含非并行 Hash Join 的查询
SET halo_pmc_list = 'instructions,cycles';
SET halo_pmc_rdpmc = on;
-- 重跑同一查询，查看 mode 和 scope
RESET halo_pmc_list;
```

事件列表大小写不敏感，允许最多 8 个名字，同一事件仅打开一次。用超级用户设置参数。Linux 是否允许读取计数由宿主机的 PMU、`perf_event_paranoid` 和容器策略决定。演示容器可使用 `--cap-add PERFMON`，内核和容器必须同时允许对应操作。使用 `EXPLAIN (ANALYZE)` 核对计划中的 `Hash Join` 和 `Hash`。演示脚本关闭并行并使用 10,000 个建表元组，结果应为 10,000 行匹配。

## 测量范围

- `mode=read scope=hash_build_loop` 在建表循环前后启停独立事件，循环外执行 `read()`。覆盖取元组、计算哈希和插入，循环后的最终扩桶排除。输出原始计数和 `raw × time_enabled / time_running` 缩放值。无有效运行时间时报告事件不可用。
- `mode=rdpmc scope=hash_insert` 在 Linux x86-64 上使用 pinned 事件和 perf 元数据页。每次插入前后使用 `RDPMC` 快照，在序列号稳定的情况下累加差值。取元组和哈希计算在此范围外。输出有效和跳过的样本数。不在循环中开启、关闭、调用 `read()` 或输出日志。
- 元数据页未授权用户态 RDPMC 或事件无法固定调度时，该事件回退到 `mode=read_fallback scope=hash_build_loop`。回退的测量范围不同，不能和单次插入累计值直接比较。不同模式均有少量测量边界开销，多个事件读取也存在先后顺序。
- 描述符和映射由 `PG_FINALLY` 清理，适用于取消或执行报错。并行 Hash 建表走 PostgreSQL 原有路径。

实现位置为 `src/backend/utils/misc/halo_pmc.c`、`src/include/utils/halo_pmc.h`、`src/backend/utils/misc/guc_tables.c` 和 `src/backend/executor/nodeHash.c`。计数接口依据 [Linux perf_event_open(2)](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)。

## 实测结果

PostgreSQL 16.14 在 Linux WSL2、GCC 14.4.0 的容器中完成编译、安装和启动。普通计数输出了指令数、周期数、L1D 读取数和 L1D 读取未命中数。RDPMC 模式的指令和周期事件分别取得 10,000 个有效样本，跳过数为 0。关闭、普通采集、RDPMC 和重置后的查询均返回 10,000。完整 SQL 实测输出见 [demo-output.txt](demo-output.txt)。
