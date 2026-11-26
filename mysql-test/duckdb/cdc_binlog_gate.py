import base64
import json
import os
from pathlib import Path
import queue
import signal
import struct
import subprocess
import tempfile
import threading
import time
import traceback


ROOT = Path(tempfile.mkdtemp(prefix='cdc-binlog-gate-', dir='/tmp'))
SOURCE = '/usr/sbin/mariadbd'
TARGET = '/git/BuildOf_mdb-w-duckdb/sql/mariadbd'
PLUGIN = '/git/duckdb-engine/build/ha_duckdb.so'
SERVERS = {}
HANDLES = []
SESSIONS = []
SUMMARY = {'artifacts': str(ROOT), 'status': 'running', 'checks': [], 'commands': []}


def run(command, **kwargs):
    SUMMARY['commands'].append(command)
    return subprocess.run(command, text=True, capture_output=True, timeout=120, **kwargs)


def client(node):
    return ['mariadb', '--no-defaults', '--protocol=SOCKET',
            '--socket=' + str(ROOT / (node + '.sock')), '-uroot',
            '--batch', '--skip-column-names', '--unbuffered', '--raw', '--skip-reconnect']


def sql(node, query):
    result = run(client(node) + ['-e', query])
    with (ROOT / (node + '-queries.log')).open('a') as log:
        log.write(query + '\n' + result.stdout + result.stderr + '\n')
    if result.returncode:
        raise RuntimeError(node + ': ' + result.stderr)
    return result.stdout.strip().splitlines()


def boot(node, binary, port):
    result = run(['mariadb-install-db', '--no-defaults', '--user=root',
                  '--datadir=' + str(ROOT / node),
                  '--auth-root-authentication-method=normal', '--skip-test-db'])
    (ROOT / (node + '-bootstrap.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(node + ' bootstrap failed')
    command = [binary, '--no-defaults', '--user=root', '--datadir=' + str(ROOT / node),
               '--socket=' + str(ROOT / (node + '.sock')),
               '--pid-file=' + str(ROOT / (node + '.pid')),
               '--log-error=' + str(ROOT / (node + '.err')),
               '--port=' + str(port), '--bind-address=127.0.0.1', '--skip-name-resolve',
               '--server-id=' + str(port), '--innodb-buffer-pool-size=64M',
               '--character-set-server=utf8mb4', '--binlog-format=ROW',
               '--binlog-row-image=FULL']
    if node == 'source':
        command += ['--log-bin=' + str(ROOT / 'source-bin')]
    else:
        command += ['--plugin-dir=' + str(Path(PLUGIN).parent),
                    '--plugin-maturity=' + os.environ.get('DUCKDB_TEST_MATURITY', 'experimental'),
                    '--plugin-load-add=ha_duckdb.so',
                    '--duckdb-memory-limit=536870912', '--duckdb-dml-in-batch=ON']
    SUMMARY['commands'].append(command)
    log = (ROOT / (node + '-console.log')).open('w')
    HANDLES.append(log)
    SERVERS[node] = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        if SERVERS[node].poll() is not None:
            raise RuntimeError(node + ' exited: ' + (ROOT / (node + '.err')).read_text())
        result = run(client(node) + ['-e', 'SELECT 1'])
        if result.returncode == 0:
            return
        time.sleep(0.25)
    raise TimeoutError(node + ' startup')


class Session:
    def __init__(self):
        self.log = (ROOT / 'replay-session.log').open('w')
        self.lines = queue.Queue()
        self.counter = 0
        command = client('target') + ['--force']
        SUMMARY['commands'].append(command)
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1)
        SESSIONS.append(self)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip('\n'))
        self.lines.put(None)

    def execute(self, query, reject=False):
        self.counter += 1
        marker = 'cdc_gate_marker_' + str(self.counter)
        self.log.write(query + '\n')
        self.log.flush()
        self.proc.stdin.write(query + ";\nSELECT '" + marker + "';\n")
        self.proc.stdin.flush()
        output = []
        while True:
            line = self.lines.get(timeout=30)
            if line is None:
                raise RuntimeError('persistent client exited: ' + repr(output))
            self.log.write(line + '\n')
            self.log.flush()
            if line == marker:
                break
            output.append(line)
        errors = [line for line in output if line.startswith('ERROR ')]
        if bool(errors) != reject:
            raise AssertionError('unexpected SQL outcome: ' + repr(output))
        return output


