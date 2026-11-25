import json
import queue
import subprocess
import threading
import time
import traceback

import cdc_plugin_integration as c


class CheckpointLock:
    def __init__(self):
        self.proc = subprocess.Popen(c.client('target'), stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        self.lines = queue.Queue()
        threading.Thread(target=self.read, daemon=True).start()
        self.proc.stdin.write("LOCK TABLES _duckdb_cdc.checkpoints WRITE; SELECT 'LOCKED';\n")
        self.proc.stdin.flush()
        while True:
            line = self.lines.get(timeout=15)
            if line == 'LOCKED':
                break
            if line is None or line.startswith('ERROR '):
                raise RuntimeError('checkpoint lock failed: ' + repr(line))

    def read(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip('\n'))
        self.lines.put(None)

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
        self.proc.wait(timeout=15)


def wait_applying():
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        values = c.status()
        if values['duckdb_cdc_apply_stage'] == 'APPLYING':
            return values
        time.sleep(0.02)
    raise AssertionError('applier did not enter active SQL: ' + repr(values))


def main():
    c.setup()
    c.enable()
    c.drain('initial checkpoint ready')
    for iteration in range(5):
        holder = CheckpointLock()
        holders.append(holder)
        c.sql('source', 'USE cdc_test; BEGIN;' +
              'UPDATE t SET v=v+1 WHERE id=1; UPDATE u SET v=v+2 WHERE id=1;' * 500 + 'COMMIT')
        values = wait_applying()
        c.SUMMARY.setdefault('shutdown_attempts', []).append({'iteration': iteration, 'status': values})
        time.sleep(0.25)
        c.stop('target')
        holder.close()
        c.start('target')
        c.enable()
        c.drain('active shutdown/restart exact rows ' + str(iteration))
    c.SUMMARY['status'] = 'pass'


holders = []
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
    for holder in holders:
        try:
            holder.close()
        except BaseException:
            code = 1
            c.SUMMARY.setdefault('cleanup_errors', []).append(traceback.format_exc())
    for handle in c.HANDLES:
        handle.close()
    (c.ROOT / 'active-shutdown-summary.json').write_text(json.dumps(c.SUMMARY, indent=2))
    print(json.dumps({'status': c.SUMMARY['status'], 'checks': len(c.SUMMARY['checks']), 'artifacts': str(c.ROOT)}))
raise SystemExit(code)
