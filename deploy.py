#!/usr/bin/env python3
"""把 verify/ 工程整体推到 openEuler，编译、运行、取回结果。

用法：
    OE_PASS='root密码' python deploy.py

流程：
  1. 密码登录（paramiko）
  2. 上传整个 verify/ 目录到 /root/verify
  3. 顺便把本地公钥写进 authorized_keys，之后可免密
  4. 检查 gcc / make / 交叉工具链，缺什么装什么（dnf）
  5. 编译 + 跑全部实验，输出实时打印
  6. 把 results/ 拉回本地
"""
import os, sys, stat, time
import paramiko

HOST = os.environ.get("OE_HOST", "192.168.17.130")
USER = os.environ.get("OE_USER", "root")
PASS = os.environ.get("OE_PASS", "")
LOCAL = os.path.dirname(os.path.abspath(__file__))   # verify/ 目录
REMOTE = "/root/verify"

def log(msg):
    print(f"[deploy] {msg}", flush=True)

def run(cli, cmd, timeout=3600, quiet=False):
    if not quiet:
        log(f"$ {cmd}")
    chan = cli.get_transport().open_session()
    chan.settimeout(timeout)
    chan.exec_command(cmd)
    out = []
    while True:
        if chan.recv_ready():
            data = chan.recv(65536).decode("utf-8", "replace")
            out.append(data)
            if not quiet:
                sys.stdout.write(data); sys.stdout.flush()
        if chan.recv_stderr_ready():
            data = chan.recv_stderr(65536).decode("utf-8", "replace")
            out.append(data)
            if not quiet:
                sys.stderr.write(data); sys.stderr.flush()
        if chan.exit_status_ready() and not chan.recv_ready() and not chan.recv_stderr_ready():
            break
        time.sleep(0.02)
    rc = chan.recv_exit_status()
    chan.close()
    return rc, "".join(out)

def sftp_mkdirs(sftp, path):
    parts, cur = path.strip("/").split("/"), ""
    for p in parts:
        cur += "/" + p
        try:
            sftp.stat(cur)
        except IOError:
            sftp.mkdir(cur)

def upload_dir(sftp, local, remote):
    sftp_mkdirs(sftp, remote)
    count = 0
    for root, dirs, files in os.walk(local):
        dirs[:] = [d for d in dirs if d not in ("results", "__pycache__", ".git")]
        rel = os.path.relpath(root, local).replace("\\", "/")
        rdir = remote if rel == "." else f"{remote}/{rel}"
        sftp_mkdirs(sftp, rdir)
        for f in files:
            if f.endswith((".o", ".x86", ".aarch64")):
                continue
            lp = os.path.join(root, f)
            sftp.put(lp, f"{rdir}/{f}")
            count += 1
    return count

def main():
    if not PASS:
        print("缺少 OE_PASS 环境变量", file=sys.stderr)
        return 2

    cli = paramiko.SSHClient()
    cli.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    log(f"连接 {USER}@{HOST} ...")
    cli.connect(HOST, port=22, username=USER, password=PASS, timeout=15,
                allow_agent=False, look_for_keys=False)
    log("已登录")

    rc, out = run(cli, "cat /etc/os-release | head -2 && uname -m && nproc", quiet=True)
    log(f"目标机: {out.strip()}")

    # 装公钥，便于后续免密
    pub = open(os.path.join(LOCAL, ".ssh", "id_ed25519.pub")).read().strip()
    run(cli, f"mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
             f"grep -qF '{pub}' ~/.ssh/authorized_keys 2>/dev/null || "
             f"echo '{pub}' >> ~/.ssh/authorized_keys; chmod 600 ~/.ssh/authorized_keys",
        quiet=True)
    log("公钥已写入 authorized_keys")

    # 依赖检查（交叉工具链已在外部准备好，这里只做存在性检查）
    log("检查工具链 ...")
    rc, out = run(cli, "command -v gcc; command -v qemu-aarch64-static; "
                       "ls /opt/cross/gcc_arm64le/bin/aarch64-linux-gnueabi-gcc 2>/dev/null; "
                       "ls /opt/arm64-sysroot/lib/ld-linux-aarch64.so.1 2>/dev/null", quiet=True)
    log(f"现状:\n{out}")
    if "gcc" not in out:
        log("缺少 gcc，尝试安装")
        run(cli, "dnf install -y gcc", timeout=900)

    # 上传
    sftp = cli.open_sftp()
    n = upload_dir(sftp, LOCAL, REMOTE)
    sftp.close()
    log(f"已上传 {n} 个文件到 {REMOTE}")

    # 编译
    rc, _ = run(cli, f"cd {REMOTE} && chmod +x *.sh */build.sh 2>/dev/null; bash build.sh")
    if rc != 0:
        log(f"构建失败 rc={rc}")
        cli.close()
        return rc

    # 运行
    run(cli, f"cd {REMOTE} && sed -i 's/\\r$//' *.sh */build.sh 2>/dev/null; bash run_all.sh")

    # 取回结果
    sftp = cli.open_sftp()
    local_res = os.path.join(LOCAL, "results")
    os.makedirs(local_res, exist_ok=True)
    for f in sftp.listdir(f"{REMOTE}/results"):
        sftp.get(f"{REMOTE}/results/{f}", os.path.join(local_res, f))
    sftp.close()
    log(f"结果已取回到 {local_res}")

    cli.close()
    log("完成")
    return 0

if __name__ == "__main__":
    sys.exit(main())
