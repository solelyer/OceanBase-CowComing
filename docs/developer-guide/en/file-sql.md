# Native local File SQL

File SQL exposes one local file as a seekdb table without creating or importing
a database table. The resolver infers its schema; ordinary seekdb operators
perform expressions, filtering, grouping, sorting and joins. No external SQL
engine or Python package participates in query execution.

```sql
SET SESSION enable_file_sql = ON;
SELECT category, COUNT(*), AVG(score)
FROM read_csv('/data/allowed/items.csv', header=true, sample_size=-1) AS items
GROUP BY category HAVING COUNT(*) > 1 ORDER BY category;
DESCRIBE SELECT * FROM read_ndjson('/data/allowed/items.jsonl');
SELECT file FROM glob('/data/allowed/*.parquet') ORDER BY file;
COPY (SELECT id, name FROM read_parquet('/data/allowed/items.parquet'))
TO '/data/allowed/export.csv' (format csv, header, nullstr 'NULL');
```

The administrator must configure a nonempty `secure_file_priv` directory using
the existing local administrator connection. File SQL defaults to OFF. Both
input and output paths must be inside that directory after canonicalization;
`secure_file_priv=NULL` or an empty value disables file access. Input is opened
read only and checked for changes during inference, execution and rescan.
File queries are resolved afresh instead of entering the plan cache.

`read_csv` accepts `header` (default true), `delim` / `delimiter`, `quote`,
`escape`, `nullstr`, and `sample_size=-1`. Delimiter, quote and escape are one
byte. UTF-8, BOM, LF, CRLF, doubled/custom escaped quotes and multiline quoted
fields are supported. Unquoted empty fields and `nullstr` represent SQL NULL;
quoted empty strings remain non-NULL. Duplicate header names receive `__2`,
`__3`, etc. Without a header, columns are named `column0`, `column1`, etc.

`read_json` and `read_ndjson` read flat objects, one per line. `read_json` also
accepts `format='newline_delimited'`. Column names are the union of object keys;
missing keys become NULL. Nested objects, arrays and invalid JSON are errors.
CSV and JSONL scan the complete file to infer BIGINT, DOUBLE, BOOLEAN, VARCHAR,
DATE, DATETIME and NULL. Integer/double and date/datetime pairs widen; other
conflicts become VARCHAR. Numeric overflow and invalid calendar dates remain
text. A positive sample size is rejected.

`read_parquet` reads the footer, flat required/optional basic columns, multiple
row groups, V1/V2 pages, PLAIN and dictionary encodings, hybrid RLE levels and
boolean RLE. Uncompressed, Snappy, GZIP and ZSTD pages are supported. Projected
columns have independent page streams; an unreferenced column's data is not
decompressed. Unsupported nested/repeated schemas, decimal annotations,
external column chunks or other value encodings fail explicitly.

Readers retain one record/page per projected column, never the whole file.
CSV/JSONL limits are 1,024 columns, 16 MiB per field and 64 MiB per record.
Parquet limits include a 16 MiB footer, 64 MiB compressed/uncompressed pages,
bounded metadata depth/node count and a 256 MiB active page/dictionary budget.
The table operator supports both scalar and batched reads and rewind.

`COPY table TO path` and `COPY (SELECT ...) TO path` consume the native query
pipeline. CSV options are `format csv`, `header [true|false]`, `delimiter`,
`quote`, `escape` and `nullstr`; options can use `=`. The default includes a
header. NULL text must not contain a CSV delimiter, quote or line separator.
COPY writes a private temporary file and publishes it with an atomic
no-replacement link after query completion. Existing targets are errors.
Runtime errors remove the temporary file and leave the target absent.

The native SQL regression is
`unittest/sql/engine/basic/test_file_sql_native.py`. Its independent Arrow
fixtures are generated only by the development test environment. The product
has no Arrow dependency. Run through the isolated workspace launcher described
in the competition workspace's `SEEKDB-FUNCTIONAL-STATUS.md`.
