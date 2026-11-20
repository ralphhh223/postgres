\set ON_ERROR_STOP on
\ir init.sql
SET max_parallel_workers_per_gather = 0;
SET enable_nestloop = off;
SET enable_mergejoin = off;
SET work_mem = '32MB';

CREATE TEMP TABLE pmc_build AS SELECT g AS id FROM generate_series(1, 10000) g;
CREATE TEMP TABLE pmc_probe AS SELECT g AS id FROM generate_series(1, 15000) g;
ANALYZE pmc_build;
ANALYZE pmc_probe;

SELECT count(*) AS disabled FROM pmc_build JOIN pmc_probe USING (id);
SET halo_pmc_list = 'Instructions,cycles,PERF_COUNT_HW_CACHE_L1D_RD,PERF_COUNT_HW_CACHE_L1D_MISS_RD';
SET halo_pmc_rdpmc = off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) FROM pmc_build JOIN pmc_probe USING (id);
SELECT count(*) AS baseline FROM pmc_build JOIN pmc_probe USING (id);

SET halo_pmc_list = 'instructions,cycles';
SET halo_pmc_rdpmc = on;
SELECT count(*) AS low_overhead FROM pmc_build JOIN pmc_probe USING (id);
RESET halo_pmc_list;
SELECT count(*) AS reset FROM pmc_build JOIN pmc_probe USING (id);
