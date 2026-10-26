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

"""Real database write failures, replay and scope-isolated support state."""

import asyncio
import sqlite3

import pytest
from sqlalchemy.exc import IntegrityError

from powercontext.artifacts import ArtifactRef
from powercontext.builtin.artifacts.memory import MemoryEntryInput
from powercontext.builtin.persistence.sqlite import SQLiteConfig
from powercontext.builtin.records import ArtifactWrite
from powercontext.builtin.runtime import (
    BuiltinConfig,
    CaptureSource,
    GetMemoryEntryRequest,
    GetTopicMemoryRequest,
    RememberMemoryRequest,
    SearchTopicMemoryRequest,
    open_builtin_runtime,
)
from powercontext.builtin.runtime.source_lifecycle import SourceLifecycleError, SourceLifecycleEvent
from powercontext.builtin.scope import ScopeDraft


@pytest.mark.parametrize("failure_table", ["pc_artifacts", "pc_source_lifecycle"])
def test_failed_persistence_retries_after_restart_without_extra_body_or_state_revision(tmp_path, failure_table) -> None:
    async def run() -> None:
        database = tmp_path / "lifecycle.db"
        config = BuiltinConfig(database=SQLiteConfig(url=f"sqlite+aiosqlite:///{database}"))
        async with open_builtin_runtime(config) as runtime:
            sid = (
                await runtime.scopes.create(ScopeDraft(title="lifecycle", summary="failure", idempotency_key="s"))
            ).scope_id
            source = (
                await runtime.sources.for_scope(sid).capture(
                    CaptureSource(source_id="s", content="source", metadata={})
                )
            ).source_ref
            citation = (
                await runtime.memory.for_scope(sid).remember(
                    RememberMemoryRequest(entries=(MemoryEntryInput(kind="fact", text="Recoverable evidence."),))
                )
            ).entry.citation
            topic = await runtime.records.for_scope(sid).create_artifact(
                "topic-memory",
                ArtifactWrite(
                    content={"title": "Recoverable", "summary": "Recoverable topic evidence.", "detail": "Full detail"}
                ),
            )
            reference = ArtifactRef(family="topic-memory", artifact_id=topic.artifact_id, revision=topic.revision)
            lifecycle = runtime.lifecycle.for_scope(sid)
            await lifecycle.register_source("source", source, revision=7)
            await lifecycle.register_memory("memory", citation, parents=("source",))
            await lifecycle.register_topic("topic", reference, parents=("memory",))
            with sqlite3.connect(database) as connection:
                if failure_table == "pc_artifacts":
                    connection.execute(
                        "CREATE TRIGGER fail_state BEFORE INSERT ON pc_artifacts WHEN NEW.family='memory' "
                        "BEGIN SELECT RAISE(ABORT, 'controlled lifecycle failure'); END"
                    )
                else:
                    connection.execute(
                        "CREATE TRIGGER fail_state BEFORE UPDATE ON pc_source_lifecycle "
                        "BEGIN SELECT RAISE(ABORT, 'controlled lifecycle failure'); END"
                    )
            event = SourceLifecycleEvent(node_id="source", revision=7, sequence=1, available=False, reason="withdrawn")
            with pytest.raises(IntegrityError):
                await lifecycle.apply(event)
            assert await lifecycle.resolve("source", purpose="generation") == source
            failed_head = (await runtime.memory.for_scope(sid).list(include_inactive=True)).memory_ref
        with sqlite3.connect(database) as connection:
            connection.execute("DROP TRIGGER fail_state")
        async with open_builtin_runtime(config) as runtime:
            lifecycle = runtime.lifecycle.for_scope(sid)
            assert await lifecycle.apply(event) == []
            listed = await runtime.memory.for_scope(sid).list(include_inactive=True)
            assert listed.entries[0].state == "inactive"
            assert listed.entries[0].entry.entry_version_id == citation.entry_version_id
            if failure_table == "pc_source_lifecycle":
                assert listed.memory_ref == failed_head
            assert await lifecycle.apply(event) == []
            assert (await runtime.memory.for_scope(sid).list(include_inactive=True)).memory_ref == listed.memory_ref
            assert (
                await runtime.memory.for_scope(sid).get(GetMemoryEntryRequest(citation=citation))
            ).entry.text == "Recoverable evidence."
            assert not (
                await runtime.topic_memory.for_scope(sid).search(SearchTopicMemoryRequest(query="Recoverable"))
            ).hits
            assert (
                await runtime.topic_memory.for_scope(sid).get(GetTopicMemoryRequest(artifact=reference))
            ).topic.as_ref() == reference
            assert await runtime.topic_memory.for_scope(sid).browse(limit=10) == ()
            with pytest.raises(SourceLifecycleError):
                await lifecycle.resolve("memory", purpose="generation")
            await lifecycle.apply(
                SourceLifecycleEvent(node_id="source", revision=7, sequence=2, available=True, reason="restored")
            )
            assert await lifecycle.resolve("memory") == citation
            assert (
                await runtime.topic_memory.for_scope(sid).search(SearchTopicMemoryRequest(query="Recoverable"))
            ).hits

    asyncio.run(run())


def test_long_unicode_node_ids_and_scope_state_remain_independent() -> None:
    async def run() -> None:
        async with open_builtin_runtime(BuiltinConfig()) as runtime:
            services = []
            node_id = "来源" * 300
            for key in ("left", "right"):
                sid = (await runtime.scopes.create(ScopeDraft(title=key, summary=key, idempotency_key=key))).scope_id
                ref = (
                    await runtime.sources.for_scope(sid).capture(
                        CaptureSource(source_id="s", content="evidence", metadata={})
                    )
                ).source_ref
                service = runtime.lifecycle.for_scope(sid)
                await service.register_source(node_id, ref, revision=1)
                await service.register_source(node_id, ref, revision=1)
                services.append((service, ref))
            await services[0][0].apply(
                SourceLifecycleEvent(node_id=node_id, revision=1, sequence=1, available=False, reason="withdrawn")
            )
            with pytest.raises(SourceLifecycleError):
                await services[0][0].resolve(node_id)
            assert await services[1][0].resolve(node_id) == services[1][1]

    asyncio.run(run())
