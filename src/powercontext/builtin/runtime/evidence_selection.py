# Copyright (c) 2026 OceanBase.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Query-weighted literal evidence selection, including full output byte costs."""

from __future__ import annotations

import json
import math
import re
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING

from powercontext.artifacts import ArtifactAddress
from powercontext.builtin.runtime.errors import PreparedContextInvariantError
from powercontext.builtin.runtime.models import PrepareContextRequest, PreparedContext
from powercontext.builtin.runtime.prepared_text import TRUST_POLICY

if TYPE_CHECKING:
    from powercontext.builtin.runtime.prepared_context import (
        MemoryEntryAddress,
        PreparedContextBuild,
        PreparedMemoryCandidates,
    )

_STOP_WORDS = frozenset([
    "a",
    "an",
    "the",
    "and",
    "or",
    "to",
    "of",
    "in",
    "on",
    "at",
    "by",
    "is",
    "are",
    "was",
    "were",
    "be",
    "been",
    "with",
    "as",
    "for",
    "from",
    "that",
    "this",
    "it",
    "its",
    "which",
    "what",
    "how",
    "do",
    "does",
    "did",
    "has",
    "have",
    "had",
])


@dataclass(frozen=True)
class MemoryEvidenceFragment:
    """An exact Python character interval in one scoped Memory entry version."""

    address: MemoryEntryAddress
    start: int
    end: int
    text: str


def _terms(text: str) -> Counter[str]:
    words = re.findall(r"[a-z]+|\d+|[\u3400-\u9fff]+", text.lower())
    return Counter(
        word[:-1] if len(word) > 4 and word.endswith("s") and not word.endswith("ss") else word
        for word in words
        if word not in _STOP_WORDS
    )


def _intervals(text: str) -> tuple[tuple[int, int], ...]:
    spans: list[tuple[int, int]] = []
    for match in re.finditer(r"\S.*?(?:[.!?。！？](?=\s|$)|\n|$)", text, flags=re.DOTALL):  # noqa: RUF001 - Unicode punctuation
        start, end = match.span()
        while end > start and text[end - 1].isspace():
            end -= 1
        if end <= start:
            continue
        spans.append((start, end))
        # A long unpunctuated paragraph can still contribute a fitting, literal
        # window; keep the full interval available when its budget permits it.
        if end - start > 768:
            spans.extend((offset, min(offset + 384, end)) for offset in range(start, end, 256))
    return tuple(spans)


def _render(fragments: Sequence[MemoryEvidenceFragment]) -> str:
    items = []
    for fragment in fragments:
        address = fragment.address
        items.append({
            "citation": {
                "scope_id": address.memory.scope_id,
                "memory_ref": address.memory.artifact.model_dump(mode="json"),
                "entry_id": address.entry_id,
                "entry_version_id": address.entry_version_id,
            },
            "content": fragment.text,
            "start": fragment.start,
            "end": fragment.end,
        })
    envelope = json.dumps({"trust": "untrusted_history", "items": items}, ensure_ascii=False, separators=(",", ":"))
    return f"{TRUST_POLICY}\n\nBEGIN_POWERCONTEXT_PREPARED_CONTEXT_V1\n{envelope}\nEND_POWERCONTEXT_PREPARED_CONTEXT_V1"


def _merge(
    chosen: Sequence[MemoryEvidenceFragment], candidate: MemoryEvidenceFragment, original: str
) -> tuple[MemoryEvidenceFragment, ...]:
    retained: list[MemoryEvidenceFragment] = []
    start, end = candidate.start, candidate.end
    insertion = len(chosen)
    for index, fragment in enumerate(chosen):
        gap_start, gap_end = min(end, fragment.end), max(start, fragment.start)
        if fragment.address == candidate.address and (gap_start >= gap_end or not original[gap_start:gap_end].strip()):
            start, end = min(start, fragment.start), max(end, fragment.end)
            insertion = min(insertion, index)
        else:
            retained.append(fragment)
    merged = MemoryEvidenceFragment(candidate.address, start, end, original[start:end])
    retained.insert(min(insertion, len(retained)), merged)
    return tuple(retained)


def _literal_entries(
    current_scope_id: str | None,
    groups: Sequence[PreparedMemoryCandidates],
) -> list[tuple[MemoryEntryAddress, str]]:
    from powercontext.builtin.runtime.prepared_context import MemoryEntryAddress

    entries: list[tuple[MemoryEntryAddress, str]] = []
    seen: set[tuple[str, str, int, str, str]] = set()
    for group in groups:
        scope = group.scope_id or current_scope_id
        for hit in group.hits:
            if group.memory_ref is None or hit.memory_ref != group.memory_ref:
                raise PreparedContextInvariantError("memory-ref-mismatch")
            if scope is None:
                raise PreparedContextInvariantError("text-scope-missing")
            identity = (scope, hit.memory_ref.artifact_id, hit.memory_ref.revision, hit.entry_id, hit.entry_version_id)
            if identity in seen or not hit.text.strip():
                continue
            seen.add(identity)
            entries.append((
                MemoryEntryAddress(
                    ArtifactAddress(scope_id=scope, artifact=hit.memory_ref), hit.entry_id, hit.entry_version_id
                ),
                hit.text,
            ))
    return entries


def build_evidence_context(
    request: PrepareContextRequest,
    current_scope_id: str | None,
    groups: Sequence[PreparedMemoryCandidates],
) -> PreparedContextBuild:
    """Select sentences using local lexical statistics, never external labels."""

    from powercontext.builtin.runtime.prepared_context import PreparedContextBuild

    entries = _literal_entries(current_scope_id, groups)
    query = _terms(request.query)
    documents = [_terms(text) for _address, text in entries]
    frequencies = Counter(term for document in documents for term in document)
    weights = {
        term: math.log1p((len(documents) - frequencies[term] + 0.5) / (frequencies[term] + 0.5)) for term in query
    }
    spans = [(address, text, start, end) for address, text in entries for start, end in _intervals(text)]
    mean_length = sum(sum(_terms(text[start:end]).values()) for _address, text, start, end in spans) / max(
        1, len(spans)
    )
    ranked = []
    for order, (address, text, start, end) in enumerate(spans):
        terms = _terms(text[start:end])
        length_norm = 1.2 * (0.25 + 0.75 * sum(terms.values()) / max(1, mean_length))
        score = sum(
            weight * terms[term] * 2.2 / (terms[term] + length_norm)
            for term, weight in weights.items()
            if term in terms
        )
        if score > 0:
            ranked.append((score, order, MemoryEvidenceFragment(address, start, end, text[start:end]), text))
    chosen: tuple[MemoryEvidenceFragment, ...] = ()
    content: str | None = None
    duplicates: list[tuple[MemoryEntryAddress, str]] = []
    for _score, _order, fragment, original in sorted(ranked, key=lambda row: (-row[0], row[1])):
        if (fragment.address, fragment.text) in duplicates:
            continue
        proposal = _merge(chosen, fragment, original)
        rendered = _render(proposal)
        if len(rendered.encode("utf-8")) <= request.max_bytes:
            chosen, content = proposal, rendered
            duplicates.append((fragment.address, fragment.text))
    if content is None:
        return PreparedContextBuild(context=PreparedContext(status="empty", content=None, content_bytes=0), origins=())
    return PreparedContextBuild(
        context=PreparedContext(status="ready", content=content, content_bytes=len(content.encode("utf-8"))),
        origins=tuple(fragment.address for fragment in chosen),
        fragments=chosen,
    )


__all__ = ["MemoryEvidenceFragment", "build_evidence_context"]
