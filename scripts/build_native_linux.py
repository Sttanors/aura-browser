"""Build native packages from the Debian-baseline runtime, not the host ABI.
Run in the packaging Debian image: dpkg-dev, rpm and bsdtar are required.
"""
from __future__ import annotations
import argparse
import gzip
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
import time
import zipfile
from release_identity import read_identity

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT/'packaging/linux/native'
ARCH_DEPENDS = ['glibc>=2.38','gcc-libs','openssl','libarchive','sqlite','libx11','libxext',
    'libxpresent','libxrandr','libxcomposite','libxdamage','libxfixes','libxcb',
    'libxkbcommon','wayland','libglvnd','mesa','nss','nspr','glib2','at-spi2-core',
    'dbus','libcups','cairo','pango','alsa-lib','expat','ca-certificates','xdg-utils','util-linux']


def stage_runtime(archive: Path, stage: Path):
    app = stage/'opt/aura-browser'
    app.mkdir(parents=True)
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            p = PurePosixPath(entry.filename)
            if p.is_absolute() or '..' in p.parts or len(p.parts)<2 or p.parts[0]!='Aura2':
                raise ValueError(f'Unsafe runtime path: {p}')
            if entry.is_dir(): continue
            target = app.joinpath(*p.parts[1:])
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open(entry) as data, target.open('wb') as output:
                shutil.copyfileobj(data,output)
            target.chmod(0o755 if target.name=='aura_browser' or '.so' in target.name else 0o644)
    for required in ['aura_browser','libcef.so','branding/aura.png','branding/aura.svg']:
        if not (app/required).is_file(): raise ValueError(f'Missing runtime file: {required}')
    if (app/'profile').exists(): raise ValueError('Runtime includes a private profile')
    files = {
        'usr/bin/aura-browser':NATIVE/'launcher',
        'usr/lib/aura-browser/migrate-launchers':NATIVE/'migrate-launchers',
        'usr/lib/aura-browser/migrate-all-users':NATIVE/'migrate-all-users',
        'usr/share/applications/aura-browser.desktop':NATIVE/'aura-browser.desktop',
        'usr/share/icons/hicolor/512x512/apps/aura-browser.png':app/'branding/aura.png',
        'usr/share/icons/hicolor/scalable/apps/aura-browser.svg':app/'branding/aura.svg',
        'usr/share/licenses/aura-browser/NOTICE':NATIVE/'NOTICE',
        'usr/share/doc/aura-browser/copyright':NATIVE/'NOTICE',
    }
    for name, source in files.items():
        target=stage/name
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(source,target)
        target.chmod(0o755 if name=='usr/bin/aura-browser' or name.startswith('usr/lib/aura-browser/') else 0o644)
    subprocess.run(['desktop-file-validate',str(stage/'usr/share/applications/aura-browser.desktop')],check=True)


def build_deb(stage: Path, output: Path, identity=None):
    identity = identity or read_identity()
    control = stage/'DEBIAN'
    control.mkdir()
    # Let Debian derive versioned dependencies from actual ELF symbol usage.
    with tempfile.TemporaryDirectory(prefix='aura-shlibs-') as tmp:
        work=Path(tmp);(work/'debian').mkdir()
        (work/'debian/control').write_text('Source: aura-browser\n\nPackage: aura-browser\nArchitecture: amd64\nDescription: Aura Browser\n')
        app=stage/'opt/aura-browser'
        command=['dpkg-shlibdeps','-O','--ignore-missing-info','-l'+str(app)]
        command += ['-e'+str(app/name) for name in ['aura_browser','libcef.so','libvulkan.so.1','libvk_swiftshader.so']]
        result=subprocess.check_output(command,cwd=work,text=True)
        depends=next(line.split('=',1)[1] for line in result.splitlines() if line.startswith('shlibs:Depends='))
    depends += ', libxpresent1, libxrandr2, ca-certificates, xdg-utils, util-linux'
    size=sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())//1024
    (control/'control').write_text(f'''Package: aura-browser
Version: {identity.native_version}-1
Section: web
Priority: optional
Architecture: amd64
Maintainer: Aura Browser project <264110797+Sttanors@users.noreply.github.com>
Installed-Size: {size}
Depends: {depends}
Homepage: https://github.com/Sttanors/aura-browser
Description: Aura desktop browser (preview)
 Native CEF and Skia browser with English/Russian UI, ad blocking and MV3 extensions.
''')
    shutil.copyfile(NATIVE/'postinst',control/'postinst')
    (control/'postinst').chmod(0o755)
    subprocess.run(['dpkg-deb','--root-owner-group','-Zgzip','-z6','--build',str(stage),str(output)],check=True)
    shutil.rmtree(control)


