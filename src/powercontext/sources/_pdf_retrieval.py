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

"""Offline financial query expansion and ranking of real Memory search hits."""

from __future__ import annotations

import math
import re
import unicodedata
from collections import Counter
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from powercontext.builtin.artifacts.memory.models import MemoryHit


_STOP = frozenset([
    "a",
    "an",
    "the",
    "of",
    "to",
    "in",
    "and",
    "or",
    "for",
    "from",
    "on",
    "at",
    "by",
    "with",
    "is",
    "are",
    "was",
    "were",
    "be",
    "been",
    "being",
    "what",
    "which",
    "who",
    "how",
    "why",
    "when",
    "where",
    "does",
    "do",
    "did",
    "has",
    "have",
    "had",
    "much",
    "many",
    "any",
    "as",
    "it",
    "its",
    "s",
    "d",
    "l",
    "fy",
    "fiscal",
    "year",
    "years",
    "annual",
    "report",
    "reported",
    "reporting",
    "company",
    "corporation",
    "inc",
    "can",
    "could",
    "would",
    "should",
    "will",
    "may",
    "must",
    "only",
    "using",
    "use",
    "used",
    "based",
    "according",
    "given",
    "give",
    "answer",
    "respond",
    "response",
    "question",
    "calculate",
    "define",
    "assuming",
    "assume",
    "perspective",
    "analyst",
    "details",
    "shown",
    "show",
    "within",
    "rely",
    "relying",
    "state",
    "then",
    "if",
    "not",
    "explicitly",
    "directly",
    "outlined",
    "quantity",
    "amount",
    "total",
    "approximately",
    "dollars",
    "dollar",
    "usd",
    "million",
    "millions",
    "billion",
    "both",
    "during",
    "ended",
    "end",
    "per",
    "each",
    "following",
    "please",
    "primarily",
    "important",
    "ongoing",
    "materially",
])

# Financial vocabulary, independent of any issuer, report or benchmark question.
# Alternatives stay in short queries: a long union would raise Memory's lexical
# admission threshold and discard pages containing just one relevant concept.
_CONCEPTS = (
    (
        "geographic geography geographically geographies region country countries segment segments division divisions",
        (
            "geographic information",
            "geographical regions",
            "countries international",
            "business segments",
            "reportable segments",
            "divisions operations",
        ),
    ),
    (
        "legal litigation lawsuit lawsuits battle battles",
        ("legal proceedings", "litigation contingencies", "lawsuits claims"),
    ),
    ("revenue revenues sales turnover", ("net revenue", "net sales")),
    ("income profit profits earnings pnl", ("income statement", "operating profit", "net earnings")),
    ("ebitda", ("operating income", "operating profit", "depreciation amortization", "cash flows")),
    ("capex expenditure expenditures", ("capital expenditures", "capital spending", "property equipment")),
    ("intensive intensity", ("property equipment", "capital expenditures", "total assets", "balance sheet")),
    ("investment investments securities", ("short term", "marketable securities", "debt securities")),
    ("cash cashflow cashflows", ("cash flows", "operating activities")),
    ("margin margins", ("net revenue", "net sales", "gross profit", "operating income")),
    (
        "balance assets liabilities liquidity ratio ratios",
        ("balance sheet", "financial position", "current assets", "current liabilities"),
    ),
    ("debt borrowing borrowings leverage", ("debt borrowings", "long term", "credit facilities")),
    ("restructure restructuring", ("restructuring charges", "restructuring costs", "income statement")),
    (
        "dividend dividends repurchase repurchases buyback buybacks",
        ("cash dividends", "share repurchases", "equity financing"),
    ),
)


def _words(text: str) -> tuple[str, ...]:
    normalized = unicodedata.normalize("NFKC", text).casefold()
    normalized = re.sub(r"\bfy\s*(\d{4})\b", r"\1", normalized)
    normalized = re.sub(r"\bd\s*&\s*a\b", "depreciation amortization", normalized)
    normalized = re.sub(r"\bp\s*&\s*l\b", "income statement", normalized)
    normalized = re.sub(r"\bpp\s*&\s*e\b", "property plant equipment", normalized)
    normalized = re.sub(r"\binvestment analyst\b", "", normalized)
    return tuple(re.findall(r"[^\W_]+", normalized))


def _stem(word: str) -> str:
    if word.endswith("ies") and len(word) > 4:
        word = word[:-3] + "y"
    elif word.endswith("s") and not word.endswith(("ss", "us")) and len(word) > 3:
        word = word[:-1]
    return {
        "geography": "geographic",
        "geographical": "geographic",
        "geographically": "geographic",
        "operating": "operate",
        "operation": "operate",
        "operated": "operate",
        "earning": "income",
        "profit": "income",
        "sale": "revenue",
        "turnover": "revenue",
    }.get(word, word)


def _expansions(question: str) -> tuple[str, ...]:
    words = {_stem(word) for word in _words(question)}
    return tuple(
        dict.fromkeys(
            query
            for triggers, queries in _CONCEPTS
            if words.intersection(_stem(word) for word in triggers.split())
            for query in queries
        )
    )


