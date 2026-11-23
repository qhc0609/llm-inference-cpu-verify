import gzip, base64, io

d = open('repro_24_25.sh', 'rb').read()
buf = io.BytesIO()
with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0) as f:
    f.write(d)
z = buf.getvalue()
b = base64.b64encode(z).decode()
open('repro_gz_b64.txt', 'w').write(b)
cmd = "echo '%s' | base64 -d | gunzip > /tmp/r.sh && bash /tmp/r.sh" % b
open('onecmd.txt', 'w').write(cmd)
print('gz bytes', len(z), '| b64 chars', len(b), '| cmd chars', len(cmd))
