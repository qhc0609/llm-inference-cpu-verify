#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""通用执行器：同步目录到 VM，跑一条命令，打印输出。

用法：python _run.py "命令" [--push 本地相对路径]
"""
import os
import sys
import paramiko

HOST = os.environ.get("OE_HOST", "192.168.17.130")
USER = os.environ.get("OE_USER", "root")
PASS = os.environ.get("OE_PASS", "")
LOCAL = os.path.dirname(os.path.abspath(__file__))
REMOTE = "/root/verify"


def sftp_mkdir(sftp, path):
    parts = path.strip("/").split("/")
    cur = ""
    for p in parts:
        cur += "/" + p
        try:
            sftp.stat(cur)
        except IOError:
            sftp.mkdir(cur)


def push(sftp, rel):
    """把本地相对路径（文件或目录）推到 REMOTE 下同名位置。"""
    src = os.path.join(LOCAL, rel)
    dst = f"{REMOTE}/{rel}"
    if os.path.isdir(src):
        for root, _dirs, files in os.walk(src):
            r = os.path.relpath(root, LOCAL).replace("\\", "/")
            sftp_mkdir(sftp, f"{REMOTE}/{r}")
            for fn in files:
                sftp.put(os.path.join(root, fn), f"{REMOTE}/{r}/{fn}")
    else:
        sftp_mkdir(sftp, os.path.dirname(dst))
        sftp.put(src, dst)


def main():
    cmd = sys.argv[1]
    pushes = []
    for i, a in enumerate(sys.argv):
        if a == "--push":
            pushes.append(sys.argv[i + 1])

    cli = paramiko.SSHClient()
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    cli.connect(HOST, username=USER, password=PASS, timeout=15,
                allow_agent=False, look_for_keys=False)
    if pushes:
        sftp = cli.open_sftp()
        for p in pushes:
            push(sftp, p)
        sftp.close()

    _in, out, err = cli.exec_command(cmd, timeout=1800, get_pty=False)
    for line in iter(out.readline, ""):
        sys.stdout.write(line)
        sys.stdout.flush()
    e = err.read().decode("utf-8", "replace")
    if e.strip():
        sys.stderr.write(e)
    code = out.channel.recv_exit_status()
    print(f"\n[exit {code}]")
    cli.close()


if __name__ == "__main__":
    main()
