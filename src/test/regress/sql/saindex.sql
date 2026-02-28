--
-- Test SA-Index (Suffix Array Index)
--
-- Tests the saindex access method which provides efficient substring,
-- prefix, and suffix search on text columns using a suffix array.
--

-- ================================================================
-- Setup: create and populate test table
-- ================================================================

CREATE TABLE saindex_test(id serial, val text) WITH (autovacuum_enabled = off);

INSERT INTO saindex_test(val) VALUES
  ('cat'),
  ('can'),
  ('cab'),
  ('act'),
  ('bat'),
  ('hello world'),
  ('hello'),
  ('world'),
  ('worldwide'),
  ('postgresql'),
  ('postgres'),
  ('postfix'),
  ('prefix'),
  ('suffix'),
  ('fix'),
  ('ab'),
  ('a'),
  (''),
  (NULL);

-- Create SA index
CREATE INDEX saindex_test_idx ON saindex_test USING saindex (val);

-- ================================================================
-- Basic smoke tests: verify operators work outside of index context
-- ================================================================

-- @> (contains substring)
SELECT val FROM saindex_test WHERE val @> 'at' ORDER BY val;

-- ^@ (starts with)
SELECT val FROM saindex_test WHERE val ^@ 'post' ORDER BY val;

-- $@ (ends with)
SELECT val FROM saindex_test WHERE val $@ 'fix' ORDER BY val;

-- ================================================================
-- Strategy 1: CONTAINS (@>) -- test index scan
-- ================================================================

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- Verify index is used
EXPLAIN (COSTS OFF)
SELECT val FROM saindex_test WHERE val @> 'at' ORDER BY val;

-- Basic contains
SELECT val FROM saindex_test WHERE val @> 'at' ORDER BY val;

-- Pattern at the beginning of value
SELECT val FROM saindex_test WHERE val @> 'hel' ORDER BY val;

-- Pattern at the end of value
SELECT val FROM saindex_test WHERE val @> 'rld' ORDER BY val;

-- Pattern in the middle
SELECT val FROM saindex_test WHERE val @> 'llo' ORDER BY val;

-- Single character
SELECT val FROM saindex_test WHERE val @> 'x' ORDER BY val;

-- No match
SELECT val FROM saindex_test WHERE val @> 'xyz' ORDER BY val;

-- Longer pattern
SELECT val FROM saindex_test WHERE val @> 'hello world' ORDER BY val;

RESET enable_seqscan;
RESET enable_bitmapscan;

-- ================================================================
-- Strategy 2: PREFIX (^@) -- test index scan
-- ================================================================

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- Verify index is used
EXPLAIN (COSTS OFF)
SELECT val FROM saindex_test WHERE val ^@ 'post' ORDER BY val;

-- Basic prefix
SELECT val FROM saindex_test WHERE val ^@ 'post' ORDER BY val;

-- Single-char prefix
SELECT val FROM saindex_test WHERE val ^@ 'c' ORDER BY val;

-- Full string match
SELECT val FROM saindex_test WHERE val ^@ 'cat' ORDER BY val;

-- No match
SELECT val FROM saindex_test WHERE val ^@ 'zzz' ORDER BY val;

-- Prefix matching multiple values
SELECT val FROM saindex_test WHERE val ^@ 'ca' ORDER BY val;

RESET enable_seqscan;
RESET enable_bitmapscan;

-- ================================================================
-- Strategy 3: SUFFIX ($@) -- test index scan
-- ================================================================

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- Verify index is used
EXPLAIN (COSTS OFF)
SELECT val FROM saindex_test WHERE val $@ 'fix' ORDER BY val;

-- Basic suffix
SELECT val FROM saindex_test WHERE val $@ 'fix' ORDER BY val;

-- Single-char suffix
SELECT val FROM saindex_test WHERE val $@ 't' ORDER BY val;

-- Full string match
SELECT val FROM saindex_test WHERE val $@ 'cat' ORDER BY val;

-- No match
SELECT val FROM saindex_test WHERE val $@ 'zzz' ORDER BY val;

-- Suffix matching multiple values
SELECT val FROM saindex_test WHERE val $@ 'ld' ORDER BY val;

RESET enable_seqscan;
RESET enable_bitmapscan;

