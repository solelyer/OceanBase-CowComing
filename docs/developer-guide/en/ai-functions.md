# AI function execution and provider contracts

`AI_PROMPT` preserves JSON parameters and reads TEXT/LONGTEXT data through the
LOB interface. `AI_COMPLETE` accepts omitted or SQL NULL configuration, or a JSON
object containing provider options. Completion responses must contain a nonempty
string. Prompt template behavior is documented in [AI prompts](ai-prompt.md).

`AI_EMBED(model, content [, dimension])` returns a JSON vector as LONGTEXT. SQL
NULL content produces SQL NULL without sending an HTTP request. An explicit
dimension must be a positive integer. Responses must contain one nonempty vector
per input, with matching dimensions and finite numeric elements. OpenAI response
indices must form a complete permutation of the submitted input positions;
response order is independent of SQL row order.

The native expression batch callback groups rows by model and dimension and sends
at most 128 inputs in one request. Filtered rows, NULL content, and previously
evaluated positions are excluded. Outputs are copied into expression result
storage using the original SQL row map. A provider error fails the SQL statement.

Ollama Endpoint provider names are case insensitive, and their access key may be
empty. Completion uses the native `/api/generate` protocol with `stream: false`,
and embedding uses `/api/embed` with an input array. Generation configuration is
mapped to `options`, including `max_tokens` to `num_predict`. The URL remains the
configured Endpoint URL. JSON Content-Type is sent without an Authorization
header. Provider configuration remains visible through the existing Endpoint
administration API and system view.

`AI_RERANK` accepts either a JSON array of strings, with an omitted or SQL NULL
fourth argument, or an array of objects and a string text-field name. Both forms
use batches of 20 documents. Each response must cover its batch exactly once with
valid integer indices and finite numeric scores. Results are sorted by descending
score; ties preserve original document order across batches. String results use
global indices. Object results preserve their original fields and IDs and add
`_model_score`.

HTTP response parsing rejects non-object JSON roots before provider decoding.
Retries discard the preceding response body. libcurl owns a copy of request data
for the duration of a request, including concurrent requests.

## Development regression

`unittest/sql/engine/expr/test_ai_functions_http.py` exposes
`verify(mysql_command, environment)` for an isolated, source-built instance. It
starts its own local protocol fixture and covers native SQL batch execution,
shuffled responses, NULL slots, different dimensions, filtering, LIMIT, LOB input,
provider administration, malformed responses, and transient HTTP failures.

The same module provides `start_mock(port)` to run the unchanged published
`ai_function_enhancement.test` against a development fixture. The competition's
official port-8000 Mock and the listed official C++ unit tests are not included
in this source snapshot. Passing this development regression and the published
SQL script does not assert a hidden-test result or competition score.
