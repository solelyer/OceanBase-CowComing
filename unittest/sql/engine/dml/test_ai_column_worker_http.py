"""Native asynchronous AI columns, DDL backfill and committed crash recovery."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import subprocess
import threading
import time


def verify(mysql_command, environment, restart_instance):
    checks = []
    requests = []
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            user = next((entry["content"] for entry in body.get("messages", []) if entry["role"] == "user"), "")
            system = next((entry["content"] for entry in body.get("messages", []) if entry["role"] == "system"), "")
            with lock:
                requests.append(user)
            if "slow" in user or "restart" in user:
                time.sleep(1.5)
            status = 200
            if user in ("retry-server", "retry-rate"):
                status = 503 if user == "retry-server" else 429
                content = "temporary failure"
            elif "malformed" in user:
                content = "invalid model output"
            elif "small-int:" in user:
                content = "200"
            elif "huge-decimal:" in user:
                content = "999.99"
            elif "decimal:" in user:
                content = "12.34"
            elif "unsigned:" in user:
                content = "18446744073709551615"
            elif "unicode:" in user:
                content = "世界"
            elif "base-10 integer" in system:
                content = "7"
            else:
                content = json.dumps({"input": user}) if "valid JSON value" in system else "mock:" + user
            payload = json.dumps({"choices": [{"message": {"content": content}}]}).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            try:
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    def query(sql, error=None):
        result = subprocess.run(mysql_command + ["--default-character-set=utf8mb4", "--batch", "--raw", "--skip-column-names", "-Dtest"],
                                input=sql, env=environment, text=True, capture_output=True, timeout=45)
        if error is None:
            assert result.returncode == 0, (sql, result.stdout, result.stderr)
        else:
            assert result.returncode != 0 and f"ERROR {error} " in result.stderr, (sql, result.stderr)
        return result.stdout.rstrip('\n')

    def wait_value(sql, expected, seconds=20):
        deadline = time.monotonic() + seconds
        actual = None
        while time.monotonic() < deadline:
            actual = query(sql)
            if actual == expected:
                return
            time.sleep(0.1)
        raise AssertionError((sql, actual, expected))

    def wait_request(text):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            with lock:
                if text in requests:
                    return
            time.sleep(0.05)
        raise AssertionError(("Model request did not arrive", text, requests))

    def tasks(table):
        return ("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE table_id="
                f"(SELECT table_id FROM oceanbase.__all_table WHERE table_name='{table}')")

    tables = ["ai_extra_copy", "ai_extra_ddl", "ai_extra_mixed", "ai_extra_null", "ai_extra_delete", "ai_extra_restart", "ai_extra_types", "ai_extra_retry"]
    try:
        query("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('ai_extra_model','{\"type\":\"completion\",\"model_name\":\"development\"}')")
        query("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('ai_extra_embed','{\"type\":\"dense_embedding\",\"model_name\":\"development\"}')")
        endpoint = json.dumps({"ai_model_name": "ai_extra_model", "url": f"http://127.0.0.1:{server.server_port}/v1/chat/completions",
                               "access_key": "development", "provider": "openai", "request_model_name": "development"})
        query("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ai_extra_endpoint','" + endpoint + "')")
        embed_endpoint = json.dumps({"ai_model_name": "ai_extra_embed", "url": f"http://127.0.0.1:{server.server_port}/v1/embeddings",
                                     "access_key": "development", "provider": "openai", "request_model_name": "development"})
        query("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ai_extra_embed_ep','" + embed_endpoint + "')")
        query("CREATE TABLE ai_extra_ddl(id BIGINT PRIMARY KEY,content VARCHAR(100)); INSERT INTO ai_extra_ddl VALUES(1,'existing'); "
              "ALTER TABLE ai_extra_ddl ADD result JSON AI COLUMN(AI_COMPLETE('ai_extra_model',CONCAT('first:',content))) WITH(max_retries=0)")
        wait_value("SELECT JSON_UNQUOTE(JSON_EXTRACT(result,'$.input')) FROM ai_extra_ddl", "first:existing")
        original_ids = query("SELECT state_column_id,request_column_id,result_spec_column_id FROM oceanbase.__all_ai_column "
                             "WHERE table_id=(SELECT table_id FROM oceanbase.__all_table WHERE table_name='ai_extra_ddl')")
        checks.append("ALTER ADD backfills existing committed rows through native transactional RETRY")
        query("ALTER TABLE ai_extra_ddl MODIFY result JSON AI COLUMN(AI_COMPLETE('ai_extra_model',CONCAT('changed:',content))) WITH(max_retries=0)")
        wait_value("SELECT JSON_UNQUOTE(JSON_EXTRACT(result,'$.input')) FROM ai_extra_ddl", "changed:existing")
        assert query("SELECT state_column_id,request_column_id,result_spec_column_id FROM oceanbase.__all_ai_column "
                     "WHERE table_id=(SELECT table_id FROM oceanbase.__all_table WHERE table_name='ai_extra_ddl')") == original_ids
        checks.append("ALTER MODIFY reuses hidden columns and invalidates a previous definition's results")
        with lock:
            count = len(requests)
        query("ALTER TABLE ai_extra_ddl ADD score BIGINT AI COLUMN(AI_COMPLETE('ai_extra_model',content)) WITH(max_retries=0)")
        wait_value("SELECT score,JSON_UNQUOTE(JSON_EXTRACT(result,'$.input')) FROM ai_extra_ddl", "7\tchanged:existing")
        time.sleep(1.1)
        with lock:
            assert len(requests) == count + 1, requests[count:]
        checks.append("adding another AI column preserves completed columns and avoids repeated inference")
        query("ALTER TABLE ai_extra_ddl DROP COLUMN content", error=1235)
        query("ALTER TABLE ai_extra_ddl RENAME COLUMN content TO renamed", error=1235)
        checks.append("DDL refuses to remove or rename an input column still referenced by an AI definition")
        query("ALTER TABLE ai_extra_ddl DROP COLUMN score")
        # Native instant DROP can retain renamed unused physical columns until reclamation.
        active_hidden = query("SELECT COUNT(*) FROM oceanbase.__all_column WHERE is_hidden=1 AND LEFT(column_name,5)='__ai_' AND table_id="
                              "(SELECT table_id FROM oceanbase.__all_table WHERE table_name='ai_extra_ddl')")
        assert active_hidden == "3", active_hidden
        assert query("SELECT COUNT(*) FROM oceanbase.__all_ai_column WHERE table_id="
                     "(SELECT table_id FROM oceanbase.__all_table WHERE table_name='ai_extra_ddl')") == "1"
        query("UPDATE ai_extra_ddl SET content='after-drop' WHERE id=1")
        wait_value("SELECT JSON_UNQUOTE(JSON_EXTRACT(result,'$.input')) FROM ai_extra_ddl", "changed:after-drop")
        checks.append("dropping an AI column removes its metadata and exactly its three hidden columns")
        query("CREATE TABLE ai_extra_copy LIKE ai_extra_ddl; INSERT INTO ai_extra_copy(id,content) VALUES(2,'copied');")
        wait_value("SELECT JSON_UNQUOTE(JSON_EXTRACT(result,'$.input')) FROM ai_extra_copy", "changed:copied")
        query("UPDATE ai_extra_copy SET content='copy-update' WHERE id=2")
        wait_value("SELECT JSON_UNQUOTE(JSON_EXTRACT(result,'$.input')) FROM ai_extra_copy", "changed:copy-update")
        assert query("SELECT COUNT(*) FROM oceanbase.__all_ai_column a JOIN oceanbase.__all_column c "
                     "ON c.table_id=a.table_id AND c.column_id IN(a.state_column_id,a.request_column_id,a.result_spec_column_id) "
                     "WHERE a.table_id=(SELECT table_id FROM oceanbase.__all_table WHERE table_name='ai_extra_copy') AND c.is_hidden=1") == '3'
        checks.append("CREATE TABLE LIKE copies and remaps AI metadata and hidden identifiers, with correct later invalidation")

        definitions = ["small VARCHAR(2)", "narrow TINYINT", "oversize DECIMAL(4,2)", "broken JSON",
                       "exact_value DECIMAL(5,2)", "large_value BIGINT UNSIGNED", "unicode_value VARCHAR(2)"]
        prefixes = ["short:", "small-int:", "huge-decimal:", "malformed:", "decimal:", "unsigned:", "unicode:"]
        columns = ",".join(definition + " AI COLUMN(AI_COMPLETE('ai_extra_model',CONCAT('" + prefix + "',content))) WITH(max_retries=0)"
                           for definition, prefix in zip(definitions, prefixes))
        query("CREATE TABLE ai_extra_types(id BIGINT PRIMARY KEY,content VARCHAR(100)," + columns + "); "
              "INSERT INTO ai_extra_types(id,content) VALUES(1,'bounds')")
        wait_value(tasks("ai_extra_types"), "0")
        assert query("SELECT exact_value,large_value,unicode_value FROM ai_extra_types") == "12.34\t18446744073709551615\t世界"
        for column in ["small", "narrow", "oversize", "broken"]:
            query("SET SESSION ai_column_strict_read=ON; SELECT " + column + " FROM ai_extra_types", error=11120)
        checks.append("string, small integer, decimal and JSON bounds fail finitely; valid decimal, uint64 and UTF-8 results remain READY")

        query("CREATE TABLE ai_extra_retry(id BIGINT PRIMARY KEY,content VARCHAR(100),result VARCHAR(200) "
              "AI COLUMN(AI_COMPLETE('ai_extra_model',content)) WITH(max_retries=1)); "
              "INSERT INTO ai_extra_retry(id,content) VALUES(1,'retry-server'),(2,'retry-rate')")
        wait_value(tasks("ai_extra_retry"), "0")
        with lock:
            assert requests.count("retry-server") == 2 and requests.count("retry-rate") == 2, requests
        checks.append("HTTP 503 and 429 honor the persisted retry budget with exactly two upstream calls per row")

        query("CREATE TABLE ai_extra_mixed(id BIGINT PRIMARY KEY,content VARCHAR(100), "
              "good VARCHAR(200) AI COLUMN(AI_COMPLETE('ai_extra_model',content)) WITH(max_retries=0), "
              "bad BIGINT AI COLUMN(AI_COMPLETE('ai_extra_model',CONCAT('malformed:',content))) WITH(max_retries=0)); "
              "INSERT INTO ai_extra_mixed(id,content) VALUES(1,'mixed')")
        wait_value(tasks("ai_extra_mixed"), "0")
        assert query("SELECT good,bad IS NULL FROM ai_extra_mixed") == "mock:mixed\t1"
        checks.append("mixed terminal outcomes publish a successful value alongside a failed NULL")
        query("SET SESSION ai_column_strict_read=ON; SELECT bad FROM ai_extra_mixed", error=11120)
        assert query("SET SESSION ai_column_strict_read=ON; SELECT good FROM ai_extra_mixed") == "mock:mixed"
        checks.append("failed typed output remains unreadable in strict mode while another READY column is readable")

        query("CREATE TABLE ai_extra_null(id BIGINT PRIMARY KEY,content VARCHAR(100),result VECTOR(4) "
              "AI COLUMN(AI_EMBED('ai_extra_embed',content)) WITH(max_retries=0)); INSERT INTO ai_extra_null(id,content) VALUES(1,NULL)")
        wait_value(tasks("ai_extra_null"), "0")
        assert query("SET SESSION ai_column_strict_read=ON; SELECT result IS NULL FROM ai_extra_null") == "1"
        checks.append("successful SQL NULL is READY and can be read in strict mode")

        query("CREATE TABLE ai_extra_delete(id BIGINT PRIMARY KEY,content VARCHAR(100),result VARCHAR(200) "
              "AI COLUMN(AI_COMPLETE('ai_extra_model',content)) WITH(max_retries=0)); INSERT INTO ai_extra_delete(id,content) VALUES(1,'slow-delete')")
        wait_request("slow-delete")
        query("DELETE FROM ai_extra_delete WHERE id=1")
        time.sleep(1.7)
        assert query("SELECT COUNT(*) FROM ai_extra_delete") == "0" and query(tasks("ai_extra_delete")) == "0"
        checks.append("deleting a claimed row removes its tasks and a late HTTP response cannot recreate it")

        query("CREATE TABLE ai_extra_restart(id BIGINT PRIMARY KEY,content VARCHAR(100),result VARCHAR(200) "
              "AI COLUMN(AI_COMPLETE('ai_extra_model',content)) WITH(max_retries=0)); INSERT INTO ai_extra_restart(id,content) VALUES(1,'restart-inflight')")
        wait_request("restart-inflight")
        restart_instance()
        wait_value("SELECT result FROM ai_extra_restart WHERE id=1", "mock:restart-inflight")
        assert query(tasks("ai_extra_restart")) == "0"
        checks.append("intentional process crash recovers a committed leased request immediately on restart")
    finally:
        for table in tables:
            query(f"DROP TABLE IF EXISTS {table}")
        query("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_extra_endpoint')")
        query("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('ai_extra_embed_ep')")
        query("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_extra_model')")
        query("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('ai_extra_embed')")
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
    return {"passed": len(checks), "checks": checks}
