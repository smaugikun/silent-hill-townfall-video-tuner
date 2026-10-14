"""Preview or install ONLY the separate TownfallVideoTuner folder into an existing UE4SS installation."""
import argparse
import ctypes
import hashlib
import os
import shutil
import stat
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MOD_NAME = 'TownfallVideoTuner'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def reject_links(path):
    """Refuse symlinks and Windows junctions before writing or following them."""
    for current in [path, *path.parents]:
        try:
            info = current.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError(f'Refusing a symlink or reparse point: {current}')


def snapshot(folder):
    if not folder.exists():
        return {}
    reject_links(folder)
    result = {}
    for path in sorted(folder.rglob('*')):
        reject_links(path)
        if path.is_file():
            result[path.relative_to(folder).as_posix()] = digest(path)
    return result


def game_is_running():
    if os.name != 'nt':
        return False
    from ctypes import wintypes as wt

    class ProcessEntry(ctypes.Structure):
        _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                    ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD),
                    ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                    ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD), ('szExeFile', wt.WCHAR * 260)]

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wt.HANDLE
    kernel.Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32FirstW.restype = wt.BOOL
    kernel.Process32NextW.argtypes = kernel.Process32FirstW.argtypes
    kernel.Process32NextW.restype = wt.BOOL
    kernel.CloseHandle.argtypes = [wt.HANDLE]
    kernel.CloseHandle.restype = wt.BOOL
    handle = kernel.CreateToolhelp32Snapshot(2, 0)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        found = kernel.Process32FirstW(handle, ctypes.byref(entry))
        while found:
            if entry.szExeFile.lower().startswith('townfall'):
                return True
            found = kernel.Process32NextW(handle, ctypes.byref(entry))
        error = ctypes.get_last_error()
        if error != 18:  # ERROR_NO_MORE_FILES
            raise ctypes.WinError(error)
        return False
    finally:
        kernel.CloseHandle(handle)


def plan(source, game_root, update=False):
    source = Path(source).absolute()
    game_root = Path(game_root).absolute()
    reject_links(source)
    reject_links(game_root)
    exe = game_root / 'Townfall/Binaries/Win64/Townfall-Win64-Shipping.exe'
    mods = exe.parent / 'ue4ss/Mods'
    reject_links(mods)
    if not exe.is_file() or not mods.is_dir():
        raise ValueError('Select the Townfall game root containing Townfall/Binaries/Win64 and an installed UE4SS/Mods folder.')
    target = mods / MOD_NAME
    reject_links(target)
    if target.exists() and not update:
        raise ValueError(f'{target} already exists. Refusing to replace its configuration or videos.')
    if update and (not (target / 'Scripts/main.lua').is_file() or not (target / 'Scripts/tf_video.dll').is_file()):
        raise ValueError('Update requires an existing TownfallVideoTuner installation.')
    if not source.is_dir():
        raise ValueError(f'Missing source mod folder: {source}')
    entries = []
    for path in sorted(source.rglob('*')):
        reject_links(path)
        if path.is_file():
            relative = path.relative_to(source)
            if path.suffix.lower() in {'.py', '.ps1', '.bin', '.log'} or '__pycache__' in relative.parts:
                raise ValueError(f'Unexpected runtime payload: {relative}')
            entries.append({'path': relative.as_posix(), 'bytes': path.stat().st_size, 'sha256': digest(path)})
    required = {'enabled.txt', 'Scripts/main.lua', 'Scripts/tfv_game.lua', 'Scripts/tfv_screen.lua',
                'Scripts/tfv_native.lua', 'Scripts/tf_video.dll', 'stations.ini', 'Videos/README.txt'}
    if not required <= {entry['path'] for entry in entries}:
        raise ValueError('The runtime mod folder is incomplete.')
    if update:
        entries = [entry for entry in entries if entry['path'] not in {'stations.ini', 'enabled.txt'}
                   and (not entry['path'].startswith('Videos/') or entry['path'] == 'Videos/README.txt')]
        for entry in entries:
            reject_links(target / entry['path'])
    return source, mods, target, entries


def user_files(target):
    saved = {str(path.relative_to(target)): digest(path) for path in [target / 'stations.ini', target / 'enabled.txt'] if path.is_file()}
    saved.update({'Videos/' + name: value for name, value in snapshot(target / 'Videos').items() if name != 'README.txt'})
    return saved