def pdf_queries(question: str) -> tuple[str, ...]:
    """Use the full query and short recall queries through the native public API."""

    terms = tuple(dict.fromkeys(word for word in _words(question) if word not in _STOP))
    topical = tuple(word for word in terms if not word.isdigit())
    queries = [" ".join(terms) or question]
    queries.extend(" ".join(topical[index : index + 2]) for index in range(0, len(topical), 2))
    queries.extend(_expansions(question))
    return tuple(dict.fromkeys(queries))


def _issuer_terms(document_id: str) -> tuple[str, ...]:
    # A filing suffix is not part of the issuer, including quarterly periods and
    # dated releases. Preserve alphanumeric issuer names such as 3M.
    name = re.split(r"[_\s-](?:19|20)\d{2}(?:q[1-4])?(?=[_\s-]|$)", document_id.casefold(), maxsplit=1)[0]
    return tuple(word for word in _words(name) if word not in _STOP and word not in {"form", "financial", "statements"})


def _issuer_matches(document_id: str, question: str) -> bool:
    terms = _issuer_terms(document_id)
    compact_question = "".join(_words(question))
    acronyms = {word.casefold() for word in re.findall(r"\b[A-Z][A-Z0-9]{2,7}\b", question)}
    issuer = "".join(terms)
    return bool(terms) and (
        issuer in compact_question
        or sum(word in _words(question) for word in terms) >= max(1, math.ceil(len(terms) / 2))
        or any(issuer.startswith(word) for word in acronyms)
    )


def pdf_issuer_queries(document_ids: tuple[str, ...], question: str) -> tuple[str, ...]:
    """Search complete observed filing IDs; Analyzer keeps underscores in tokens."""

    return tuple(document for document in sorted(set(document_ids)) if _issuer_matches(document, question))


def _heading_bonus(body: str, question_terms: set[str]) -> float:
    """Prefer actual statement headings over incidental narrative mentions."""

    heading = " ".join(_words(body)[:60])
    bonus = 0.0
    if question_terms.intersection({
        "income",
        "revenue",
        "ebitda",
        "ebit",
        "margin",
        "restructuring",
        "expense",
        "return",
    }) and re.search(r"(?:consolidated )?statements? of (?:income|operations|earnings)|income statements?", heading):
        bonus += 12.0
    if question_terms.intersection({
        "cash",
        "flow",
        "ebitda",
        "capex",
        "expenditure",
        "depreciation",
        "amortization",
        "dividend",
        "repurchase",
    }) and re.search(r"statements? of cash flows?|cash flow statements?", heading):
        bonus += 12.0
    if question_terms.intersection({
        "asset",
        "liability",
        "balance",
        "ratio",
        "liquidity",
        "debt",
        "intensive",
    }) and re.search(r"balance sheets?|statements? of financial position", heading):
        bonus += 12.0
    if "geographic" in question_terms and re.search(r"geographic|our divisions|reportable segments", heading):
        bonus += 6.0
    if question_terms.intersection({"segment", "division"}) and re.search(
        r"segment (?:results|information|reporting)|business segments?|reportable segments?", heading
    ):
        bonus += 6.0
    return bonus


def rank_pdf_hits(hits: tuple[MemoryHit, ...], question: str) -> tuple[MemoryHit, ...]:
    """BM25 over candidate bodies, with issuer/year priors kept out of body TF."""

    if not hits:
        return ()
    parts = [hit.text.partition("\n") for hit in hits]
    documents = {document for document, _, _ in parts}
    matched_documents = {document for document in documents if _issuer_matches(document, question)}
    issuer_words = {_stem(word) for document in matched_documents for word in _issuer_terms(document)}
    primary = {_stem(word) for word in _words(question) if word not in _STOP and not word.isdigit()} - issuer_words
    weights = {_stem(word): 0.4 for query in _expansions(question) for word in _words(query) if word not in _STOP}
    weights.update(dict.fromkeys(primary, 1.0))
    frequencies = [Counter(_stem(word) for word in _words(body) if word not in _STOP) for _, _, body in parts]
    document_frequency: Counter[str] = Counter()
    for frequency in frequencies:
        document_frequency.update(frequency.keys())
    lengths = [sum(frequency.values()) for frequency in frequencies]
    average = max(sum(lengths) / len(lengths), 1)
    years = set(re.findall(r"(?:19|20)\d{2}", question))
    scores = []
    for index, (document, _, body) in enumerate(parts):
        frequency = frequencies[index]
        normalizer = 1.2 * (0.7 + 0.3 * lengths[index] / average)
        score = sum(
            weight
            * math.log1p((len(hits) - document_frequency[word] + 0.5) / (document_frequency[word] + 0.5))
            * (frequency[word] * 2.2 / (frequency[word] + normalizer))
            for word, weight in weights.items()
            if frequency[word]
        )
        score += _heading_bonus(body, primary)
        if matched_documents and document not in matched_documents:
            score *= 0.15
        if years and years.intersection(re.findall(r"(?:19|20)\d{2}", document)):
            score *= 1.25
        scores.append(score)
    return tuple(hits[index] for index in sorted(range(len(hits)), key=lambda index: (-scores[index], index)))
