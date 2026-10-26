# JSON and large-text AI prompts

`AI_PROMPT(template, args...)` preserves the template and arguments as a JSON
object. Templates and string arguments support TEXT and LONGTEXT, including
out-of-row LOB data. JSON arguments preserve their original structure, including
arrays, objects and scalar values. SQL NULL arguments remain invalid.

```sql
SELECT AI_PROMPT('{0}/{0}/{1}', JSON_ARRAY(1, TRUE), JSON_OBJECT('k', 2));
```

When `AI_COMPLETE` consumes this object, decimal placeholders such as `{0}`
select an argument. Repeated placeholders can repeat arbitrarily long argument
values. String values are inserted verbatim; JSON values are serialized as JSON.
A nested AI_PROMPT object is rendered recursively. Signed, overflowing and
out-of-range numeric indices fail before a provider request is sent. Other text
is preserved, including Unicode, embedded NUL, quotes and line breaks.

AI_COMPLETE accepts omitted configuration, SQL NULL, JSON object configuration,
or a string containing a JSON object. Arrays and scalar configuration are
rejected. OpenAI completion responses must contain a nonempty string at
`choices[0].message.content`.

## Verification

The existing `tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test`
is unchanged. Run it with the repository's mysqltest client against an isolated
instance, using the corresponding `r/mysql/ai_prompt.result` file.

`unittest/sql/engine/expr/test_ai_prompt_http.py` provides additional native SQL
regression checks through a local echo HTTP provider. Its `verify(mysql_command,
environment)` function accepts a list of MySQL client connection arguments and
an environment mapping. It creates and removes the `prompt_contract` model and
endpoint, so use a dedicated test instance. It exercises the actual expression,
endpoint resolver, HTTP client, prompt rendering and response parsing. It does
not need an online model or model credentials.

These checks cover the prompt/completion path; they do not establish acceptance
of Embedding, Ollama, reranking, cross-row batching, File SQL or AI COLUMN.
