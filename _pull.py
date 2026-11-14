#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 VM 上的目录拉回本地。用法：python _pull.py 20_runtime_probe/results"""
import os
import stat
import sys
import paramiko

HOST = os.environ.get("OE_HOST", "192.168.17.130")
USER = os.environ.get("OE_USER", "root")
PASS = os.environ.get("OE_PASS", "")
LOCAL = os.path.dirname(os.path.abspath(__file__))
REMOTE = "/root/verify"


def pull_dir(sftp, rel):
    rdir = f"{REMOTE}/{rel}"
    ldir = os.path.join(LOCAL, rel.replace("/", os.sep))
    os.makedirs(ldir, exist_ok=True)
    for ent in sftp.listdir_attr(rdir):
        rpath = f"{rdir}/{ent.filename}"
        lpath = os.path.join(ldir, ent.filename)
        if stat.S_ISDIR(ent.st_mode):
            pull_dir(sftp, f"{rel}/{ent.filename}")
        else:
            sftp.get(rpath, lpath)
            print(f"{ent.filename}  {ent.st_size} B")


def main():
    rel = sys.argv[1].strip("/")
    cli = paramiko.SSHClient()
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    cli.connect(HOST, username=USER, password=PASS, timeout=15,
                allow_agent=False, look_for_keys=False)
    sftp = cli.open_sftp()
    pull_dir(sftp, rel)
    sftp.close()
    cli.close()


if __name__ == "__main__":
    main()
