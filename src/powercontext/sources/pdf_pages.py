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

"""Complete physical PDF pages as immutable, revision-specific evidence."""

from __future__ import annotations

import hashlib
import io
import json
from typing import TYPE_CHECKING

from pydantic import BaseModel, Field, field_validator
from pypdf import PdfReader

from powercontext.builtin.artifacts.memory.models import MemoryCitation
from powercontext.sources._pdf_retrieval import pdf_queries, rank_pdf_hits
from powercontext.sources.definitions import AdapterSourceDefinition
from powercontext.sources.models import Source, SourceMaterialization
from powercontext.sources.projections import TEXT_EVIDENCE_PROJECTION_KEY, TextEvidence

if TYPE_CHECKING:
    from powercontext.builtin.runtime.application import ScopedMemoryApplication


class PDFDocumentInput(BaseModel):
    """Original complete PDF bytes with a caller-chosen document identity."""

    document_id: str
    content: bytes


class PDFPageInput(BaseModel):
    """One physical page, anchored to the digest of the entire original file."""

    document_id: str
    document_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    page_number: int = Field(gt=0, strict=True)
    text: str

    @field_validator("document_id")
    @classmethod
    def require_document_id(cls, value: str) -> str:
        if not value.strip():
            raise ValueError("document_id must not be blank")  # noqa: TRY003
        return value


def parse_pdf_document(value: PDFDocumentInput) -> tuple[PDFPageInput, ...]:
    """Extract every physical page atomically, including pages with no text."""

    if not value.document_id.strip() or not value.content:
        raise ValueError("PDF requires a nonblank document ID and complete file bytes")  # noqa: TRY003
    digest = hashlib.sha256(value.content).hexdigest()
    try:
        reader = PdfReader(io.BytesIO(value.content), strict=True)
        if reader.is_encrypted and not reader.decrypt(""):
            raise ValueError("PDF needs a nonempty password")  # noqa: TRY003, TRY301
        return tuple(
            PDFPageInput(
                document_id=value.document_id,
                document_sha256=digest,
                page_number=number,
                text=page.extract_text() or "",
            )
            for number, page in enumerate(reader.pages, start=1)
        )
    except Exception as error:
        # Parser failures must not expose a partially extracted page sequence.
        raise ValueError("PDF could not be completely parsed") from error  # noqa: TRY003


class PDFPageSource(Source):
    """Captured full page with its original file identity."""

    page: PDFPageInput


class _PDFPageAdapter:
    name = "pdf-page"
    input_class: type[PDFPageInput] = PDFPageInput
    source_class: type[PDFPageSource] = PDFPageSource

    async def resolve(self, value: PDFPageInput, /) -> PDFPageSource:
        identity = json.dumps(
            [value.document_id, value.document_sha256, value.page_number], ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        return PDFPageSource(
            name="pdf:" + hashlib.sha256(identity).hexdigest(),
            materialization=SourceMaterialization.CAPTURED,
            page=value,
        )

    async def read(self, source: PDFPageSource, /) -> PDFPageInput:
        return source.page


class _PDFPageProjection:
    name = TEXT_EVIDENCE_PROJECTION_KEY.name
    version = TEXT_EVIDENCE_PROJECTION_KEY.version
    source_class: type[PDFPageSource] = PDFPageSource
    output_class: type[BaseModel] = TextEvidence

    def project(self, source: PDFPageSource, /) -> TextEvidence:
        page = source.page
        return TextEvidence(
            source_type="pdf-page",
            source_id=source.name,
            content=f"{page.document_id}\n{page.text}",
            metadata={
                "document_id": page.document_id,
                "document_sha256": page.document_sha256,
                "page_number": page.page_number,
            },
        )


PDF_PAGE_SOURCE_DEFINITION = AdapterSourceDefinition(_PDFPageAdapter(), projections=(_PDFPageProjection(),))


async def search_pdf_pages(
    memory: ScopedMemoryApplication, question: str, limit: int = 10
) -> tuple[MemoryCitation, ...]:
    """Search the scoped native Memory index and return its exact page citations."""

    from powercontext.builtin.runtime.models import SearchMemoryRequest

    if type(limit) is not int or limit < 1 or not question.strip():
        raise ValueError("PDF search requires a question and positive limit")  # noqa: TRY003
    candidates = {}
    # Search more than the final page budget before applying financial vocabulary
    # and document identity. Every candidate and citation still comes from Memory.
    for query in pdf_queries(question):
        result = await memory.search(SearchMemoryRequest(query=query, mode="fts", limit=max(80, limit * 4)))
        for hit in result.hits:
            identity = (hit.memory_ref.artifact_id, hit.entry_id, hit.entry_version_id)
            candidates.setdefault(identity, hit)
    return tuple(
        MemoryCitation(memory_ref=hit.memory_ref, entry_id=hit.entry_id, entry_version_id=hit.entry_version_id)
        for hit in rank_pdf_hits(tuple(candidates.values()), question)[:limit]
    )


__all__ = [
    "PDF_PAGE_SOURCE_DEFINITION",
    "PDFDocumentInput",
    "PDFPageInput",
    "PDFPageSource",
    "parse_pdf_document",
    "search_pdf_pages",
]
