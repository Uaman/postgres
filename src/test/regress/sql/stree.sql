--
-- Test suffix tree index
--

-- Create a test table
CREATE TABLE stree_test (
    id serial PRIMARY KEY,
    content text
);

-- Insert test data
INSERT INTO stree_test (content) VALUES
    ('hello world'),
    ('hello there'),
    ('world peace'),
    ('peaceful world'),
    ('the quick brown fox'),
    ('fox jumps over the lazy dog'),
    ('suffix tree test'),
    ('testing suffix trees'),
    ('substring search'),
    ('search for substrings');

-- Create suffix tree index
CREATE INDEX stree_test_idx ON stree_test USING stree (content);

-- Verify index exists
SELECT indexname, indexdef FROM pg_indexes WHERE tablename = 'stree_test';

-- Test substring search with LIKE
SET enable_seqscan = OFF;
SET enable_indexscan = ON;

-- Search for 'world' - should find rows 1, 3, 4
EXPLAIN (COSTS OFF) SELECT * FROM stree_test WHERE content LIKE '%world%';
SELECT * FROM stree_test WHERE content LIKE '%world%' ORDER BY id;

-- Search for 'hello' - should find rows 1, 2
EXPLAIN (COSTS OFF) SELECT * FROM stree_test WHERE content LIKE '%hello%';
SELECT * FROM stree_test WHERE content LIKE '%hello%' ORDER BY id;

-- Search for 'fox' - should find rows 5, 6
EXPLAIN (COSTS OFF) SELECT * FROM stree_test WHERE content LIKE '%fox%';
SELECT * FROM stree_test WHERE content LIKE '%fox%' ORDER BY id;

-- Search for 'suffix' - should find rows 7, 8
EXPLAIN (COSTS OFF) SELECT * FROM stree_test WHERE content LIKE '%suffix%';
SELECT * FROM stree_test WHERE content LIKE '%suffix%' ORDER BY id;

-- Search for 'substring' - should find rows 9, 10
EXPLAIN (COSTS OFF) SELECT * FROM stree_test WHERE content LIKE '%substring%';
SELECT * FROM stree_test WHERE content LIKE '%substring%' ORDER BY id;

-- Search for non-existent pattern
EXPLAIN (COSTS OFF) SELECT * FROM stree_test WHERE content LIKE '%zzzzz%';
SELECT * FROM stree_test WHERE content LIKE '%zzzzz%' ORDER BY id;

-- Reset settings
RESET enable_seqscan;
RESET enable_indexscan;

-- Test with UTF-8 characters
CREATE TABLE stree_utf8_test (
    id serial PRIMARY KEY,
    content text
);

INSERT INTO stree_utf8_test (content) VALUES
    ('привіт світ'),     -- Ukrainian: hello world
    ('привіт друг'),      -- Ukrainian: hello friend
    ('світовий мир'),     -- Ukrainian: world peace
    ('こんにちは世界'),    -- Japanese: hello world
    ('世界平和'),          -- Japanese: world peace
    ('你好世界');          -- Chinese: hello world

CREATE INDEX stree_utf8_idx ON stree_utf8_test USING stree (content);

SET enable_seqscan = OFF;
SET enable_indexscan = ON;

-- Search for Ukrainian 'світ' (world)
SELECT * FROM stree_utf8_test WHERE content LIKE '%світ%' ORDER BY id;

-- Search for Japanese '世界' (world)
SELECT * FROM stree_utf8_test WHERE content LIKE '%世界%' ORDER BY id;

RESET enable_seqscan;
RESET enable_indexscan;

-- Cleanup
DROP TABLE stree_test;
DROP TABLE stree_utf8_test;

--
-- Comprehensive edge case tests
--

-- Test table with various string patterns
CREATE TABLE stree_edge_cases (
    id serial PRIMARY KEY,
    t text
);

INSERT INTO stree_edge_cases (t) VALUES
    -- Basic strings
    ('testing'),
    ('best'),
    ('test123'),
    ('hello'),
    ('world'),
    -- Repeated characters
    ('aaa'),
    ('ababab'),
    -- Overlapping patterns
    ('abcabc'),
    ('mississippi'),
    -- Numbers and mixed
    ('abc123xyz'),
    ('test test'),
    -- Long strings
    ('abcdefghijklmnopqrstuvwxyz'),
    -- Unicode
    ('café'),
    -- Prefix/suffix patterns
    ('prefix'),
    ('suffix'),
    ('prefixsuffix');

CREATE INDEX stree_edge_idx ON stree_edge_cases USING stree (t);

SET enable_seqscan = OFF;
SET enable_indexscan = ON;

-- Test 1: Single character patterns
SELECT '=== Test 1: Single character patterns ===' as test;
SELECT '%t%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%t%';

SELECT '%a%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%a%';

SELECT '%o%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%o%';

-- Test 2: Multi-character patterns
SELECT '=== Test 2: Multi-character patterns ===' as test;
SELECT '%est%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%est%';

SELECT '%abc%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%abc%';

SELECT '%iss%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%iss%';

SELECT '%fix%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%fix%';

-- Test 3: Repeated character patterns
SELECT '=== Test 3: Repeated patterns ===' as test;
SELECT '%aa%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%aa%';

