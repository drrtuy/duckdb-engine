import json
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import traceback


ROOT = Path(tempfile.mkdtemp(prefix='cdc-plugin-', dir='/tmp'))
PLUGIN = Path('/git/duckdb-engine/build/ha_duckdb.so')
TLS = Path('/usr/share/mariadb/mariadb-test/std_data')
PASSWORD = 'cdc_isolated_test_42'
SERVERS = {}
COMMANDS = {}
HANDLES = []
SUMMARY = {'artifacts': str(ROOT), 'status': 'running', 'checks': [], 'commands': [], 'states': []}


def run(command):
    SUMMARY['commands'].append(command)
    return subprocess.run(command, capture_output=True, text=True, timeout=60)


def client(node):
    return ['mariadb', '--no-defaults', '--protocol=SOCKET',
            '--socket=' + str(ROOT / (node + '.sock')), '-uroot', '--batch',
            '--skip-column-names', '--raw', '--unbuffered', '--skip-reconnect']


def sql(node, query):
    result = run(client(node) + ['-e', query])
    with (ROOT / (node + '-sql.log')).open('a') as log:
        log.write(query + '\n' + result.stdout + result.stderr + '\n')
    if result.returncode:
        raise RuntimeError(node + ': ' + result.stderr)
    return result.stdout.rstrip('\n').splitlines()


def check(name, actual, expected=True):
    SUMMARY['checks'].append({'name': name, 'actual': actual, 'expected': expected})
    if actual != expected:
        raise AssertionError(name + ': ' + repr(actual) + ' != ' + repr(expected))


def start(node, extra=None):
    if extra is not None:
        result = run(['mariadb-install-db', '--no-defaults', '--user=root',
                      '--datadir=' + str(ROOT / node), '--skip-test-db',
                      '--auth-root-authentication-method=normal'])
        (ROOT / (node + '-bootstrap.log')).write_text(result.stdout + result.stderr)
        check(node + ' bootstrap', result.returncode, 0)
        binary, port = (('/usr/sbin/mariadbd', 24491) if node == 'source' else
                        ('/git/BuildOf_mdb-w-duckdb/sql/mariadbd', 24492))
        COMMANDS[node] = [binary, '--no-defaults', '--user=root',
                          '--datadir=' + str(ROOT / node),
                          '--socket=' + str(ROOT / (node + '.sock')),
                          '--pid-file=' + str(ROOT / (node + '.pid')),
                          '--log-error=' + str(ROOT / (node + '.err')),
                          '--bind-address=127.0.0.1', '--port=' + str(port),
                          '--server-id=' + str(port), '--skip-name-resolve',
                          '--innodb-buffer-pool-size=64M', '--character-set-server=utf8mb4',
                          '--binlog-format=ROW', '--binlog-row-image=FULL'] + extra
    log = (ROOT / (node + '-console.log')).open('a')
    HANDLES.append(log)
    SUMMARY['commands'].append(COMMANDS[node])
    SERVERS[node] = subprocess.Popen(COMMANDS[node], stdout=log, stderr=subprocess.STDOUT,
                                    start_new_session=True)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if SERVERS[node].poll() is not None:
            raise RuntimeError(node + ' startup exit: ' + (ROOT / (node + '.err')).read_text())
        if run(client(node) + ['-e', 'SELECT 1']).returncode == 0:
            return
        time.sleep(0.2)
    raise TimeoutError(node + ' startup')


def stop(node):
    proc = SERVERS[node]
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            raise TimeoutError(node + ' graceful shutdown required SIGKILL')
    SUMMARY.setdefault('server_exits', []).append({'node': node, 'code': proc.returncode})
    check(node + ' clean shutdown', proc.returncode, 0)


def status(allow_error=False):
    values = dict(line.split('\t', 1) for line in
                  sql('target', "SHOW GLOBAL STATUS LIKE 'Duckdb_cdc_%'"))
    values = {key.lower(): value for key, value in values.items()}
    SUMMARY['states'].append(values)
    if values.get('duckdb_cdc_state') == 'ERROR' and not allow_error:
        raise RuntimeError('CDC ERROR: ' + values.get('duckdb_cdc_error', repr(values)))
    return values


