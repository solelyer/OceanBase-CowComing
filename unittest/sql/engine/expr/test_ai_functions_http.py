"""Native SQL/HTTP contracts for AI functions using a development provider.

The fixture intentionally returns shuffled data. Product code must recover row
identity, validate the whole response, and batch compatible SQL rows itself.
This is an independent regression, not the unpublished competition Mock.
"""

import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import subprocess
import threading


def vector(text, dimension):
    digest = hashlib.sha256(text.encode()).digest()
    return [(digest[i % len(digest)] - 128) / 128 for i in range(dimension)]


class Mock:
    def __init__(self, port=0):
        self.requests = []
        self.mode = None
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                owner.requests.append({"path": self.path, "body": payload, "authorization": self.headers.get("Authorization")})
                mode = owner.mode
                code = 200
                if "emb" in self.path:
                    texts = payload["input"]
                    if not isinstance(texts, list):
                        texts = [texts]
                    dimension = payload.get("dimensions", 768)
                    values = [vector(text, dimension) for text in texts]
                    if mode == "empty-vector":
                        values[0] = []
                    elif mode == "wrong-dimension":
                        values[0] = [1, 2]
                    elif mode == "non-numeric":
                        values[0][0] = "bad"
                    elif mode == "boolean-element":
                        values[0][0] = True
                    elif mode == "null-element":
                        values[0][0] = None
                    elif mode == "vector-object":
                        values[0] = {"value": 1}
                    if self.path.startswith("/api/"):
                        result = {"embeddings": values}
                        if mode == "bad-container":
                            result = {"embeddings": {"invalid": 1}}
                    else:
                        data = [{"index": i, "embedding": value} for i, value in enumerate(values)]
                        if mode == "missing-index":
                            del data[0]["index"]
                        elif mode == "negative-index":
                            data[0]["index"] = -1
                        elif mode == "fractional-index":
                            data[0]["index"] = 0.5
                        elif mode == "out-of-range-index":
                            data[0]["index"] = len(data)
                        elif mode == "duplicate-index" and len(data) > 1:
                            data[1]["index"] = data[0]["index"]
                        elif mode == "missing-result":
                            data.pop()
                        result = {"data": list(reversed(data))}
                        if mode == "bad-container":
                            result = {"data": {"invalid": 1}}
                elif "rerank" in self.path:
                    query = set(payload["query"].casefold().split())
                    results = [{"index": i, "relevance_score": len(query & set(text.casefold().split()))}
                               for i, text in enumerate(payload["documents"])]
                    if mode == "missing-result":
                        results.pop()
                    elif mode == "duplicate-index" and len(results) > 1:
                        results[1]["index"] = results[0]["index"]
                    elif mode == "out-of-range-index":
                        results[0]["index"] = len(results)
                    elif mode == "negative-index":
                        results[0]["index"] = -1
                    elif mode == "fractional-index":
                        results[0]["index"] = 0.5
                    elif mode == "missing-index":
                        del results[0]["index"]
                    elif mode == "invalid-score":
                        results[0]["relevance_score"] = "0.9"
                    elif mode == "null-score":
                        results[0]["relevance_score"] = None
                    result = {"results": list(reversed(results))}
                    if mode == "bad-container":
                        result = {"results": "invalid"}
                elif self.path.startswith("/api/generate"):
                    assert payload["stream"] is False
                    result = {"response": payload["prompt"]}
                    if mode == "empty-completion":
                        result = {"response": ""}
                    elif mode == "invalid-completion":
                        result = {"response": []}
                else:
                    content = next(message["content"] for message in payload["messages"] if message["role"] == "user")
                    result = {"choices": [{"message": {"content": content}}]}
                    if mode == "empty-completion":
                        result = {"choices": [{"message": {"content": ""}}]}
                    elif mode == "invalid-completion":
                        result = {"choices": [{"message": {"content": 123}}]}
                if mode == "root-array":
                    result = []
                elif mode == "http-error":
                    code = 400
                    result = {"error": "development fixture failure"}
                elif mode == "retry-once":
                    owner.mode = None
                    code = 503
                    result = {"error": "retry fixture"}
                response = json.dumps(result, ensure_ascii=False).encode()
                if mode == "invalid-json":
                    response = b"{incomplete json"
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_port

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def start_mock(port=0):
    return Mock(port)


