import json
import queue
import signal
import subprocess
import threading
import time
import traceback

import cdc_plugin_integration as c


class Session:
    def __init__(self):
        self.proc = subprocess.Popen(c.client('target'), stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        self.lines = queue.Queue()
        threading.Thread(target=self.read, daemon=True).start()
        self.count = 0

    def read(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip('\n'))
        self.lines.put(None)

    def execute(self, sql):
        self.count += 1
        marker = 'ddl_lock_' + str(self.count)
        self.proc.stdin.write(sql + "; SELECT '" + marker + "';\n")
        self.proc.stdin.flush()
        while True:
            line = self.lines.get(timeout=15)
            if line is None or line.startswith('ERROR '):
                raise RuntimeError('lock session failed: ' + repr(line))
            if line == marker:
                return

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            self.proc.wait(timeout=10)


def wait_query(query, expected):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        actual = c.sql('target', query)
        if actual == expected:
            return
        c.status()
        time.sleep(0.02)
    raise AssertionError('query did not reach ' + repr(expected) + ': ' + query)


def crash():
    process = c.SERVERS.pop('target')
    process.kill()
    process.wait(timeout=10)
    c.check('owned target killed', process.returncode, -signal.SIGKILL)


def restart():
    c.start('target')
    c.enable()


def main():
    c.setup()
    c.enable()
    c.drain('initial drain')
    held = Session()
    sessions.append(held)
    held.execute('START TRANSACTION; SELECT id FROM cdc_test.t LIMIT 1')
    c.sql('source', 'TRUNCATE TABLE cdc_test.t')
    wait_query('SELECT done FROM _duckdb_cdc.ddl_recovery', ['0'])
    crash()
    held.close()
    restart()
    c.drain('prepared TRUNCATE recovered before execution')
    c.check('TRUNCATE journal removed', c.sql('target', 'SELECT COUNT(*) FROM _duckdb_cdc.ddl_recovery'), ['0'])
    c.sql('source', 'INSERT INTO cdc_test.t VALUES(1,10)')
    c.drain('DML resumes after TRUNCATE recovery')

    held = Session()
    sessions.append(held)
    held.execute('LOCK TABLES _duckdb_cdc.checkpoints WRITE')
    c.sql('source', 'ALTER TABLE cdc_test.t ADD COLUMN extra INT DEFAULT 7')
    wait_query('SELECT done FROM _duckdb_cdc.ddl_recovery', ['1'])
    crash()
    held.close()
    restart()
    c.drain('completed ALTER confirmed without duplicate ADD')
    c.check('ALTER journal removed', c.sql('target', 'SELECT COUNT(*) FROM _duckdb_cdc.ddl_recovery'), ['0'])

    c.sql('source', 'CREATE TABLE cdc_test.w(id INT PRIMARY KEY,v INT) ENGINE=InnoDB; '
          'INSERT INTO cdc_test.w VALUES(1,20)')
    c.drain('rename seed', ('t', 'u', 'w'))
    held = Session()
    sessions.append(held)
    held.execute('START TRANSACTION; SELECT id FROM cdc_test.w LIMIT 1')
    c.sql('source', 'RENAME TABLE cdc_test.w TO cdc_test.x')
    wait_query('SELECT done FROM _duckdb_cdc.ddl_recovery', ['0'])
    journal = Session()
    sessions.append(journal)
    journal.execute('LOCK TABLES _duckdb_cdc.ddl_recovery WRITE')
    held.execute('ROLLBACK')
    wait_query("SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA='cdc_test' AND TABLE_NAME='x'", ['1'])
    crash()
    held.close()
    journal.close()
    restart()
    c.drain('executed RENAME recovered without completion marker', ('t', 'u', 'x'))
    c.check('RENAME journal removed', c.sql('target', 'SELECT COUNT(*) FROM _duckdb_cdc.ddl_recovery'), ['0'])
    c.sql('source', 'INSERT INTO cdc_test.x VALUES(2,30)')
    c.drain('DML resumes after RENAME recovery', ('t', 'u', 'x'))
    before = c.sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints')
    held = Session()
    sessions.append(held)
    held.execute('START TRANSACTION; SELECT id FROM cdc_test.t LIMIT 1')
    c.sql('source', 'ALTER TABLE cdc_test.t ADD COLUMN unconfirmed INT DEFAULT 11')
    wait_query('SELECT done FROM _duckdb_cdc.ddl_recovery', ['0'])
    journal = Session()
    sessions.append(journal)
    journal.execute('LOCK TABLES _duckdb_cdc.ddl_recovery WRITE')
    held.execute('ROLLBACK')
    wait_query("SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='cdc_test' "
               "AND TABLE_NAME='t' AND COLUMN_NAME='unconfirmed'", ['1'])
    crash()
    held.close()
    journal.close()
    c.start('target')
    c.sql('target', 'SET GLOBAL duckdb_cdc_enabled=ON')
    values = c.wait_state('ERROR', allow_error=True)
    c.check('unmarked ALTER remains fail closed', 'ambiguous DDL recovery' in values['duckdb_cdc_error'])
    c.check('ambiguous ALTER preserves checkpoint',
            c.sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints'), before)
    c.check('ambiguous ALTER preserves journal', c.sql('target', 'SELECT done FROM _duckdb_cdc.ddl_recovery'), ['0'])
    c.SUMMARY['status'] = 'pass'


sessions = []
code = 0
try:
    main()
except BaseException:
    code = 1
    c.SUMMARY['status'] = 'fail'
    c.SUMMARY['traceback'] = traceback.format_exc()
    print(c.SUMMARY['traceback'])
finally:
    for node in list(c.SERVERS):
        try:
            c.stop(node)
        except BaseException:
            code = 1
            c.SUMMARY.setdefault('cleanup_errors', []).append(traceback.format_exc())
    for session in sessions:
        try:
            session.close()
        except BaseException:
            code = 1
            c.SUMMARY.setdefault('cleanup_errors', []).append(traceback.format_exc())
    for handle in c.HANDLES:
        handle.close()
    (c.ROOT / 'ddl-recovery-summary.json').write_text(json.dumps(c.SUMMARY, indent=2))
    print(json.dumps({'status': c.SUMMARY['status'], 'checks': len(c.SUMMARY['checks']), 'artifacts': str(c.ROOT)}))
raise SystemExit(code)
