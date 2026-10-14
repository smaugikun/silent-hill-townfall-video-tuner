"""The game profile tf_video.dll writes after a game update (game_profile.c, run through Windows' rundll32.exe), against
the installed game: the profile it writes, and the games it refuses.

expected_profile.json is the profile for Steam build 25697972 (from the generator before this one): for that build the
DLL must write exactly it. Skipped without the game (TF_VIDEO_GAME_EXE, set by test.py --game-root)."""
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from build import write_profile

DLL = ROOT / 'TownfallVideoTuner/Scripts/tf_video.dll'
GAME = Path(os.environ.get('TF_VIDEO_GAME_EXE',
                           'C:/Program Files (x86)/Steam/steamapps/common/Townfall/Townfall/Binaries/Win64/Townfall-Win64-Shipping.exe'))
EXPECTED = json.loads((ROOT / 'tests/expected_profile.json').read_text())
FUNCTIONS, VALUES = len(EXPECTED['functions']), len(EXPECTED['values'])
HEADER = struct.Struct('<8sIIII24s')
FUNCTION = struct.Struct('<I16s')


def parse(data):
    """A profile file as {timestamp, image_size, identity, functions: [(rva, code)], values: [...]}."""
    magic, version, size, timestamp, image_size, identity = HEADER.unpack_from(data)
    assert magic == b'TFVPROF1' and version == 1 and size == len(data), (magic, version, size, len(data))
    functions = [FUNCTION.unpack_from(data, HEADER.size + i * FUNCTION.size) for i in range(FUNCTIONS)]
    values = struct.unpack_from(f'<{VALUES}I', data, HEADER.size + FUNCTIONS * FUNCTION.size)
    return {'timestamp': timestamp, 'image_size': image_size, 'identity': identity.hex(),
            'functions': [(rva, code.hex()) for rva, code in functions], 'values': list(values)}


@unittest.skipUnless(DLL.is_file(), 'build the DLL first (build.py)')
class GameProfileTest(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp())
        self.profile = self.folder / 'tf_video.profile'
        self.log = self.folder / 'tf-video.log'

    def tearDown(self):
        for path in self.folder.iterdir():
            path.unlink()
        self.folder.rmdir()

    def write(self, game, *options):
        """The exit code, as the DLL starts it: its own log for the outcome."""
        return write_profile(DLL, game, self.profile, '--log', self.log, *options)

    def said(self):
        return self.log.read_text() if self.log.exists() else ''

    @unittest.skipUnless(GAME.is_file(), 'the game is not installed (set TF_VIDEO_GAME_EXE)')
    def test_the_installed_game_gets_its_profile(self):
        self.assertEqual(self.write(GAME), 0, self.said())
        profile = parse(self.profile.read_bytes())
        self.assertIn('Game profile: written for this game version', self.said())
        self.assertFalse(self.profile.with_suffix('.profile.tmp').exists())
        if profile['timestamp'] != EXPECTED['timestamp']:
            self.skipTest('another game build than expected_profile.json: written, not compared')
        self.assertEqual(profile['image_size'], EXPECTED['image_size'])
        self.assertEqual(profile['identity'], EXPECTED['identity'])
        self.assertEqual(profile['functions'], [(f['rva'], f['code']) for f in EXPECTED['functions'].values()])
        self.assertEqual(profile['values'], list(EXPECTED['values'].values()))

    @unittest.skipUnless(GAME.is_file(), 'the game is not installed (set TF_VIDEO_GAME_EXE)')
    def test_a_game_that_changed_a_function_gets_no_profile(self):
        self.profile.write_bytes(b'the old profile')
        self.assertEqual(self.write(GAME, '--expect', 'UMediaPlayer::OpenFile', 'base10:1(FString*, base10:1)'), 2,
                         self.said())
        self.assertIn('changed what the mod relies on (UMediaPlayer::OpenFile is now base10:1(FString*)', self.said())
        self.assertEqual(self.profile.read_bytes(), b'the old profile')  # nothing half-written

    def test_a_program_without_its_pdb_gets_no_profile(self):
        self.assertEqual(self.write(DLL), 3, self.said())
        self.assertIn("couldn't read the game's symbols", self.said())
        self.assertFalse(self.profile.exists())


if __name__ == '__main__':
    unittest.main()
