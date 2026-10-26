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

"""CSV boundaries beyond the public airport sample."""

import asyncio
import json

import pytest

from powercontext.sources import SourceDefinitionRegistry, TextEvidence
from powercontext.sources.csv_rows import (
    CSV_ROW_SOURCE_DEFINITION,
    CSVDocumentInput,
    csv_row_source_id,
    parse_csv_document,
)
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY


def test_unicode_percent_encoding_and_exact_source_id_limit() -> None:
    assert csv_row_source_id("表:%", " /甲 ") == "csv:%E8%A1%A8%3A%25:%20%2F%E7%94%B2%20"
    assert len(csv_row_source_id("d", "x" * 250)) == 256
    with pytest.raises(ValueError):
        csv_row_source_id("d", "x" * 251)
    assert csv_row_source_id("a:b", "c") != csv_row_source_id("a", "b:c")


def test_blank_physical_records_differ_from_empty_or_whitespace_fields() -> None:
    document = CSVDocumentInput(document_id="d", primary_key="id", content=b'id,name\n\r\n1,""\n')
    assert parse_csv_document(document)[0].values == ("1", "")
    for bad in (b'id\n""\n', b"id\n  \n", b"\nid\n1\n", b'id,name\n1,ok\n2,"bad"x\n'):
        with pytest.raises(ValueError):
            parse_csv_document(CSVDocumentInput(document_id="d", primary_key="id", content=bad))


def test_columns_remain_distinct_when_their_trimmed_spellings_match() -> None:
    row = parse_csv_document(
        CSVDocumentInput(document_id="d", primary_key=" id", content='id, id,note\n000, 甲 ,"false"\n'.encode())
    )[0]
    registry = SourceDefinitionRegistry((CSV_ROW_SOURCE_DEFINITION,))
    source = asyncio.run(registry.resolve(row))
    evidence = TextEvidence.model_validate(registry.project(source, TEXT_EVIDENCE_PROJECTION_KEY))
    assert json.loads(evidence.content) == {"id": "000", " id": " 甲 ", "note": "false"}
    assert evidence.content == '{"id":"000"," id":" 甲 ","note":"false"}'
    assert asyncio.run(registry.read(source)) == row
