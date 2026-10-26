# Asynchronous AI columns

AI columns store model results in ordinary native seekdb columns. Source-row changes and persistent jobs commit in the same transaction. Four background workers evaluate committed jobs through the existing `AI_COMPLETE` and `AI_EMBED` implementations; no model HTTP request runs in the business transaction.

Register the model and endpoint with `DBMS_AI_SERVICE` before using a definition. For an existing completion model named `classification`:

```sql
CREATE TABLE documents (
  id BIGINT PRIMARY KEY,
  content VARCHAR(1000),
  tags JSON AI COLUMN (
    AI_COMPLETE('classification', CONCAT('Classify: ', content))
  ) WITH (sync_mode='async', max_retries=2)
);

INSERT INTO documents(id, content) VALUES (1, 'Example text');
SHOW AI COLUMNS FROM documents;
SELECT id, tags FROM documents;
```

The result is initially SQL NULL. Writes to a dependency or primary key invalidate the affected AI columns and enqueue a new request; unrelated updates preserve completed results. Deleting a source row removes its tasks. Row changes and task changes both roll back with the transaction. Result columns cannot be assigned explicitly by ordinary INSERT or UPDATE statements.

The input must be a deterministic expression over ordinary columns. AI chains, generated-column inputs, subqueries, user variables and nondeterministic functions are rejected. Definitions use a constant model name; completion results support strings, JSON and numeric types. Embedding results require `VECTOR(n)` and a compatible declared dimension. Completion type instructions are sent as a separate system message, preserving the original user input. Options cannot override transport fields such as model, messages, input or stream. `sync_mode` currently accepts `async`; `max_retries` is an integer from 0 through 16, defaulting to 2.

## Publication, failure and retry

Each AI result owns three hidden physical columns: state, request identifier and definition fingerprint. States are PENDING (1), READY (2) and FAILED (3). A successful NULL result, such as `AI_EMBED(model, NULL)`, is READY; NULL alone does not indicate completion. JSON, numeric, vector and string bounds are checked before publication.

Columns invalidated by the same row mutation share one request identifier. Workers persist individual outcomes, retain successful outcomes while siblings retry, then publish the entire group with one native UPDATE transaction. Successful columns become READY and exhausted failures become FAILED with a NULL result. Native UPDATE maintains vector indexes, including removal during invalidation and restoration after rollback.

`max_retries` counts additional model attempts after the initial request. Worker inference disables the HTTP client's independent retry loop, so HTTP 429/5xx use the persistent task budget. Task retries increment `retry_count`, enter state 3, and become eligible after one second. Completion or terminal failure removes the task after publishing its source-row state.

```sql
ALTER TABLE documents RETRY AI COLUMN tags WHERE id IN (1, 3);
ALTER TABLE documents RETRY AI COLUMN tags
  WHERE status = 'FAILED' AND id < 100;
ALTER TABLE documents RETRY AI COLUMN tags WHERE status = 'READY';
```

RETRY affects one AI column and matching rows, and can regenerate READY results. An empty match is a no-op. Its `status` pseudo column names the target AI state; backticks select a physical business column named `status`. Predicates support normal SQL expressions and subqueries. Combining RETRY with another ALTER action is rejected. RETRY uses native transactional DML and participates in explicit commit or rollback.

## Strict reads and DDL

`ai_column_strict_read` defaults to OFF. When enabled, SELECT, UPDATE and DELETE check every AI column referenced by the statement, including subqueries, before executing the plan. If any row of a referenced column is PENDING or FAILED, error 11120 reports its base table and column identifiers. A WHERE clause selecting only READY rows does not bypass this column-wide check. Other columns remain readable. The execution-time check also runs for cached plans and observes uncommitted changes in the caller's transaction.

```sql
SET SESSION ai_column_strict_read = ON;
SELECT tags FROM documents;
```

ALTER ADD backfills existing rows asynchronously. ALTER MODIFY validates and persists a new definition, reuses hidden column identifiers, and regenerates stale results. CREATE TABLE LIKE preserves AI definitions and remaps their dependency and hidden identifiers. Dropping an AI column removes its metadata, tasks and active hidden columns; native instant DROP may retain renamed unused physical columns until reclamation. Removing, renaming or changing the type of an ordinary input column is rejected while an AI definition references it; drop or redefine the dependent AI column first.

## Persistence and recovery

`oceanbase.__all_ai_column` stores normalized definitions, dependency identifiers, result fingerprints, hidden identifiers and retry settings. `oceanbase.__all_ai_column_task` stores row predicates, request identifiers, definition fingerprints, eligibility, leases, retry counts and cached outcomes. Task states are pending (1), running (2), retry wait (3), cached success (4) and terminal failure (5).

The lowest column identifier anchors each request group. Conditional lease acquisition serializes claims, heartbeats renew ownership, and publication checks ownership, the current definition and each source request identifier. Changed or deleted rows cannot be overwritten or recreated by a late response. On standalone server restart, leases from the previous process are released and committed requests resume without waiting for their lease timeout. Inference is at least once: a crash after an upstream response can require another model call. This does not permit an obsolete result to publish.

An inference payload is limited to 16 MiB, a stored definition to 65,535 bytes, a canonical row predicate to 4,096 bytes and a request group to 1,024 columns. This implementation targets the contest's standalone seekdb runtime; the startup lease recovery policy is not a distributed multi-server ownership protocol.

## Validation

The unmodified competition suites are in `tools/deploy/mysql_test/test_suite/ai_column/`: `ai_column_ddl`, `ai_column_t1_dml`, `ai_column_worker`, `ai_column_retry_strict` and `ai_column_vector_index`. They use their bundled local HTTP mock.

Additional native regressions are `unittest/sql/engine/dml/test_ai_column_transactions.py` and `test_ai_column_worker_http.py`. They cover transactional RETRY, strict reads of uncommitted rows, DDL backfill and modification, mixed outcomes, typed boundaries, actual upstream retry counts, deletion races, successful NULL, and an intentional process crash during inference followed by recovery from the same data directory. No real model credentials are needed. Validation results and exact commands are recorded outside the product tree in the workspace's functional test evidence directories.
