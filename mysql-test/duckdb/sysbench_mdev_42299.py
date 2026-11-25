import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import threading
import time

p = argparse.ArgumentParser(description='Isolated MDEV-42299 original sysbench workload; retains all artifacts and stops its servers.')
p.add_argument('output', type=Path, help='Existing empty directory (use a short absolute path for Unix sockets)')
for name, default in [('rows', 10000000), ('tables', 8), ('seconds', 600), ('seconds16', 60), ('drain-seconds', 600)]:
    p.add_argument('--' + name, type=int, default=default)
p.add_argument('--verify-full', action='store_true', help='Stream SHA256 of every ordered row on both servers after each successful drain')
a = p.parse_args()
root = a.output.resolve()
if not root.is_dir() or any(root.iterdir()) or len(os.fsencode(str(root))) > 80:
    p.error('output must be an existing empty directory with an absolute path no longer than 80 bytes')
if os.geteuid() != 0 or min(a.rows, a.tables, a.seconds, a.seconds16, a.drain_seconds) <= 0:
    p.error('run as root; all numeric arguments must be positive')
primary = '/usr/sbin/mariadbd'
replica = '/git/DuckdbBuildOf_mdb-w-duckdb/sql/mariadbd'
plugin = '/git/duckdb-engine/build/ha_duckdb.so'
stock = '/usr/share/sysbench/oltp_read_write.lua'
children, servers, handles = [], {}, []
source_log = None
summary = {'arguments': {**vars(a), 'output': str(root)}, 'stage': 'initializing', 'phases': [], 'commands': [],
           'deviations': ['Installed primary, not Jira MariaDB 10.11.14; versions recorded below',
                          'Host architecture recorded below, not Jira aarch64',
                          'Replica DuckDB memory_limit=8GiB; other DuckDB settings retain defaults',
                          'Fixed sysbench random seed 42299; optimistic probes last 5 seconds each'],
           'timing_note': '600s/60s are source benchmark phase durations matching reported failure windows, not end-to-end completion estimates; preparation, drains and verification add time',
           'verification': 'Counts and SUM(id), SUM(k) only; c/pad corruption and compensating changes are not excluded unless --verify-full is used',
           'host': {'architecture': platform.machine(), 'platform': platform.platform(), 'cpus': os.cpu_count()},
           'settings': {'clients': 16, 'rate': 400, 'binlog_format': 'ROW', 'binlog_row_image': 'FULL',
                        'charset': 'utf8mb4', 'innodb_buffer_pool_bytes_each': 2 * 1024**3,
                        'duckdb_memory_limit_bytes': 8 * 1024**3, 'dml_per_transaction': '2 UPDATE, 1 DELETE, 1 INSERT'}}


