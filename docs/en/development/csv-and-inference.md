# CSV and inference adapters

CSV rows preserve every field as a string, including whitespace, leading zeroes and empty values. The first record defines the ordered columns. Parsing validates the entire document before returning rows; duplicate keys, malformed input and identities over 256 characters fail with `ValueError`.

```python
from powercontext.sources import SourceDefinitionRegistry
from powercontext.sources.csv_rows import CSV_ROW_SOURCE_DEFINITION, CSVDocumentInput, parse_csv_document

rows = parse_csv_document(CSVDocumentInput(
    document_id="airports",
    primary_key="id",
    content=b"id,name\n001,Example Airport\n",
))
registry = SourceDefinitionRegistry((CSV_ROW_SOURCE_DEFINITION,))
# In an async function:
# source = await registry.resolve(rows[0])
```

Register the definition manifest and submit its observations through Runtime ingestion to persist rows. The standard `powercontext.text-evidence/1` projection includes compact row JSON and metadata identifying the document, primary key and ordered columns. Reordering rows or editing a non-key field retains the row identity.

`HTTPMemoryReranker(endpoint, client=client, batch_size=32)` uses a caller-owned `httpx.AsyncClient`. POST bodies contain `query` and `candidates`, each with a global one-based `rank` and `text`. The response must contain only `scores`, an array of objects containing only `rank` and finite numeric `score`. Responses cover their batch exactly once. All batches participate in score-descending selection; ties use ascending original rank. HTTP and transport errors propagate. Pass the adapter as `memory_reranker` when opening built-in contexts or Runtime.

`CoalescingEmbeddingModel(model, max_batch_size=64, coalesce_ms=5.0, cache_size=1024)` exposes the provider's embedding profile and `embed(texts)` capability. Share one wrapper among callers on the same event loop. Duplicate or overlapping inputs share queued and executing work, while results retain original order and repeated positions. Only successful vectors enter the bounded LRU. Setting `cache_size=0` disables completed-result reuse while retaining in-flight sharing. Cancelling one caller leaves shared work running; provider failures affect only callers awaiting that batch and can be retried.

Each caller reports every provider batch it awaited once, including the provider's request and token counts. Pure cache hits report zero requests. Shared batches can appear in multiple callers' usage; summing those usages is not a count of unique provider calls. Pass the wrapper as `embedding_model` to `open_builtin_contexts` or `open_builtin_runtime`.
