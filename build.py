"""Build the native mod and test harnesses. Python is development tooling only."""
import argparse
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def pip_zig():
    """zig.exe from the ziglang package (requirements-dev.txt), if it is installed."""
    try:
        import ziglang
    except ImportError:
        return None
    path = Path(ziglang.__file__).parent / 'zig.exe'
    return str(path) if path.is_file() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', help='Path to Zig 0.15.2; also accepts ZIG_EXE')
    parser.add_argument('--game-root', type=Path,
                        help="also write the installed game's profile (Scripts/tf_video.profile), as the release ships it")
    args = parser.parse_args()
    zig = args.zig or os.environ.get('ZIG_EXE') or pip_zig() or shutil.which('zig')
    if not zig:
        parser.error('Install Zig 0.15.2 (py -m pip install -r requirements-dev.txt), or pass --zig with its path.')
    zig = str(Path(zig).resolve())
    version = subprocess.check_output([zig, 'version'], text=True).strip()
    if version != '0.15.2':
        parser.error(f'This recipe requires Zig 0.15.2; found {version}.')
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(build / 'zig-global-cache'),
               ZIG_LOCAL_CACHE_DIR=str(build / 'zig-local-cache'))
    common = [zig, 'cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O2',
              '-Wall', '-Wextra', '-Wshadow', '-Werror']
    shared = ['native/core.c', 'native/audio.c', 'native/files.c']
    recipes = [
        ('tf_video.dll', ['-s', '-shared'], ['native/tf_video.c', *shared, 'native/game_profile.c', 'native/version.rc']),
        ('native_tests.exe', ['-DTF_AUDIO_TEST'], ['tests/native_tests.c', *shared]),
        ('driver_tests.exe', ['-DTF_AUDIO_TEST'], ['tests/driver_tests.c', *shared]),
        ('fmod_integration.exe', [], ['tests/fmod_integration.c', 'native/core.c']),
    ]
    for name, flags, sources in recipes:
        print(f'Building {name}', flush=True)
        subprocess.run([*common, *flags, *sources, '-lwinmm', '-o', str(build / name)],
                       cwd=ROOT, env=env, check=True)
    scripts = ROOT / 'TownfallVideoTuner/Scripts'
    shutil.copy2(build / 'tf_video.dll', scripts / 'tf_video.dll')
    if args.game_root:
        game = args.game_root / 'Townfall/Binaries/Win64/Townfall-Win64-Shipping.exe'
        if write_profile(scripts / 'tf_video.dll', game, scripts / 'tf_video.profile'):
            raise SystemExit("Couldn't write the game profile: run the same through rundll32 with --log to see why.")
    print('Built the DLL and all native test harnesses. Nothing was installed into the game.')


def write_profile(dll, game, profile, *options):
    """What tf_video.dll does after a game update: its game_profile.c, in Windows' rundll32.exe. The exit code."""
    rundll = Path(os.environ['SystemRoot']) / 'System32' / 'rundll32.exe'
    arguments = ' '.join(f'"{option}"' for option in (game, profile, *options))
    return subprocess.run(f'"{rundll}" "{dll}",tf_video_profile {arguments}').returncode


if __name__ == '__main__':
    main()