def other_mod_files(mods):
    result = {}
    for folder in sorted(mods.iterdir()):
        if folder.is_dir() and folder.name != MOD_NAME:
            result.update({folder.name + '/' + name: value for name,value in snapshot(folder).items()})
    return result


def install(source, game_root, update=False, backup_dir=None):
    source, mods, target, entries = plan(source, game_root, update)
    if game_is_running():
        raise ValueError('Close Townfall before installing the mod.')
    if backup_dir:
        backup_dir = Path(backup_dir).absolute()
        reject_links(backup_dir)
        if backup_dir.resolve().is_relative_to(Path(game_root).resolve()):
            raise ValueError('Store update backups outside the game folder.')
    others_before = other_mod_files(mods)
    loader_before = {path.name: digest(path) for path in [mods / 'mods.txt', mods / 'mods.json'] if path.is_file()}
    user_before = user_files(target) if update else {}
    # Exclusive creation prevents overwriting a target created after the preview.
    if not update:
        target.mkdir()
    changed = []
    for entry in entries:
        original = source / entry['path']
        destination = target / entry['path']
        if digest(original) != entry['sha256']:
            raise ValueError(f'Source changed since planning: {original}')
        reject_links(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if update and destination.is_file() and digest(destination) == entry['sha256']:
            continue
        if update and destination.exists():
            if backup_dir:
                backup = backup_dir / entry['path']
                reject_links(backup)
                backup.parent.mkdir(parents=True, exist_ok=True)
                with destination.open('rb') as incoming, backup.open('xb') as outgoing:
                    shutil.copyfileobj(incoming, outgoing)
            with tempfile.NamedTemporaryFile(dir=destination.parent, prefix='tfv-update-', delete=False) as outgoing:
                temporary = Path(outgoing.name)
                with original.open('rb') as incoming:
                    shutil.copyfileobj(incoming, outgoing)
            # Every replacement remains within the explicitly selected mod folder.
            if not temporary.resolve().is_relative_to(target.resolve()) or not destination.resolve().is_relative_to(target.resolve()):
                raise ValueError('Update paths escaped the video mod folder.')
            if digest(temporary) != entry['sha256']:
                raise ValueError('Temporary update file verification failed.')
            os.replace(temporary, destination)
        else:
            with original.open('rb') as incoming, destination.open('xb') as outgoing:
                shutil.copyfileobj(incoming, outgoing)
        if digest(destination) != entry['sha256']:
            raise ValueError(f'Installed file verification failed: {destination}')
        changed.append(entry['path'])
    others_after = other_mod_files(mods)
    loader_after = {path.name: digest(path) for path in [mods / 'mods.txt', mods / 'mods.json'] if path.is_file()}
    if others_before != others_after or loader_before != loader_after:
        raise ValueError('Protected other-mod or loader files changed during installation.')
    if update and user_before != user_files(target):
        raise ValueError('User videos, frequency map or configuration changed during update.')
    return {'changed_files': changed, 'other_mod_files_verified': len(others_before)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', required=True, type=Path)
    parser.add_argument('--source', type=Path, default=ROOT / MOD_NAME)
    parser.add_argument('--install', action='store_true', help='Copy files; the default only previews')
    parser.add_argument('--update', action='store_true', help='Update runtime/docs while preserving videos, station settings and enable state')
    parser.add_argument('--backup-dir', type=Path, help='Save old runtime files outside the game before replacing them')
    args = parser.parse_args()
    source, mods, target, entries = plan(args.source, args.game_root, args.update)
    print(f'{"Install" if args.install else "Preview"}: {target}')
    for entry in entries:
        print(f'  {entry["path"]} ({entry["bytes"]} bytes)')
    if args.install:
        result = install(source, args.game_root, args.update, args.backup_dir)
        print(f'Verified {len(entries)} files ({len(result["changed_files"])} changed); {result["other_mod_files_verified"]} other-mod files unchanged.')
        if args.update:
            print('User videos, frequency map, stations.ini and enable state preserved.')
    else:
        print('Preview only. Add --install to copy this separate mod folder. Other mods and loader settings are preserved.')


if __name__ == '__main__':
    main()
