"""Download the pinned research archive with Python standard libraries."""
import argparse,json,re,hashlib,urllib.request,zipfile,stat
from pathlib import Path
REPOSITORY='chauko0423/xiaomanbox';DIRECTORY='research/local-engineering-study'
def main():
 ap=argparse.ArgumentParser(description='Download a fixed commit and verify all archive/part hashes.');ap.add_argument('--ref',required=True,help='full40-character Git commit from the paper');ap.add_argument('--output',required=True,help='new directory');ap.add_argument('--expected-sha256');a=ap.parse_args()
 if not re.fullmatch(r'[0-9a-f]{40}',a.ref):raise ValueError('full immutable Git commit required')
 out=Path(a.output).resolve();assert not out.exists(),'new output directory required';out.mkdir(parents=True)
 base=f'https://raw.githubusercontent.com/{REPOSITORY}/{a.ref}/{DIRECTORY}/'
 def fetch(url):
  with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'local-engineering-study-verifier'}),timeout=60) as f:return f.read()
 manifest=json.loads(fetch(base+'archive_manifest.json'));expected=manifest['archive_sha256']
 if a.expected_sha256 and a.expected_sha256!=expected:raise ValueError('paper archive hash differs from pinned manifest')
 archive=out/'research_bundle_v10.zip';digest=hashlib.sha256();count=0
 with archive.open('xb') as dest:
  for item in manifest['parts']:
   assert re.fullmatch(r'archive\.part\d{3}\.bin',item['name']);raw=fetch(base+'parts/'+item['name']);assert len(raw)==item['bytes'] and hashlib.sha256(raw).hexdigest()==item['sha256'],'part hash/size mismatch';dest.write(raw);digest.update(raw);count+=len(raw)
 assert digest.hexdigest()==expected and count==manifest['archive_bytes'],'complete archive mismatch'
 bundle=out/'bundle';bundle.mkdir()
 with zipfile.ZipFile(archive) as z:
  assert z.testzip() is None;names=z.namelist();assert len(names)==len(set(names)),'duplicate archive paths'
  for item in z.infolist():
   target=(bundle/item.filename).resolve();assert target.is_relative_to(bundle) and not stat.S_ISLNK(item.external_attr>>16),'unsafe archive member'
  z.extractall(bundle)
 (out/'download_verification.json').write_text(json.dumps({'passed':True,'commit':a.ref,'archive_sha256':expected,'archive_bytes':count,'anonymous_HTTPS':True,'offline_record_audit_separate_command':'python -B bundle/verify_public_evidence.py --self-test'},indent=2)+'\n',encoding='utf-8')
 print('PINNED_PUBLIC_ARCHIVE_DOWNLOADED_HASH_VERIFIED',bundle)
if __name__=='__main__':main()
