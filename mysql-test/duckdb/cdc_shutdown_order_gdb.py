import json
import os
import queue
import re
import signal
import subprocess
import threading
import time
import traceback
from pathlib import Path

import cdc_plugin_integration as c


class Gdb:
    def __init__(self):
        self.lines = queue.Queue()
        self.events = []
        self.token = 0
        self.pid = None
        self.thread = None
        self.breakpoint = None
        self.exited = False
        self.log = (c.ROOT / 'gdb-mi.log').open('w')
        self.proc = subprocess.Popen(['/usr/bin/gdb', '--quiet', '--nx', '--interpreter=mi2',
                                      '--args'] + c.COMMANDS['target'], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1, start_new_session=True)
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.proc.stdout:
            self.log.write(line)
            self.log.flush()
            self.lines.put(line.rstrip('\n'))
        self.lines.put(None)

    def receive(self, timeout):
        line = self.lines.get(timeout=timeout)
        if line is None:
            raise RuntimeError('GDB ended unexpectedly')
        if line.startswith('=thread-group-started,'):
            self.pid = int(re.search(r'pid="(\d+)"', line)[1])
        if line.startswith('=thread-group-exited,'):
            self.exited = True
        if line.startswith(('*', '=')):
            self.events.append(line)
        return line

    def command(self, command, timeout=30):
        self.token += 1
        token = str(self.token)
        self.log.write('>>> ' + token + command + '\n')
        self.log.flush()
        self.proc.stdin.write(token + command + '\n')
        self.proc.stdin.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.receive(max(0.01, deadline - time.monotonic()))
            if line.startswith(token + '^'):
                if line.startswith(token + '^error'):
                    raise RuntimeError(line)
                return line
        raise TimeoutError(command)

    def event(self, predicate, timeout):
        deadline = time.monotonic() + timeout
        while True:
            for line in self.events:
                if predicate(line):
                    return line
            if time.monotonic() >= deadline:
                raise TimeoutError('GDB event; see gdb-mi.log')
            self.receive(max(0.01, deadline - time.monotonic()))

    def resume(self):
        if self.breakpoint:
            self.command('-break-disable ' + self.breakpoint)
            self.breakpoint = None
        if self.thread and not self.exited:
            self.command('-exec-continue --thread ' + self.thread)
            self.thread = None

    def close(self):
        try:
            if self.proc.poll() is None:
                self.resume()
                if self.pid and not self.exited:
                    os.kill(self.pid, signal.SIGTERM)
                    self.event(lambda x: x.startswith('=thread-group-exited,'), 20)
        finally:
            if self.proc.poll() is None:
                if self.pid and not self.exited:
                    try:
                        os.kill(self.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                try:
                    self.command('-gdb-exit', 5)
                    self.proc.wait(timeout=5)
                except (Exception, subprocess.TimeoutExpired):
                    self.proc.kill()
                    self.proc.wait()
            self.log.close()


def main():
    global debugger
    for path in ('/usr/bin/gdb', '/git/BuildOf_mdb-w-duckdb/sql/mariadbd', str(c.PLUGIN)):
        c.check('exists ' + path, Path(path).is_file())
    c.setup()
    c.stop('target')
    old_target = c.SERVERS.pop('target')
    debugger = Gdb()
    g = debugger
    for command in ('-gdb-set non-stop on', '-gdb-set mi-async on', '-gdb-set pagination off',
                    '-gdb-set breakpoint pending on'):
        g.command(command)
    for name in ('SIGTERM', 'SIGSEGV', 'SIGABRT'):
        g.command('-interpreter-exec console ' + json.dumps('handle ' + name + ' nostop noprint pass'))
    result = g.command('-break-insert -f -c ' + json.dumps("sql._M_dataplus._M_p[0]=='U'") +
                       ' ' + json.dumps('myduck::cdc::Applier::Impl::Sql'))
    g.breakpoint = re.search(r'number="(\d+)"', result)[1]
    g.command('-exec-run')
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if c.run(c.client('target') + ['-e', 'SELECT 1']).returncode == 0:
            break
        time.sleep(0.2)
    else:
        raise TimeoutError('GDB target startup; inspect gdb-mi.log for ptrace errors')
    c.enable()
    c.sql('source', 'USE cdc_test; BEGIN;' +
          'UPDATE t SET v=v+1 WHERE id=1; UPDATE u SET v=v+2 WHERE id=1;' * 500 + 'COMMIT')
    hit = g.event(lambda x: x.startswith('*stopped,reason="breakpoint-hit"'), 40)
    g.thread = re.search(r'thread-id="(\d+)"', hit)[1]
    c.SUMMARY['checkpoint_breakpoint'] = hit
    c.SUMMARY['inferior_pid'] = g.pid
    c.check('checkpoint breakpoint matched', re.search(r'bkptno="(\d+)"', hit)[1], g.breakpoint)
    threads = g.command('-thread-info')
    c.SUMMARY['paused_threads'] = threads
    c.check('other inferior threads running', 'state="running"' in threads)
    c.SUMMARY['paused_sql'] = g.command('-data-evaluate-expression --thread ' + g.thread +
                                      ' --frame 0 "sql._M_dataplus._M_p"')
    error_log = c.ROOT / 'target.err'
    offset = error_log.stat().st_size
    os.kill(g.pid, signal.SIGTERM)
    deadline = time.monotonic() + 3
    premature = False
    while time.monotonic() < deadline:
        tail = error_log.read_text(errors='replace')[offset:]
        if re.search(r'InnoDB: (?:Starting shutdown\.\.\.|Shutdown completed)', tail):
            premature = True
            break
        time.sleep(0.05)
    c.SUMMARY['full_teardown_while_paused'] = premature
    c.SUMMARY['paused_shutdown_log'] = tail
    g.resume()
    exit_event = g.event(lambda x: x.startswith('=thread-group-exited,'), 60)
    c.SUMMARY['inferior_exit'] = exit_event
    c.SUMMARY['shutdown_log'] = error_log.read_text(errors='replace')[offset:]
    g.close()
    debugger = None
    c.SERVERS['target'] = old_target
    c.check('no full InnoDB teardown while checkpoint worker paused', premature, False)
    c.check('inferior normal zero exit', 'exit-code="0"' in exit_event)
    c.check('no fatal shutdown log', bool(re.search(
        r'got signal|Assertion .*failed|Segmentation fault|mysqld got|mariadbd got',
        c.SUMMARY['shutdown_log'], re.I)), False)
    c.start('target')
    c.enable()
    c.drain('restart exact source rows and checkpoint coordinates')
    c.check('updated t rows', c.sql('target', 'SELECT v FROM cdc_test.t WHERE id=1'), ['510'])
    c.check('updated u rows', c.sql('target', 'SELECT v FROM cdc_test.u WHERE id=1'), ['1100'])
    c.SUMMARY['status'] = 'pass'


debugger = None
code = 0
try:
    main()
except BaseException:
    code = 1
    c.SUMMARY['status'] = 'fail'
    c.SUMMARY['traceback'] = traceback.format_exc()
    print(c.SUMMARY['traceback'], flush=True)
finally:
    if debugger is not None:
        try:
            debugger.close()
        except BaseException:
            code = 1
            c.SUMMARY.setdefault('cleanup_errors', []).append(traceback.format_exc())
    for node in list(c.SERVERS):
        try:
            c.stop(node)
        except BaseException:
            code = 1
            c.SUMMARY.setdefault('cleanup_errors', []).append(traceback.format_exc())
    for handle in c.HANDLES:
        handle.close()
    (c.ROOT / 'shutdown-order-gdb-summary.json').write_text(json.dumps(c.SUMMARY, indent=2))
    print(json.dumps({'status': c.SUMMARY['status'], 'artifacts': str(c.ROOT)}), flush=True)
raise SystemExit(code)