def snapshot():
    return sql('target', 'SELECT id,v FROM gate.t ORDER BY id; '
               'SELECT id,v FROM gate.u ORDER BY id; SELECT id,pos FROM gate.checkpoint ORDER BY id')


def equal(name, actual, expected):
    SUMMARY['checks'].append({'name': name, 'actual': actual, 'expected': expected,
                              'pass': actual == expected})
    if actual != expected:
        raise AssertionError(name + ': actual=' + repr(actual) + ' expected=' + repr(expected))


def events(path):
    data = path.read_bytes()
    if data[:4] != b'\xfebin':
        raise AssertionError('bad binlog magic')
    offset = 4
    parsed = []
    while offset < len(data):
        if len(data) - offset < 19:
            raise AssertionError('truncated binlog header')
        stamp, kind, server, size, end, flags = struct.unpack_from('<IBIIIH', data, offset)
        if size < 19 or offset + size > len(data):
            raise AssertionError('invalid binlog event length')
        parsed.append((offset, kind, data[offset:offset + size]))
        offset += size
    return parsed


def bundles(parsed, start, stop):
    result = []
    pending = []
    rows = 0
    for offset, kind, raw in parsed:
        if not start <= offset < stop:
            continue
        if kind == 19:
            pending.append(raw)
        elif kind in (23, 24, 25, 30, 31, 32):
            if not pending:
                raise AssertionError('row event without paired table map')
            pending.append(raw)
            rows += 1
            if struct.unpack_from('<H', raw, 25)[0] & 1:
                result.append(b''.join(pending))
                pending = []
    if pending or not result:
        raise AssertionError('incomplete or empty row statement bundles')
    return result, rows


def inject(session, raw):
    session.execute("BINLOG '" + base64.b64encode(raw).decode('ascii') + "'")


