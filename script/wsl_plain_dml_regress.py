import pymysql
c=pymysql.connect(host="127.0.0.1",port=2881,user="root",password="",database="test",autocommit=True,charset="utf8mb4")
cur=c.cursor()
ok=True
def run(sql, expect=None):
    global ok
    try:
        cur.execute(sql)
        try: rows=cur.fetchall()
        except Exception: rows=None
        if expect is not None:
            got = rows[0][0] if rows else None
            flag = "PASS" if got == expect else "FAIL"
            if flag=="FAIL": ok=False
            print("%-4s %-70s %s (expect %s)" % (flag, sql[:70], got, expect))
        else:
            print("ok   %s" % sql[:100])
        return rows
    except Exception as e:
        ok=False
        print("ERR  %s -> %s" % (sql[:70], e))
run("drop database if exists plain_dml")
run("create database plain_dml")
run("create table plain_dml.t(id int primary key, a int, b varchar(20), key idx_a(a))")
run("insert into plain_dml.t values(1,10,'x'),(2,20,'y'),(3,30,'z')")
run("select count(*) from plain_dml.t", 3)
run("update plain_dml.t set b='yy' where id=2")
run("select b from plain_dml.t where id=2", 'yy')
run("update plain_dml.t set a=a+1 where a>=20")
run("select sum(a) from plain_dml.t", 81)
run("update plain_dml.t set id=4 where id=3")
run("select count(*) from plain_dml.t where id in (3,4)", 1)
run("select a from plain_dml.t where id=4", 31)
run("delete from plain_dml.t where id=1")
run("select count(*) from plain_dml.t", 2)
run("insert into plain_dml.t values(1,10,'x')")
run("delete from plain_dml.t where id in (1,4)")
run("select count(*) from plain_dml.t", 1)
# rollback semantics
c2=pymysql.connect(host="127.0.0.1",port=2881,user="root",password="",database="test",autocommit=False,charset="utf8mb4")
k2=c2.cursor()
k2.execute("update plain_dml.t set a=999 where id=2"); c2.commit()
k2.execute("update plain_dml.t set a=1 where id=2"); c2.rollback()
k2.execute("select a from plain_dml.t where id=2")
print("%-4s rollback restores value %s (expect 999)" % ("PASS" if k2.fetchone()[0]==999 else "FAIL",))
c2.close()
run("drop database plain_dml")
print("PLAIN DML:", "PASS" if ok else "FAIL")