def verify(mysql_command, environment):
    checks = []
    mock = Mock()
    models = []
    endpoints = []

    def literal(value):
        # Connection defaults retain MySQL backslash escaping.
        return "'" + value.replace("\\", "\\\\").replace("'", "''") + "'"

    def query(sql, success=True):
        result = subprocess.run(mysql_command + ["--default-character-set=utf8mb4", "--batch", "--raw",
                                                "--skip-column-names", "-Dtest"], input=sql,
                                env=environment, capture_output=True, text=True, timeout=40)
        if success:
            assert result.returncode == 0, (sql[:400], result.stderr)
        else:
            assert result.returncode != 0, f"Invalid input succeeded: {sql[:400]}"
        return result.stdout.rstrip("\n")

    def register(name, kind, provider, path, access_key="offline-fixture"):
        config = json.dumps({"type": kind, "model_name": name + "_upstream"})
        query(f"CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('{name}', {literal(config)})")
        models.append(name)
        endpoint = {"ai_model_name": name, "scope": "all", "provider": provider,
                    "url": f"http://127.0.0.1:{mock.port}{path}", "access_key": access_key}
        query(f"CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('{name}_ep', {literal(json.dumps(endpoint))})")
        endpoints.append(name + "_ep")

    def embed(text, dimension=None, name="native_embed"):
        dim = "" if dimension is None else f", {dimension}"
        return json.loads(query(f"SELECT AI_EMBED('{name}', {literal(text)}{dim})"))

    def rerank(documents, key=None):
        key_arg = "NULL" if key is None else literal(key)
        return json.loads(query(f"SELECT AI_RERANK('native_rerank', 'apple', {literal(json.dumps(documents))}, {key_arg})"))

    try:
        register("native_embed", "dense_embedding", "openai", "/v1/embeddings")
        register("native_complete", "completion", "openai", "/v1/chat/completions")
        register("native_rerank", "rerank", "siliconflow", "/v1/rerank")
        register("native_ollama", "completion", "oLlAmA", "/api/generate", "")
        register("native_ollama_embed", "dense_embedding", "OLLAMA", "/api/embed", "")
        checks.append("endpoint registration, provider case, Ollama empty key")
        assert embed("单行\n\\\"text") == vector("单行\n\\\"text", 768)
        checks.append("default dimension and finite numeric vector")
        assert embed("dimension", 7) == vector("dimension", 7)
        checks.append("explicit dimension")
        for dim in ["0", "-1", "NULL", "1.5", "'4'"]:
            before = len(mock.requests)
            query(f"SELECT AI_EMBED('native_embed', 'bad dimension', {dim})", success=False)
            assert len(mock.requests) == before
            checks.append(f"dimension {dim} rejected before HTTP")
        before = len(mock.requests)
        assert query("SELECT AI_EMBED('native_embed', NULL)") == "NULL"
        assert len(mock.requests) == before
        checks.append("NULL propagation without HTTP")
        large = "大文本" * 26000
        assert embed(large, 5) == vector(large, 5)
        checks.append("LONGTEXT content preserves full input")
        query("CREATE TABLE ai_native_rows(id INT PRIMARY KEY, content LONGTEXT, dim INT)")
        query("INSERT INTO ai_native_rows VALUES " + ",".join(
            f"({i}, {'NULL' if i % 7 == 0 else literal('row ' + str(i))}, {4 + i % 3})" for i in range(1, 277)))
        before = len(mock.requests)
        output = query("SELECT id, AI_EMBED('native_embed', content, 6) FROM ai_native_rows ORDER BY id")
        for line in output.splitlines():
            row, raw = line.split("\t")
            number = int(row)
            assert (raw == "NULL") if number % 7 == 0 else (json.loads(raw) == vector(f"row {number}", 6))
        requests = mock.requests[before:]
        assert sum(len(item["body"]["input"]) for item in requests) == 276 - 276 // 7
        assert any(len(item["body"]["input"]) > 1 for item in requests), "SQL evaluated every row as a separate request"
        checks.append(f"cross-row batches, NULL slots, shuffled index mapping ({len(requests)} requests)")
        output = query("SELECT id, AI_EMBED('native_embed', content, dim) FROM ai_native_rows WHERE id > 21 AND id < 70 ORDER BY id LIMIT 23")
        assert len(output.splitlines()) == 23
        for line in output.splitlines():
            row, raw = line.split("\t")
            number = int(row)
            assert (raw == "NULL") if number % 7 == 0 else (json.loads(raw) == vector(f"row {number}", 4 + number % 3))
        checks.append("filter, LIMIT and per-row dimension groups")
        before = len(mock.requests)
        assert query("SELECT AI_EMBED('native_embed', content) FROM ai_native_rows WHERE id < 0") == ""
        assert len(mock.requests) == before
        checks.append("empty scan makes no HTTP request")
        for mode in ["missing-index", "negative-index", "out-of-range-index", "fractional-index", "bad-container",
                     "missing-result", "empty-vector", "non-numeric", "boolean-element", "null-element", "vector-object",
                     "wrong-dimension", "root-array", "invalid-json", "http-error"]:
            mock.mode = mode
            query("SELECT AI_EMBED('native_embed', 'invalid response', 5)", success=False)
            checks.append(f"embedding rejects {mode}")
        mock.mode = "duplicate-index"
        query("SELECT AI_EMBED('native_embed', content, 5) FROM ai_native_rows WHERE id BETWEEN 1 AND 3", success=False)
        checks.append("batch embedding rejects duplicate indices")
        mock.mode = "retry-once"
        assert embed("retry", 5) == vector("retry", 5)
        checks.append("transient HTTP failure retries without concatenating responses")
        docs = ["banana", "apple", "vegetable", "apple"]
        assert [item["index"] for item in rerank(docs)] == [1, 3, 0, 2]
        checks.append("rerank NULL key, integer scores, unordered response and stable ties")
        docs = ["apple" if i % 4 == 0 else "banana" for i in range(53)]
        expected = sorted(range(len(docs)), key=lambda i: docs[i] != "apple")
        before = len(mock.requests)
        assert [item["index"] for item in rerank(docs)] == expected
        assert [len(item["body"]["documents"]) for item in mock.requests[before:]] == [20, 20, 13]
        checks.append("rerank unequal batches, global indices and global stable ordering")
        objects = [{"id": str(i), "content": doc, "extra": {"keep": i}} for i, doc in enumerate(docs)]
        ranked = rerank(objects, "content")
        assert [item["id"] for item in ranked] == [str(i) for i in expected]
        assert all(item["extra"]["keep"] == int(item["id"]) and "_model_score" in item for item in ranked)
        checks.append("object rerank batches preserve IDs and document fields")
        for mode in ["missing-result", "duplicate-index", "out-of-range-index", "negative-index", "fractional-index",
                     "missing-index", "invalid-score", "null-score", "bad-container", "root-array"]:
            mock.mode = mode
            query("SELECT AI_RERANK('native_rerank', 'apple', '[\"apple\",\"banana\"]', NULL)", success=False)
            checks.append(f"rerank rejects {mode}")
        mock.mode = None
        before = len(mock.requests)
        for documents in ["[1]", "[{\"other\":\"text\"}]", "{}"]:
            query(f"SELECT AI_RERANK('native_rerank','apple',{literal(documents)}, 'content')", success=False)
        assert len(mock.requests) == before
        checks.append("invalid document structure rejected before HTTP")
        assert query("SELECT JSON_UNQUOTE(AI_COMPLETE('native_ollama', '你好 Ollama', JSON_OBJECT('temperature',0.25,'max_tokens',9)))") == "你好 Ollama"
        assert mock.requests[-1]["body"]["stream"] is False
        assert mock.requests[-1]["body"]["options"] == {"temperature": 0.25, "num_predict": 9}
        assert mock.requests[-1]["authorization"] is None
        checks.append("Ollama non-streaming native generate, options and no authorization")
        query("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('native_ollama_ep', '{\"provider\":\"Ollama\"}')")
        assert query("SELECT provider FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE endpoint_name='native_ollama_ep'") == "Ollama"
        checks.append("Ollama provider alteration persists original case")
        assert embed("Ollama 向量", 5, "native_ollama_embed") == vector("Ollama 向量", 5)
        checks.append("Ollama native embedding")
        for mode in ["empty-completion", "invalid-completion", "root-array", "invalid-json", "http-error"]:
            mock.mode = mode
            for name in ["native_complete", "native_ollama"]:
                query(f"SELECT AI_COMPLETE('{name}', 'invalid response')", success=False)
            checks.append(f"completion providers reject {mode}")
        mock.mode = None
        assert query("SELECT 1+2") == "3"
        checks.append("ordinary SQL remains usable after errors")
        return {"passed": len(checks), "checks": checks, "provider_requests": len(mock.requests),
                "embedding_batch_sizes": [len(item["body"]["input"]) for item in mock.requests if "emb" in item["path"]]}
    finally:
        mock.mode = None
        try:
            query("DROP TABLE IF EXISTS ai_native_rows")
            for name in models:
                query(f"CALL DBMS_AI_SERVICE.DROP_AI_MODEL('{name}')")
            for name in endpoints:
                query(f"CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('{name}')")
        finally:
            mock.close()