def main():
    print('Artifacts: ' + str(ROOT), flush=True)
    boot('source', SOURCE, 24491)
    boot('target', TARGET, 24492)
    SUMMARY['versions'] = {node: sql(node, 'SELECT VERSION()')[0]
                           for node in ('source', 'target')}
    equal('source is MariaDB', 'MariaDB' in SUMMARY['versions']['source'], True)
    equal('target is MariaDB', 'MariaDB' in SUMMARY['versions']['target'], True)
    equal('batching enabled', sql('target', 'SELECT @@global.duckdb_dml_in_batch'), ['1'])
    for node, engine in [('source', 'InnoDB'), ('target', 'DuckDB')]:
        sql(node, 'CREATE DATABASE gate; '
            'CREATE TABLE gate.t(id INT PRIMARY KEY,v INT NOT NULL) ENGINE=' + engine + '; '
            'CREATE TABLE gate.u(id INT PRIMARY KEY,v INT NOT NULL) ENGINE=' + engine + '; '
            'INSERT INTO gate.t VALUES(1,10),(2,20),(3,30); '
            'INSERT INTO gate.u VALUES(1,100),(2,200),(3,300)')
    sql('target', 'CREATE TABLE gate.checkpoint(id INT PRIMARY KEY,pos BIGINT NOT NULL) ENGINE=DuckDB; '
        'INSERT INTO gate.checkpoint VALUES(1,0)')
    old = ['1\t10', '2\t20', '3\t30', '1\t100', '2\t200', '3\t300', '1\t0']
    equal('seed target', snapshot(), old)
    status = sql('source', 'SHOW MASTER STATUS')[0].split('\t')
    start = int(status[1])
    sql('source', 'USE gate; START TRANSACTION; '
        'INSERT INTO t VALUES(4,40),(5,50); UPDATE u SET v=v+7 WHERE id=1; '
        'DELETE FROM t WHERE id=2; COMMIT; '
        'START TRANSACTION; UPDATE t SET id=11,v=111 WHERE id=1; '
        'INSERT INTO u VALUES(4,400); DELETE FROM u WHERE id=2; '
        'UPDATE t SET v=v+1 WHERE id IN(3,4); COMMIT')
    end_status = sql('source', 'SHOW MASTER STATUS')[0].split('\t')
    equal('no binlog rotation', end_status[0], status[0])
    stop = int(end_status[1])
    parsed = events(ROOT / status[0])
    fde = next(raw for offset, kind, raw in parsed if kind == 15)
    replay, row_count = bundles(parsed, start, stop)
    equal('one bundle per source DML statement', len(replay), 7)
    SUMMARY['binlog'] = {'file': status[0], 'start': start, 'stop': stop,
                         'row_events': row_count, 'bundles': len(replay),
                         'events': [{'offset': offset, 'type': kind, 'bytes': len(raw)}
                                    for offset, kind, raw in parsed]}
    expected_rows = ['3\t31', '4\t41', '5\t50', '11\t111', '1\t107', '3\t300', '4\t400']
    equal('source complete final data', sql('source', 'SELECT id,v FROM gate.t ORDER BY id; '
          'SELECT id,v FROM gate.u ORDER BY id'), expected_rows)
    session = Session()
    session.execute('USE gate')
    inject(session, fde)
    for action in ('ROLLBACK', 'COMMIT'):
        session.execute('START TRANSACTION')
        for raw in replay:
            inject(session, raw)
        session.execute('UPDATE gate.checkpoint SET pos=' + str(stop) + ' WHERE id=1')
        equal(action + ' second connection before finish', snapshot(), old)
        session.execute(action)
        equal(action + ' second connection after finish', snapshot(),
              old if action == 'ROLLBACK' else expected_rows + ['1\t' + str(stop)])
    session.execute('START TRANSACTION')
    rejected = session.execute('UPDATE gate.t JOIN gate.u ON t.id=u.id '
                               'SET t.v=t.v+1000,u.v=u.v+1000', reject=True)
    SUMMARY['ordinary_sql_rejection'] = rejected
    equal('ordinary SQL wrong-command rejection',
          any(line.startswith('ERROR 1031 (HY000)') for line in rejected), True)
    session.execute('ROLLBACK')
    equal('ordinary SQL cannot reuse fake row context', snapshot(),
          expected_rows + ['1\t' + str(stop)])
    images = ['MINIMAL']
    if 'FULL_NODUP' in ''.join(sql('source', "SELECT ENUM_VALUE_LIST FROM information_schema.SYSTEM_VARIABLES "
                                  "WHERE VARIABLE_NAME='BINLOG_ROW_IMAGE'")):
        images.append('FULL_NODUP')
    for image in images:
        begin = int(sql('source', 'SHOW MASTER STATUS')[0].split('\t')[1])
        sql('source', 'SET SESSION binlog_row_image=FULL; UPDATE gate.t SET v=v+1 WHERE id=5; '
            'SET SESSION binlog_row_image=' + image + '; UPDATE gate.t SET v=v+1 WHERE id=11')
        finish = int(sql('source', 'SHOW MASTER STATUS')[0].split('\t')[1])
        partial, _ = bundles(events(ROOT / status[0]), begin, finish)
        session.execute('START TRANSACTION')
        rejected = session.execute("BINLOG '" + base64.b64encode(b''.join(partial)).decode('ascii') + "'", reject=True)
        equal(image + ' rejected after a full-image statement in the same packet',
              any(line.startswith('ERROR 1296 (HY000)') for line in rejected), True)
        session.execute('ROLLBACK')
        equal(image + ' rollback preserves data and checkpoint', snapshot(),
              expected_rows + ['1\t' + str(stop)])
    SUMMARY['status'] = 'pass'


if __name__ == '__main__':
    code = 0
    try:
        main()
    except BaseException:
        code = 1
        SUMMARY['status'] = 'fail'
        SUMMARY['error'] = traceback.format_exc()
        print(SUMMARY['error'], flush=True)
    finally:
        for session in SESSIONS:
            if session.proc.poll() is None:
                session.proc.stdin.close()
                try:
                    session.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    session.proc.kill()
                    session.proc.wait()
            session.log.close()
        SUMMARY['server_exit_codes'] = {}
        for node, proc in reversed(list(SERVERS.items())):
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            SUMMARY['server_exit_codes'][node] = proc.returncode
        for handle in HANDLES:
            handle.close()
        (ROOT / 'summary.json').write_text(json.dumps(SUMMARY, indent=2) + '\n')
        print(json.dumps({'status': SUMMARY['status'], 'artifacts': str(ROOT),
                          'checks': len(SUMMARY['checks']),
                          'server_exit_codes': SUMMARY['server_exit_codes']}), flush=True)
    raise SystemExit(code)
