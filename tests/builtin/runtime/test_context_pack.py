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

"""Wire encoding errors and native empty-context transitions."""

import asyncio
import json

import pytest

from powercontext.builtin.runtime import BuiltinConfig, PrepareContextRequest, open_builtin_runtime
from powercontext.builtin.runtime.context_pack import (
    ContextPack,
    ContextPackDelta,
    ContextPackError,
    apply_context_pack_delta,
)
from powercontext.builtin.scope import ScopeDraft


def test_size_field_stabilizes_across_decimal_boundaries() -> None:
    for size in (1, 99, 999, 9999):
        pack = ContextPack.from_content("a" * size)
        wire = pack.to_bytes()
        assert json.loads(wire)["byte_count"] == len(wire) == pack.byte_count
        assert ContextPack.from_bytes(wire) == pack
    for bound in (4, 5, 7, 512):
        text = "海🙂é\r\n" * 200
        pack = ContextPack.from_content(text, chunk_bytes=bound)
        assert b"".join(pack.items).decode() == text
        assert all(len(item) <= bound and item.decode() for item in pack.items)


@pytest.mark.parametrize("items", [["YQ==\n"], ["YR=="], ["/w=="], [1], "YQ=="])
def test_wire_rejects_noncanonical_base64_invalid_utf8_and_wrong_types(items) -> None:
    payload = json.loads(ContextPack.build((b"a",)).to_bytes())
    payload["items"] = items
    with pytest.raises(ContextPackError):
        ContextPack.from_bytes(json.dumps(payload).encode())


def test_duplicate_wire_fields_are_rejected() -> None:
    wire = ContextPack.build((b"a",)).to_bytes()
    with pytest.raises(ContextPackError):
        ContextPack.from_bytes(b'{"schema":"powercontext.context-pack.v1",' + wire[1:])


def test_native_empty_prepare_can_replace_a_nonempty_base() -> None:
    async def run() -> None:
        async with open_builtin_runtime(BuiltinConfig()) as runtime:
            scope = await runtime.scopes.create(
                ScopeDraft(title="empty", summary="empty", idempotency_key="empty-pack")
            )
            context = runtime.context.for_scope(scope.scope_id)
            request = PrepareContextRequest(query="no stored evidence")
            assert (await context.prepare(request)).content is None
            first = await context.prepare_incremental(request)
            assert isinstance(first, ContextPack) and first.items == ()
            base = ContextPack.from_content("previous context")
            delta = await context.prepare_incremental(request, base_pack=base)
            assert isinstance(delta, ContextPackDelta)
            assert apply_context_pack_delta(base, ContextPackDelta.from_bytes(delta.to_bytes())).items == ()

    asyncio.run(run())
