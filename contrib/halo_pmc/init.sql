-- Run as a database administrator in the database to be observed.
BEGIN;
SET LOCAL allow_system_table_mods = on;
CREATE TABLE IF NOT EXISTS pg_catalog.pmc_name (
    name text PRIMARY KEY CHECK (name = lower(name)),
    event_type integer NOT NULL CHECK (event_type >= 0),
    config bigint NOT NULL,
    config1 bigint NOT NULL DEFAULT 0,
    config2 bigint NOT NULL DEFAULT 0
);
GRANT SELECT ON pg_catalog.pmc_name TO PUBLIC;

INSERT INTO pg_catalog.pmc_name(name, event_type, config) VALUES
    ('instructions', 0, 1),
    ('cycles', 0, 0),
    ('cache_references', 0, 2),
    ('cache_misses', 0, 3),
    ('branch_instructions', 0, 4),
    ('branch_misses', 0, 5),
    ('perf_count_hw_cache_l1d_rd', 3, 0),
    ('perf_count_hw_cache_l1d_miss_rd', 3, 65536),
    ('perf_count_hw_cache_l1d_wr', 3, 256)
ON CONFLICT (name) DO NOTHING;
COMMIT;

-- Optional Intel event, only after checking the host model's event list.
-- INSERT INTO pg_catalog.pmc_name(name, event_type, config)
-- VALUES ('mem_inst_retired_all_stores', 4, 33488);
