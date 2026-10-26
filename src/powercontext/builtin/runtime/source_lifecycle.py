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

"""Durable all/any Source support with native Memory state propagation."""

# ruff: noqa: TRY003 -- Lifecycle errors name the violated registration/event invariant.

from __future__ import annotations

import asyncio
import hashlib
from typing import TYPE_CHECKING, Literal

from pydantic import BaseModel, ConfigDict, Field, StrictBool, StrictInt, StrictStr, field_validator

from powercontext.artifacts import ArtifactRef
from powercontext.builtin.artifacts.memory import MemoryCitation, MemoryEntryVersion
from powercontext.builtin.artifacts.memory.errors import InvalidMemoryCitationError, MemoryEntryNotFoundError
from powercontext.builtin.persistence.database import AsyncDatabase
from powercontext.builtin.persistence.errors import RepositoryNotFoundError
from powercontext.builtin.persistence.source_lifecycle import lifecycle_payloads, store_lifecycle_nodes
from powercontext.builtin.persistence.sources import SourceRepository
from powercontext.builtin.records import BaseValueNotFoundError
from powercontext.builtin.runtime.models import GetMemoryEntryRequest
from powercontext.builtin.sources import validate_scope_id
from powercontext.errors import ArtifactNotFoundError
from powercontext.sources import SourceMaterialization, SourceRef

if TYPE_CHECKING:
    from powercontext.builtin.runtime.application import BuiltinRuntime


class SourceLifecycleError(ValueError):
    """Invalid identity, dependency, lifecycle event or generation use."""


class SourceLifecycleEvent(BaseModel):
    """One strictly typed, revision-bound Source availability transition."""

    model_config = ConfigDict(extra="forbid", frozen=True)
    node_id: StrictStr
    reason: StrictStr
    revision: StrictInt = Field(gt=0)
    sequence: StrictInt = Field(gt=0)
    available: StrictBool

    @field_validator("node_id", "reason")
    @classmethod
    def nonblank(cls, value: str) -> str:
        if not value.strip():
            raise ValueError("lifecycle node_id and reason must not be blank")
        return value


class _Node(BaseModel):
    model_config = ConfigDict(extra="forbid")
    schema_version: Literal[1] = 1
    node_id: str
    kind: Literal["source", "memory", "topic"]
    reference: SourceRef | MemoryCitation | ArtifactRef
    revision: int = 1
    parents: tuple[str, ...] = ()
    policy: Literal["all", "any"] = "all"
    available: bool = True
    last_event: SourceLifecycleEvent | None = None


def _states(nodes: dict[str, _Node]) -> dict[str, bool]:
    states: dict[str, bool] = {}
    visiting: set[str] = set()

    def visit(node_id: str) -> bool:
        if node_id in states:
            return states[node_id]
        if node_id in visiting or node_id not in nodes:
            raise SourceLifecycleError("invalid dependency graph")
        visiting.add(node_id)
        node = nodes[node_id]
        support = [visit(parent) for parent in node.parents]
        available = (
            node.available if node.kind == "source" else (all(support) if node.policy == "all" else any(support))
        )
        visiting.remove(node_id)
        states[node_id] = available
        return available

    for node_id in nodes:
        visit(node_id)
    return states


class SourceLifecycleApplication:
    """Share scope locks while loading authoritative state from durable storage."""

    def __init__(self, runtime: BuiltinRuntime, *, database: AsyncDatabase, sources: SourceRepository) -> None:
        self.runtime, self.database, self.sources = runtime, database, sources
        self._locks: dict[str, asyncio.Lock] = {}

    def for_scope(self, scope_id: str) -> ScopedSourceLifecycle:
        scope = validate_scope_id(scope_id)
        return ScopedSourceLifecycle(self, scope, self._locks.setdefault(scope, asyncio.Lock()))


