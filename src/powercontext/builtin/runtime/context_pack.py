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

"""Canonical, independently verifiable context packs and prefix deltas."""

# ruff: noqa: TRY003 -- Protocol errors carry the specific violated wire invariant.

from __future__ import annotations

import base64
import binascii
import hashlib
import json
import re
from dataclasses import dataclass
from typing import cast

_PACK_SCHEMA = "powercontext.context-pack.v1"
_DELTA_SCHEMA = "powercontext.context-pack-delta.v1"


class ContextPackError(ValueError):
    """Invalid encoding, chunk boundary, identity or reconstruction."""


def _json(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _chunks(items: tuple[bytes, ...]) -> None:
    if not isinstance(items, tuple) or any(not isinstance(item, bytes) for item in items):
        raise ContextPackError("chunks must be a tuple of bytes")
    try:
        for item in items:
            item.decode("utf-8")
    except UnicodeError as error:
        raise ContextPackError("every chunk must be valid UTF-8") from error


def _encode(items: tuple[bytes, ...]) -> list[str]:
    return [base64.b64encode(item).decode("ascii") for item in items]


def _decode(value: object) -> tuple[bytes, ...]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ContextPackError("chunks must be a Base64 string array")
    result: list[bytes] = []
    for item in value:
        try:
            raw = base64.b64decode(item, validate=True)
        except (ValueError, binascii.Error) as error:
            raise ContextPackError("invalid Base64 chunk") from error
        if base64.b64encode(raw).decode("ascii") != item:
            raise ContextPackError("chunk is not canonical Base64")
        result.append(raw)
    items = tuple(result)
    _chunks(items)
    return items


def _digest(value: object) -> str:
    return "sha256:" + hashlib.sha256(_json(value)).hexdigest()


def _require_digest(value: object) -> None:
    if not isinstance(value, str) or re.fullmatch(r"sha256:[0-9a-f]{64}", value) is None:
        raise ContextPackError("invalid SHA-256 digest")


def _unique_fields(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ContextPackError("duplicate JSON field")
        result[key] = value
    return result


def _payload(data: bytes, schema: str, fields: set[str]) -> dict[str, object]:
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=_unique_fields)
    except (ValueError, UnicodeError, AttributeError) as error:
        raise ContextPackError("invalid UTF-8 JSON payload") from error
    if not isinstance(value, dict) or set(value) != fields or value["schema"] != schema:
        raise ContextPackError("unexpected context-pack schema or fields")
    return value


@dataclass(frozen=True, slots=True)
class ContextPack:
    """Immutable UTF-8 chunks with a canonical content digest and exact wire size."""

    items: tuple[bytes, ...]

    def __post_init__(self) -> None:
        _chunks(self.items)

    @classmethod
    def build(cls, items: tuple[bytes, ...]) -> ContextPack:
        return cls(items)

    @classmethod
    def from_content(cls, content: str | None, *, chunk_bytes: int = 512) -> ContextPack:
        """Split only at UTF-8 boundaries, preserving all original bytes."""

        if type(chunk_bytes) is not int or chunk_bytes <= 0:
            raise ContextPackError("chunk_bytes must be a positive integer")
        if content is not None and not isinstance(content, str):
            raise ContextPackError("content must be text or None")
        try:
            data = (content or "").encode("utf-8")
        except UnicodeError as error:
            raise ContextPackError("content must be encodable as UTF-8") from error
        items: list[bytes] = []
        start = 0
        while start < len(data):
            end = min(start + chunk_bytes, len(data))
            while end < len(data) and end > start and data[end] & 0xC0 == 0x80:
                end -= 1
            if end == start:
                raise ContextPackError("chunk limit cannot contain the next UTF-8 character")
            items.append(data[start:end])
            start = end
        return cls.build(tuple(items))

    @property
    def digest(self) -> str:
        return _digest({"schema": _PACK_SCHEMA, "items": _encode(self.items)})

    @property
    def byte_count(self) -> int:
        return len(self.to_bytes())

    def to_bytes(self) -> bytes:
        payload = {"schema": _PACK_SCHEMA, "items": _encode(self.items), "digest": self.digest, "byte_count": 0}
        while True:
            wire = _json(payload)
            if payload["byte_count"] == len(wire):
                return wire
            payload["byte_count"] = len(wire)

    @classmethod
    def from_bytes(cls, data: bytes) -> ContextPack:
        payload = _payload(data, _PACK_SCHEMA, {"schema", "items", "digest", "byte_count"})
        result = cls.build(_decode(payload["items"]))
        if payload["digest"] != result.digest:
            raise ContextPackError("pack digest mismatch")
        if type(payload["byte_count"]) is not int or payload["byte_count"] != result.byte_count:
            raise ContextPackError("pack byte_count mismatch")
        return result


@dataclass(frozen=True, slots=True)
class ContextPackDelta:
    """Retained base prefix and complete target suffix, independent of the sender."""

    base_digest: str
    target_digest: str
    prefix_items: int
    appended: tuple[bytes, ...]

    def __post_init__(self) -> None:
        _require_digest(self.base_digest)
        _require_digest(self.target_digest)
        if type(self.prefix_items) is not int or self.prefix_items < 0:
            raise ContextPackError("prefix_items must be a nonnegative integer")
        _chunks(self.appended)

    @classmethod
    def between(cls, base: ContextPack, target: ContextPack) -> ContextPackDelta:
        prefix = 0
        for before, after in zip(base.items, target.items, strict=False):
            if before != after:
                break
            prefix += 1
        return cls(base.digest, target.digest, prefix, target.items[prefix:])

    @property
    def byte_count(self) -> int:
        return len(self.to_bytes())

    def to_bytes(self) -> bytes:
        return _json({
            "schema": _DELTA_SCHEMA,
            "base_digest": self.base_digest,
            "target_digest": self.target_digest,
            "prefix_items": self.prefix_items,
            "appended": _encode(self.appended),
        })

    @classmethod
    def from_bytes(cls, data: bytes) -> ContextPackDelta:
        payload = _payload(data, _DELTA_SCHEMA, {"schema", "base_digest", "target_digest", "prefix_items", "appended"})
        _require_digest(payload["base_digest"])
        _require_digest(payload["target_digest"])
        if type(payload["prefix_items"]) is not int or payload["prefix_items"] < 0:
            raise ContextPackError("prefix_items must be a nonnegative integer")
        return cls(
            cast(str, payload["base_digest"]),
            cast(str, payload["target_digest"]),
            payload["prefix_items"],
            _decode(payload["appended"]),
        )


def apply_context_pack_delta(base: ContextPack, delta: ContextPackDelta) -> ContextPack:
    """Validate the base identity, prefix bounds and reconstructed target digest."""

    if base.digest != delta.base_digest or delta.prefix_items > len(base.items):
        raise ContextPackError("delta does not match its base pack")
    result = ContextPack.build(base.items[: delta.prefix_items] + delta.appended)
    if result.digest != delta.target_digest:
        raise ContextPackError("reconstructed target digest mismatch")
    return result


__all__ = ["ContextPack", "ContextPackDelta", "ContextPackError", "apply_context_pack_delta"]
