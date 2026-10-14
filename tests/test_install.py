"""Check installation boundaries using fake game folders, never the live installation."""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import install as installer


class InstallSafetyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'Game'
        binary = self.game / 'Townfall/Binaries/Win64'
        binary.mkdir(parents=True)
        (binary / 'Townfall-Win64-Shipping.exe').write_bytes(b'fake game, never executed')
        self.mods = binary / 'ue4ss/Mods'
        other = self.mods / 'OtherMod/Scripts'
        other.mkdir(parents=True)
        (other / 'addon.dll').write_bytes(b'keep other DLL')
        (self.mods / 'OtherMod/client.py').write_bytes(b'keep other Python')
        (self.mods / 'mods.txt').write_bytes(b'keep existing loader configuration')
        (self.mods / 'mods.json').write_bytes(b'[]')
        self.source = self.root / installer.MOD_NAME
        for relative in ['enabled.txt', 'Scripts/main.lua', 'Scripts/tfv_game.lua', 'Scripts/tfv_screen.lua',
                         'Scripts/tfv_native.lua', 'Scripts/tf_video.dll', 'stations.ini', 'Videos/README.txt']:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(relative.encode())
        (self.source / 'Videos/example.mp4').write_bytes(b'user supplied video')
        self.running = patch.object(installer, 'game_is_running', return_value=False)
        self.running.start()
        self.addCleanup(self.running.stop)

    def test_preview_writes_nothing(self):
        before = installer.snapshot(self.game)
        installer.plan(self.source, self.game)
        self.assertEqual(before, installer.snapshot(self.game))

    def test_install_only_adds_separate_mod_and_preserves_other_mods_and_loader(self):
        before = installer.snapshot(self.game)
        result = installer.install(self.source, self.game)
        after = installer.snapshot(self.game)
        self.assertTrue(all(after[key] == value for key, value in before.items()))
        self.assertEqual(installer.snapshot(self.source), installer.snapshot(self.mods / installer.MOD_NAME))
        self.assertEqual(result['other_mod_files_verified'], 2)
        self.assertTrue(all('/Mods/TownfallVideoTuner/' in key for key in after.keys() - before.keys()))

    def test_existing_mod_and_user_videos_are_never_overwritten(self):
        target = self.mods / installer.MOD_NAME
        target.mkdir()
        (target / 'stations.ini').write_bytes(b'user frequencies')
        (target / 'user.mp4').write_bytes(b'user video')
        before = installer.snapshot(self.game)
        with self.assertRaisesRegex(ValueError, 'already exists'):
            installer.install(self.source, self.game)
        self.assertEqual(before, installer.snapshot(self.game))

    def test_running_game_prevents_any_write(self):
        before = installer.snapshot(self.game)
        with patch.object(installer, 'game_is_running', return_value=True):
            with self.assertRaisesRegex(ValueError, 'Close Townfall'):
                installer.install(self.source, self.game)
        self.assertEqual(before, installer.snapshot(self.game))

    def test_wrong_game_root_and_incomplete_package_prevent_any_write(self):
        before = installer.snapshot(self.game)
        with self.assertRaisesRegex(ValueError, 'Select the Townfall game root'):
            installer.install(self.source, self.root)
        (self.source / 'Scripts/tf_video.dll').unlink()
        with self.assertRaisesRegex(ValueError, 'incomplete'):
            installer.install(self.source, self.game)
        self.assertEqual(before, installer.snapshot(self.game))

    def test_python_and_powershell_cannot_enter_runtime_folder(self):
        for name in ['launch.py', 'Start.ps1']:
            (self.source / name).write_bytes(b'unexpected launcher')
            with self.assertRaisesRegex(ValueError, 'Unexpected runtime payload'):
                installer.install(self.source, self.game)
            (self.source / name).unlink()
        self.assertFalse((self.mods / installer.MOD_NAME).exists())

    def test_update_preserves_user_configuration_videos_mapping_and_enable_state(self):
        installer.install(self.source, self.game)
        target = self.mods / installer.MOD_NAME
        (target / 'stations.ini').write_bytes(b'user pinned frequencies')
        (target / 'enabled.txt').unlink()  # a disabled mod stays disabled during an update
        (target / 'Videos/user.mkv').write_bytes(b'user MKV')
        (target / 'Videos/stations-auto.ini').write_bytes(b'cached automatic frequencies')
        (self.source / 'Videos/example.mp4').write_bytes(b'changed source video should never replace user files')
        old = (target / 'Scripts/tf_video.dll').read_bytes()
        (self.source / 'Scripts/tf_video.dll').write_bytes(b'new DLL')
        before = installer.snapshot(self.game)
        backup = self.root / 'Backup'
        result = installer.install(self.source, self.game, update=True, backup_dir=backup)
        after = installer.snapshot(self.game)
        changed = [name for name in before if before[name] != after[name]]
        self.assertEqual(changed, ['Townfall/Binaries/Win64/ue4ss/Mods/TownfallVideoTuner/Scripts/tf_video.dll'])
        self.assertEqual(before.keys(), after.keys())
        self.assertEqual((backup / 'Scripts/tf_video.dll').read_bytes(), old)
        self.assertEqual(result['changed_files'], ['Scripts/tf_video.dll'])

    def test_update_cannot_create_a_mod_or_replace_an_unrecognized_folder(self):
        before = installer.snapshot(self.game)
        with self.assertRaisesRegex(ValueError, 'existing TownfallVideoTuner'):
            installer.install(self.source, self.game, update=True)
        self.assertEqual(before, installer.snapshot(self.game))
        target = self.mods / installer.MOD_NAME
        target.mkdir()
        (target / 'other-file.txt').write_bytes(b'keep this folder')
        before = installer.snapshot(self.game)
        with self.assertRaisesRegex(ValueError, 'existing TownfallVideoTuner'):
            installer.install(self.source, self.game, update=True)
        self.assertEqual(before, installer.snapshot(self.game))


if __name__ == '__main__':
    unittest.main()
