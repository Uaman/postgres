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
