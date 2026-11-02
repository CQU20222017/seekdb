"""Verify durable rollback of a logged, uncommitted local transaction after SIGTERM."""
import argparse
import json
from pathlib import Path
import re
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ['binary','client','runtime','output']:
        parser.add_argument('--'+option, required=True)
    parser.add_argument('--port',type=int,default=2895)
    args=parser.parse_args()
    runtime=Path(args.runtime);runtime.mkdir(parents=True,exist_ok=False)
    output=Path(args.output);output.mkdir(parents=True,exist_ok=False)
    command=[args.client,'-h127.0.0.1','-P'+str(args.port),'-uroot','-N','-B','--default-character-set=utf8mb4']

    def sql(text):
        r=subprocess.run(command,input=text,text=True,capture_output=True,timeout=40)
        if r.returncode:raise RuntimeError(r.stderr)
        return r.stdout

    def start(label):
        log=(output/(label+'.log')).open('w')
        p=subprocess.Popen([args.binary,'--base-dir='+str(runtime),'--port='+str(args.port),'--nodaemon','--parameter=datafile_size=1G','--parameter=datafile_maxsize=3G','--parameter=log_disk_size=2G'],stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT)
        try:
            for _ in range(120):
                if p.poll() is not None:raise RuntimeError('server exited')
                try:
                    if sql('SELECT 1;').strip()=='1':return p,log
                except RuntimeError:pass
                time.sleep(1)
            raise RuntimeError('startup timeout')
        except BaseException:
            p.terminate();p.wait(timeout=60);log.close();raise

    def stop(p,log):
        p.send_signal(signal.SIGTERM);p.wait(timeout=60);log.close()

    p,log=start('initial');held=None
    held_out=(output/'held-session.txt').open('w')
    try:
        sql("CREATE DATABASE local_tx_recovery;CREATE TABLE local_tx_recovery.t(id INT PRIMARY KEY,v VARCHAR(1024));INSERT INTO local_tx_recovery.t VALUES(0,'committed');")
        held=subprocess.Popen(command+['--unbuffered'],stdin=subprocess.PIPE,stdout=held_out,stderr=subprocess.STDOUT,text=True)
        held.stdin.write('BEGIN;\n')
        for offset in range(1,5001,500):
            values=','.join("(%d,'%s')"%(i,'pending-'+'x'*500) for i in range(offset,offset+500))
            held.stdin.write('INSERT INTO local_tx_recovery.t VALUES '+values+';\n')
        held.stdin.write("SELECT CONCAT('ready:',CONNECTION_ID());\n");held.stdin.flush()
        for _ in range(300):
            text=(output/'held-session.txt').read_text()
            match=re.search(r'ready:(\d+)',text)
            if match:break
            assert held.poll() is None,text
            time.sleep(.1)
        else:raise RuntimeError('held transaction did not finish its inserts')
        assert sql('SELECT COUNT(*) FROM local_tx_recovery.t;').strip()=='1'
        sql('ALTER SYSTEM MINOR FREEZE;')
        for _ in range(100):
            stats=sql('SELECT trans_id,flushed_log_size FROM oceanbase.__all_virtual_trans_stat WHERE session_id='+match[1]+' AND flushed_log_size>0;')
            if stats.strip():break
            time.sleep(.1)
        else:raise RuntimeError('transaction redo was not observed; cannot test durable rollback')
        (output/'before-crash-transactions.txt').write_text(stats)
        stop(p,log);p=None
        held.stdin.close();held.wait(timeout=15)
    finally:
        if p is not None:stop(p,log)
        if held is not None and held.poll() is None:held.terminate();held.wait(timeout=15)
        held_out.close()
    for label in ['recovered','second-restart']:
        p,log=start(label)
        try:
            rows=sql('SELECT id,v FROM local_tx_recovery.t ORDER BY id;')
            assert rows=='0\tcommitted\n',rows[:1000]
            assert sql('SELECT COUNT(*) FROM oceanbase.__all_virtual_trans_stat WHERE session_id=0 AND is_decided=0;').strip()=='0'
            sql("INSERT INTO local_tx_recovery.t VALUES(1,'new write');DELETE FROM local_tx_recovery.t WHERE id=1;")
            (output/(label+'-rows.txt')).write_text(rows)
        finally:stop(p,log)
    (output/'result.json').write_text(json.dumps({'committed_preserved':True,'uncommitted_rows':5000,'redo_observed':True,'rollback_durable_across_second_restart':True},indent=2))
    print('local transaction crash recovery checks passed',flush=True)


if __name__=='__main__':main()
