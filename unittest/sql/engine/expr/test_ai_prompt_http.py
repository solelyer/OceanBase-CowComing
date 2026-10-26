"""Native SQL regression using a local echo provider; no online model required.

Call verify(mysql_command, environment) against a dedicated test instance.
The fixture creates its own completion model and removes it before returning.
"""

import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import subprocess
import threading


def verify(mysql_command, environment):
    requests = []
    checks = []

    class EchoHandler(BaseHTTPRequestHandler):
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            requests.append(payload)
            content = next(
                message["content"]
                for message in payload["messages"]
                if message["role"] == "user"
            )
            response_content = {"empty-response": "", "invalid-response": None}.get(
                content, content
            )
            response = json.dumps(
                {"choices": [{"message": {"content": response_content}}]}
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

        def log_message(self, *_args):
            pass

    def query(sql, success=True):
        result = subprocess.run(
            mysql_command
            + [
                "--default-character-set=utf8mb4",
                "--batch",
                "--raw",
                "--skip-column-names",
                "-e",
                sql,
            ],
            env=environment,
            capture_output=True,
            text=True,
            timeout=20,
        )
        if success:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0, f"Invalid input succeeded: {sql}"
        return result.stdout.rstrip("\n")

    def complete(prompt, config="NULL"):
        return query(
            f"SELECT JSON_UNQUOTE(AI_COMPLETE('prompt_contract', {prompt}, {config}))"
        )

    server = HTTPServer(("127.0.0.1", 0), EchoHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    created_model = False
    created_endpoint = False
    try:
        query(
            "CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('prompt_contract', "
            '\'{"type":"completion","model_name":"echo-fixture"}\')'
        )
        created_model = True
        endpoint = json.dumps(
            {
                "ai_model_name": "prompt_contract",
                "scope": "all",
                "provider": "openai",
                "url": f"http://127.0.0.1:{server.server_port}/v1/chat/completions",
                "access_key": "offline-test",
            }
        )
        query(
            f"CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('prompt_contract_ep', '{endpoint}')"
        )
        created_endpoint = True

        assert complete("'plain prompt'") == "plain prompt"
        checks.append("explicit NULL config")
        assert (
            query(
                "SELECT JSON_UNQUOTE(AI_COMPLETE('prompt_contract', 'two arguments'))"
            )
            == "two arguments"
        )
        checks.append("optional config")
        assert complete("AI_PROMPT('{0}/{0}/{0}', REPEAT('word', 1000))") == "/".join(
            ["word" * 1000] * 3
        )
        checks.append("repeated placeholder capacity")
        assert json.loads(complete("AI_PROMPT('{0}', JSON_ARRAY(1, TRUE, NULL))")) == [
            1,
            True,
            None,
        ]
        checks.append("JSON array argument")
        assert json.loads(
            complete("AI_PROMPT('{0}', JSON_OBJECT('text', '你好'))")
        ) == {"text": "你好"}
        checks.append("JSON object argument")
        assert (
            complete("AI_PROMPT('outer {0}', AI_PROMPT('inner {0}', '世界'))")
            == "outer inner 世界"
        )
        checks.append("nested prompt")
        escaped = '世界\n"\\\0'
        assert (
            complete(
                "AI_PROMPT('{0}|{0}', CONCAT('世界', CHAR(10), CHAR(34), CHAR(92), CHAR(0)))"
            )
            == escaped + "|" + escaped
        )
        checks.append("Unicode, newline, quotes, slash and NUL")
        assert (
            json.loads(complete("AI_PROMPT('{0}', JSON_EXTRACT('true', '$'))")) is True
        )
        checks.append("JSON scalar argument")
        assert (
            complete(
                "AI_PROMPT(CONCAT(REPEAT('t', 70000), '{0}'), REPEAT('语', 30000))"
            )
            == "t" * 70000 + "语" * 30000
        )
        checks.append("large LOB template and argument")
        assert (
            complete("'config'", "JSON_OBJECT('temperature', 0.25, 'max_tokens', 16)")
            == "config"
        )
        assert requests[-1]["temperature"] == 0.25 and requests[-1]["max_tokens"] == 16
        checks.append("native JSON config")
        assert (
            complete("'text config'", '\'{"temperature":0.5,"max_tokens":8}\'')
            == "text config"
        )
        assert requests[-1]["temperature"] == 0.5 and requests[-1]["max_tokens"] == 8
        checks.append("text JSON config")

        for template in [
            "{1}",
            "{-1}",
            "{+0}",
            "{9223372036854775808}",
            "{99999999999999999999999999999999999}",
        ]:
            before = len(requests)
            query(
                f"SELECT AI_COMPLETE('prompt_contract', AI_PROMPT('{template}', 'only argument'))",
                success=False,
            )
            assert len(requests) == before, (
                "Invalid placeholder sent a provider request"
            )
            checks.append(f"invalid index {template}")
        before = len(requests)
        query(
            "SELECT AI_COMPLETE('prompt_contract', 'config', JSON_ARRAY(1))",
            success=False,
        )
        assert len(requests) == before
        checks.append("non-object config rejected before HTTP")
        for text in ["empty-response", "invalid-response"]:
            query(f"SELECT AI_COMPLETE('prompt_contract', '{text}')", success=False)
            checks.append(f"reject {text}")

        return {
            "passed": len(checks),
            "checks": checks,
            "provider_requests": len(requests),
            "prompt_hashes": [
                hashlib.sha256(
                    json.dumps(item, ensure_ascii=False, sort_keys=True).encode()
                ).hexdigest()
                for item in requests
            ],
        }
    finally:
        try:
            if created_model:
                query("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('prompt_contract')")
            if created_endpoint:
                query(
                    "CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('prompt_contract_ep')"
                )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
