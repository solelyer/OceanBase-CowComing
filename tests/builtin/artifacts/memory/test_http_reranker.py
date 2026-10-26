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

"""HTTP response and global-ordering contracts."""

import asyncio
import json

import httpx
import pytest

from powercontext.artifacts import ArtifactRef
from powercontext.builtin.artifacts.memory.http_reranker import HTTPMemoryReranker
from powercontext.builtin.artifacts.memory.models import MemoryHit


def _hit(rank: int) -> MemoryHit:
    return MemoryHit(
        memory_ref=ArtifactRef(family="memory", artifact_id="m", revision=1),
        entry_id=f"entry-{rank}",
        entry_version_id=f"version-{rank}",
        text=f"Text {rank}",
        score=0,
        matched_by=("fts",),
    )


@pytest.mark.parametrize("rank,score", [(True, 1), (1.0, 1), ("1", 1), (1, True), (1, "0.7"), (1, None)])
def test_rank_and_score_do_not_coerce_json_types(rank, score) -> None:
    async def run() -> None:
        async with httpx.AsyncClient(
            transport=httpx.MockTransport(
                lambda _r: httpx.Response(200, json={"scores": [{"rank": rank, "score": score}]})
            )
        ) as client:
            with pytest.raises(ValueError):
                await HTTPMemoryReranker("https://score.invalid", client=client).rerank("q", (_hit(1),), 1)

    asyncio.run(run())


def test_large_integer_scores_keep_numeric_order_and_caller_client_remains_open() -> None:
    async def run() -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            candidates = json.loads(request.content)["candidates"]
            return httpx.Response(
                200, json={"scores": [{"rank": c["rank"], "score": 2**54 + c["rank"]} for c in candidates]}
            )

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            ranker = HTTPMemoryReranker("https://score.invalid", client=client, batch_size=1)
            decision = await ranker.rerank("q", tuple(_hit(i) for i in range(1, 5)), 4)
            assert decision.selected_ranks == (4, 3, 2, 1)
            assert decision.usage.requests == 4
            assert not client.is_closed

    asyncio.run(run())


def test_later_batch_http_status_failure_is_preserved() -> None:
    async def run() -> None:
        def handler(request: httpx.Request) -> httpx.Response:
            rank = json.loads(request.content)["candidates"][0]["rank"]
            return (
                httpx.Response(503) if rank == 2 else httpx.Response(200, json={"scores": [{"rank": rank, "score": 1}]})
            )

        async with httpx.AsyncClient(transport=httpx.MockTransport(handler)) as client:
            with pytest.raises(httpx.HTTPStatusError) as failure:
                await HTTPMemoryReranker("https://score.invalid", client=client, batch_size=1).rerank(
                    "q", (_hit(1), _hit(2)), 1
                )
            assert failure.value.response.status_code == 503

    asyncio.run(run())


@pytest.mark.parametrize("content", [b"not-json", b"[]", b'{"scores":null}', b'{"scores":[null]}'])
def test_invalid_json_or_container_shapes_fail(content: bytes) -> None:
    async def run() -> None:
        async with httpx.AsyncClient(
            transport=httpx.MockTransport(lambda _r: httpx.Response(200, content=content))
        ) as client:
            with pytest.raises(ValueError):
                await HTTPMemoryReranker("https://score.invalid", client=client).rerank("q", (_hit(1),), 1)

    asyncio.run(run())
