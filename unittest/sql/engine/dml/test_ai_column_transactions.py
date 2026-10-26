"""Native task DML checks; all mutations remain uncommitted until rollback."""
import subprocess


def verify(mysql_command, environment):
    checks = []
    def query(statement, error=None):
        if environment.get("SEEKDB_TEST_DEBUG_SQL"):
            statement = "SET SESSION ob_log_level='DEBUG'; " + statement
        result = subprocess.run(mysql_command + ["--batch", "--raw", "--skip-column-names", "-Dtest"],
                                input=statement, env=environment, text=True, capture_output=True, timeout=45)
        if error is None:
            assert result.returncode == 0, (statement, result.stdout, result.stderr)
        else:
            assert result.returncode != 0 and f"ERROR {error} " in result.stderr, (statement, result.stdout, result.stderr)
        return result.stdout.rstrip('\n')

    query("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('transaction_model', '{\"type\":\"completion\",\"model_name\":\"unused\"}')")
    try:
        query("CREATE TABLE ai_transaction_test(id BIGINT PRIMARY KEY, content VARCHAR(100), title VARCHAR(20), "
              "result VARCHAR(200) AI COLUMN(AI_COMPLETE('transaction_model',content)), "
              "label VARCHAR(200) AI COLUMN(AI_COMPLETE('transaction_model',CONCAT('label:',content))))")
        tasks = "SELECT COUNT(*),COUNT(DISTINCT request_id) FROM oceanbase.__all_ai_column_task WHERE table_id=(SELECT table_id FROM oceanbase.__all_table WHERE table_name='ai_transaction_test')"
        result = query("START TRANSACTION; INSERT INTO ai_transaction_test(id,content) VALUES(1,'a'),(2,'b'); "
                       "SELECT COUNT(*) FROM ai_transaction_test WHERE result IS NULL AND label IS NULL; " + tasks + "; ROLLBACK; " + tasks)
        assert result == '2\n4\t2\n0\t0', result
        checks.append("batch inserts create one shared request per row; rollback removes rows and tasks")
        result = query("START TRANSACTION; INSERT INTO ai_transaction_test(id,content) VALUES(1,'a'); " + tasks + "; "
                       "UPDATE ai_transaction_test SET title='unchanged' WHERE id=1; " + tasks + "; "
                       "UPDATE ai_transaction_test SET content='updated' WHERE id=1; " + tasks + "; "
                       "UPDATE ai_transaction_test SET id=3 WHERE id=1; "
                       "SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE rowkey_predicate='`id` = 3'; "
                       "DELETE FROM ai_transaction_test WHERE id=3; " + tasks + "; ROLLBACK")
        assert result == '2\t1\n2\t1\n2\t1\n2\n0\t0', result
        checks.append("dependency and primary key updates replace tasks; unrelated updates preserve them; deletes clean them")
        metadata = query("SHOW AI COLUMNS FROM ai_transaction_test").splitlines()
        assert len(metadata) == 2 and metadata[0].startswith('result\tAI_COMPLETE(') and metadata[1].startswith('label\tAI_COMPLETE('), metadata
        checks.append("SHOW AI COLUMNS resolves stored normalized definitions")
        result = query("START TRANSACTION; INSERT INTO ai_transaction_test(id,content) VALUES(1,'pending'); "
                       "ALTER TABLE ai_transaction_test RETRY AI COLUMN result WHERE status='PENDING' AND id=1; " + tasks + "; ROLLBACK; " + tasks)
        assert result == '2\t2\n0\t0', result
        checks.append("RETRY updates only its target column and shares the business transaction rollback")
        query("START TRANSACTION; INSERT INTO ai_transaction_test(id,content) VALUES(1,'pending'); "
              "SET SESSION ai_column_strict_read=ON; SELECT result FROM ai_transaction_test WHERE id=999", error=11120)
        checks.append("strict reads check referenced AI columns across the table, including uncommitted pending rows")
        query("ALTER TABLE ai_transaction_test RETRY AI COLUMN content WHERE id=1", error=1235)
        query("ALTER TABLE ai_transaction_test RETRY AI COLUMN result WHERE id=1, ADD COLUMN extra INT", error=1235)
        checks.append("RETRY rejects ordinary columns and mixed ALTER actions")
    finally:
        query("DROP TABLE IF EXISTS ai_transaction_test")
        query("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('transaction_model')")
    return {"passed": len(checks), "checks": checks}