def wait_state(expected, allow_error=False):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        values = status(allow_error)
        if values.get('duckdb_cdc_state') == expected:
            return values
        time.sleep(0.1)
    raise TimeoutError('waiting for CDC ' + expected + ': ' + repr(values))


def snapshot(node, tables=('t', 'u')):
    return {table: sql(node, 'SELECT * FROM cdc_test.`' + table + '` ORDER BY id')
            for table in tables}


def drain(name, tables=('t', 'u')):
    expected = snapshot('source', tables)
    head = sql('source', 'SHOW MASTER STATUS')[0].split('\t')[:2]
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        values = status()
        if (values.get('duckdb_cdc_state') == 'RUNNING' and
                values.get('duckdb_cdc_received') == values.get('duckdb_cdc_applied') and
                values.get('duckdb_cdc_buffered_records', '0') == '0' and
                values.get('duckdb_cdc_applied_file') == head[0] and
                values.get('duckdb_cdc_applied_position') == head[1]):
            actual = snapshot('target', tables)
            if actual == expected:
                check(name, actual, expected)
                return values
        time.sleep(0.1)
    raise TimeoutError(name + ': ' + repr(values) + '; target=' + repr(snapshot('target', tables)) +
                       '; source=' + repr(expected))


def enable():
    sql('target', 'SET GLOBAL duckdb_cdc_enabled=ON')
    wait_state('RUNNING')


def disable():
    sql('target', 'SET GLOBAL duckdb_cdc_enabled=OFF')
    wait_state('STOPPED')


def setup():
    print('Artifacts: ' + str(ROOT), flush=True)
    SUMMARY['plugin'] = {'mtime_ns': PLUGIN.stat().st_mtime_ns, 'size': PLUGIN.stat().st_size}
    start('source', ['--log-bin=' + str(ROOT / 'source-bin'),
                     '--ssl-cert=' + str(TLS / 'server-cert.pem'),
                     '--ssl-key=' + str(TLS / 'server-key.pem'),
                     '--ssl-ca=' + str(TLS / 'cacert.pem')])
    sql('source', "CREATE DATABASE cdc_test; CREATE USER 'cdc'@'127.0.0.1' IDENTIFIED BY '" +
        PASSWORD + "' REQUIRE SSL; GRANT REPLICATION SLAVE ON *.* TO 'cdc'@'127.0.0.1'; "
        'CREATE TABLE cdc_test.t(id INT PRIMARY KEY,v INT NOT NULL) ENGINE=InnoDB; '
        'CREATE TABLE cdc_test.u(id INT PRIMARY KEY,v INT NOT NULL) ENGINE=InnoDB; '
        'INSERT INTO cdc_test.t VALUES(1,10),(2,20),(3,30); '
        'INSERT INTO cdc_test.u VALUES(1,100),(2,200),(3,300)')
    source_file, position = sql('source', 'SHOW MASTER STATUS')[0].split('\t')[:2]
    SUMMARY['initial_coordinates'] = {'file': source_file, 'position': int(position)}
    check('initial source binlog', source_file, 'source-bin.000001')
    start('target', ['--plugin-dir=' + str(PLUGIN.parent), '--plugin-maturity=experimental',
                     '--plugin-load-add=ha_duckdb.so', '--duckdb-memory-limit=536870912',
                     '--duckdb-dml-in-batch=ON', '--duckdb-cdc-source-host=localhost',
                     '--duckdb-cdc-source-port=24491', '--duckdb-cdc-source-user=cdc',
                     '--duckdb-cdc-server-id=24493',
                     '--duckdb-cdc-source-password=' + PASSWORD,
                     '--duckdb-cdc-source-tls-ca=' + str(TLS / 'cacert.pem'),
                     '--duckdb-cdc-source-file=' + source_file,
                     '--duckdb-cdc-source-position=' + position,
                     '--duckdb-cdc-database=cdc_test',
                     '--duckdb-cdc-spool-directory=' + str(ROOT / 'spool'),
                     '--duckdb-cdc-batch-bytes=65536', '--duckdb-cdc-batch-timeout-ms=100',
                     '--max-allowed-packet=16384'])
    for node in ('source', 'target'):
        version = sql(node, 'SELECT VERSION()')[0]
        SUMMARY.setdefault('versions', {})[node] = version
        check(node + ' is MariaDB', 'MariaDB' in version)
    check('source TLS available', sql('source', "SHOW VARIABLES LIKE 'have_ssl'")[0], 'have_ssl\tYES')
    check('CDC default disabled', sql('target', 'SELECT @@global.duckdb_cdc_enabled'), ['0'])
    check('CDC initial state', status()['duckdb_cdc_state'], 'STOPPED')
    sql('target', 'CREATE DATABASE cdc_test; '
        'CREATE TABLE cdc_test.t(id INT PRIMARY KEY,v INT NOT NULL) ENGINE=DuckDB; '
        'CREATE TABLE cdc_test.u(id INT PRIMARY KEY,v INT NOT NULL) ENGINE=DuckDB; '
        'INSERT INTO cdc_test.t VALUES(1,10),(2,20),(3,30); '
        'INSERT INTO cdc_test.u VALUES(1,100),(2,200),(3,300)')
    check('seed parity', snapshot('target'), snapshot('source'))


