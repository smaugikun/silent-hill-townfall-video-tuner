"""Run development checks without PowerShell scripts; never launches Townfall."""
import argparse
import json
import os
import subprocess
import sys
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, help='Adds the real FMOD and exact game-profile checks')
    args = parser.parse_args()
    env = os.environ.copy()
    if args.game_root:
        args.game_root = args.game_root.resolve()
        env['TF_VIDEO_GAME_EXE'] = str(args.game_root / 'Townfall/Binaries/Win64/Townfall-Win64-Shipping.exe')
    report_path = ROOT / 'build/verification.json'
    report = json.loads(report_path.read_text()) if report_path.is_file() else {}
    report.update(date=date.today().isoformat())
    jobs = [
        ('native_core', [str(ROOT / 'build/native_tests.exe')]),
        ('native_driver_mock', [str(ROOT / 'build/driver_tests.exe')]),
    ]
    if args.game_root:
        jobs.append(('real_fmod_mock_pcm', [str(ROOT / 'build/fmod_integration.exe'),
                    str(args.game_root / 'Engine/Plugins/FMODStudio/Binaries/Win64'),
                    str(args.game_root / 'Townfall/Content/FMOD/Banks/Desktop')]))
    for key, pattern in [('binary_profile', 'test_binary.py'), ('game_profile', 'test_profile.py'),
                         ('lua_game_mock', 'test_lua.py'),
                         ('installer_safety', 'test_install.py')]:
        jobs.append((key, [sys.executable, '-m', 'unittest', 'discover', '-s', str(ROOT / 'tests'),
                           '-p', pattern, '-v']))
    for key, command in jobs:
        print(f'Running {key}', flush=True)
        result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        report[key] = {'exit_code': result.returncode, 'output': result.stdout, 'command': command}
        report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        if result.returncode:
            raise SystemExit(result.returncode)
    print('All selected checks passed. These automated checks do not exercise live decoding or GPU rendering.')


if __name__ == '__main__':
    main()
