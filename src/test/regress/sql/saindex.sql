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

-- ~@ (ends with)
SELECT val FROM saindex_test WHERE val ~@ 'fix' ORDER BY val;

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
-- Strategy 3: SUFFIX (~@) -- test index scan
-- ================================================================

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- Verify index is used
EXPLAIN (COSTS OFF)
SELECT val FROM saindex_test WHERE val ~@ 'fix' ORDER BY val;

-- Basic suffix
SELECT val FROM saindex_test WHERE val ~@ 'fix' ORDER BY val;

-- Single-char suffix
SELECT val FROM saindex_test WHERE val ~@ 't' ORDER BY val;

-- Full string match
SELECT val FROM saindex_test WHERE val ~@ 'cat' ORDER BY val;

-- No match
SELECT val FROM saindex_test WHERE val ~@ 'zzz' ORDER BY val;

-- Suffix matching multiple values
SELECT val FROM saindex_test WHERE val ~@ 'ld' ORDER BY val;

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
  SELECT val FROM saindex_test WHERE val ~@ 'es' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;

SET enable_seqscan = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE seq_suffix AS
  SELECT val FROM saindex_test WHERE val ~@ 'es' ORDER BY val;
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
SELECT val FROM saindex_empty WHERE val ~@ 'test';

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
-- Repeated characters (classic suffix array edge cases)
-- Strings like 'aaa', 'ababab', 'banana' have many overlapping
-- suffix pairs and stress the binary-search deduplication logic.
-- ================================================================

CREATE TABLE saindex_repeated(id serial, val text) WITH (autovacuum_enabled = off);

INSERT INTO saindex_repeated(val) VALUES
  ('aaa'),
  ('aaaaa'),
  ('ababab'),
  ('abcabc'),
  ('abababab'),
  ('aabaab'),
  ('xxyyzz'),
  ('banana');

CREATE INDEX saindex_repeated_idx ON saindex_repeated USING saindex (val);

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- Repeated single character
SELECT val FROM saindex_repeated WHERE val @> 'aa' ORDER BY val;

-- Alternating two-char pattern
SELECT val FROM saindex_repeated WHERE val @> 'aba' ORDER BY val;

-- Pattern appearing multiple times in same string
SELECT val FROM saindex_repeated WHERE val @> 'ab' ORDER BY val;

-- 'ana' occurs twice in 'banana'
SELECT val FROM saindex_repeated WHERE val @> 'ana' ORDER BY val;

-- Prefix
SELECT val FROM saindex_repeated WHERE val ^@ 'ab' ORDER BY val;

-- Suffix
SELECT val FROM saindex_repeated WHERE val ~@ 'ab' ORDER BY val;

-- Correctness: index vs seqscan
CREATE TEMP TABLE idx_repeated AS
  SELECT val FROM saindex_repeated WHERE val @> 'ab' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;

SET enable_seqscan = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE seq_repeated AS
  SELECT val FROM saindex_repeated WHERE val @> 'ab' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

SELECT * FROM idx_repeated EXCEPT SELECT * FROM seq_repeated;
SELECT * FROM seq_repeated EXCEPT SELECT * FROM idx_repeated;

DROP TABLE saindex_repeated;

-- ================================================================
-- Mississippi tests (canonical suffix array test case)
-- 'mississippi' has many repeated substrings and is the standard
-- benchmark string for suffix array / suffix tree algorithms.
-- ================================================================

CREATE TABLE saindex_mississippi(id serial, val text) WITH (autovacuum_enabled = off);

INSERT INTO saindex_mississippi(val) VALUES
  ('mississippi'),
  ('missouri'),
  ('miss'),
  ('ippi'),
  ('issi'),
  ('ssissippi'),
  ('ab'),
  ('ba'),
  ('hello'),
  ('world');

CREATE INDEX saindex_mississippi_idx ON saindex_mississippi USING saindex (val);

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- Various substrings of 'mississippi'
SELECT val FROM saindex_mississippi WHERE val @> 'issi' ORDER BY val;
SELECT val FROM saindex_mississippi WHERE val @> 'ssi'  ORDER BY val;
SELECT val FROM saindex_mississippi WHERE val @> 'iss'  ORDER BY val;
SELECT val FROM saindex_mississippi WHERE val @> 'ss'   ORDER BY val;
SELECT val FROM saindex_mississippi WHERE val @> 'pp'   ORDER BY val;
SELECT val FROM saindex_mississippi WHERE val @> 'pi'   ORDER BY val;
SELECT val FROM saindex_mississippi WHERE val @> 'miss' ORDER BY val;

-- Prefix queries
SELECT val FROM saindex_mississippi WHERE val ^@ 'miss' ORDER BY val;
SELECT val FROM saindex_mississippi WHERE val ^@ 'mis'  ORDER BY val;

-- Suffix queries
SELECT val FROM saindex_mississippi WHERE val ~@ 'ippi' ORDER BY val;
SELECT val FROM saindex_mississippi WHERE val ~@ 'ppi'  ORDER BY val;

-- Correctness: index vs seqscan
CREATE TEMP TABLE idx_miss AS
  SELECT val FROM saindex_mississippi WHERE val @> 'issi' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;

SET enable_seqscan = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE seq_miss AS
  SELECT val FROM saindex_mississippi WHERE val @> 'issi' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

