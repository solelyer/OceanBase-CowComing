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

"""Scope-local lifecycle records and Topic generation eligibility."""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

from sqlalchemy import ColumnElement, select
from sqlalchemy.dialects.mysql import insert as mysql_insert
from sqlalchemy.dialects.sqlite import insert as sqlite_insert
from sqlalchemy.ext.asyncio import AsyncConnection

from powercontext.builtin.persistence.codec import stored_bytes
from powercontext.builtin.persistence.tables import SOURCE_LIFECYCLE_TABLE


async def lifecycle_payloads(connection: AsyncConnection, scope_id: str) -> tuple[bytes, ...]:
    rows = await connection.scalars(
        select(SOURCE_LIFECYCLE_TABLE.c.payload).where(SOURCE_LIFECYCLE_TABLE.c.scope_id == scope_id)
    )
    return tuple(stored_bytes(row, column="payload") for row in rows)


async def store_lifecycle_nodes(connection: AsyncConnection, rows: Sequence[dict[str, Any]]) -> None:
    if not rows:
        return
    table = SOURCE_LIFECYCLE_TABLE
    if connection.dialect.name == "sqlite":
        statement = sqlite_insert(table).values(list(rows))
        statement = statement.on_conflict_do_update(
            index_elements=["scope_id", "node_id"],
            set_={key: statement.excluded[key] for key in ("node_kind", "artifact_id", "available", "payload")},
        )
    else:
        statement = mysql_insert(table).values(list(rows))
        statement = statement.on_duplicate_key_update(**{
            key: statement.inserted[key] for key in ("node_kind", "artifact_id", "available", "payload")
        })
    await connection.execute(statement)


def available_topic_predicate(scope_id: str, artifact_id: ColumnElement[str]) -> ColumnElement[bool]:
    table = SOURCE_LIFECYCLE_TABLE
    return (
        ~select(table.c.node_id)
        .where(
            table.c.scope_id == scope_id,
            table.c.node_kind == "topic",
            table.c.artifact_id == artifact_id,
            table.c.available.is_(False),
        )
        .exists()
    )


async def unavailable_topics(connection: AsyncConnection, scope_id: str) -> frozenset[str]:
    table = SOURCE_LIFECYCLE_TABLE
    return frozenset(
        await connection.scalars(
            select(table.c.artifact_id).where(
                table.c.scope_id == scope_id, table.c.node_kind == "topic", table.c.available.is_(False)
            )
        )
    )
