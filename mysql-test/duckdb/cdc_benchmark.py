import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import threading
import time
import traceback


p = argparse.ArgumentParser(description='Retained isolated actual-CDC single/dual-target smoke/perf harness, not an original-scale gate')
p.add_argument('output', type=Path, help='Existing empty directory; retained, never removed')
for name, default in [('rows', 1000), ('tables', 2), ('threads', 4), ('rate', 100), ('seconds', 10), ('drain-seconds', 60)]:
    p.add_argument('--' + name, type=int, default=default)
p.add_argument('--batch-timeout-ms', type=int, default=100)
p.add_argument('--spool-group-timeout-ms', type=int)
p.add_argument('--spool-group-bytes', type=int)
p.add_argument('--batched-only', action='store_true')
p.add_argument('--capture-only', action='store_true', help='Validate received cursor with an instrumented capture-only plugin')
p.add_argument('--capture-no-fsync', action='store_true', help='Label an instrumented non-durable capture experiment; does not change plugin behavior')
p.add_argument('--plugin', default='/git/duckdb-engine/build/ha_duckdb.so')
a = p.parse_args()
root = a.output.resolve()
if not root.is_dir() or any(root.iterdir()) or len(os.fsencode(str(root))) > 75:
    p.error('output must be an existing empty directory with path <=75 bytes')
if os.geteuid() or min(a.rows, a.tables, a.threads, a.rate, a.seconds, a.drain_seconds, a.batch_timeout_ms) <= 0:
    p.error('run as root with positive numeric arguments')
if a.rows * a.tables > 1000000 or a.seconds > 600 or a.threads > 64 or a.rate > 1600:
    p.error('smoke-only guard: <=1M total rows, <=600 seconds, <=64 threads, <=1600 requested TPS; full-scale dual-target capacity preflight not implemented')
if a.spool_group_timeout_ms is not None and not 1 <= a.spool_group_timeout_ms <= 60000:
    p.error('spool group timeout must be 1..60000 ms')
if a.spool_group_bytes is not None and not 1024 <= a.spool_group_bytes <= 64 * 1024 * 1024:
    p.error('spool group byte budget must be 1024..64MiB')
if a.capture_only and not a.batched_only:
    p.error('capture-only measurement requires --batched-only')
if a.capture_no_fsync and not a.capture_only:
    p.error('--capture-no-fsync requires --capture-only')
plugin = Path(a.plugin).resolve()
if plugin.name != 'ha_duckdb.so' or not plugin.is_file():
    p.error('--plugin must point to an existing ha_duckdb.so')
tls = Path('/usr/share/mariadb/mariadb-test/std_data')
password = 'cdc_isolated_test_42'
servers, children, handles = {}, [], []
target_specs = [('batched', 24594, 24595, 4194304)] if a.batched_only else [
    ('per_tx', 24592, 24593, 1024), ('batched', 24594, 24595, 4194304)]
targets = [spec[0] for spec in target_specs]
summary = {'status': 'running', 'artifacts': str(root), 'arguments': {**vars(a), 'output': str(root)},
           'commands': [], 'limitations': ['Smoke-only capacity guard; 80M raw rows >=16GB and source + two DuckDB + WAL/spools can approach 55GB',
           'Actual CDC only; no native replication barrier or native baseline comparison',
           'Source and selected targets share host resources; requested rate is not achieved throughput',
           'Lag is approximate: sequential same-host samples, second-resolution source event timestamps, not a source commit-time SLA; idle age can rise'],
           'settings': {'ports': [24591] + [spec[1] for spec in target_specs],
                        'reader_ids': [spec[2] for spec in target_specs], 'targets': targets,
                        'per_tx_batch_bytes': 1024, 'batched_batch_bytes': 4194304,
                        'batch_timeout_ms': a.batch_timeout_ms, 'prepare_threads': 1,
                        'received_durability': 'buffered-no-transaction-fsync' if a.capture_no_fsync else 'fsync',
                        'workload': '2 UPDATE / 1 DELETE / 1 INSERT, no SELECT, FULL row images'}}


