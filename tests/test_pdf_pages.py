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

from powercontext.sources.pdf_pages import PDF_PAGE_SOURCE_DEFINITION, PDFDocumentInput, parse_pdf_document


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