def build_arch(stage: Path, output: Path, identity=None):
    identity = identity or read_identity()
    size=sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
    meta=f'''pkgname = aura-browser
pkgbase = aura-browser
pkgver = {identity.arch_version}-1
pkgdesc = Aura desktop browser (preview)
url = https://github.com/Sttanors/aura-browser
builddate = {int(time.time())}
packager = Aura Browser project
size = {size}
arch = x86_64
license = custom:license-not-specified
'''+''.join('depend = '+dep+'\n' for dep in ARCH_DEPENDS)
    (stage/'.PKGINFO').write_text(meta)
    shutil.copyfile(NATIVE/'arch.install',stage/'.INSTALL')
    # Use libarchive's mtree writer; package ownership is root regardless of build uid.
    mtree=subprocess.check_output(['bsdtar','-cf','-','--format=mtree','--options=!all,use-set,type,uid,gid,mode,time,size,sha256,link','--uid','0','--gid','0','-C',str(stage),'.PKGINFO','.INSTALL','opt','usr'])
    (stage/'.MTREE').write_bytes(gzip.compress(mtree))
    subprocess.run(['bsdtar','-czf',str(output),'--uid','0','--gid','0','--uname','root','--gname','root','-C',str(stage),'.PKGINFO','.MTREE','.INSTALL','opt','usr'],check=True)
    (stage/'.PKGINFO').unlink();(stage/'.MTREE').unlink();(stage/'.INSTALL').unlink()


def build_rpm(stage: Path, output: Path, identity=None):
    identity = identity or read_identity()
    with tempfile.TemporaryDirectory(prefix='aura-rpm-') as tmp:
        top=Path(tmp)
        for directory in ['BUILD','BUILDROOT','RPMS','SOURCES','SPECS','SRPMS']:
            (top/directory).mkdir()
        spec=top/'SPECS/aura.spec'
        spec.write_text('''%global __os_install_post %{nil}
%global __provides_exclude_from ^/opt/aura-browser/.*$
%global __requires_exclude ^(libcef[.]so|libvulkan[.]so[.]1|libvk_swiftshader[.]so).*$
Name: aura-browser
Version: @AURA_VERSION@
Release: 1
Summary: Aura desktop browser (preview)
License: LicenseRef-License-Not-Specified
URL: https://github.com/Sttanors/aura-browser
BuildArch: x86_64
Requires: ca-certificates, xdg-utils, util-linux, libXpresent.so.1()(64bit), libXrandr.so.2()(64bit)
%description
Native CEF and Skia browser with English/Russian UI, ad blocking and MV3 extensions.
%install
mkdir -p %{buildroot}
cp -a "%{aura_stage}/." %{buildroot}/
%post
/usr/lib/aura-browser/migrate-all-users || :
%files
/opt/aura-browser
/usr/bin/aura-browser
/usr/lib/aura-browser
/usr/share/applications/aura-browser.desktop
/usr/share/icons/hicolor/512x512/apps/aura-browser.png
/usr/share/icons/hicolor/scalable/apps/aura-browser.svg
/usr/share/licenses/aura-browser
/usr/share/doc/aura-browser
'''.replace('@AURA_VERSION@',identity.native_version))
        subprocess.run(['rpmbuild','-bb',str(spec),'--define','_topdir '+str(top),'--define','aura_stage '+str(stage),'--define','_binary_payload w6.gzdio'],check=True)
        packages=list((top/'RPMS').rglob('*.rpm'));assert len(packages)==1
        shutil.copyfile(packages[0],output)


def build(archive: Path, output: Path):
    identity=read_identity()
    output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='aura-native-') as tmp:
        stage=Path(tmp)
        stage_runtime(archive.resolve(),stage)
        actual=subprocess.check_output([str(stage/'opt/aura-browser/aura_browser'),'--version'],text=True).strip()
        if actual != identity.version:raise ValueError('Runtime version does not match package identity: '+actual)
        for name, builder in [
            (identity.package_name('debian'),build_deb),
            (identity.package_name('arch'),build_arch),
            (identity.package_name('fedora'),build_rpm),
        ]:
            path=output.resolve()/name
            builder(stage,path,identity)
            path.with_suffix(path.suffix+'.sha256').write_text(hashlib.sha256(path.read_bytes()).hexdigest()+'  '+path.name+'\n')
            print(path,flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--archive',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=ROOT/'artifacts/native-packages')
    args=parser.parse_args();build(args.archive,args.output)
