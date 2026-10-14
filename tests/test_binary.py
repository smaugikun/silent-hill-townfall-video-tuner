"""Binary/build gates; PE validation reads a mapped game image without executing game code."""
import ctypes as ct
import os
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path
import pefile
ROOT=Path(__file__).resolve().parents[1]
SCRIPTS=ROOT/'TownfallVideoTuner/Scripts'
sys.path.insert(0,str(ROOT))
from build import write_profile

def dll():
    library=ct.WinDLL(str(SCRIPTS/'tf_video.dll'))
    library.tf_video_validate.argtypes=[ct.c_void_p,ct.c_wchar_p]; library.tf_video_validate.restype=ct.c_int
    return library

class BinaryTests(unittest.TestCase):
    def test_standalone_system_imports_and_exports(self):
        pe=pefile.PE(str(SCRIPTS/'tf_video.dll'))
        self.assertEqual(pe.FILE_HEADER.Machine,0x8664)
        imports={entry.dll.decode().lower() for entry in pe.DIRECTORY_ENTRY_IMPORT}
        ordinary={'kernel32.dll','ntdll.dll','msvcrt.dll','winmm.dll','user32.dll'}
        unexpected={i for i in imports if i not in ordinary and not re.fullmatch(r'api-ms-win-crt-[a-z]+-l\d+-\d+-\d+\.dll',i)}
        self.assertEqual(unexpected,set(),'only Windows system/Universal CRT imports are allowed (dbghelp is loaded from System32 when needed)')
        self.assertIn('winmm.dll',imports)
        exports={s.name.decode() for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.name}
        self.assertLessEqual({'tf_video_init','tf_video_tick','tf_video_shutdown','tf_video_validate','tf_video_profileW'},exports)
        pe.close()
    def test_invalid_image_or_profile_is_rejected(self):
        library=dll()
        with tempfile.TemporaryDirectory() as folder:
            profile=Path(folder)/'tf_video.profile'
            self.assertEqual(library.tf_video_validate(None,str(profile)),0)          # no profile
            profile.write_bytes(b'TFVPROF1'+bytes(840))
            bad=ct.create_string_buffer(4096)
            self.assertEqual(library.tf_video_validate(ct.addressof(bad),str(profile)),0)

def mapped_image(path):
    """The program as Windows lays it out in memory (headers, then each section at its RVA), without running it."""
    data=path.read_bytes()
    header=struct.unpack_from('<I',data,0x3c)[0]
    sections=struct.unpack_from('<H',data,header+6)[0]
    optional_size=struct.unpack_from('<H',data,header+20)[0]
    image_size,headers_size=struct.unpack_from('<II',data,header+24+56)
    image=bytearray(image_size)
    image[:headers_size]=data[:headers_size]
    for i in range(sections):
        _,address,raw_size,raw=struct.unpack_from('<IIII',data,header+24+optional_size+40*i+8)
        size=min(raw_size,image_size-address)
        if size>0: image[address:address+size]=data[raw:raw+size]
    return image

class ExactGameProfileTest(unittest.TestCase):
    def test_actual_game_image_and_mutations(self):
        path=Path(os.environ.get('TF_VIDEO_GAME_EXE','C:/Program Files (x86)/Steam/steamapps/common/Townfall/Townfall/Binaries/Win64/Townfall-Win64-Shipping.exe'))
        if not path.is_file(): self.skipTest('matching game executable not supplied; set TF_VIDEO_GAME_EXE')
        with tempfile.TemporaryDirectory() as folder:
            profile=Path(folder)/'tf_video.profile'
            self.assertEqual(write_profile(SCRIPTS/'tf_video.dll',path,profile),0)
            image=mapped_image(path)
            base=ct.addressof((ct.c_char*len(image)).from_buffer(image))
            library=dll()
            self.assertEqual(library.tf_video_validate(base,str(profile)),1,'the profile must fit the game it was written for')
            stamp=struct.unpack_from('<I',image,0x3c)[0]+8                 # the build stamp
            first=struct.unpack_from('<I',profile.read_bytes(),48)[0]       # the first function's RVA
            for at in (stamp,first):
                image[at]^=1
                self.assertEqual(library.tf_video_validate(base,str(profile)),0)
                image[at]^=1
            self.assertEqual(library.tf_video_validate(base,str(profile)),1)

if __name__=='__main__': unittest.main()