def save(stage=None):
    if stage:
        summary['stage'] = stage
    summary['updated_at'] = time.time()
    (root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')


def resources():
    mem = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
    return {'time': time.time(), 'disk_free_bytes': shutil.disk_usage(root).free,
            'mem_available_bytes': int(mem['MemAvailable'].split()[0]) * 1024}


def guard():
    r = resources()
    if r['disk_free_bytes'] < 10 * 1024**3 or r['mem_available_bytes'] < 2 * 1024**3:
        summary['watchdog'] = r
        raise RuntimeError('Resource watchdog: less than 10GiB disk free or 2GiB MemAvailable')
    for node, proc in servers.items():
        if proc.poll() is not None:
            raise RuntimeError(node + ' server exited; inspect its logs')


def spawn(cmd, **kw):
    guard()
    summary['commands'].append(cmd)
    proc = subprocess.Popen(cmd, start_new_session=True, **kw)
    children.append(proc)
    return proc


def run(cmd, log=None, check=True, timeout=None, tick=None):
    h = open(root / log, 'wb') if log else None
    if h:
        handles.append(h)
    proc = spawn(cmd, stdout=h or subprocess.PIPE, stderr=subprocess.STDOUT if h else subprocess.PIPE)
    start = time.monotonic()
    while True:
        guard()
        if tick:
            tick()
        if timeout and time.monotonic() - start > timeout:
            raise TimeoutError(str(cmd))
        try:
            out, err = proc.communicate(timeout=1)
            break
        except subprocess.TimeoutExpired:
            pass
    result = subprocess.CompletedProcess(cmd, proc.returncode, (out or b'').decode(), (err or b'').decode())
    if h:
        h.close()
    if check and (result.returncode or (log and cmd[0] == 'sysbench' and
                                        b'FATAL:' in (root / log).read_bytes())):
        raise RuntimeError(f'{cmd[0]} exited {result.returncode}: {result.stderr or result.stdout}; log={log}')
    return result


def client(node):
    return ['mariadb', '--no-defaults', '--protocol=SOCKET', '--socket=' + str(root / (node + '.sock')),
            '-uroot', '--default-character-set=utf8mb4', '--batch', '--raw']


def sql(node, query, timeout=3600):
    return run(client(node) + ['-e', query], timeout=timeout).stdout


def record_sql(node, query, name):
    text = sql(node, query)
    (root / name).write_text(text)
    return text


def boot(node, binary, port, sid):
    save('bootstrap-' + node)
    run(['mariadb-install-db', '--no-defaults', '--user=root', '--datadir=' + str(root / node),
         '--auth-root-authentication-method=normal', '--skip-test-db'], node + '-bootstrap.log', timeout=300)
    cmd = [binary, '--no-defaults', '--user=root', '--datadir=' + str(root / node),
           '--socket=' + str(root / (node + '.sock')), '--pid-file=' + str(root / (node + '.pid')),
           '--log-error=' + str(root / (node + '.err')), '--port=' + str(port), '--bind-address=127.0.0.1',
           '--skip-name-resolve', '--server-id=' + str(sid), '--innodb-buffer-pool-size=2G',
           '--binlog-format=ROW', '--binlog-row-image=FULL', '--character-set-server=utf8mb4']
    if node == 'primary':
        cmd += ['--log-bin=' + str(root / 'source-bin')]
    else:
        cmd += ['--relay-log=' + str(root / 'replica-relay'), '--skip-slave-start', '--slave-parallel-threads=8',
                '--slave-parallel-mode=optimistic', '--plugin-dir=' + str(Path(plugin).parent),
                '--plugin-maturity=experimental', '--plugin-load-add=ha_duckdb.so', '--duckdb-memory-limit=8589934592']
    h = open(root / (node + '-console.log'), 'wb')
    handles.append(h)
    servers[node] = spawn(cmd, stdout=h, stderr=subprocess.STDOUT)
    for _ in range(120):
        result = run(client(node) + ['-e', 'SELECT 1'], check=False, timeout=5)
        if not result.returncode:
            return
        time.sleep(1)
    raise TimeoutError(node + ' startup')


def stream(proc, consume, finish=lambda: None):
    errors = []
    def pump():
        try:
            while True:
                chunk = proc.stdout.read(65536)
                if not chunk:
                    break
                consume(chunk)
        except Exception as exc:
            errors.append(str(exc))
        finally:
            proc.stdout.close()
            try:
                finish()
            except Exception as exc:
                errors.append(str(exc))
    thread = threading.Thread(target=pump, daemon=True)
    thread.start()
    while thread.is_alive() or proc.poll() is None:
        guard()
        if errors:
            raise RuntimeError(str(errors))
        time.sleep(0.5)
    if errors or proc.returncode:
        raise RuntimeError(f'Stream failed: rc={proc.returncode}, errors={errors}')


def status():
    lines = sql('replica', 'SHOW SLAVE STATUS', timeout=10).splitlines()
    return dict(zip(lines[0].split('\t'), lines[1].split('\t'))) if len(lines) > 1 else {}


def sample(label):
    state = status()
    row = {'phase': label, **resources(), 'slave_status': state,
           'retries': sql('replica', "SHOW GLOBAL STATUS LIKE 'Slave_retried_transactions'", timeout=10).strip(),
           'source_tps_latest_report': None, 'source_log': source_log}
    if source_log and (root / source_log).exists():
        with open(root / source_log, 'rb') as h:
            h.seek(max(0, os.fstat(h.fileno()).st_size - 8192))
            reports = re.findall(rb'\[\s*([0-9]+)s\s*\].*?tps:\s*([0-9.]+)', h.read())
        if reports:
            row['source_tps_latest_report'] = {'elapsed_seconds': int(reports[-1][0]), 'tps': float(reports[-1][1])}
    with open(root / 'samples.jsonl', 'a') as h:
        h.write(json.dumps(row) + '\n')
    save()
    return state


def drain(label):
    target = sql('primary', 'SHOW MASTER STATUS', timeout=10).splitlines()[1].split('\t')[:2]
    deadline = time.monotonic() + a.drain_seconds
    while True:
        state = sample(label)
        healthy = all(state.get(key, '0') == '0' for key in ['Last_SQL_Errno', 'Last_IO_Errno'])
        caught = healthy and state.get('Relay_Master_Log_File') == target[0] and state.get('Exec_Master_Log_Pos') == target[1]
        if caught:
            sql('replica', 'STOP SLAVE SQL_THREAD', timeout=120)
            return {'target': target, 'caught_up': True, 'status': state,
                    'quiescent_status': status()}
        if not healthy or time.monotonic() >= deadline:
            return {'target': target, 'caught_up': False, 'status': state}
        for _ in range(10):
            guard()
            time.sleep(min(1, max(0, deadline - time.monotonic())))


def verify(label, initial=False):
    result = {}
    summary.setdefault('verification_results', {})[label] = result
    for n in range(1, a.tables + 1):
        q = f'SELECT COUNT(*),SUM(id),SUM(k) FROM sbtest.sbtest{n}'
        values = {node: sql(node, q).splitlines()[1] for node in servers}
        entry = result[str(n)] = {'aggregates': values, 'equal': values['primary'] == values['replica']}
        if initial and any(int(v.split('\t')[0]) != a.rows for v in values.values()):
            raise RuntimeError('Initial row count mismatch: ' + str(entry))
        if a.verify_full:
            hashes = {}
            for node in servers:
                h = hashlib.sha256()
                err = open(root / f'{label}-{node}-sbtest{n}-hash.err', 'wb')
                handles.append(err)
                proc = spawn(client(node) + ['--skip-column-names', '--quick', '-e',
                             f'SELECT id,k,HEX(c),HEX(pad) FROM sbtest.sbtest{n} ORDER BY id'], stdout=subprocess.PIPE, stderr=err)
                stream(proc, h.update)
                hashes[node] = h.hexdigest()
            entry['sha256'] = hashes
            entry['equal'] = entry['equal'] and hashes['primary'] == hashes['replica']
        save()
        if not entry['equal']:
            raise RuntimeError('Verification mismatch: ' + str(entry))


def workload(label, seconds):
    global source_log
    source_log = label + '-sysbench.log'
    last = [0]
    def tick():
        if time.monotonic() - last[0] >= 10:
            last[0] = time.monotonic()
            state = sample(label)
            if label.startswith('conservative') and any(
                    state.get(key, '0') != '0' for key in ['Last_SQL_Errno', 'Last_IO_Errno']):
                raise RuntimeError(label + ' replication error: ' + str(state))
    run(sb + ['--time=' + str(seconds), 'run'], label + '-sysbench.log', timeout=seconds + 120, tick=tick)
    sample(label + '-end')


def configure(workers, mode):
    sql('replica', f"STOP SLAVE; SET GLOBAL slave_parallel_threads={workers}; SET GLOBAL slave_parallel_mode='{mode}'; START SLAVE IO_THREAD;", timeout=30)
    return run(client('replica') + ['-e', 'START SLAVE SQL_THREAD'], check=False, timeout=30)


def terminate(proc, sig=signal.SIGTERM):
    try:
        os.killpg(proc.pid, sig)
    except ProcessLookupError:
        pass


def interrupted(signum, frame):
    raise KeyboardInterrupt('signal ' + str(signum))


signal.signal(signal.SIGTERM, interrupted)
try:
    save()
    for tool in ['mariadb', 'mariadb-install-db', 'mariadb-dump', 'mariadb-admin', 'sysbench', primary, replica]:
        if not shutil.which(tool):
            raise RuntimeError('Missing executable: ' + tool)
    for path in [plugin, stock, '/usr/share/sysbench/oltp_common.lua']:
        h = hashlib.sha256()
        with open(path, 'rb') as source:
            for chunk in iter(lambda: source.read(65536), b''):
                guard()
                h.update(chunk)
        summary.setdefault('input_sha256', {})[path] = h.hexdigest()
    summary['initial_resources'] = resources()
    for node, binary, port, sid in [('primary', primary, 24391, 422991), ('replica', replica, 24392, 422992)]:
        boot(node, binary, port, sid)
        summary.setdefault('versions', {})[node] = sql(node, 'SELECT VERSION(); SHOW VARIABLES LIKE "duckdb%"')
        record_sql(node, 'SHOW GLOBAL VARIABLES', node + '-variables.tsv')
    sql('primary', "CREATE DATABASE sbtest CHARACTER SET utf8mb4; CREATE USER 'bench'@'127.0.0.1' IDENTIFIED BY 'isolated-42299'; GRANT ALL ON sbtest.* TO 'bench'@'127.0.0.1'; GRANT BINLOG ADMIN ON *.* TO 'bench'@'127.0.0.1'; CREATE USER 'repl'@'127.0.0.1' IDENTIFIED BY 'isolated-42299'; GRANT REPLICATION SLAVE ON *.* TO 'repl'@'127.0.0.1';")
    sql('replica', 'CREATE DATABASE sbtest CHARACTER SET utf8mb4')
    wrapper = root / 'workload.lua'
    wrapper.write_text("dofile('/usr/share/sysbench/oltp_read_write.lua')\nlocal original = create_table\nfunction create_table(drv, con, n)\n  con:query('SET SESSION sql_log_bin=0')\n  original(drv, con, n)\nend\n")
    sb = ['sysbench', str(wrapper), '--db-driver=mysql', '--mysql-host=127.0.0.1', '--mysql-port=24391',
          '--mysql-user=bench', '--mysql-password=isolated-42299', '--mysql-db=sbtest',
          '--mysql-storage-engine=innodb', '--auto-inc=off', '--tables=' + str(a.tables), '--table-size=' + str(a.rows),
          '--threads=16', '--rate=400', '--report-interval=10', '--rand-seed=42299',
          '--index-updates=1', '--non-index-updates=1', '--delete-inserts=1']
    summary['versions']['sysbench'] = run(['sysbench', '--version']).stdout
    save('sysbench-prepare')
    run(sb + ['prepare'], 'prepare-sysbench.log')
    for n in range(1, a.tables + 1):
        ddl = record_sql('primary', f'SHOW CREATE TABLE sbtest.sbtest{n}', f'sbtest{n}-source-ddl.tsv').split('\n', 1)[1].split('\t', 1)[1].strip()
        ddl, changed = re.subn(r'ENGINE=InnoDB', 'ENGINE=DuckDB', ddl)
        if changed != 1:
            raise RuntimeError('Unexpected source DDL')
        sql('replica', 'USE sbtest; ' + ddl)
    save('streaming-snapshot')
    dump_err = open(root / 'dump.err', 'wb')
    load_err = open(root / 'load.err', 'wb')
    prefix = open(root / 'dump-header.sql', 'wb')
    handles.extend([dump_err, load_err, prefix])
    loader = spawn(client('replica') + ['sbtest'], stdin=subprocess.PIPE, stdout=load_err, stderr=subprocess.STDOUT)
    dumper = spawn(['mariadb-dump', '--no-defaults', '--socket=' + str(root / 'primary.sock'), '-uroot',
                    '--default-character-set=utf8mb4', '--single-transaction', '--master-data=2', '--no-create-info',
                    '--skip-add-locks', '--skip-disable-keys', 'sbtest'], stdout=subprocess.PIPE, stderr=dump_err)
    remaining = [65536]
    def transfer(chunk):
        prefix.write(chunk[:remaining[0]])
        remaining[0] = max(0, remaining[0] - len(chunk))
        loader.stdin.write(chunk)
    stream(dumper, transfer, loader.stdin.close)
    prefix.close()
    while loader.poll() is None:
        guard()
        time.sleep(1)
    if loader.returncode:
        raise RuntimeError('Snapshot import failed; see load.err')
    pos = re.search(r"CHANGE MASTER TO MASTER_LOG_FILE='([^']+)', MASTER_LOG_POS=(\d+)", (root / 'dump-header.sql').read_text(errors='replace'))
    if not pos:
        raise RuntimeError('Dump snapshot position not found in saved header')
    summary['snapshot_position'] = list(pos.groups())
    verify('initial', initial=True)
    sql('replica', f"CHANGE MASTER TO MASTER_HOST='127.0.0.1',MASTER_PORT=24391,MASTER_USER='repl',MASTER_PASSWORD='isolated-42299',MASTER_LOG_FILE='{pos[1]}',MASTER_LOG_POS={pos[2]},MASTER_USE_GTID=no; START SLAVE IO_THREAD;")
    for workers in [8, 16]:
        label = f'optimistic{workers}'
        save(label)
        phase = {'name': label}
        summary['phases'].append(phase)
        start = configure(workers, 'optimistic')
        phase['start_sql_thread'] = {'returncode': start.returncode, 'stdout': start.stdout, 'stderr': start.stderr}
        workload(label, 5)
        state = phase['status'] = sample(label)
        phase['expected_1235'] = state.get('Last_SQL_Errno') == '1235' or bool(re.search(r'\b1235\b', start.stderr))
        save()
        if not phase['expected_1235']:
            raise RuntimeError('Optimistic probe did not produce expected rejection 1235')
        start = configure(workers, 'conservative')
        if start.returncode:
            raise RuntimeError(start.stderr)
        phase['conservative_recovery'] = drain(label + '-recovery')
        save()
        if not phase['conservative_recovery']['caught_up']:
            raise RuntimeError('Recovery incomplete; no events skipped')
    for workers, seconds in [(8, a.seconds), (16, a.seconds16)]:
        label = f'conservative{workers}'
        save(label)
        phase = {'name': label, 'requested_seconds': seconds}
        summary['phases'].append(phase)
        start = configure(workers, 'conservative')
        if start.returncode:
            raise RuntimeError(start.stderr)
        workload(label, seconds)
        phase['drain'] = drain(label + '-drain')
        save()
        if not phase['drain']['caught_up']:
            raise RuntimeError(label + ' incomplete: drain timeout or replication error')
        verify(label)
    summary['success'] = True
    save('completed')
except BaseException as exc:
    summary.update(success=False, error=repr(exc))
    save('failed-or-interrupted')
    raise
finally:
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    for proc in reversed(children):
        if proc not in servers.values() and proc.poll() is None:
            terminate(proc)
    for node, proc in reversed(list(servers.items())):
        if proc.poll() is None:
            try:
                subprocess.run(['mariadb-admin', '--no-defaults', '--socket=' + str(root / (node + '.sock')), '-uroot', 'shutdown'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
            except subprocess.TimeoutExpired:
                pass
    for proc in reversed(children):
        if proc.poll() is None:
            terminate(proc)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            terminate(proc, signal.SIGKILL)
            proc.wait()
    for h in handles:
        h.close()
    summary['final_resources'] = resources()
    summary['servers_stopped'] = all(proc.poll() is not None for proc in servers.values())
    save()
