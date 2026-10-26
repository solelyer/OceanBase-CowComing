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

"""Strict HTTP scoring with stable ranking across bounded candidate batches."""

from __future__ import annotations

import math

import httpx

from powercontext.builtin.artifacts.memory.models import MemoryHit
from powercontext.builtin.artifacts.memory.reranking import MemoryRerankDecision
from powercontext.builtin.inference import InferenceUsage


def _batch_scores(payload: object, expected: set[int]) -> dict[int, int | float]:
    if not isinstance(payload, dict) or set(payload) != {"scores"} or not isinstance(payload["scores"], list):
        raise ValueError("response must contain only a scores array")  # noqa: TRY003
    scores: dict[int, int | float] = {}
    for item in payload["scores"]:
        if not isinstance(item, dict) or set(item) != {"rank", "score"}:
            raise ValueError("each score must contain exactly rank and score")  # noqa: TRY003
        rank, score = item["rank"], item["score"]
        if type(rank) is not int or rank not in expected or rank in scores:
            raise ValueError("score ranks must cover this batch exactly once")  # noqa: TRY003
        if type(score) not in (int, float):
            raise ValueError("score must be a finite number")  # noqa: TRY003
        try:
            finite = math.isfinite(score)
        except OverflowError as error:
            raise ValueError("score is outside the finite numeric range") from error  # noqa: TRY003
        if not finite:
            raise ValueError("score must be finite")  # noqa: TRY003
        scores[rank] = score
    if set(scores) != expected:
        raise ValueError("score response does not cover this batch")  # noqa: TRY003
    return scores


class HTTPMemoryReranker:
    """Use the caller's client to score each original candidate exactly once."""

    def __init__(
        self,
        endpoint: str,
        *,
        client: httpx.AsyncClient,
        batch_size: int = 32,
        policy_id: str = "http-cross-encoder-v1",
    ) -> None:
        if type(batch_size) is not int or batch_size <= 0:
            raise ValueError("batch_size must be a positive integer")  # noqa: TRY003
        self._endpoint = endpoint
        self._client = client
        self._batch_size = batch_size
        self.policy_id = policy_id

    async def rerank(self, query: str, candidates: tuple[MemoryHit, ...], limit: int, /) -> MemoryRerankDecision:
        """Validate each response, then select by global score and original rank."""

        if not candidates or type(limit) is not int or not 1 <= limit <= len(candidates):
            raise ValueError("limit must be between one and the nonempty candidate count")  # noqa: TRY003
        scores: dict[int, int | float] = {}
        requests = 0
        for start in range(0, len(candidates), self._batch_size):
            batch = [
                {"rank": rank, "text": candidate.text}
                for rank, candidate in enumerate(candidates[start : start + self._batch_size], start=start + 1)
            ]
            response = await self._client.post(self._endpoint, json={"query": query, "candidates": batch})
            requests += 1
            response.raise_for_status()
            try:
                payload = response.json()
            except (ValueError, UnicodeError) as error:
                raise ValueError("reranker returned invalid JSON") from error  # noqa: TRY003
            expected = set(range(start + 1, min(start + self._batch_size, len(candidates)) + 1))
            scores.update(_batch_scores(payload, expected))
        ranked = sorted(scores, key=lambda rank: (-scores[rank], rank))
        return MemoryRerankDecision(selected_ranks=tuple(ranked[:limit]), usage=InferenceUsage(requests=requests))


__all__ = ["HTTPMemoryReranker"]
