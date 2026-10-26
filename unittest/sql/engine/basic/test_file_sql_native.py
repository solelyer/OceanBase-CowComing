"""Native File SQL regression. Runs against a dedicated seekdb instance."""

import base64
from datetime import date, datetime, timedelta
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def verify(mysql_command, environment):
    checks = []
    repo = Path(__file__).resolve().parents[4]
    workspace = repo.parent
    fixture = Path(tempfile.mkdtemp(prefix="file-sql-", dir=workspace / ".tooling"))
    allowed = fixture / "allowed"
    allowed.mkdir()

    def literal(value):
        return "'" + str(value).replace("\\", "\\\\").replace("'", "''") + "'"

    def query(sql, success=True, enabled=True):
        statement = ("SET SESSION enable_file_sql=ON; " if enabled else "") + sql
        if environment.get("SEEKDB_TEST_DEBUG_SQL"):
            statement = "SET SESSION ob_log_level='DEBUG'; " + statement
        command = mysql_command
        if sql.upper().startswith("SET GLOBAL"):
            command = ["mysql", "--no-defaults", "--protocol=SOCKET", "-uroot",
                       "--socket=" + environment["SEEKDB_TEST_SOCKET"]]
        result = subprocess.run(command + ["--default-character-set=utf8mb4", "--batch", "--raw",
                                                "--skip-column-names", "-Dtest"], input=statement,
                                env=environment, capture_output=True, text=True, timeout=45)
        if success:
            assert result.returncode == 0, (sql, result.stderr)
        else:
            assert result.returncode != 0, f"Invalid file query succeeded: {sql}"
        return result.stdout.rstrip("\n")

    def table(path, function="read_csv", options=""):
        return f"{function}({literal(path)}{options})"

    old_secure = query("SELECT @@global.secure_file_priv", enabled=False)
    try:
        query(f"SET GLOBAL secure_file_priv={literal(allowed)}", enabled=False)
        source = repo / "tools/deploy/mysql_test/test_suite/file_sql/data"
        for name in ["basic.csv", "basic.jsonl"]:
            shutil.copyfile(source / name, allowed / name)
        (allowed / "simple.parquet").write_bytes(base64.b64decode((source / "simple.parquet.b64").read_bytes()))
        csv = table(allowed / "basic.csv")
        jsonl = table(allowed / "basic.jsonl", "read_json")
        assert query("SELECT @@session.enable_file_sql", enabled=False) == "0"
        query(f"SELECT * FROM {csv}", success=False, enabled=False)
        checks.append("File SQL defaults OFF and rejects reads")
        assert query(f"SELECT COUNT(*),SUM(id),ROUND(AVG(score),2),SUM(active) FROM {csv}") == "3\t6\t91.92\t2"
        checks.append("official CSV fixture, native COUNT SUM AVG and boolean conversion")
        assert query(f"SELECT id,name,id__2,REPLACE(note,CHAR(10),'<NL>') FROM {csv} ORDER BY id") == (
            '1\tAlice\t101\thello,<NL>world\n2\tBob\t102\tplain\n3\tChen\t103\tsay "hi"')
        checks.append("CSV duplicate headers, multiline quote and escaped quote")
        assert query(f"SELECT id,day,created FROM {csv} ORDER BY id") == (
            "1\t2026-08-01\t2026-08-01 10:11:12\n2\tNULL\tNULL\n3\t2026-08-03\t2026-08-03 12:13:14")
        checks.append("CSV DATE and DATETIME inference and NULL conversion")
        description = query(f"DESCRIBE SELECT * FROM {csv}")
        assert description.splitlines() == [
            "id\tBIGINT\tNO\tNULL\tNULL\tNULL", "name\tVARCHAR\tNO\tNULL\tNULL\tNULL",
            "score\tDOUBLE\tNO\tNULL\tNULL\tNULL", "active\tBOOLEAN\tNO\tNULL\tNULL\tNULL",
            "day\tDATE\tYES\tNULL\tNULL\tNULL", "created\tDATETIME\tYES\tNULL\tNULL\tNULL",
            "note\tVARCHAR\tNO\tNULL\tNULL\tNULL", "id__2\tBIGINT\tNO\tNULL\tNULL\tNULL"]
        checks.append("DESCRIBE returns inferred dynamic column metadata")
        assert query(f"SELECT id,name,note FROM {jsonl} j ORDER BY id") == "1\tAlice\tNULL\n2\tBob\tNULL\n3\tChen\tok"
        checks.append("JSONL field union and missing fields")
        assert query(f"SELECT c.id,c.name,j.note FROM {csv} c LEFT JOIN {jsonl} j ON c.id=j.id AND j.id>1 ORDER BY c.id") == (
            "1\tAlice\tNULL\n2\tBob\tNULL\n3\tChen\tok")
        checks.append("native cross-format LEFT JOIN")
        assert query(f"SELECT c.id FROM {csv} c INNER JOIN {jsonl} j ON c.id=j.id WHERE c.score>90 ORDER BY c.id DESC LIMIT 1") == "3"
        checks.append("native INNER JOIN, expression filter, ordering and LIMIT")
        parquet = table(allowed / "simple.parquet", "read_parquet")
        assert query(f"SELECT COUNT(*),MIN(c1),MAX(c1),SUM(c1) FROM {parquet}") == "7\t0\t6\t21"
        checks.append("official Parquet fixture through native aggregation")
        sys.path.insert(0, str(workspace / ".tooling/file-sql-fixture-libs"))
        import pyarrow as pa
        import pyarrow.parquet as pq

        rows = 41
        arrow = pa.table({
            "id": pa.array(range(rows), type=pa.int64()),
            "name": pa.array([None if i % 5 == 0 else f"世界,{i}" for i in range(rows)]),
            "score": pa.array([None if i % 7 == 0 else i + 0.25 for i in range(rows)], type=pa.float64()),
            "active": pa.array([None if i % 3 == 0 else bool(i % 2) for i in range(rows)]),
            "day": pa.array([date(2026, 1, 1) + timedelta(days=i) for i in range(rows)], type=pa.date32()),
            "created": pa.array([datetime(2026, 1, 1, 1, 2, 3, i) for i in range(rows)], type=pa.timestamp("us")),
        })
        for codec in [None, "snappy", "gzip", "zstd"]:
            for version in ["1.0", "2.0"]:
                for dictionary in [False, True]:
                    path = allowed / f"arrow-{codec}-{version}-{dictionary}.parquet"
                    pq.write_table(arrow, path, compression=codec, use_dictionary=dictionary,
                                   data_page_version=version, row_group_size=7,
                                   data_page_size=128, write_batch_size=4)
                    read = table(path, "read_parquet")
                    assert query(f"SELECT COUNT(*),SUM(id),COUNT(name),COUNT(score),COUNT(active) FROM {read}") == "41\t820\t32\t35\t27"
                    assert query(f"SELECT id,name,active,day FROM {read} WHERE id IN (1,5,7,40) ORDER BY id") == (
                        "1\t世界,1\t1\t2026-01-02\n5\tNULL\t1\t2026-01-06\n7\t世界,7\t1\t2026-01-08\n40\tNULL\t0\t2026-02-10")
                    checks.append(f"Parquet independent Arrow fixture: {codec}, pages {version}, dictionary {dictionary}")
        checks.append("Parquet multiple row groups, nullable basic types and projected columns")
        pruned = allowed / "pruned.parquet"
        pq.write_table(pa.table({"id": range(9), "unused": ['text'] * 9}), pruned,
                       compression=None, use_dictionary=False)
        broken_offset = pq.read_metadata(pruned).row_group(0).column(1).data_page_offset
        with pruned.open('r+b') as output:
            output.seek(broken_offset)
            output.write(b'\x00\x00\x00\x00')
        assert query(f"SELECT COUNT(*),SUM(id) FROM {table(pruned,'read_parquet')}") == "9\t36"
        query(f"SELECT unused FROM {table(pruned,'read_parquet')}", success=False)
        checks.append("Parquet column pruning avoids decoding corrupt unreferenced column data")
        boundaries = allowed / "boundaries.csv"
        boundaries.write_text("i,f,b,d,dt,overflow,bad_date,empty,mixed\n"
            "-9223372036854775808,1,true,2024-02-29,2024-02-29,9223372036854775808,2023-02-29,,1\n"
            "9223372036854775807,2.5,false,2000-02-29,2024-02-29 01:02:03.123456,-9223372036854775809,0000-01-01,,word\n")
        edge = table(boundaries)
        description = [line.split('\t')[1] for line in query(f"DESCRIBE SELECT * FROM {edge}").splitlines()]
        assert description == ['BIGINT','DOUBLE','BOOLEAN','DATE','DATETIME','VARCHAR','VARCHAR','NULL','VARCHAR']
        assert query(f"SELECT MIN(i),MAX(i),SUM(f),COUNT(empty),MAX(CHAR_LENGTH(overflow)) FROM {edge}") == '-9223372036854775808\t9223372036854775807\t3.5\t0\t20'
        assert query(f"SELECT dt FROM {edge} WHERE b=false") == '2024-02-29 01:02:03.123456'
        checks.append("deterministic widening, int64 boundaries, overflow text, Gregorian dates and all-NULL columns")
        assert query(f"SELECT active,COUNT(*),MIN(id),MAX(id) FROM {csv} GROUP BY active HAVING COUNT(*)>1") == "1\t2\t1\t3"
        checks.append("GROUP BY HAVING and MIN MAX")
        plan = query(f"EXPLAIN SELECT c.id FROM {csv} c JOIN {jsonl} j ON c.id=j.id")
        assert "FUNCTION_TABLE" in plan and "JOIN" in plan
        checks.append("file JOIN uses native seekdb execution plan")
        headerless = allowed / "headerless.csv"
        headerless.write_bytes(b"\xef\xbb\xbf1;true;\"\"\r\n2;false;NULL\r\n")
        assert query(f"SELECT column0,column1,column2 FROM {table(headerless, options=",header=false,delim=';',nullstr='NULL'")} ORDER BY column0") == "1\t1\t\n2\t0\tNULL"
        checks.append("BOM, CRLF, custom delimiter, header=false and quoted empty string")
        for content in [b"a,b\n1\n", b'a,b\n1,"unterminated\n', b'a,b\n1,un"quoted\n', b'a,b\n1,"ok"extra\n', b"a\n\xff\n"]:
            path = allowed / "bad.csv"
            path.write_bytes(content)
            query(f"SELECT COUNT(*) FROM {table(path)}", success=False)
        checks.append("malformed CSV, invalid UTF-8 and full-file validation fail before results")
        for content in ['{"x":1}\n{"x":{}}\n', '[1,2]\n', '{bad}\n', '{"x":[]}\n']:
            path = allowed / "bad.jsonl"
            path.write_text(content)
            query(f"SELECT COUNT(*) FROM {table(path, 'read_ndjson')}", success=False)
        checks.append("invalid JSONL and nested structures rejected")
        query(f"SELECT * FROM {table(allowed / 'basic.csv', options=',sample_size=10')}", success=False)
        query(f"SELECT * FROM {table(allowed / 'basic.csv', options=',header=true,header=false')}", success=False)
        checks.append("unsupported sample_size and duplicate options rejected")
        changing = allowed / "changing.csv"
        changing.write_text("one\n1\n")
        assert query(f"SELECT * FROM {table(changing)}") == "1"
        changing.write_text("one,two\n2,new column\n")
        assert query(f"SELECT two FROM {table(changing)}") == "new column"
        checks.append("repeated file path resolves fresh dynamic schema")
        secret = fixture / "outside.csv"
        secret.write_text("secret\nprivate\n")
        query(f"SELECT * FROM {table(secret)}", success=False)
        query(f"SELECT * FROM {table(allowed / '../outside.csv')}", success=False)
        link = allowed / "escape.csv"
        link.symlink_to(secret)
        query(f"SELECT * FROM {table(link)}", success=False)
        link.unlink()
        checks.append("secure_file_priv rejects absolute, traversal and symlink escapes")
        assert query(f"SELECT COUNT(*) FROM {table(allowed / 'no-match-*.csv', 'glob')}") == "0"
        names = query(f"SELECT SUBSTRING_INDEX(file,'/',-1) FROM {table(allowed / 'basic.*', 'glob')} ORDER BY file")
        assert names == "basic.csv\nbasic.jsonl"
        checks.append("glob discovery and empty matches")
        large = allowed / "batch.csv"
        large.write_text("id,label,bucket\n" + "".join(f"{i},row-{i}-世界,{i % 7}\n" for i in range(4097)))
        batch = table(large)
        assert query(f"SELECT COUNT(*),SUM(id),COUNT(DISTINCT label),MIN(label=CONCAT('row-',id,'-世界')) FROM {batch}") == "4097\t8390656\t4097\t1"
        assert query(f"SELECT bucket,COUNT(*) FROM {batch} GROUP BY bucket ORDER BY bucket") == (
            "0\t586\n1\t586\n2\t585\n3\t585\n4\t585\n5\t585\n6\t585")
        checks.append("multiple native batches retain independent UTF-8 strings and row positions")
        assert query(f"SELECT /*+ USE_NL(c j) */ COUNT(*) FROM {csv} c JOIN {jsonl} j ON c.id=j.id") == "3"
        checks.append("file reader supports nested-loop rescan")
        exported = allowed / "export.csv"
        query(f"COPY (SELECT id,name,score,note,day,created FROM {csv} ORDER BY id) TO {literal(exported)} (format csv,header,nullstr 'NULL')")
        readback = table(exported, options=",nullstr='NULL'")
        assert query(f"SELECT COUNT(*),SUM(id),COUNT(day),COUNT(created),ROUND(SUM(score),2) FROM {readback}") == "3\t6\t2\t2\t275.75"
        assert query(f"SELECT REPLACE(note,CHAR(10),'<NL>') FROM {readback} ORDER BY id") == 'hello,<NL>world\nplain\nsay "hi"'
        previous = exported.read_bytes()
        query(f"COPY (SELECT 1) TO {literal(exported)}", success=False)
        assert exported.read_bytes() == previous
        checks.append("COPY query, header, typed readback, multiline escaping and no overwrite")
        custom = allowed / "custom-export.csv"
        query(f"COPY (SELECT id,note,day FROM {csv} ORDER BY id) TO {literal(custom)} (header true,delimiter ';',quote '|',escape '~',nullstr 'NONE')")
        assert query(f"SELECT COUNT(*),SUM(id),COUNT(day) FROM {table(custom, options=",delim=';',quote='|',escape='~',nullstr='NONE'")}") == "3\t6\t2"
        checks.append("COPY custom delimiter, quote, escape and NULL text")
        query("CREATE TABLE file_sql_copy_test(id BIGINT PRIMARY KEY, payload LONGTEXT)")
        try:
            query("INSERT INTO file_sql_copy_test VALUES(1,REPEAT('世界,',30000)),(2,NULL),(3,'')")
            ordinary = allowed / "table-export.csv"
            query(f"COPY test.file_sql_copy_test TO {literal(ordinary)} (header, nullstr 'NULL')")
            assert query(f"SELECT id,CHAR_LENGTH(payload) FROM {table(ordinary, options=",nullstr='NULL'")} ORDER BY id") == "1\t90000\n2\tNULL\n3\t0"
            checks.append("COPY ordinary table, real LONGTEXT and empty strings")
        finally:
            query("DROP TABLE file_sql_copy_test")
        big_export = allowed / "batch-export.csv"
        query(f"COPY (SELECT * FROM {batch}) TO {literal(big_export)}")
        assert query(f"SELECT COUNT(*),SUM(id),MIN(label=CONCAT('row-',id,'-世界')) FROM {table(big_export)}") == "4097\t8390656\t1"
        checks.append("COPY consumes native batches with filters and independent row values")
        failed = allowed / "failed-export.csv"
        query(f"COPY (SELECT CAST(IF(id=2,'broken','{{}}') AS JSON) AS value FROM {csv}) TO {literal(failed)}", success=False)
        assert not failed.exists() and not list(allowed.glob('.seekdb-copy-*.tmp'))
        checks.append("COPY runtime query failure removes temporary file and leaves no target")
        query(f"COPY (SELECT 1) TO {literal(fixture / 'outside-export.csv')}", success=False)
        query(f"COPY (SELECT 1) TO {literal(allowed / '../outside-export.csv')}", success=False)
        checks.append("COPY enforces secure_file_priv for output paths")
        query(f"COPY (SELECT 1) TO {literal(allowed / 'disabled.csv')}", success=False, enabled=False)
        checks.append("COPY is disabled by default")
        race = allowed / "mutating.csv"
        shutil.copyfile(large, race)
        statement = f"SET SESSION enable_file_sql=ON; SELECT SUM(SLEEP(0.001)),SUM(id) FROM {table(race)};"
        process = subprocess.Popen(mysql_command + ["--batch", "--skip-column-names", "-Dtest"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   env=environment, text=True)
        process.stdin.write(statement)
        process.stdin.close()
        process.stdin = None
        time.sleep(0.5)
        with race.open('a') as output:
            output.write("4097,added,0\n")
        stdout, stderr = process.communicate(timeout=30)
        assert process.returncode != 0, (stdout, stderr)
        checks.append("file modified during active scan is detected and aborts query")
        query("SET GLOBAL secure_file_priv='NULL'", enabled=False)
        query(f"SELECT * FROM {csv}", success=False)
        checks.append("secure_file_priv=NULL disables reads")
        return {"passed": len(checks), "checks": checks, "fixture": str(fixture)}
    finally:
        query(f"SET GLOBAL secure_file_priv={literal(old_secure)}", enabled=False)


if __name__ == "__main__":
    raise SystemExit("Import verify(mysql_command, environment) from an isolated instance runner.")