def main():
    setup()
    work = ['USE cdc_test']
    for i in range(100):
        work.append(f'START TRANSACTION; INSERT INTO t VALUES({i+10},{i}); '
                    f'INSERT INTO u VALUES({i+10},{i}); '
                    f'UPDATE t SET id={i+1000},v=v+1 WHERE id={i+10}; '
                    f'DELETE FROM u WHERE id={i+10}; UPDATE u SET v=v+1 WHERE id=1; COMMIT')
    sql('source', ';'.join(work))
    enable()
    values = drain('100 transaction backlog exact rows')
    check('transactions merged', 0 < int(values['duckdb_cdc_batches']) < 100)
    check('CDC metrics recovered', values['duckdb_cdc_metrics_ready'], '1')
    check('idle pending records', values['duckdb_cdc_pending_records'], '0')
    check('idle pending payload bytes', values['duckdb_cdc_pending_bytes'], '0')
    check('idle pending age', values['duckdb_cdc_pending_age_seconds'], '0')
    master = sql('source', 'SHOW MASTER STATUS')[0].split('\t')
    check('applied source file metric', values['duckdb_cdc_applied_file'], master[0])
    check('applied source position metric', values['duckdb_cdc_applied_position'], master[1])
    check('apply duration measured', int(values['duckdb_cdc_last_apply_us']) > 0)
    check('injection duration measured', int(values['duckdb_cdc_last_injection_us']) > 0)
    check('checkpoint duration measured', int(values['duckdb_cdc_last_checkpoint_us']) > 0)
    check('commit duration measured', int(values['duckdb_cdc_last_commit_us']) > 0)
    disable()
    before = snapshot('target')
    sql('source', 'UPDATE cdc_test.t SET v=v+500 WHERE id=1')
    check('disabled worker does not apply', snapshot('target'), before)
    enable()
    drain('OFF ON catches up')
    sql('source', 'FLUSH BINARY LOGS; UPDATE cdc_test.u SET v=v+700 WHERE id=2')
    drain('binlog rotation and fresh table maps')
    check('rotation checkpoint file', sql('target', 'SELECT file FROM _duckdb_cdc.checkpoints'),
          [sql('source', 'SHOW MASTER STATUS')[0].split('\t')[0]])
    stop('target')
    sql('source', 'INSERT INTO cdc_test.t VALUES(5000,5000); DELETE FROM cdc_test.u WHERE id=3')
    start('target')
    check('restart stays disabled', sql('target', 'SELECT @@global.duckdb_cdc_enabled'), ['0'])
    enable()
    drain('graceful restart resumes original configuration without skips')
    pending = False
    for transactions in (1000, 5000):
        disable()
        for offset in range(0, transactions, 100):
            sql('source', 'USE cdc_test;' + ('START TRANSACTION; '
                'UPDATE t SET v=v+1 WHERE id=1; UPDATE u SET v=v+2 WHERE id=1; COMMIT;' * 100))
        sql('target', 'SET GLOBAL duckdb_cdc_enabled=ON')
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            values = status()
            pending = int(values['duckdb_cdc_received']) > int(values['duckdb_cdc_applied'])
            if pending:
                break
            time.sleep(0.01)
        if pending:
            break
    check('SIGKILL observed pending DML', pending)
    proc = SERVERS['target']
    proc.kill()
    proc.wait(timeout=10)
    SERVERS.pop('target')
    SUMMARY['crash'] = {'pid': proc.pid, 'returncode': proc.returncode,
                        'transactions': transactions, 'observed_status': values}
    check('controlled target SIGKILL', proc.returncode, -signal.SIGKILL)
    start('target')
    check('crash restart stays disabled', sql('target', 'SELECT @@global.duckdb_cdc_enabled'), ['0'])
    check('crash restart has no CDC error', status()['duckdb_cdc_error'], '')
    enable()
    values = drain('SIGKILL recovery exact rows without skipped or duplicate increments')
    check('recovered daemon has no error', values['duckdb_cdc_error'], '')
    check('source remains live after target crash', sql('source', 'SELECT 1'), ['1'])
    disable()
    checkpoint_before = sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints')
    sql('source', 'SET GLOBAL binlog_row_image=MINIMAL')
    sql('target', 'SET GLOBAL duckdb_cdc_enabled=ON')
    values = wait_state('ERROR', allow_error=True)
    check('MINIMAL source rejected before replay', 'ROW, FULL and CRC32' in values['duckdb_cdc_error'])
    check('MINIMAL rejection preserves checkpoint',
          sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints'), checkpoint_before)
    sql('target', 'SET GLOBAL duckdb_cdc_enabled=OFF')
    wait_state('STOPPED', allow_error=True)
    sql('source', 'SET GLOBAL binlog_row_image=FULL')
    enable()
    drain('FULL restored after source preflight rejection')
    disable()
    stop('target')
    correct_command = COMMANDS['target']
    COMMANDS['target'] = [arg.replace(str(TLS / 'cacert.pem'), str(TLS / 'untrusted-cacert.pem'))
                          for arg in correct_command]
    start('target')
    sql('target', 'SET GLOBAL duckdb_cdc_enabled=ON')
    values = wait_state('ERROR', allow_error=True)
    check('untrusted source certificate rejected', 'upstream connection failed' in values['duckdb_cdc_error'])
    check('TLS rejection preserves checkpoint',
          sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints'), checkpoint_before)
    sql('target', 'SET GLOBAL duckdb_cdc_enabled=OFF')
    wait_state('STOPPED', allow_error=True)
    stop('target')
    COMMANDS['target'] = correct_command
    start('target')
    enable()
    drain('trusted source certificate restored')
    disable()
    stop('target')
    stop('source')
    default_command = COMMANDS['target']
    COMMANDS['target'] = default_command + ['--duckdb-cdc-enabled=ON']
    start('target')
    wait_state('CONNECTING')
    check('startup with unavailable source keeps SQL available', sql('target', 'SELECT 1'), ['1'])
    check('startup enable preserved', sql('target', 'SELECT @@global.duckdb_cdc_enabled'), ['1'])
    start('source')
    wait_state('RUNNING')
    sql('source', 'UPDATE cdc_test.t SET v=v+17 WHERE id=1')
    drain('source restart and automatic CDC startup recovery')
    disable()
    stop('target')
    COMMANDS['target'] = default_command
    start('target')
    enable()
    drain('default-disabled configuration restored')
    sql('source', 'USE cdc_test; CREATE TABLE w(id INT PRIMARY KEY,v INT NOT NULL) ENGINE=InnoDB; '
        'INSERT INTO w VALUES(1,10); ALTER TABLE w ADD COLUMN extra INT DEFAULT 7; '
        'UPDATE w SET extra=8 WHERE id=1; ALTER TABLE w DROP COLUMN extra; '
        'INSERT INTO w VALUES(2,20); RENAME TABLE w TO x; UPDATE x SET v=21 WHERE id=2; '
        'TRUNCATE TABLE x; INSERT INTO x VALUES(3,30)')
    drain('CREATE ALTER RENAME TRUNCATE barriers', ('t', 'u', 'x'))
    check('created table uses DuckDB', sql('target', "SELECT UPPER(ENGINE) FROM information_schema.TABLES "
          "WHERE TABLE_SCHEMA='cdc_test' AND TABLE_NAME='x'"), ['DUCKDB'])
    sql('source', 'DROP TABLE cdc_test.x; UPDATE cdc_test.t SET v=v+1 WHERE id=2')
    drain('DROP barrier followed by DML')
    check('dropped table absent', sql('target', "SELECT COUNT(*) FROM information_schema.TABLES "
          "WHERE TABLE_SCHEMA='cdc_test' AND TABLE_NAME='x'"), ['0'])
    before = snapshot('target')
    checkpoint = sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints')
    sql('source', 'ALTER TABLE cdc_test.t MODIFY COLUMN v BIGINT NOT NULL; '
        'INSERT INTO cdc_test.t VALUES(9000,9000)')
    values = wait_state('ERROR', allow_error=True)
    check('unsupported DDL reports error', bool(values.get('duckdb_cdc_error')))
    check('pending metric after stable error', int(values['duckdb_cdc_pending_records']),
          int(values['duckdb_cdc_received']) - int(values['duckdb_cdc_applied']))
    check('pending payload metric after error', int(values['duckdb_cdc_pending_bytes']) > 0)
    check('disconnected lag is unknown', values['duckdb_cdc_apply_lag_seconds'], 'UNKNOWN')
    check('unsupported DDL remains unapplied', int(values['duckdb_cdc_received']) >
          int(values['duckdb_cdc_applied']))
    check('unsupported DDL retains spool', int(values['duckdb_cdc_spool_bytes']) > 0)
    check('unsupported DDL preserves rows', snapshot('target'), before)
    check('unsupported DDL preserves checkpoint',
          sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints'), checkpoint)
    sql('target', 'SET GLOBAL duckdb_cdc_enabled=OFF')
    stopped = wait_state('STOPPED', allow_error=True)
    check('OFF after ERROR preserves diagnostic', stopped['duckdb_cdc_error'],
          values['duckdb_cdc_error'])
    sql('target', 'INSERT INTO _duckdb_cdc.ddl_intents '
        'SELECT stream,sequence+1,file,position,\'TRUNCATE TABLE cdc_test.t\' '
        'FROM _duckdb_cdc.checkpoints')
    stop('target')
    start('target')
    sql('target', 'SET GLOBAL duckdb_cdc_enabled=ON')
    values = wait_state('ERROR', allow_error=True)
    check('unresolved DDL intent blocks restart', 'unresolved DDL intent' in values['duckdb_cdc_error'])
    check('unresolved DDL intent preserves rows', snapshot('target'), before)
    check('unresolved DDL intent preserves checkpoint',
          sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints'), checkpoint)
    sql('target', 'SET GLOBAL duckdb_cdc_enabled=OFF')
    wait_state('STOPPED', allow_error=True)
    SUMMARY['spool_files'] = [str(path.relative_to(ROOT)) for path in (ROOT / 'spool').rglob('*')]
    sql('target', 'UNINSTALL PLUGIN DUCKDB_CDC')
    check('CDC plugin metadata removed', sql('target', 'SELECT COUNT(*) FROM information_schema.PLUGINS '
          "WHERE PLUGIN_NAME='DUCKDB_CDC'"), ['0'])
    check('CDC worker status removed', status(), {})
    check('DuckDB readable after CDC uninstall', snapshot('target'), before)
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
        for node in reversed(list(SERVERS)):
            try:
                stop(node)
            except BaseException:
                code = 1
                SUMMARY['status'] = 'fail'
                SUMMARY.setdefault('cleanup_errors', []).append(traceback.format_exc())
        for handle in HANDLES:
            handle.close()
        (ROOT / 'summary.json').write_text(json.dumps(SUMMARY, indent=2) + '\n')
        print(json.dumps({'status': SUMMARY['status'], 'checks': len(SUMMARY['checks']),
                          'artifacts': str(ROOT)}), flush=True)
    raise SystemExit(code)
