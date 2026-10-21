"""Read the compiled public identity for packaging; no private key access."""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[1]

@dataclass(frozen=True)
class ReleaseIdentity:
    version: str
    sequence: int
    def __post_init__(self):
        match=re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-preview\.([1-9][0-9]*(?:\.[1-9][0-9]*)*))?',self.version)
        if not match or not 1<=self.sequence<=65535:
            raise ValueError('Invalid Aura release identity')
        if any(int(n)>65535 for n in match.groups()[:3]):
            raise ValueError('Aura numeric version exceeds Windows resource limits')
    @property
    def numeric_version(self):
        return [int(n) for n in self.version.split('-')[0].split('.')]+[self.sequence]
    @property
    def tag(self):return 'v'+self.version
    @property
    def native_version(self):return self.version.replace('-preview.','~preview.')
    @property
    def arch_version(self):return self.version.replace('-preview.','rc')
    def package_name(self,family):
        return {'debian':f'aura-browser_{self.version}-1_amd64.deb',
                'fedora':f'aura-browser-{self.version}-1.x86_64.rpm',
                'arch':f'aura-browser-{self.arch_version}-1-x86_64.pkg.tar.gz'}[family]
    def as_dict(self):
        return {'version':self.version,'sequence':self.sequence,'tag':self.tag,
                'numeric_version':self.numeric_version,'native_version':self.native_version,
                'arch_version':self.arch_version,
                'packages':{f:self.package_name(f) for f in ('debian','fedora','arch')}}

def read_identity(root=ROOT):
    text=(root/'common/update_key.hpp').read_text(encoding='utf-8')
    versions=re.findall(r'inline constexpr std::string_view browser_version = "([^"]+)";',text)
    sequences=re.findall(r'inline constexpr long long browser_release_sequence = ([0-9]+);',text)
    if len(versions)!=1 or len(sequences)!=1:raise ValueError('Missing or ambiguous compiled Aura identity')
    return ReleaseIdentity(versions[0],int(sequences[0]))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    group=parser.add_mutually_exclusive_group()
    group.add_argument('--package',choices=('debian','fedora','arch'))
    group.add_argument('--version',action='store_true')
    group.add_argument('--native-version',action='store_true')
    group.add_argument('--arch-version',action='store_true')
    args=parser.parse_args();identity=read_identity()
    print(identity.package_name(args.package) if args.package else identity.version if args.version else identity.native_version if args.native_version else identity.arch_version if args.arch_version else json.dumps(identity.as_dict()))
if __name__=='__main__':main()
