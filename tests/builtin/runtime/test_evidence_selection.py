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

"""Literal intervals, complete budgets and distinct scoped citations."""

import json

from powercontext.artifacts import ArtifactRef
from powercontext.builtin.artifacts.memory import MemoryHit
from powercontext.builtin.runtime import PrepareContextRequest
from powercontext.builtin.runtime.prepared_context import PreparedContextBuilder, PreparedMemoryCandidates


def _group(scope: str, text: str) -> PreparedMemoryCandidates:
    ref = ArtifactRef(family="memory", artifact_id="m", revision=1)
    return PreparedMemoryCandidates(
        scope,
        ref,
        (MemoryHit(memory_ref=ref, entry_id="e", entry_version_id="v", text=text, score=1, matched_by=("fts",)),),
    )


def test_equal_texts_in_different_scopes_keep_both_origins() -> None:
    text = "海洋 station measured 19 degrees."
    request = PrepareContextRequest(query="海洋 station", evidence_selection=True, max_bytes=1400)
    built = PreparedContextBuilder().build_scopes_result(
        request=request, current_scope_id="left", memory_candidates=(_group("left", text), _group("right", text))
    )
    assert {fragment.address.memory.scope_id for fragment in built.fragments} == {"left", "right"}
    assert built.origins == tuple(fragment.address for fragment in built.fragments)
    assert built.context.content_bytes == len(built.context.content.encode()) <= request.max_bytes
    items = json.loads(built.context.content.splitlines()[-2])["items"]
    for item, fragment in zip(items, built.fragments, strict=True):
        assert fragment.text == text[fragment.start : fragment.end]
        assert item["citation"]["scope_id"] == fragment.address.memory.scope_id


def test_budget_probe_agrees_with_selection_including_empty_output() -> None:
    builder = PreparedContextBuilder()
    text = "Long unrelated prefix. " * 100 + 'The "cobalt" instrument reports 海洋 data at 37.2 degrees.'
    group = _group("one", text)
    for budget in (512, 1000, 1400):
        request = PrepareContextRequest(query="cobalt instrument 海洋", evidence_selection=True, max_bytes=budget)
        built = builder.build_scopes_result(request=request, current_scope_id="one", memory_candidates=(group,))
        probe = builder.probe_budget(request=request, current_scope_id="one", memory_candidates=(group,))
        assert probe.delivered_items == len(built.fragments)
        assert probe.unused_bytes == budget - built.context.content_bytes
        for fragment in built.fragments:
            assert type(fragment.start) is int and type(fragment.end) is int
            assert fragment.text == text[fragment.start : fragment.end]
    assert built.fragments and any("37.2" in fragment.text for fragment in built.fragments)
