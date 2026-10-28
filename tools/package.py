"""Build the release ZIP (dist/TownfallVideoTuner-<version>.zip): the TownfallVideoTuner folder, LICENSE.txt and
THIRD_PARTY_NOTICES.md. Only after test.py has passed."""
import hashlib
import json
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'dist'
MOD = ROOT / 'TownfallVideoTuner'
VERSION = '1.0.2'
# Runtime files, the player's videos and what the mod writes next to them never ship.
EXCLUDED = {'.pyc', '.pyo', '.bin', '.log', '.ps1', '.mp4', '.mkv', '.tmp'}
PLAYER_FILES = {'stations-auto.ini'}
EXTRAS = ['LICENSE.txt', 'THIRD_PARTY_NOTICES.md']


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def files(folder):
    return sorted(path for path in folder.rglob('*') if path.is_file() and '__pycache__' not in path.parts
                  and path.suffix.lower() not in EXCLUDED and path.name not in PLAYER_FILES)


def package(archive):
    manifest = {}
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
        def put(source, name):
            zipped.write(source, name)
            manifest[name] = {'sha256': digest(source), 'bytes': source.stat().st_size}

        for path in files(MOD):
            put(path, path.relative_to(ROOT).as_posix())
        zipped.writestr('TownfallVideoTuner/Videos/', b'')
        for name in EXTRAS:
            put(ROOT / name, name)
    with zipfile.ZipFile(archive) as zipped:
        if zipped.testzip() is not None:
            raise SystemExit('ZIP CRC verification failed')
        names = set(zipped.namelist())
        required = {'TownfallVideoTuner/Scripts/main.lua', 'TownfallVideoTuner/Scripts/tf_video.dll',
                    'TownfallVideoTuner/Scripts/tf_video.profile',
                    'TownfallVideoTuner/enabled.txt', 'TownfallVideoTuner/stations.ini', 'TownfallVideoTuner/README.txt',
                    'TownfallVideoTuner/Videos/', *EXTRAS}
        if not required <= names:
            raise SystemExit(f'ZIP is incomplete: {sorted(required - names)}')
        for name, entry in manifest.items():
            content = zipped.read(name)
            if len(content) != entry['bytes'] or hashlib.sha256(content).hexdigest() != entry['sha256']:
                raise SystemExit(f'ZIP mismatch: {name}')
        roots = {'TownfallVideoTuner', *EXTRAS}
        forbidden = [name for name in names if name.split('/')[0] not in roots or '__pycache__' in name
                     or name.lower().endswith(('.py', '.ps1'))]
        if forbidden:
            raise SystemExit(f'Unwanted package entries: {forbidden}')
    archive.with_suffix('.sha256').write_text(digest(archive) + '  ' + archive.name + '\n', encoding='ascii')
    print(f'Packaged and verified {archive} ({archive.stat().st_size} bytes, {len(manifest)} files)')


def main():
    dll = MOD / 'Scripts/tf_video.dll'
    if not dll.is_file():
        raise SystemExit('Build the DLL first.')
    if not (MOD / 'Scripts/tf_video.profile').is_file():
        raise SystemExit('Write the game profile the release ships first: build.py --game-root <game>.')
    report_path = ROOT / 'build/verification.json'
    report = json.loads(report_path.read_text()) if report_path.is_file() else {}
    for key in ['native_core', 'native_driver_mock', 'real_fmod_mock_pcm', 'binary_profile', 'game_profile',
                'lua_game_mock', 'installer_safety']:
        if report.get(key, {}).get('exit_code') != 0:
            raise SystemExit(f'Cannot package: run test.py with --game-root first ({key} has not passed).')
    OUT.mkdir(exist_ok=True)
    package(OUT / f'TownfallVideoTuner-{VERSION}.zip')
    print(f'DLL SHA256: {digest(dll)}')


if __name__ == '__main__':
    main()