def save():
    (root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')


def resources():
    mem = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
    return {'time': time.time(), 'disk_free_bytes': shutil.disk_usage(root).free,
            'mem_available_bytes': int(mem['MemAvailable'].split()[0]) * 1024}


def guard():
    r = resources()
    if r['disk_free_bytes'] < 10 * 1024**3 or r['mem_available_bytes'] < 2 * 1024**3:
        summary['watchdog'] = r
        raise RuntimeError('Watchdog: need >=10GiB disk and >=2GiB MemAvailable')
    for node, proc in servers.items():
        if proc.poll() is not None:
            raise RuntimeError(node + ' exited: inspect retained logs')


def spawn(cmd, **kwargs):
    guard()
    summary['commands'].append(cmd)
    proc = subprocess.Popen(cmd, start_new_session=True, **kwargs)
    children.append(proc)
    return proc


def logfile(name):
    h = (root / name).open('wb')
    handles.append(h)
    return h


def run(cmd, name=None, check=True, timeout=60, tick=None):
    name = name or ('command-%05d' % len(summary['commands']))
    out, err = logfile(name + '.out'), logfile(name + '.err')
    proc = spawn(cmd, stdout=out, stderr=err)
    deadline = time.monotonic() + timeout
    while proc.poll() is None:
        started = time.monotonic()
        guard()
        if tick:
            tick()
        if time.monotonic() >= deadline:
            raise TimeoutError(str(cmd))
        try:
            proc.wait(timeout=max(0.01, 1 - (time.monotonic() - started)))
        except subprocess.TimeoutExpired:
            pass
    out.close()
    err.close()
    text = (root / (name + '.out')).read_text()
    if check and (proc.returncode or (cmd[0] == 'sysbench' and 'FATAL:' in text)):
        raise RuntimeError(str(cmd) + ': see ' + name + '.out/.err')
    return proc.returncode, text


def client(node):
    return ['mariadb', '--no-defaults', '--protocol=SOCKET', '--socket=' + str(root / (node + '.sock')),
            '-uroot', '--default-character-set=utf8mb4', '--batch', '--raw', '--skip-column-names', '--quick', '--skip-reconnect']


def sql(node, query):
    return run(client(node) + ['-e', query])[1].rstrip('\n')


def boot(node, binary, port, extra):
    if (root / node).exists():
        raise RuntimeError('Refusing to initialize existing datadir')
    run(['mariadb-install-db', '--no-defaults', '--user=root', '--datadir=' + str(root / node),
         '--auth-root-authentication-method=normal', '--skip-test-db'], node + '-bootstrap', timeout=300)
    cmd = [binary, '--no-defaults', '--user=root', '--datadir=' + str(root / node),
           '--socket=' + str(root / (node + '.sock')), '--pid-file=' + str(root / (node + '.pid')),
           '--log-error=' + str(root / (node + '.err')), '--port=' + str(port), '--bind-address=127.0.0.1',
           '--server-id=' + str(port), '--skip-name-resolve', '--innodb-buffer-pool-size=64M',
           '--character-set-server=utf8mb4', '--binlog-format=ROW', '--binlog-row-image=FULL'] + extra
    servers[node] = spawn(cmd, stdout=logfile(node + '-console.log'), stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if run(client(node) + ['-e', 'SELECT 1'], check=False)[0] == 0:
            return
    raise TimeoutError(node + ' startup')


def pump(proc, consume, finish=lambda: None):
    errors = []
    def copy():
        try:
            for chunk in iter(lambda: proc.stdout.read(65536), b''):
                consume(chunk)
            finish()
        except BaseException as exc:
            errors.append(repr(exc))
        finally:
            proc.stdout.close()
    thread = threading.Thread(target=copy, daemon=True)
    thread.start()
    while thread.is_alive() or proc.poll() is None:
        guard()
        if errors:
            raise RuntimeError(str(errors))
        time.sleep(1)
    if errors or proc.returncode:
        raise RuntimeError('Streaming command failed: ' + str(errors) + '; rc=' + str(proc.returncode))


def head():
    return sql('source', 'SHOW MASTER STATUS').split('\t')[:2]


def sample(phase):
    row = {'phase': phase, **resources(), 'source_head': head(), 'targets': {}}
    for node in targets:
        values = dict(line.split('\t', 1) for line in sql(node, "SHOW GLOBAL STATUS LIKE 'Duckdb_cdc_%'").splitlines())
        values = {key.lower(): value for key, value in values.items()}
        row['targets'][node] = values
        now = time.time()
        for metric in ('received', 'applied'):
            ts = int(values.get('duckdb_cdc_' + metric + '_commit_timestamp', '0'))
            values[metric + '_source_ts_age_approx_seconds'] = max(0, now - ts) if ts else None
        if values.get('duckdb_cdc_state') == 'ERROR':
            raise RuntimeError(node + ': ' + values.get('duckdb_cdc_error', str(values)))
        if a.capture_only:
            if int(values.get('duckdb_cdc_applied', '0')) or int(values.get('duckdb_cdc_apply_calls', '0')):
                raise RuntimeError('Capture-only measurement unexpectedly applied records')
            sentinel = 'CAPTURE_ONLY_NO_TX_SYNC' if a.capture_no_fsync else 'CAPTURE_ONLY'
            if values.get('duckdb_cdc_apply_stage') == sentinel:
                summary['capture_only_bypass_observed'] = True
        if node == 'per_tx' and int(values.get('duckdb_cdc_batch_records', '0')) > 1:
            raise RuntimeError('Per-TX label invalid: observed input batch_records >1')
    with (root / 'samples.jsonl').open('a') as h:
        h.write(json.dumps(row) + '\n')
    print(json.dumps(row), flush=True)
    return row


def convergence(samples, node):
    samples = [row for row in samples if row['time'] >= samples[0]['time'] + a.seconds / 2]
    result = {'samples': len(samples)}
    if len(samples) < 2:
        return result
    elapsed = samples[-1]['time'] - samples[0]['time']
    for metric in ('received', 'applied'):
        values = [(row['time'], row['targets'][node][metric + '_source_ts_age_approx_seconds'])
                  for row in samples if row['targets'][node][metric + '_source_ts_age_approx_seconds'] is not None]
        if len(values) > 1:
            mt = sum(t for t, v in values) / len(values)
            mv = sum(v for t, v in values) / len(values)
            result[metric + '_age_seconds'] = {
                'start': values[0][1], 'end': values[-1][1], 'max': max(v for t, v in values),
                'slope_seconds_per_second': sum((t - mt) * (v - mv) for t, v in values) /
                                          sum((t - mt) ** 2 for t, v in values)}
        key = 'duckdb_cdc_' + metric
        result[metric + '_records_per_second'] = (int(samples[-1]['targets'][node][key]) -
                                                   int(samples[0]['targets'][node][key])) / elapsed
    return result


def verify(nodes, label):
    result = summary.setdefault('hashes', {}).setdefault(label, {})
    for n in range(1, a.tables + 1):
        hashes = result[str(n)] = {}
        for node in nodes:
            digest = hashlib.sha256()
            proc = spawn(client(node) + ['-e', f'SELECT id,k,HEX(c),HEX(pad) FROM cdc_bench.sbtest{n} ORDER BY id'],
                         stdout=subprocess.PIPE, stderr=logfile(f'{label}-{node}-{n}-hash.err'))
            pump(proc, digest.update)
            hashes[node] = digest.hexdigest()
        save()
        if len(set(hashes.values())) != 1:
            raise RuntimeError('Full ordered row hash mismatch: ' + str(hashes))


def main():
    summary['initial_resources'] = resources()
    guard()
    summary['plugin'] = {'size': plugin.stat().st_size, 'mtime_ns': plugin.stat().st_mtime_ns}
    boot('source', '/usr/sbin/mariadbd', 24591, ['--log-bin=' + str(root / 'source-bin'),
         '--ssl-cert=' + str(tls / 'server-cert.pem'), '--ssl-key=' + str(tls / 'server-key.pem'), '--ssl-ca=' + str(tls / 'cacert.pem')])
    sql('source', "CREATE DATABASE cdc_bench; CREATE USER 'bench'@'127.0.0.1' IDENTIFIED BY '" + password +
        "'; GRANT ALL ON cdc_bench.* TO 'bench'@'127.0.0.1'; GRANT BINLOG ADMIN ON *.* TO 'bench'@'127.0.0.1'; "
        "CREATE USER 'cdc'@'127.0.0.1' IDENTIFIED BY '" + password + "' REQUIRE SSL; GRANT REPLICATION SLAVE ON *.* TO 'cdc'@'127.0.0.1'")
    wrapper = root / 'workload.lua'
    wrapper.write_text("dofile('/usr/share/sysbench/oltp_read_write.lua')\nlocal original = create_table\nfunction create_table(drv, con, n)\n  con:query('SET SESSION sql_log_bin=0')\n  original(drv, con, n)\nend\n")
    sb = ['sysbench', str(wrapper), '--db-driver=mysql', '--mysql-host=127.0.0.1', '--mysql-port=24591',
          '--mysql-user=bench', '--mysql-password=' + password, '--mysql-db=cdc_bench', '--mysql-storage-engine=innodb',
          '--auto-inc=off', '--tables=' + str(a.tables), '--table-size=' + str(a.rows), '--threads=' + str(a.threads),
          '--rate=' + str(a.rate), '--report-interval=1', '--rand-seed=42299', '--point-selects=0', '--range-selects=off',
          '--index-updates=1', '--non-index-updates=1', '--delete-inserts=1']
    before = head()
    prepare = [arg for arg in sb if not arg.startswith('--threads=')]
    run(prepare + ['--threads=1', 'prepare'], 'prepare-sysbench', timeout=3600)
    summary['source_row_counts'] = [int(sql('source', f'SELECT COUNT(*) FROM cdc_bench.sbtest{n}'))
                                    for n in range(1, a.tables + 1)]
    if summary['source_row_counts'] != [a.rows] * a.tables:
        raise RuntimeError('Snapshot row counts differ from requested fixture')
    initial = head()
    if initial != before:
        raise RuntimeError('Preparation unexpectedly generated binlog events')
    summary['snapshot_position'] = initial
    group_options = []
    if a.spool_group_timeout_ms is not None:
        group_options.append('--duckdb-cdc-spool-group-timeout-ms=' + str(a.spool_group_timeout_ms))
    if a.spool_group_bytes is not None:
        group_options.append('--duckdb-cdc-spool-group-bytes=' + str(a.spool_group_bytes))
    for node, port, reader, size in target_specs:
        boot(node, '/git/BuildOf_mdb-w-duckdb/sql/mariadbd', port,
             ['--plugin-dir=' + str(plugin.parent), '--plugin-maturity=experimental', '--plugin-load-add=ha_duckdb.so',
              '--duckdb-memory-limit=1073741824', '--duckdb-dml-in-batch=ON', '--duckdb-cdc-source-host=localhost',
              '--duckdb-cdc-source-port=24591', '--duckdb-cdc-source-user=cdc', '--duckdb-cdc-source-password=' + password,
              '--duckdb-cdc-source-tls-ca=' + str(tls / 'cacert.pem'), '--duckdb-cdc-server-id=' + str(reader),
              '--duckdb-cdc-source-file=' + initial[0], '--duckdb-cdc-source-position=' + initial[1],
              '--duckdb-cdc-database=cdc_bench', '--duckdb-cdc-spool-directory=' + str(root / (node + '-spool')),
              '--duckdb-cdc-batch-bytes=' + str(size),
              '--duckdb-cdc-batch-timeout-ms=' + str(a.batch_timeout_ms if node == 'batched' else 100)] + group_options)
        sql(node, 'CREATE DATABASE cdc_bench')
        for n in range(1, a.tables + 1):
            ddl = sql('source', f'SHOW CREATE TABLE cdc_bench.sbtest{n}').split('\t', 1)[1]
            ddl, count = re.subn(r'ENGINE=InnoDB', 'ENGINE=DuckDB', ddl)
            if count != 1:
                raise RuntimeError('Unexpected fixture DDL')
            sql(node, 'USE cdc_bench; ' + ddl)
        loader = spawn(client(node) + ['cdc_bench'], stdin=subprocess.PIPE,
                       stdout=logfile(node + '-load.out'), stderr=logfile(node + '-load.err'))
        dumper = spawn(['mariadb-dump', '--no-defaults', '--socket=' + str(root / 'source.sock'), '-uroot',
                        '--single-transaction', '--quick', '--no-create-info', '--skip-add-locks', '--skip-disable-keys', 'cdc_bench'],
                       stdout=subprocess.PIPE, stderr=logfile(node + '-dump.err'))
        pump(dumper, loader.stdin.write, loader.stdin.close)
        while loader.poll() is None:
            guard()
            time.sleep(1)
        if loader.returncode:
            raise RuntimeError(node + ' snapshot load failed')
    if head() != initial:
        raise RuntimeError('Source not quiescent during snapshot')
    verify(['source'] + targets, 'bootstrap')
    summary['versions'] = {node: sql(node, 'SELECT VERSION()') for node in servers}
    summary['runtime_cdc_settings'] = {node: sql(node, 'SELECT @@global.duckdb_cdc_batch_timeout_ms,'
        '@@global.duckdb_cdc_batch_bytes').split('\t') for node in targets}
    if summary['runtime_cdc_settings']['batched'] != [str(a.batch_timeout_ms), '4194304']:
        raise RuntimeError('Runtime batched settings differ from requested settings')
    if group_options:
        summary['runtime_spool_group_settings'] = {node: sql(node,
            'SELECT @@global.duckdb_cdc_spool_group_timeout_ms,@@global.duckdb_cdc_spool_group_bytes').split('\t')
            for node in targets}
        for values in summary['runtime_spool_group_settings'].values():
            if ((a.spool_group_timeout_ms is not None and int(values[0]) != a.spool_group_timeout_ms) or
                    (a.spool_group_bytes is not None and int(values[1]) != a.spool_group_bytes)):
                raise RuntimeError('Runtime spool group settings differ from requested settings')
    summary['native_replication_metadata_not_cdc'] = {node: sql(node, 'SHOW SLAVE STATUS') for node in targets}
    for node in targets:
        sql(node, 'SET GLOBAL duckdb_cdc_enabled=ON')
    load_samples = []
    run(sb + ['--time=' + str(a.seconds), 'run'], 'workload-sysbench', timeout=a.seconds + 120,
        tick=lambda: load_samples.append(sample('load')))
    report = (root / 'workload-sysbench.out').read_text()
    tx = re.search(r'transactions:\s+(\d+)\s+\(([\d.]+) per sec', report)
    counts = {key: int(re.search(r'^\s*' + key + r':\s+(\d+)', report, re.M)[1]) for key in ('read', 'write', 'other')}
    if not tx or counts != {'read': 0, 'write': 4 * int(tx[1]), 'other': 2 * int(tx[1])}:
        raise RuntimeError('Unexpected workload: need exactly four writes, BEGIN/COMMIT and no reads per transaction')
    summary['source_workload'] = {'transactions': int(tx[1]), 'achieved_tps': float(tx[2]), 'queries': counts}
    summary['load_samples'] = len(load_samples)
    summary['steady_second_half'] = {node: convergence(load_samples, node) for node in targets}
    load_end = sample('load_end')
    summary['load_end_metrics'] = load_end['targets']
    summary['load_finished_at'] = load_end['time']
    summary['catchup_after_load_seconds'] = {}
    summary['load_lag_trend'] = {node: [r['targets'][node]['applied_source_ts_age_approx_seconds'] for r in load_samples]
                                 for node in targets}
    final_head, caught = head(), {}
    cursor = 'received' if a.capture_only else 'applied'
    summary['catchup_cursor'] = cursor
    deadline = time.monotonic() + a.drain_seconds
    while True:
        row = sample('drain')
        for node, values in row['targets'].items():
            caught[node] = (values.get('duckdb_cdc_' + cursor + '_file') == final_head[0] and
                            values.get('duckdb_cdc_' + cursor + '_position') == final_head[1] and
                            (a.capture_only or values.get('duckdb_cdc_pending_records') == '0'))
            if caught[node] and node not in summary['catchup_after_load_seconds']:
                summary['catchup_after_load_seconds'][node] = max(0, time.time() - load_end['time'])
        if all(caught.values()) or time.monotonic() >= deadline:
            break
        time.sleep(1)
    summary['caught_up'] = caught
    summary['final_source_head'] = final_head
    summary['final_metrics'] = row['targets']
    if 'per_tx' in targets:
        values = row['targets']['per_tx']
        if caught['per_tx'] and (values['duckdb_cdc_batches'] != values['duckdb_cdc_applied'] or int(values['duckdb_cdc_applied']) != int(tx[1])):
            raise RuntimeError('Per-TX cumulative evidence invalid: need exactly one input record and one batch per source transaction')
        if int(values['duckdb_cdc_batch_records']) != 1 or int(values['duckdb_cdc_batch_bytes']) <= 1024:
            raise RuntimeError('Per-TX input evidence absent: need batch_records=1 and FULL payload >1024 bytes')
    if a.capture_only:
        if not summary.get('capture_only_bypass_observed'):
            raise RuntimeError('Instrumented capture-only code path was not observed')
        verify(targets, 'capture_unchanged')
        for table, hashes in summary['hashes']['capture_unchanged'].items():
            for node, digest in hashes.items():
                if summary['hashes']['bootstrap'][table][node] != digest:
                    raise RuntimeError('Capture-only target data unexpectedly changed')
        summary['capture_checkpoints'] = {node: sql(node, 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints')
                                           for node in targets}
        if any(value != '0\t' + initial[0] + '\t' + initial[1] for value in summary['capture_checkpoints'].values()):
            raise RuntimeError('Capture-only checkpoint unexpectedly advanced')
        if any(caught[node] and int(row['targets'][node]['duckdb_cdc_received']) != int(tx[1]) for node in targets):
            raise RuntimeError('Captured record count differs from source transactions')
    else:
        verify(['source'] + [node for node in caught if caught[node]], 'final')
    summary['status'] = 'pass' if all(caught.values()) else 'not_caught_up'


def interrupted(signum, frame):
    raise KeyboardInterrupt('signal ' + str(signum))


if __name__ == '__main__':
    print('Artifacts: ' + str(root), flush=True)
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except BaseException:
        summary.update(status='fail', error=traceback.format_exc())
        print(summary['error'], flush=True)
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        for proc in reversed(children):
            if proc.poll() is None:
                proc.terminate()
        for proc in reversed(children):
            try:
                proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for handle in handles:
            handle.close()
        summary['final_resources'] = resources()
        summary['server_exits'] = {node: proc.returncode for node, proc in servers.items()}
        save()
        print(json.dumps({'status': summary['status'], 'summary': str(root / 'summary.json')}), flush=True)
    raise SystemExit(0 if summary['status'] == 'pass' else 1)
