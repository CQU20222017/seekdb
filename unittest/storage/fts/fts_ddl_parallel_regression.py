"""Concurrent DDL, failed-index retry and persisted FTS checks in a private instance."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import time
import threading
import signal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--client', required=True)
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--port', type=int, default=2895)
    parser.add_argument('--memory-limit', help='Optional seekdb memory limit, such as 1G')
    parser.add_argument('--debug-after', type=int, help='Optional gdb snapshot after this many seconds waiting for DROP INDEX')
    parser.add_argument('--shutdown-mode', choices=['crash','graceful'], default='crash',
                        help='SIGTERM triggers SIGKILL in seekdb; SIGUSR1 requests graceful stop')
    args = parser.parse_args()
    runtime = Path(args.runtime).resolve()
    runtime.mkdir(parents=True, exist_ok=False)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    command = [args.client, '-h127.0.0.1', '-P'+str(args.port), '-uroot', '-N', '-B', '--default-character-set=utf8mb4']
    server = {}

    def record_wait():
        process = server.get('process')
        if process is None or process.poll() is not None:
            return
        try:
            result = subprocess.run(['gdb', '-nx', '-batch', '-ex', 'set pagination off', '-ex', 'thread apply all bt 8', '-p', str(process.pid)],
                                    capture_output=True, text=True, timeout=45)
            (output/'waiting-threads.txt').write_text(result.stdout+result.stderr)
        except subprocess.TimeoutExpired:
            (output/'waiting-threads.txt').write_text('gdb timed out')

    def sql(text, expected_success=True):
        timer = None
        if args.debug_after and text.startswith('DROP INDEX'):
            timer = threading.Timer(args.debug_after, record_wait)
            timer.start()
        try:
            result = subprocess.run(command, input='SET ob_query_timeout=120000000;\n'+text, text=True, capture_output=True, timeout=150)
        except subprocess.TimeoutExpired:
            (output/'timed-out-sql.txt').write_text(text)
            try:
                debug = subprocess.run(command+['--connect-timeout=3','-e','SHOW FULL PROCESSLIST'], text=True, capture_output=True, timeout=5)
                (output/'timeout-processlist.txt').write_text(debug.stdout+debug.stderr)
            except subprocess.TimeoutExpired:
                (output/'timeout-processlist.txt').write_text('diagnostic connection also timed out')
            raise
        finally:
            if timer is not None:
                timer.cancel()
                timer.join()
        if expected_success and result.returncode:
            raise RuntimeError(result.stderr)
        return result

    def start(label):
        log = (output/(label+'-server.log')).open('w')
        server_args = [args.binary, '--base-dir='+str(runtime), '--port='+str(args.port), '--nodaemon',
                       '--parameter=datafile_size=1G', '--parameter=datafile_maxsize=3G', '--parameter=log_disk_size=2G']
        if args.memory_limit:
            server_args.append('--parameter=memory_limit='+args.memory_limit)
        process = subprocess.Popen(server_args,
                                   stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        server['process'] = process
        server['label'] = label
        try:
            for _ in range(120):
                if process.poll() is not None:
                    raise RuntimeError('server exited')
                if sql('SELECT 1;', False).returncode == 0:
                    return process, log
                time.sleep(1)
            raise RuntimeError('startup timeout')
        except BaseException:
            process.terminate()
            process.wait(timeout=60)
            log.close()
            raise

    def stop(process, log):
        requested_signal = signal.SIGTERM if args.shutdown_mode == 'crash' else signal.SIGUSR1
        process.send_signal(requested_signal)
        process.wait(timeout=60)
        (output/(server['label']+'-exit.json')).write_text(json.dumps({'returncode':process.returncode,'requested_signal':requested_signal.name,'mode':args.shutdown_mode},indent=2))
        log.close()

    def snapshot():
        return sql("SELECT COUNT(*) FROM ddl_parallel_regression.a WHERE MATCH(body) AGAINST('database');"
                   "SELECT COUNT(*) FROM ddl_parallel_regression.b WHERE MATCH(body) AGAINST('database');"
                   "SELECT COUNT(*) FROM ddl_parallel_regression.c WHERE n=1;"
                   "CHECK TABLE ddl_parallel_regression.a,ddl_parallel_regression.b,ddl_parallel_regression.c;").stdout

    process, log = start('initial')
    try:
        sql('CREATE DATABASE ddl_parallel_regression;')
        for table in ['a', 'b', 'c']:
            sql('CREATE TABLE ddl_parallel_regression.'+table+'(id INT PRIMARY KEY,n INT,body TEXT);')
            for offset in range(0, 3000, 500):
                rows = ','.join("(%d,%d,'database index performance audit')" % (i, i % 17) for i in range(offset, offset+500))
                sql('INSERT INTO ddl_parallel_regression.'+table+' VALUES '+rows+';')
        ddls = ["CREATE FULLTEXT INDEX f ON ddl_parallel_regression.a(body) WITH PARSER beng;",
                "CREATE FULLTEXT INDEX f ON ddl_parallel_regression.b(body) WITH PARSER ik;",
                "CREATE INDEX ix ON ddl_parallel_regression.c(n);"]
        with ThreadPoolExecutor(max_workers=3) as pool:
            list(pool.map(sql, ddls))
        failed = sql('CREATE UNIQUE INDEX bad ON ddl_parallel_regression.c(n);', False)
        assert failed.returncode != 0 and 'Duplicate' in failed.stderr, failed.stderr
        # Retrying with a valid unique key must work after the failed job.
        sql('CREATE UNIQUE INDEX good ON ddl_parallel_regression.c(id,n);')
        before = snapshot()
        assert before.splitlines()[:3] == ['3000','3000','177'], before
        (output/'before.txt').write_text(before)
    finally:
        stop(process, log)
    process, log = start('restart')
    try:
        after = snapshot()
        assert before == after
        sql('DROP INDEX f ON ddl_parallel_regression.a; CREATE FULLTEXT INDEX f ON ddl_parallel_regression.a(body) WITH PARSER beng;')
        assert snapshot() == before
        (output/'after.txt').write_text(after)
    finally:
        stop(process, log)
    (output/'result.json').write_text(json.dumps({'rows_per_table':3000, 'concurrent_ddls':3, 'duplicate_key_failure':True, 'valid_retry':True, 'restart_equal':True, 'rebuild_equal':True}, indent=2))
    print('DDL concurrent/retry/restart checks passed', flush=True)


if __name__ == '__main__':
    main()
