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

"""Text fidelity and original-file identity independent of the report fixture."""

import asyncio
import hashlib
import io

import pytest
from pypdf import PdfWriter
from pypdf.generic import DecodedStreamObject, DictionaryObject, NameObject

from powercontext.builtin.artifacts.memory import MemoryEntryInput
from powercontext.builtin.persistence.sqlite import SQLiteConfig
from powercontext.builtin.runtime import (
    BuiltinConfig,
    GetMemoryEntryRequest,
    RememberMemoryRequest,
    SubmitSourceObservation,
    open_builtin_runtime,
)
from powercontext.builtin.scope import ScopeDraft
from powercontext.sources import SourceDefinitionRegistry, manifest_for_definition, project_source_for_transport
from powercontext.sources.pdf_pages import (
    PDF_PAGE_SOURCE_DEFINITION,
    PDFDocumentInput,
    PDFPageInput,
    parse_pdf_document,
    search_pdf_pages,
)
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY


class _PagePipeline:
    async def extract(self, request, /):
        return tuple(
            MemoryEntryInput(
                kind="source-evidence",
                text=source.projection(TEXT_EVIDENCE_PROJECTION_KEY)["content"],
                sources=(source,),
            )
            for source in request.sources
        )


def test_full_file_digest_preserves_numbers_and_changes_with_metadata() -> None:
    writer = PdfWriter()
    page = writer.add_blank_page(width=300, height=300)
    page[NameObject("/Resources")] = DictionaryObject({
        NameObject("/Font"): DictionaryObject({
            NameObject("/F1"): DictionaryObject({
                NameObject("/Type"): NameObject("/Font"),
                NameObject("/Subtype"): NameObject("/Type1"),
                NameObject("/BaseFont"): NameObject("/Helvetica"),
            })
        })
    })
    stream = DecodedStreamObject()
    stream.set_data(b"BT /F1 12 Tf 20 200 Td (Revenue 001 37.25) Tj ET")
    page[NameObject("/Contents")] = writer._add_object(stream)
    buffer = io.BytesIO()
    writer.write(buffer)
    original = buffer.getvalue()
    first = parse_pdf_document(PDFDocumentInput(document_id="财报" * 100, content=original))[0]
    assert "Revenue 001 37.25" in first.text
    assert first.document_sha256 == hashlib.sha256(original).hexdigest()
    writer.add_metadata({"/Title": "Changed metadata, same body"})
    buffer = io.BytesIO()
    writer.write(buffer)
    second = parse_pdf_document(PDFDocumentInput(document_id=first.document_id, content=buffer.getvalue()))[0]
    assert first.text == second.text
    first_source = asyncio.run(PDF_PAGE_SOURCE_DEFINITION.resolve(first))
    second_source = asyncio.run(PDF_PAGE_SOURCE_DEFINITION.resolve(second))
    assert len(first_source.name) <= 256 and first_source.name != second_source.name


def test_nonempty_password_and_blank_document_id_fail() -> None:
    writer = PdfWriter()
    writer.add_blank_page(width=100, height=100)
    writer.encrypt("private")
    buffer = io.BytesIO()
    writer.write(buffer)
    with pytest.raises(ValueError):
        parse_pdf_document(PDFDocumentInput(document_id="protected", content=buffer.getvalue()))
    with pytest.raises(ValueError):
        parse_pdf_document(PDFDocumentInput(document_id=" \t", content=buffer.getvalue()))


async def test_natural_question_retrieves_evidence_after_restart(tmp_path) -> None:
    config = BuiltinConfig(database=SQLiteConfig(url=f"sqlite+aiosqlite:///{tmp_path / 'pdf.db'}"))
    documents = {
        "ORION_2025_10K": [
            "Consolidated statements of income\nNet sales 85,400\nOperating profit 12,300\nNet income 9,750",
            "Consolidated statements of cash flows\nDepreciation and amortization 1,750\nCapital expenditures 2,100",
            "Geographic information\nOur divisions serve North America, Europe and Asia.",
            "Legal proceedings\nLitigation and claims may lead to an adverse judgment. A provision of $18 million is recorded.",
        ],
        "ORION_2024_10K": ["Consolidated statements of income\nNet sales 60,000\nOperating profit 8,000"],
        "NOVA_2025_10K": [
            "Net sales operating income cash flows depreciation amortization capital expenditures\n" * 5
            for _ in range(25)
        ],
    }
    async with open_builtin_runtime(config, candidate_pipeline=_PagePipeline()) as runtime:
        scope = await runtime.scopes.create(ScopeDraft(title="PDF", summary="reports", idempotency_key="pdf"))
        registry = SourceDefinitionRegistry((PDF_PAGE_SOURCE_DEFINITION,))
        await runtime.ingestion.register(manifest_for_definition(PDF_PAGE_SOURCE_DEFINITION))
        for document, bodies in documents.items():
            for number, body in enumerate(bodies, 1):
                page = PDFPageInput(
                    document_id=document,
                    document_sha256=hashlib.sha256(document.encode()).hexdigest(),
                    page_number=number,
                    text=body,
                )
                source = await registry.resolve(page)
                await runtime.ingestion.submit(
                    SubmitSourceObservation(
                        scope_id=scope.scope_id, observation=project_source_for_transport(registry, source)
                    )
                )
        for _ in range(32):
            if not (await runtime.memory.for_scope(scope.scope_id).flush()).processed:
                break
        else:
            pytest.fail("page ingestion did not drain")
        other = await runtime.scopes.create(ScopeDraft(title="other", summary="isolated", idempotency_key="other"))
        await runtime.memory.for_scope(other.scope_id).remember(
            RememberMemoryRequest(entries=(MemoryEntryInput(kind="fact", text="ORION_2025_10K\nNet sales private"),))
        )
    async with open_builtin_runtime(config) as runtime:
        memory = runtime.memory.for_scope(scope.scope_id)
        questions = [
            (
                "How much were Orion's net sales for FY2025? Respond from the perspective of an investment analyst "
                "using only the details shown in the annual report. Give the response in millions of dollars.",
                [documents["ORION_2025_10K"][0]],
            ),
            (
                "What is Orion's FY2025 EBITDA less capex, using operating income and D&A from the cash flow statement?",
                documents["ORION_2025_10K"][:2],
            ),
            ("Where does Orion operate geographically in FY2025?", [documents["ORION_2025_10K"][2]]),
            ("Does Orion face any ongoing legal battles in FY2025?", [documents["ORION_2025_10K"][3]]),
        ]
        for question, expected in questions:
            citations = await search_pdf_pages(memory, question, limit=3)
            assert 0 < len(citations) <= 3
            assert len({citation.entry_id for citation in citations}) == len(citations)
            records = [await memory.get(GetMemoryEntryRequest(citation=citation)) for citation in citations]
            texts = [record.entry.text for record in records]
            for body in expected:
                assert f"ORION_2025_10K\n{body}" in texts, (question, texts)
            assert all("private" not in text for text in texts)
        single = await search_pdf_pages(memory, "Orion net sales FY2025", limit=1)
        assert len(single) == 1
        assert (await memory.get(GetMemoryEntryRequest(citation=single[0]))).entry.text == (
            "ORION_2025_10K\n" + documents["ORION_2025_10K"][0]
        )