class ScopedSourceLifecycle:
    def __init__(self, application: SourceLifecycleApplication, scope_id: str, lock: asyncio.Lock) -> None:
        self._app, self.scope_id, self._lock = application, scope_id, lock

    async def _load(self) -> dict[str, _Node]:
        async with self._app.database.transaction() as connection:
            payloads = await lifecycle_payloads(connection, self.scope_id)
        nodes = [_Node.model_validate_json(payload) for payload in payloads]
        return {node.node_id: node for node in nodes}

    async def register_source(self, node_id: str, source_ref: SourceRef, *, revision: int = 1) -> None:
        if type(revision) is not int or revision <= 0:
            raise SourceLifecycleError("revision must be a positive integer")
        async with self._app.database.transaction() as connection:
            try:
                source = await self._app.sources.get(connection, self.scope_id, source_ref)
            except RepositoryNotFoundError as error:
                raise SourceLifecycleError("registered Source must exist") from error
            if source.value.materialization != SourceMaterialization.CAPTURED:
                raise SourceLifecycleError("registered Source must be captured")
        await self._register(_Node(node_id=node_id, kind="source", reference=source_ref, revision=revision))

    async def register_memory(
        self, node_id: str, citation: MemoryCitation, *, parents: tuple[str, ...], policy: Literal["all", "any"] = "all"
    ) -> None:
        try:
            await self._app.runtime.memory.for_scope(self.scope_id).get(GetMemoryEntryRequest(citation=citation))
        except (ArtifactNotFoundError, InvalidMemoryCitationError, MemoryEntryNotFoundError) as error:
            raise SourceLifecycleError("registered Memory citation must exist and be readable") from error
        await self._register(_Node(node_id=node_id, kind="memory", reference=citation, parents=parents, policy=policy))

    async def register_topic(
        self,
        node_id: str,
        artifact_ref: ArtifactRef,
        *,
        parents: tuple[str, ...],
        policy: Literal["all", "any"] = "all",
    ) -> None:
        if artifact_ref.family != "topic-memory":
            raise SourceLifecycleError("registered Topic must have family topic-memory")
        try:
            await self._app.runtime.records.for_scope(self.scope_id).get_artifact_revision(
                artifact_ref.family, artifact_ref.artifact_id, artifact_ref.revision
            )
        except (ArtifactNotFoundError, BaseValueNotFoundError) as error:
            raise SourceLifecycleError("registered Topic revision must exist and be readable") from error
        await self._register(
            _Node(node_id=node_id, kind="topic", reference=artifact_ref, parents=parents, policy=policy)
        )

    async def _register(self, node: _Node) -> None:
        if not node.node_id.strip():
            raise SourceLifecycleError("node_id must not be blank")
        if len(set(node.parents)) != len(node.parents):
            raise SourceLifecycleError("parents must not repeat")
        node.parents = tuple(sorted(node.parents))
        async with self._lock:
            nodes = await self._load()
            existing = nodes.get(node.node_id)
            if existing is not None:
                fields = ("kind", "reference", "revision", "parents", "policy")
                if any(getattr(existing, field) != getattr(node, field) for field in fields):
                    raise SourceLifecycleError("node_id is already bound differently")
                return
            if node.kind != "source" and (not node.parents or any(parent not in nodes for parent in node.parents)):
                raise SourceLifecycleError("derived nodes require existing, distinct parents")
            nodes[node.node_id] = node
            states = _states(nodes)
            await self._synchronize(nodes, states)

    async def apply(self, event: SourceLifecycleEvent) -> list[str]:
        async with self._lock:
            nodes = await self._load()
            source = nodes.get(event.node_id)
            if source is None or source.kind != "source" or event.revision != source.revision:
                raise SourceLifecycleError("event must match a registered Source revision")
            if source.last_event is not None and event.sequence <= source.last_event.sequence:
                if event == source.last_event:
                    return sorted(node_id for node_id, available in _states(nodes).items() if available)
                raise SourceLifecycleError("event sequence is stale or conflicts with its previous contents")
            source.available, source.last_event = event.available, event
            states = _states(nodes)
            await self._synchronize(nodes, states)
            return sorted(node_id for node_id, available in states.items() if available)

    async def _sync_memory(self, nodes: dict[str, _Node], states: dict[str, bool]) -> None:
        runtime = self._app.runtime
        async with runtime._context(self.scope_id) as context, runtime._locked(self.scope_id):
            service = context.artifacts.memory
            memories: dict[str, list[_Node]] = {}
            for node in nodes.values():
                if isinstance(node.reference, MemoryCitation):
                    memories.setdefault(node.reference.memory_ref.artifact_id, []).append(node)
            for artifact_id, bound in memories.items():
                current = await service.head(artifact_id)
                entries = {entry.entry_version_id: entry for entry in await service.entries(current)}
                active = {item.entry_version_id for item in current.content.manifest.entries if item.state == "active"}
                retire: list[MemoryEntryVersion] = []
                restore: list[MemoryEntryVersion] = []
                for node in bound:
                    reference = node.reference
                    if not isinstance(reference, MemoryCitation) or reference.entry_version_id not in entries:
                        raise SourceLifecycleError("registered Memory body changed")
                    entry = entries[reference.entry_version_id]
                    if states[node.node_id] and entry.entry_version_id not in active:
                        restore.append(entry)
                    elif not states[node.node_id] and entry.entry_version_id in active:
                        retire.append(entry)
                if retire:
                    current = await service.forget(current, entries=retire, reason="Source support withdrawn")
                if restore:
                    await service.reactivate(current, entries=restore, reason="Source support restored")

    async def _synchronize(self, nodes: dict[str, _Node], states: dict[str, bool]) -> None:
        # Publish event/Topic state only after native Memory writes succeed. If
        # interrupted, retry from the previous event and skip already-set states.
        await self._sync_memory(nodes, states)
        rows = []
        for node in nodes.values():
            node.available = states[node.node_id]
            reference = node.reference
            artifact_id = (
                reference.artifact_id
                if isinstance(reference, ArtifactRef)
                else (reference.memory_ref.artifact_id if isinstance(reference, MemoryCitation) else None)
            )
            rows.append({
                "scope_id": self.scope_id,
                "node_id": hashlib.sha256(node.node_id.encode("utf-8")).hexdigest(),
                "node_kind": node.kind,
                "artifact_id": artifact_id,
                "available": node.available,
                "payload": node.model_dump_json().encode("utf-8"),
            })
        async with self._app.database.transaction() as connection:
            await store_lifecycle_nodes(connection, rows)

    async def resolve(
        self, node_id: str, *, purpose: Literal["generation", "audit"] = "generation"
    ) -> SourceRef | MemoryCitation | ArtifactRef:
        if purpose not in ("generation", "audit"):
            raise SourceLifecycleError("unknown lifecycle purpose")
        async with self._lock:
            nodes = await self._load()
            if node_id not in nodes or (purpose == "generation" and not _states(nodes)[node_id]):
                raise SourceLifecycleError("node is unknown or unavailable for generation")
            return nodes[node_id].reference


__all__ = ["SourceLifecycleApplication", "SourceLifecycleError", "SourceLifecycleEvent"]
