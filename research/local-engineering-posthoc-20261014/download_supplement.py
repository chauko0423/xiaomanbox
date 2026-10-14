from pathlib import Path
import argparse,json,hashlib,urllib.request,zipfile,subprocess,sys
p=argparse.ArgumentParser();p.add_argument('--commit',required=True);p.add_argument('--out',required=True);a=p.parse_args();assert len(a.commit)==40 and all(c in '0123456789abcdef' for c in a.commit)
b=Path(a.out).resolve();assert not b.exists();b.mkdir(parents=True)
base='https://raw.githubusercontent.com/chauko0423/xiaomanbox/'+a.commit+'/research/local-engineering-posthoc-20261014/'
def fetch(n):
 with urllib.request.urlopen(base+n,timeout=45) as f:return f.read()
m=json.loads(fetch('archive_manifest.json'));data=[]
for x in m['parts']:
 raw=fetch(x['path']);assert len(raw)==x['bytes'] and hashlib.sha256(raw).hexdigest()==x['sha256'];data.append(raw)
raw=b''.join(data);assert len(raw)==m['archive_bytes'] and hashlib.sha256(raw).hexdigest()==m['archive_sha256'];(b/m['archive_name']).write_bytes(raw)
with zipfile.ZipFile(b/m['archive_name']) as z:
 assert z.testzip() is None
 for n in z.namelist():assert not Path(n).is_absolute() and '..' not in Path(n).parts and '\\' not in n
 z.extractall(b/'bundle')
subprocess.run([sys.executable,'-B',str(b/'bundle/verify_supplement.py'),'--self-test'],check=True)
print('ANONYMOUS_DOWNLOAD_VERIFIED',m['archive_sha256'])
