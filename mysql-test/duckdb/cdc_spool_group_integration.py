import json
import signal
import time
import traceback

import cdc_plugin_integration as c


def configure(timeout, size):
    c.stop('target')
    c.COMMANDS['target'] = [arg for arg in c.COMMANDS['target']
        if not arg.startswith(('--duckdb-cdc-spool-group-timeout-ms=', '--duckdb-cdc-spool-group-bytes='))]
    c.COMMANDS['target'] += ['--duckdb-cdc-spool-group-timeout-ms=' + str(timeout),
                             '--duckdb-cdc-spool-group-bytes=' + str(size)]
    c.start('target')
    c.enable()


def wait_for(predicate, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        values = c.status()
        if predicate(values):
            return values
        time.sleep(0.02)
    raise AssertionError('group condition not observed: ' + repr(values))


def main():
    c.setup()
    configure(5000, 4194304)
    before = c.snapshot('target')
    initial = c.sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints')
    c.sql('source', 'USE cdc_test;' + ('BEGIN; UPDATE t SET v=v+1 WHERE id=1; '
        'UPDATE u SET v=v+2 WHERE id=1; COMMIT;' * 20))
    values = wait_for(lambda s: int(s['duckdb_cdc_buffered_records']) == 20 and s['duckdb_cdc_received'] == '0')
    c.check('unflushed group not exposed to applier', values['duckdb_cdc_applied'], '0')
    c.check('unflushed group does not change rows', c.snapshot('target'), before)
    c.check('unflushed group does not advance checkpoint',
            c.sql('target', 'SELECT sequence,file,position FROM _duckdb_cdc.checkpoints'), initial)
    process = c.SERVERS.pop('target')
    process.kill()
    process.wait(timeout=10)
    c.check('owned target killed before group flush', process.returncode, -signal.SIGKILL)
    c.start('target')
    c.enable()
    values = c.drain('unflushed group replayed from durable source cursor')
    c.check('one durable group after replay', values['duckdb_cdc_spool_groups'], '1')
    c.check('all twenty transactions applied once', values['duckdb_cdc_applied'], '20')
    c.check('buffer released after sync', values['duckdb_cdc_buffered_records'], '0')

    configure(60000, 1024)
    c.sql('source', 'INSERT INTO cdc_test.t VALUES' + ','.join(f'({10000+i},{i})' for i in range(100)))
    values = wait_for(lambda s: int(s['duckdb_cdc_applied']) == 21)
    c.check('byte budget flushes before long timer', values['duckdb_cdc_last_spool_group_records'], '1')
    c.check('oversized complete transaction stays whole', int(values['duckdb_cdc_last_spool_group_bytes']) >= 1024)
    c.drain('byte-triggered group exact rows')

    configure(500, 4194304)
    c.sql('source', 'UPDATE cdc_test.t SET v=v+3 WHERE id=1')
    values = wait_for(lambda s: int(s['duckdb_cdc_applied']) == 22)
    c.check('small group flushed by timer', int(values['duckdb_cdc_last_spool_group_bytes']) < 4194304)
    c.check('small timer group transaction count', values['duckdb_cdc_last_spool_group_records'], '1')
    c.drain('timer-triggered group exact rows')

    configure(60000, 4194304)
    before = c.snapshot('target')
    c.sql('source', 'UPDATE cdc_test.u SET v=v+5 WHERE id=1')
    wait_for(lambda s: int(s['duckdb_cdc_buffered_records']) == 1 and s['duckdb_cdc_received'] == '22')
    c.disable()
    values = c.status()
    c.check('graceful stop syncs completed group', values['duckdb_cdc_received'], '23')
    c.check('graceful stop does not fake application', values['duckdb_cdc_applied'], '22')
    c.check('graceful stop preserves unapplied rows', c.snapshot('target'), before)
    c.enable()
    c.drain('durable stop group applied on next enable')
    c.SUMMARY['status'] = 'pass'


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
    for handle in c.HANDLES:
        handle.close()
    (c.ROOT / 'spool-group-summary.json').write_text(json.dumps(c.SUMMARY, indent=2))
    print(json.dumps({'status': c.SUMMARY['status'], 'checks': len(c.SUMMARY['checks']), 'artifacts': str(c.ROOT)}))
raise SystemExit(code)