-- ================================================================
-- Correctness: compare index results with sequential scan
-- ================================================================

-- Contains: results from index scan should match sequential scan
SET enable_seqscan = off;
SET enable_bitmapscan = on;
CREATE TEMP TABLE idx_contains AS
  SELECT val FROM saindex_test WHERE val @> 'or' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;

SET enable_seqscan = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE seq_contains AS
  SELECT val FROM saindex_test WHERE val @> 'or' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

-- Should return no rows if results match
SELECT * FROM idx_contains EXCEPT SELECT * FROM seq_contains;
SELECT * FROM seq_contains EXCEPT SELECT * FROM idx_contains;

-- Prefix: compare index vs sequential
SET enable_seqscan = off;
SET enable_bitmapscan = on;
CREATE TEMP TABLE idx_prefix AS
  SELECT val FROM saindex_test WHERE val ^@ 'pre' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;

SET enable_seqscan = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE seq_prefix AS
  SELECT val FROM saindex_test WHERE val ^@ 'pre' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

SELECT * FROM idx_prefix EXCEPT SELECT * FROM seq_prefix;
SELECT * FROM seq_prefix EXCEPT SELECT * FROM idx_prefix;

-- Suffix: compare index vs sequential
SET enable_seqscan = off;
SET enable_bitmapscan = on;
CREATE TEMP TABLE idx_suffix AS
  SELECT val FROM saindex_test WHERE val $@ 'es' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;

SET enable_seqscan = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE seq_suffix AS
  SELECT val FROM saindex_test WHERE val $@ 'es' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

SELECT * FROM idx_suffix EXCEPT SELECT * FROM seq_suffix;
SELECT * FROM seq_suffix EXCEPT SELECT * FROM idx_suffix;

-- ================================================================
-- Edge cases
-- ================================================================

-- Empty pattern: @> '' should match all non-null rows
SET enable_seqscan = off;
SET enable_bitmapscan = on;

SELECT count(*) FROM saindex_test WHERE val @> '';

RESET enable_seqscan;
RESET enable_bitmapscan;

-- Index on an empty table
CREATE TABLE saindex_empty(val text);
CREATE INDEX saindex_empty_idx ON saindex_empty USING saindex (val);

SET enable_seqscan = off;
SET enable_bitmapscan = on;

SELECT val FROM saindex_empty WHERE val @> 'test';
SELECT val FROM saindex_empty WHERE val ^@ 'test';
SELECT val FROM saindex_empty WHERE val $@ 'test';

RESET enable_seqscan;
RESET enable_bitmapscan;

DROP TABLE saindex_empty;

-- Index with reloption: custom prefix length
CREATE INDEX saindex_test_short_idx ON saindex_test
  USING saindex (val) WITH (max_prefix_len = 16);

SET enable_seqscan = off;
SET enable_bitmapscan = on;

SELECT val FROM saindex_test WHERE val @> 'post' ORDER BY val;

RESET enable_seqscan;
RESET enable_bitmapscan;

DROP INDEX saindex_test_short_idx;

-- Drop and recreate
DROP INDEX saindex_test_idx;

CREATE INDEX saindex_test_idx ON saindex_test USING saindex (val);

SET enable_seqscan = off;
SET enable_bitmapscan = on;

SELECT val FROM saindex_test WHERE val @> 'cat' ORDER BY val;

RESET enable_seqscan;
RESET enable_bitmapscan;

-- ================================================================
-- Larger dataset: test with generated data
-- ================================================================

CREATE TABLE saindex_large(val text) WITH (autovacuum_enabled = off);

INSERT INTO saindex_large
  SELECT 'item_' || g || '_data_' || (g % 100)
  FROM generate_series(1, 1000) g;

CREATE INDEX saindex_large_idx ON saindex_large USING saindex (val);

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- Contains
SELECT count(*) FROM saindex_large WHERE val @> '_data_42';

-- Prefix
SELECT count(*) FROM saindex_large WHERE val ^@ 'item_5';

-- Suffix
SELECT count(*) FROM saindex_large WHERE val $@ '_data_0';

RESET enable_seqscan;
RESET enable_bitmapscan;

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE saindex_large;
DROP TABLE saindex_test CASCADE;
