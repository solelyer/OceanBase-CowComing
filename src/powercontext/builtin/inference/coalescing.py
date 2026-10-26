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

"""Share in-flight embeddings and cache successful vectors within one event loop."""

from __future__ import annotations

import asyncio
import math
from collections import OrderedDict
from dataclasses import dataclass

from powercontext.builtin.inference.models import EmbeddingResult, EmbeddingVector, InferenceUsage
from powercontext.builtin.inference.protocols import EmbeddingModel


@dataclass(frozen=True, slots=True)
class _ComputedEmbedding:
    vector: EmbeddingVector
    batch_id: object
    usage: InferenceUsage


class CoalescingEmbeddingModel:
    """Batch unique texts while preserving each caller's ordering and usage.

    Shared work is owned by the wrapper rather than any individual waiter, so
    cancelling a caller does not cancel the provider or other callers.
    """

    def __init__(
        self,
        model: EmbeddingModel,
        *,
        max_batch_size: int = 64,
        coalesce_ms: float = 5.0,
        cache_size: int = 1024,
    ) -> None:
        if type(max_batch_size) is not int or max_batch_size <= 0:
            raise ValueError("max_batch_size must be a positive integer")  # noqa: TRY003
        if type(cache_size) is not int or cache_size < 0:
            raise ValueError("cache_size must be a nonnegative integer")  # noqa: TRY003
        if type(coalesce_ms) not in (int, float) or not math.isfinite(coalesce_ms) or coalesce_ms < 0:
            raise ValueError("coalesce_ms must be finite and nonnegative")  # noqa: TRY003
        self._model = model
        self.profile = model.profile
        self._max_batch_size = max_batch_size
        self._coalesce_seconds = coalesce_ms / 1000
        self._cache_size = cache_size
        self._cache: OrderedDict[str, EmbeddingVector] = OrderedDict()
        self._inflight: dict[str, asyncio.Future[_ComputedEmbedding]] = {}
        self._pending: dict[str, asyncio.Future[_ComputedEmbedding]] = {}
        self._flush_task: asyncio.Task[None] | None = None
        self._workers: set[asyncio.Task[None]] = set()

    async def embed(self, texts: tuple[str, ...], /) -> EmbeddingResult:
        """Return original positions and charge each awaited provider batch once."""

        cached: dict[str, EmbeddingVector] = {}
        waiting: dict[str, asyncio.Future[_ComputedEmbedding]] = {}
        loop = asyncio.get_running_loop()
        for text in texts:
            if text in self._cache:
                cached[text] = self._cache[text]
                self._cache.move_to_end(text)
                continue
            future = self._inflight.get(text)
            if future is None:
                future = loop.create_future()
                # A failed provider must remain observed even if all waiters cancel.
                future.add_done_callback(lambda result: None if result.cancelled() else result.exception())
                self._inflight[text] = future
                self._pending[text] = future
            waiting[text] = future
        if self._pending and self._flush_task is None:
            self._flush_task = asyncio.create_task(self._flush())

        results = await asyncio.gather(*(asyncio.shield(future) for future in waiting.values()))
        computed = dict(zip(waiting, results, strict=True))
        batches = {value.batch_id: value.usage for value in results}
        usages = tuple(batches.values())
        input_tokens = tuple(usage.input_tokens for usage in usages if usage.input_tokens is not None)
        output_tokens = tuple(usage.output_tokens for usage in usages if usage.output_tokens is not None)
        return EmbeddingResult(
            vectors=tuple(cached[text] if text in cached else computed[text].vector for text in texts),
            usage=InferenceUsage(
                requests=sum(usage.requests for usage in usages),
                input_tokens=sum(input_tokens) if input_tokens else None,
                output_tokens=sum(output_tokens) if output_tokens else None,
            ),
        )

    async def _flush(self) -> None:
        # Yield even for a zero window to collect calls started in the same turn.
        await asyncio.sleep(self._coalesce_seconds)
        pending = tuple(self._pending.items())
        self._pending.clear()
        self._flush_task = None
        for start in range(0, len(pending), self._max_batch_size):
            task = asyncio.create_task(self._run_batch(pending[start : start + self._max_batch_size]))
            self._workers.add(task)
            task.add_done_callback(self._workers.discard)

    async def _run_batch(self, batch: tuple[tuple[str, asyncio.Future[_ComputedEmbedding]], ...]) -> None:
        texts = tuple(text for text, _future in batch)
        try:
            result = await self._model.embed(texts)
            if len(result.vectors) != len(batch):
                # Validation failures must reach the same waiters as provider errors.
                raise ValueError("provider vector count does not match its input batch")  # noqa: TRY003, TRY301
        except (Exception, asyncio.CancelledError) as error:
            for text, future in batch:
                self._inflight.pop(text)
                future.set_exception(error)
            return
        batch_id = object()
        for (text, future), vector in zip(batch, result.vectors, strict=True):
            if self._cache_size:
                self._cache[text] = vector
                self._cache.move_to_end(text)
                if len(self._cache) > self._cache_size:
                    self._cache.popitem(last=False)
            self._inflight.pop(text)
            future.set_result(_ComputedEmbedding(vector=vector, batch_id=batch_id, usage=result.usage))


__all__ = ["CoalescingEmbeddingModel"]