SELECT '%ab%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%ab%';

-- Test 4: Unicode patterns
SELECT '=== Test 4: Unicode patterns ===' as test;
SELECT '%caf%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%caf%';

SELECT '%é%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%é%';

-- Test 5: Long patterns
SELECT '=== Test 5: Long patterns ===' as test;
SELECT '%defgh%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%defgh%';

SELECT '%klmnop%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%klmnop%';

-- Test 6: Prefix/suffix patterns
SELECT '=== Test 6: Prefix/suffix patterns ===' as test;
SELECT '%pre%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%pre%';

SELECT '%suf%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%suf%';

SELECT '%fix%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%fix%';

-- Test 7: Pattern with space
SELECT '=== Test 7: Pattern with space ===' as test;
SELECT '%test %' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%test %';

SELECT '% test%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '% test%';

-- Test 8: Non-existent patterns
SELECT '=== Test 8: Non-existent patterns ===' as test;
SELECT '%xyz123%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%xyz123%';

SELECT '%qqq%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_edge_cases WHERE t LIKE '%qqq%';

-- Test 9: Verify results match sequential scan
SELECT '=== Test 9: Verify against sequential scan ===' as test;

-- Compare index scan vs seq scan for %est%
SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SELECT array_agg(t ORDER BY id) as index_result 
FROM stree_edge_cases WHERE t LIKE '%est%';

SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SELECT array_agg(t ORDER BY id) as seqscan_result 
FROM stree_edge_cases WHERE t LIKE '%est%';

-- Compare for %abc%
SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SELECT array_agg(t ORDER BY id) as index_result 
FROM stree_edge_cases WHERE t LIKE '%abc%';

SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SELECT array_agg(t ORDER BY id) as seqscan_result 
FROM stree_edge_cases WHERE t LIKE '%abc%';

-- Compare for %o%
SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SELECT array_agg(t ORDER BY id) as index_result 
FROM stree_edge_cases WHERE t LIKE '%o%';

SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SELECT array_agg(t ORDER BY id) as seqscan_result 
FROM stree_edge_cases WHERE t LIKE '%o%';

RESET enable_seqscan;
RESET enable_indexscan;

-- Cleanup
DROP TABLE stree_edge_cases;

--
-- Test with empty and special strings
--
CREATE TABLE stree_special (
    id serial PRIMARY KEY,
    t text
);

INSERT INTO stree_special (t) VALUES
    (''),           -- empty string
    (' '),          -- single space
    ('  '),         -- double space
    ('123'),        -- numbers only
    ('!@#$%'),      -- special characters
    ('a b c'),      -- spaces between chars
    ('   leading'), -- leading spaces
    ('trailing   ');-- trailing spaces

CREATE INDEX stree_special_idx ON stree_special USING stree (t);

SET enable_seqscan = OFF;
SET enable_indexscan = ON;

SELECT '=== Test 10: Special strings ===' as test;
SELECT '% %' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_special WHERE t LIKE '% %';

SELECT '%123%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_special WHERE t LIKE '%123%';

SELECT '%lead%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_special WHERE t LIKE '%lead%';

SELECT '%trail%' as pattern, array_agg(t ORDER BY id) as result 
FROM stree_special WHERE t LIKE '%trail%';

RESET enable_seqscan;
RESET enable_indexscan;

-- Cleanup
DROP TABLE stree_special;

--
-- Performance-oriented test with larger dataset
-- Using unique string structures that don't have prefix collision issues
--
CREATE TABLE stree_perf (
    id serial PRIMARY KEY,
    t text
);

-- Insert 50 varied strings with truly distinct patterns
INSERT INTO stree_perf (t)
SELECT 
    CASE (i % 5)
        WHEN 0 THEN 'product_electronics_' || i
        WHEN 1 THEN 'product_clothing_' || i
        WHEN 2 THEN 'product_furniture_' || i
        WHEN 3 THEN 'product_books_' || i
        ELSE 'product_sports_' || i
    END
FROM generate_series(1, 50) i;

CREATE INDEX stree_perf_idx ON stree_perf USING stree (t);

SET enable_seqscan = OFF;
SET enable_indexscan = ON;

SELECT '=== Test 11: Performance test ===' as test;

-- Test patterns - each category has 10 items
SELECT '%electronics%' as pattern, count(*) as match_count 
FROM stree_perf WHERE t LIKE '%electronics%';

SELECT '%clothing%' as pattern, count(*) as match_count 
FROM stree_perf WHERE t LIKE '%clothing%';

SELECT '%furniture%' as pattern, count(*) as match_count 
FROM stree_perf WHERE t LIKE '%furniture%';

SELECT '%product_%' as pattern, count(*) as match_count 
FROM stree_perf WHERE t LIKE '%product_%';

-- Verify counts match sequential scan
SET enable_seqscan = ON;
SET enable_indexscan = OFF;

SELECT '%electronics%' as pattern, count(*) as seqscan_count 
FROM stree_perf WHERE t LIKE '%electronics%';

SELECT '%clothing%' as pattern, count(*) as seqscan_count 
FROM stree_perf WHERE t LIKE '%clothing%';

RESET enable_seqscan;
RESET enable_indexscan;

-- Cleanup
DROP TABLE stree_perf;