SELECT * FROM idx_miss EXCEPT SELECT * FROM seq_miss;
SELECT * FROM seq_miss EXCEPT SELECT * FROM idx_miss;

DROP TABLE saindex_mississippi;

-- ================================================================
-- Special strings: spaces, digits, punctuation, uppercase
-- Tests that byte-level suffix array handles non-alpha chars.
-- ================================================================

CREATE TABLE saindex_special(id serial, val text) WITH (autovacuum_enabled = off);

INSERT INTO saindex_special(val) VALUES
  (' '),            -- single space
  ('  '),           -- double space
  ('123'),          -- digits only
  ('abc 123'),      -- space in middle
  ('   leading'),   -- leading spaces
  ('trailing   '),  -- trailing spaces
  ('a b c'),        -- alternating spaces
  ('hello world'),  -- space separating words
  ('test test'),    -- repeated word with space
  ('!@#'),          -- special chars
  ('0123456789'),   -- all digits
  ('UPPERCASE');    -- uppercase letters

CREATE INDEX saindex_special_idx ON saindex_special USING saindex (val);

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- Space in data
SELECT val FROM saindex_special WHERE val @> ' '   ORDER BY val;
SELECT val FROM saindex_special WHERE val @> '  '  ORDER BY val;

-- Starts/ends with space
SELECT val FROM saindex_special WHERE val ^@ ' '   ORDER BY val;
SELECT val FROM saindex_special WHERE val ~@ ' '   ORDER BY val;

-- Digit patterns
SELECT val FROM saindex_special WHERE val @> '123' ORDER BY val;
SELECT val FROM saindex_special WHERE val ^@ '123' ORDER BY val;
SELECT val FROM saindex_special WHERE val ~@ '123' ORDER BY val;

-- Space inside the pattern
SELECT val FROM saindex_special WHERE val @> 'lo w'  ORDER BY val;
SELECT val FROM saindex_special WHERE val @> ' b '   ORDER BY val;

-- Uppercase
SELECT val FROM saindex_special WHERE val @> 'UPPER' ORDER BY val;
SELECT val FROM saindex_special WHERE val ^@ 'UPPER' ORDER BY val;
SELECT val FROM saindex_special WHERE val ~@ 'CASE'  ORDER BY val;

-- Correctness: index vs seqscan
CREATE TEMP TABLE idx_special AS
  SELECT val FROM saindex_special WHERE val @> ' ' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;

SET enable_seqscan = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE seq_special AS
  SELECT val FROM saindex_special WHERE val @> ' ' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

SELECT * FROM idx_special EXCEPT SELECT * FROM seq_special;
SELECT * FROM seq_special EXCEPT SELECT * FROM idx_special;

DROP TABLE saindex_special;

-- ================================================================
-- Unicode / multi-byte character tests
-- The suffix array operates on raw bytes (UTF-8), so multi-byte
-- characters work as long as patterns are also valid UTF-8.
-- ================================================================

CREATE TABLE saindex_unicode(id serial, val text) WITH (autovacuum_enabled = off);

INSERT INTO saindex_unicode(val) VALUES
  ('привіт світ'),      -- Ukrainian: hello world
  ('привіт друг'),      -- Ukrainian: hello friend
  ('світовий мир'),     -- Ukrainian: world peace
  ('こんにちは世界'),   -- Japanese: hello world
  ('世界平和'),          -- Japanese: world peace
  ('你好世界'),          -- Chinese: hello world
  ('café'),             -- accented Latin
  ('naïve'),            -- diaeresis
  ('résumé');           -- accented vowels

CREATE INDEX saindex_unicode_idx ON saindex_unicode USING saindex (val);

SET enable_seqscan = off;
SET enable_bitmapscan = on;

-- Cyrillic contains / prefix / suffix
SELECT val FROM saindex_unicode WHERE val @> 'світ'    ORDER BY val;
SELECT val FROM saindex_unicode WHERE val ^@ 'привіт'  ORDER BY val;
SELECT val FROM saindex_unicode WHERE val ~@ 'мир'     ORDER BY val;

-- CJK contains / prefix / suffix
SELECT val FROM saindex_unicode WHERE val @> '世界'    ORDER BY val;
SELECT val FROM saindex_unicode WHERE val ^@ '世界'    ORDER BY val;
SELECT val FROM saindex_unicode WHERE val ~@ '平和'    ORDER BY val;

-- Accented Latin
SELECT val FROM saindex_unicode WHERE val @> 'é'       ORDER BY val;
SELECT val FROM saindex_unicode WHERE val ~@ 'umé'     ORDER BY val;

-- Correctness: index vs seqscan for CJK
CREATE TEMP TABLE idx_unicode AS
  SELECT val FROM saindex_unicode WHERE val @> '世界' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;

SET enable_seqscan = on;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
CREATE TEMP TABLE seq_unicode AS
  SELECT val FROM saindex_unicode WHERE val @> '世界' ORDER BY val;
RESET enable_seqscan;
RESET enable_bitmapscan;
RESET enable_indexscan;

SELECT * FROM idx_unicode EXCEPT SELECT * FROM seq_unicode;
SELECT * FROM seq_unicode EXCEPT SELECT * FROM idx_unicode;

DROP TABLE saindex_unicode;

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
SELECT count(*) FROM saindex_large WHERE val ~@ '_data_0';

RESET enable_seqscan;
RESET enable_bitmapscan;

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE saindex_large;
DROP TABLE saindex_test CASCADE;
