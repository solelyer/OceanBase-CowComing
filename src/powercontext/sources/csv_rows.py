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

"""Lossless CSV records with stable, captured row identities."""

from __future__ import annotations

import csv
import io
import json
from urllib.parse import quote

from pydantic import BaseModel, model_validator

from powercontext.limits import MAX_SOURCE_ID_LENGTH
from powercontext.sources.definitions import AdapterSourceDefinition
from powercontext.sources.models import Source, SourceMaterialization
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY, TextEvidence


class CSVDocumentInput(BaseModel):
    """An entire UTF-8 document and the exact name of its primary-key column."""

    document_id: str
    primary_key: str
    content: bytes


def csv_row_source_id(document_id: str, primary_key_value: str) -> str:
    """Encode each identity component independently without normalizing either."""

    if not document_id or document_id != document_id.strip():
        raise ValueError("document_id must be nonempty and have no surrounding whitespace")  # noqa: TRY003
    if not primary_key_value.strip():
        raise ValueError("the primary-key value must not be blank")  # noqa: TRY003
    identity = f"csv:{quote(document_id, safe='')}:{quote(primary_key_value, safe='')}"
    if len(identity) > MAX_SOURCE_ID_LENGTH:
        raise ValueError("CSV row identity exceeds the Source ID limit")  # noqa: TRY003
    return identity


class CSVRowInput(BaseModel):
    """One complete row, preserving header order and every original field."""

    document_id: str
    primary_key: str
    primary_key_value: str
    columns: tuple[str, ...]
    values: tuple[str, ...]

    @model_validator(mode="after")
    def validate_row(self) -> CSVRowInput:
        csv_row_source_id(self.document_id, self.primary_key_value)
        if not self.columns or any(not column.strip() for column in self.columns):
            raise ValueError("CSV columns must not be empty or blank")  # noqa: TRY003
        if len(set(self.columns)) != len(self.columns):
            raise ValueError("CSV columns must be unique")  # noqa: TRY003
        if not self.primary_key or self.primary_key not in self.columns:
            raise ValueError("primary_key must exactly match a column")  # noqa: TRY003
        if len(self.values) != len(self.columns):
            raise ValueError("CSV row width must match its header")  # noqa: TRY003
        if self.values[self.columns.index(self.primary_key)] != self.primary_key_value:
            raise ValueError("primary_key_value must match the row")  # noqa: TRY003
        return self


class CSVRowSource(Source):
    """Captured original fields for one stable document/key pair."""

    source_id: str
    document_id: str
    primary_key: str
    primary_key_value: str
    columns: tuple[str, ...]
    values: tuple[str, ...]


def parse_csv_document(value: CSVDocumentInput) -> tuple[CSVRowInput, ...]:
    """Parse and validate the complete document before returning any rows."""

    if not value.document_id or value.document_id != value.document_id.strip():
        raise ValueError("document_id must be nonempty and have no surrounding whitespace")  # noqa: TRY003
    reader = csv.reader(io.StringIO(value.content.decode("utf-8-sig"), newline=""), strict=True)
    rows: list[CSVRowInput] = []
    seen: set[str] = set()
    try:
        columns = tuple(next(reader, ()))
        if not columns or any(not column.strip() for column in columns) or len(set(columns)) != len(columns):
            raise ValueError("CSV requires nonblank, unique column names")  # noqa: TRY003
        if not value.primary_key or value.primary_key not in columns:
            raise ValueError("primary_key must exactly match a column")  # noqa: TRY003
        key_index = columns.index(value.primary_key)
        for record in reader:
            if not record:
                continue
            if len(record) != len(columns):
                raise ValueError("CSV row width must match its header")  # noqa: TRY003
            key = record[key_index]
            if key in seen:
                raise ValueError("duplicate CSV primary-key value")  # noqa: TRY003
            row = CSVRowInput(
                document_id=value.document_id,
                primary_key=value.primary_key,
                primary_key_value=key,
                columns=columns,
                values=tuple(record),
            )
            seen.add(key)
            rows.append(row)
    except csv.Error as error:
        raise ValueError("malformed CSV document") from error  # noqa: TRY003
    return tuple(rows)


class _CSVRowAdapter:
    input_class: type[CSVRowInput] = CSVRowInput
    name = "csv-row"
    source_class: type[CSVRowSource] = CSVRowSource

    async def resolve(self, value: CSVRowInput, /) -> CSVRowSource:
        identity = csv_row_source_id(value.document_id, value.primary_key_value)
        return CSVRowSource(
            name=identity, source_id=identity, materialization=SourceMaterialization.CAPTURED, **value.model_dump()
        )

    async def read(self, source: CSVRowSource, /) -> CSVRowInput:
        return CSVRowInput.model_validate(source.model_dump())


class _CSVRowProjection:
    name = TEXT_EVIDENCE_PROJECTION_KEY.name
    version = TEXT_EVIDENCE_PROJECTION_KEY.version
    source_class: type[CSVRowSource] = CSVRowSource
    output_class: type[BaseModel] = TextEvidence

    def project(self, source: CSVRowSource, /) -> TextEvidence:
        row = dict(zip(source.columns, source.values, strict=True))
        return TextEvidence(
            source_type="csv-row",
            source_id=source.source_id,
            content=json.dumps(row, ensure_ascii=False, separators=(",", ":")),
            metadata={
                "document_id": source.document_id,
                "primary_key": source.primary_key,
                "primary_key_value": source.primary_key_value,
                "columns": list(source.columns),
                "row": row,
            },
        )


CSV_ROW_SOURCE_DEFINITION = AdapterSourceDefinition(_CSVRowAdapter(), projections=(_CSVRowProjection(),))

__all__ = [
    "CSV_ROW_SOURCE_DEFINITION",
    "CSVDocumentInput",
    "CSVRowInput",
    "CSVRowSource",
    "csv_row_source_id",
    "parse_csv_document",
]
