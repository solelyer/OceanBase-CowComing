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

"""Capture original Pi v3 branch events with durable, verified checkpoints."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import cast

from pydantic import BaseModel, JsonValue

from powercontext.sources.connectors import ConnectorRunCompletion, ConnectorRunSession, ConnectorRunStatus
from powercontext.sources.definitions import AdapterSourceDefinition
from powercontext.sources.models import Source, SourceMaterialization
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY, TextEvidence


class PiSessionInput(BaseModel):
    """A complete UTF-8 JSONL archive and optional branch leaf."""

    content: bytes
    leaf_id: str | None = None


class PiEventInput(BaseModel):
    """Original visible content and the complete event record that produced it."""

    session_id: str
    event_id: str
    parent_id: str | None
    event_type: str
    role: str
    content: str
    record: dict[str, JsonValue]


def _canonical(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def _event_id(session_id: str, event_id: str) -> str:
    # Framed components avoid delimiter ambiguity and bound arbitrarily long IDs.
    return "pi:" + hashlib.sha256(_canonical([session_id, event_id])).hexdigest()


def _archive(value: PiSessionInput) -> tuple[str, dict[str, dict[str, JsonValue]], str | None]:
    records = [json.loads(line) for line in value.content.decode("utf-8-sig").splitlines() if line.strip()]
    if not records or not isinstance(records[0], dict):
        raise ValueError("Pi archive must begin with a session header")  # noqa: TRY003
    header = records[0]
    if (
        header.get("type") != "session"
        or type(header.get("version")) is not int
        or header["version"] != 3
        or not isinstance(header.get("id"), str)
        or not header["id"]
    ):
        raise ValueError("Pi archive requires a version-3 session and nonempty ID")  # noqa: TRY003
    nodes: dict[str, dict[str, JsonValue]] = {}
    last = None
    for record in records[1:]:
        if not isinstance(record, dict):
            raise ValueError("Pi event must be an object")  # noqa: TRY003, TRY004 - archive errors use ValueError
        node_id = record.get("id")
        if not isinstance(record.get("type"), str) or not record["type"]:
            raise ValueError("Pi events require a type")  # noqa: TRY003
        if not isinstance(node_id, str) or not node_id or node_id in nodes or "parentId" not in record:
            raise ValueError("Pi events require unique IDs and parentId")  # noqa: TRY003
        parent = record["parentId"]
        if parent is not None and (not isinstance(parent, str) or parent not in nodes):
            raise ValueError("Pi parent must refer to an earlier event")  # noqa: TRY003
        # Reject non-JSON numeric values before any selected event is returned.
        _canonical(record)
        nodes[node_id] = record
        last = node_id
    leaf = value.leaf_id if value.leaf_id is not None else last
    if leaf is not None and leaf not in nodes:
        raise ValueError("Pi leaf is missing from the archive")  # noqa: TRY003
    return header["id"], nodes, leaf


def _visible_blocks(content: JsonValue, *, tool_calls: bool = False) -> str:
    if isinstance(content, str):
        return content
    blocks: list[str] = []
    if not isinstance(content, list):
        return ""
    for block in content:
        if not isinstance(block, dict):
            continue
        text = block.get("text")
        if block.get("type") == "text" and isinstance(text, str):
            blocks.append(text)
        elif tool_calls and block.get("type") == "toolCall":
            name, call_id = block.get("name"), block.get("id")
            if not isinstance(name, str) or not name or not isinstance(call_id, str) or not call_id:
                raise ValueError("Pi tool calls require name and ID")  # noqa: TRY003
            arguments = _canonical(block.get("arguments", {})).decode("utf-8")
            blocks.append(f"[tool_call {name} {call_id}] {arguments}")
    return "\n".join(blocks)


def _message_body(message: JsonValue) -> tuple[str, str]:
    if not isinstance(message, dict):
        raise ValueError("Pi messages require a role")  # noqa: TRY003, TRY004 - archive errors use ValueError
    role = message.get("role")
    if not isinstance(role, str) or not role:
        raise ValueError("Pi messages require a role")  # noqa: TRY003
    body = _visible_blocks(message.get("content"), tool_calls=True)
    if role == "toolResult" and body:
        body = f"[tool_result {message.get('toolName', '')} {message.get('toolCallId', '')}] {body}"
    elif role == "bashExecution":
        command, output = message.get("command", ""), message.get("output", "")
        if not isinstance(command, str) or not isinstance(output, str):
            raise ValueError("Pi bash command and output must be strings")  # noqa: TRY003
        body = f"$ {command}\n{output}" if command or output else ""
    return role, body


def _visible_event(session_id: str, record: dict[str, JsonValue]) -> PiEventInput | None:
    event_type = cast(str, record.get("type"))
    if event_type == "message":
        role, body = _message_body(record.get("message"))
    elif event_type in ("compaction", "branch_summary"):
        role = event_type
        body = record.get("summary", "")
        if not isinstance(body, str):
            raise ValueError("Pi summary must be text")  # noqa: TRY003
    elif event_type == "custom_message":
        role = "custom"
        body = _visible_blocks(record.get("content"))
    else:
        return None
    if not body.strip():
        return None
    return PiEventInput(
        session_id=session_id,
        event_id=cast(str, record["id"]),
        parent_id=cast(str | None, record["parentId"]),
        event_type=event_type,
        role=role,
        content=f"{role}: {body}",
        record=record,
    )


def _branch_events(
    session_id: str, nodes: dict[str, dict[str, JsonValue]], leaf: str | None
) -> tuple[PiEventInput, ...]:
    branch: list[dict[str, JsonValue]] = []
    while leaf is not None:
        record = nodes[leaf]
        branch.append(record)
        leaf = cast(str | None, record["parentId"])
    events = [_visible_event(session_id, record) for record in reversed(branch)]
    return tuple(event for event in events if event is not None)


def parse_pi_session(value: PiSessionInput) -> tuple[PiEventInput, ...]:
    """Validate every graph record, then expose the selected full ancestor chain."""

    session_id, nodes, leaf = _archive(value)
    return _branch_events(session_id, nodes, leaf)


class PiEventSource(Source):
    """Captured event including its original structured record."""

    event: PiEventInput


class _PiEventAdapter:
    name = "pi-event"
    input_class: type[PiEventInput] = PiEventInput
    source_class: type[PiEventSource] = PiEventSource

    async def resolve(self, value: PiEventInput, /) -> PiEventSource:
        return PiEventSource(
            name=_event_id(value.session_id, value.event_id),
            materialization=SourceMaterialization.CAPTURED,
            event=value,
        )

    async def read(self, source: PiEventSource, /) -> PiEventInput:
        return source.event


class _PiEventProjection:
    name = TEXT_EVIDENCE_PROJECTION_KEY.name
    version = TEXT_EVIDENCE_PROJECTION_KEY.version
    source_class: type[PiEventSource] = PiEventSource
    output_class: type[BaseModel] = TextEvidence

    def project(self, source: PiEventSource, /) -> TextEvidence:
        event = source.event
        return TextEvidence(
            source_type="pi-event",
            source_id=source.name,
            content=event.content,
            metadata={
                key: getattr(event, key) for key in ("session_id", "event_id", "parent_id", "event_type", "role")
            },
        )


PI_EVENT_SOURCE_DEFINITION = AdapterSourceDefinition(_PiEventAdapter(), projections=(_PiEventProjection(),))


class _Checkpoint(BaseModel):
    session_id: str
    imported: dict[str, str]


class PiSessionConnector:
    """Persist only new selected events, verifying all previously captured records."""

    name = "pi-session-history"
    version = "1"
    source_definitions = frozenset({"pi-event"})

    def __init__(self, path: str | Path, leaf_id: str | None = None) -> None:
        self._path = Path(path)
        self._leaf_id = leaf_id

    async def run(self, session: ConnectorRunSession, /) -> ConnectorRunCompletion:
        session_id, nodes, leaf = _archive(PiSessionInput(content=self._path.read_bytes(), leaf_id=self._leaf_id))
        events = _branch_events(session_id, nodes, leaf)
        imported: dict[str, str] = {}
        if session.checkpoint is not None:
            previous = _Checkpoint.model_validate(session.checkpoint)
            if previous.session_id != session_id:
                raise ValueError("Pi session changed since the checkpoint")  # noqa: TRY003
            imported = dict(previous.imported)
        for event_id, digest in imported.items():
            if event_id not in nodes or hashlib.sha256(_canonical(nodes[event_id])).hexdigest() != digest:
                raise ValueError("a previously captured Pi event was removed or rewritten")  # noqa: TRY003
        for event in events:
            if event.event_id not in imported:
                await session.submit(_event_id(session_id, event.event_id), "pi-event", event)
                imported[event.event_id] = hashlib.sha256(_canonical(event.record)).hexdigest()
        return ConnectorRunCompletion(
            status=ConnectorRunStatus.COMPLETE,
            checkpoint=_Checkpoint(session_id=session_id, imported=imported).model_dump(mode="json"),
        )


__all__ = [
    "PI_EVENT_SOURCE_DEFINITION",
    "PiEventInput",
    "PiEventSource",
    "PiSessionConnector",
    "PiSessionInput",
    "parse_pi_session",
]
