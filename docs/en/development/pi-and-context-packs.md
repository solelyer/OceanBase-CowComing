# Pi history and context packs

`parse_pi_session(PiSessionInput(content=archive_bytes, leaf_id=None))` validates a complete UTF-8 Pi v3 JSONL archive. It returns visible events on the selected leaf's full ancestor chain, in root-to-leaf order; the default leaf is the final node. Control nodes connect the graph without creating text evidence. Compaction and branch summaries preserve the events that preceded them. Text, tool calls, complete tool results, bash executions and custom messages retain their original records. Thinking and image blocks do not produce text.

`PI_EVENT_SOURCE_DEFINITION` resolves these events into captured Sources with the standard text-evidence projection. Identities derive only from the session and event IDs. Use `PiSessionConnector(path, leaf_id=None)` with the existing `ConnectorLifecycle` and a persistent checkpoint store. The connector imports unseen selected events and verifies previously imported records throughout the archive, including other branches. Failed runs leave the committed checkpoint unchanged; a retry can resubmit partially accepted events through the ingestion API's existing idempotency checks. A changed session, removed event or rewritten imported record fails explicitly.

The Python scoped Context service supports `await context.prepare_incremental(request, base_pack=None)`. It first uses ordinary `prepare`, preserving its content, references, scope authorization and legal empty result. Without a base it returns a `ContextPack`; with one it returns `ContextPackDelta`. This method does not add an HTTP endpoint.

```python
from powercontext.builtin.runtime.context_pack import ContextPack, ContextPackDelta, apply_context_pack_delta

base = ContextPack.from_content("Earlier evidence 海洋", chunk_bytes=512)
target = ContextPack.from_content("Earlier evidence 海洋\nNew evidence", chunk_bytes=512)
wire = ContextPackDelta.between(base, target).to_bytes()
received = apply_context_pack_delta(ContextPack.from_bytes(base.to_bytes()), ContextPackDelta.from_bytes(wire))
assert b"".join(received.items).decode("utf-8") == "Earlier evidence 海洋\nNew evidence"
```

Packs use sorted compact UTF-8 JSON, canonical standard Base64 chunks and a SHA-256 digest over schema and items. A pack's `byte_count` includes its own field and equals the length of its canonical wire encoding. Deltas contain the base and target digests, a nonnegative prefix length and appended chunks; the receiver validates all three before returning the target. Invalid UTF-8, encoding, fields, sizes or identities raise `ContextPackError`. Each chunk is valid UTF-8 and respects the requested byte limit; an impossible boundary fails instead of splitting a character. Empty content becomes an empty pack.

The prefix protocol benefits repeated contexts. Measure its actual first-pack and delta bytes against the original complete body, including protocol and Base64 overhead. Changing an early chunk requires retransmitting the following suffix, so savings depend on the real request sequence.
