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

"""Lossless original events and checkpoint-safe failure recovery."""

import asyncio
import json
from copy import deepcopy

import pytest

from powercontext.sources import ConnectorBinding, SourceDefinitionRegistry, SourceRef
from powercontext.sources.connectors import ConnectorLifecycle
from powercontext.sources.pi_session import (
    PI_EVENT_SOURCE_DEFINITION,
    PiSessionConnector,
    PiSessionInput,
    parse_pi_session,
)


def _archive(records) -> bytes:
    return "\n".join(
        json.dumps(row, ensure_ascii=False) for row in [{"type": "session", "version": 3, "id": "s"}, *records]
    ).encode()


def test_large_tool_result_is_not_truncated_and_arguments_are_canonical() -> None:
    records = [
        {
            "type": "message",
            "id": "a",
            "parentId": None,
            "message": {
                "role": "assistant",
                "content": [
                    {"type": "toolCall", "name": "read", "id": "call", "arguments": {"z": "海", "a": 1}},
                ],
            },
        },
        {
            "type": "message",
            "id": "b",
            "parentId": "a",
            "message": {"role": "toolResult", "toolName": "read", "toolCallId": "call", "content": "海" * 100000},
        },
    ]
    events = parse_pi_session(PiSessionInput(content=_archive(records)))
    assert events[0].content == 'assistant: [tool_call read call] {"a":1,"z":"海"}'
    assert events[1].content == "toolResult: [tool_result read call] " + "海" * 100000
    registry = SourceDefinitionRegistry((PI_EVENT_SOURCE_DEFINITION,))
    source = asyncio.run(registry.resolve(events[1]))
    assert asyncio.run(registry.read(source)).record == records[1]


def test_failed_run_retries_partial_acceptance_and_detects_changes_on_old_branch(tmp_path) -> None:
    async def run() -> None:
        records = [
            {"type": "message", "id": "root", "parentId": None, "message": {"role": "user", "content": "root"}},
            {"type": "message", "id": "left", "parentId": "root", "message": {"role": "assistant", "content": "left"}},
            {
                "type": "message",
                "id": "right",
                "parentId": "root",
                "message": {"role": "assistant", "content": "right"},
            },
            {"type": "message", "id": "tail", "parentId": "right", "message": {"role": "assistant", "content": "tail"}},
        ]
        path = tmp_path / "session.jsonl"
        path.write_bytes(_archive(records))

        class Checkpoints:
            value = None

            async def load(self, binding, /):
                return deepcopy(self.value)

            async def save(self, binding, value, /, *, expected):
                assert self.value == expected
                self.value = deepcopy(value)

        class Sink:
            fail = True

            def __init__(self):
                self.values = {}

            async def submit(self, binding, item_id, definition, value, /):
                if value.event_id == "tail" and self.fail:
                    raise OSError("capture failed")  # noqa: TRY003
                if item_id in self.values:
                    assert self.values[item_id] == value
                self.values[item_id] = value
                return SourceRef(source_type=definition, source_id=item_id)

        checkpoints, sink = Checkpoints(), Sink()
        lifecycle = ConnectorLifecycle(sink=sink, checkpoints=checkpoints)
        binding = ConnectorBinding(
            scope_id="s", binding_id="b", connector_name="pi-session-history", connector_version="1"
        )
        first = await lifecycle.run(PiSessionConnector(path, "left"), binding)
        with pytest.raises(OSError):
            await lifecycle.run(PiSessionConnector(path, "tail"), binding)
        assert checkpoints.value == first.committed_checkpoint
        assert len(sink.values) == 3
        sink.fail = False
        recovered = await lifecycle.run(PiSessionConnector(path, "tail"), binding)
        assert len(sink.values) == 4
        assert (await lifecycle.run(PiSessionConnector(path, "tail"), binding)).items == ()
        records[1]["message"]["content"] = "rewritten old branch"
        path.write_bytes(_archive(records))
        with pytest.raises(ValueError):
            await lifecycle.run(PiSessionConnector(path, "tail"), binding)
        assert checkpoints.value == recovered.committed_checkpoint

    asyncio.run(run())
