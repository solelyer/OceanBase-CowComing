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

"""Sharing, independent failures and per-caller provider usage."""

import asyncio

import pytest

from powercontext.builtin.artifacts.memory import EmbeddingProfile
from powercontext.builtin.inference import EmbeddingResult, InferenceUsage
from powercontext.builtin.inference.coalescing import CoalescingEmbeddingModel


class _Embedding:
    profile = EmbeddingProfile(profile_id="coalescing-contract", model="offline", dimension=1)

    def __init__(self) -> None:
        self.calls: list[tuple[str, ...]] = []

    async def embed(self, texts: tuple[str, ...], /) -> EmbeddingResult:
        self.calls.append(texts)
        return EmbeddingResult(
            vectors=tuple((float(len(text)),) for text in texts),
            usage=InferenceUsage(requests=2, input_tokens=11, output_tokens=0),
        )


def test_late_waiter_shares_running_provider_even_with_cache_disabled() -> None:
    async def run() -> None:
        started, release = asyncio.Event(), asyncio.Event()

        class Blocked(_Embedding):
            async def embed(self, texts, /):
                started.set()
                await release.wait()
                return await super().embed(texts)

        base = Blocked()
        model = CoalescingEmbeddingModel(base, cache_size=0, coalesce_ms=0)
        first = asyncio.create_task(model.embed(("shared", "shared")))
        await started.wait()
        second = asyncio.create_task(model.embed(("shared",)))
        await asyncio.sleep(0)
        first.cancel()
        with pytest.raises(asyncio.CancelledError):
            await first
        release.set()
        assert (await second).vectors == ((6.0,),)
        assert base.calls == [("shared",)]
        await model.embed(("shared",))
        assert len(base.calls) == 2

    asyncio.run(run())


def test_independent_batch_finishes_when_another_batch_fails_then_retries() -> None:
    async def run() -> None:
        class Flaky(_Embedding):
            async def embed(self, texts, /):
                if "bad" in texts and not self.calls:
                    self.calls.append(texts)
                    raise RuntimeError("provider failure")  # noqa: TRY003
                return await super().embed(texts)

        base = Flaky()
        model = CoalescingEmbeddingModel(base, max_batch_size=1, coalesce_ms=0)
        bad, good = await asyncio.gather(model.embed(("bad",)), model.embed(("good",)), return_exceptions=True)
        assert isinstance(bad, RuntimeError)
        assert isinstance(good, EmbeddingResult)
        assert good.vectors == ((4.0,),)
        assert (await model.embed(("bad",))).vectors == ((3.0,),)
        assert len(base.calls) == 3

    asyncio.run(run())


def test_usage_counts_shared_batches_once_per_caller_and_excludes_cached_vectors() -> None:
    async def run() -> None:
        base = _Embedding()
        model = CoalescingEmbeddingModel(base, max_batch_size=2, coalesce_ms=0)
        first, second = await asyncio.gather(model.embed(("a", "bb", "a", "ccc")), model.embed(("bb",)))
        assert first.usage == InferenceUsage(requests=4, input_tokens=22, output_tokens=0)
        assert second.usage == InferenceUsage(requests=2, input_tokens=11, output_tokens=0)
        mixed = await model.embed(("a", "dddd", "a"))
        assert mixed.vectors == ((1.0,), (4.0,), (1.0,))
        assert mixed.usage == InferenceUsage(requests=2, input_tokens=11, output_tokens=0)

    asyncio.run(run())
