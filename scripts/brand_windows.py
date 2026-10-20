"""Embed Aura icons and version identity into the CEF bootstrap without relinking it."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def version_resource(dll=False):
    def block(key, value=b'', children=b'', text=False):
        name = (key+'\0').encode('utf-16le')
        data = struct.pack('<HHH', 0, len(value)//2 if text else len(value), int(text))+name
        data += b'\0'*((-len(data))%4)
        data += value
        data += b'\0'*((-len(data))%4)
        data += children
        return struct.pack('<H', len(data))+data[2:]
    strings = {'CompanyName':'Aura Browser project', 'FileDescription':'Aura Browser',
               'FileVersion':'0.2.0.3', 'ProductName':'Aura Browser', 'ProductVersion':'0.2.0-preview.3',
               'OriginalFilename':'aura_browser.dll' if dll else 'aura_browser.exe'}
    table = b''.join(block(k,(v+'\0').encode('utf-16le'),text=True) for k,v in strings.items())
    info = block('StringFileInfo',children=block('040904B0',children=table,text=True),text=True)
    var = block('VarFileInfo',children=block('Translation',struct.pack('<HH',0x409,1200)),text=True)
    fixed = struct.pack('<13I',0xFEEF04BD,0x10000,0x00020000,3,0x00020000,3,0x3f,0,0x40004,2 if dll else 1,0,0,0)
    return block('VS_VERSION_INFO',fixed,info+var)


def brand(executable):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR,wintypes.BOOL]
    kernel.BeginUpdateResourceW.restype = wintypes.HANDLE
    kernel.UpdateResourceW.argtypes = [wintypes.HANDLE,ctypes.c_void_p,ctypes.c_void_p,wintypes.WORD,ctypes.c_void_p,wintypes.DWORD]
    kernel.UpdateResourceW.restype = wintypes.BOOL
    kernel.EndUpdateResourceW.argtypes = [wintypes.HANDLE,wintypes.BOOL]
    kernel.EndUpdateResourceW.restype = wintypes.BOOL
    handle = kernel.BeginUpdateResourceW(str(executable),False)
    if not handle: raise ctypes.WinError(ctypes.get_last_error())
    try:
        def update(kind, identifier, data):
            buffer = ctypes.create_string_buffer(data)
            if not kernel.UpdateResourceW(handle,kind,identifier,0x409,buffer,len(data)):
                raise ctypes.WinError(ctypes.get_last_error())
        ico = (ROOT/'resources/branding/aura.ico').read_bytes()
        reserved, kind, count = struct.unpack_from('<HHH',ico)
        assert reserved==0 and kind==1
        group = struct.pack('<HHH',0,1,count)
        for i in range(count):
            w,h,colors,res,planes,bpp,size,offset = struct.unpack_from('<BBBBHHII',ico,6+16*i)
            update(3,i+1,ico[offset:offset+size])
            group += struct.pack('<BBBBHHIH',w,h,colors,res,planes,bpp,size,i+1)
        update(14,1,group)
        update(16,1,version_resource(executable.suffix.lower()=='.dll'))
    except BaseException:
        kernel.EndUpdateResourceW(handle,True)
        raise
    if not kernel.EndUpdateResourceW(handle,False): raise ctypes.WinError(ctypes.get_last_error())
