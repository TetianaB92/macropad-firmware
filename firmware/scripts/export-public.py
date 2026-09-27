"""Export a clean firmware source tree without private app code or git history."""
import pathlib
import shutil
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
out = root / 'dist/macropad-firmware'
if out.exists():
    raise SystemExit(f'Refusing to overwrite existing export: {out}')
paths = subprocess.check_output(
    ['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard', 'firmware'],
    cwd=root,
).decode().split('\0')
allowed = {'src', 'include', 'lib', 'certs', 'scripts', 'web-installer'}
count = 0
for name in sorted(set(filter(None, paths))):
    path = pathlib.Path(name)
    if len(path.parts) < 2 or (path.parts[1] not in allowed and name != 'firmware/platformio.ini'):
        continue
    if path.name == 'secrets.h' or any(part.startswith('.') for part in path.parts):
        raise SystemExit(f'Unexpected private path: {name}')
    source = root / path
    if source.is_symlink():
        raise SystemExit(f'Unexpected symlink: {name}')
    target = out / path
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    count += 1
(out / '.github/workflows').mkdir(parents=True)
shutil.copyfile(root / '.github/workflows/firmware.yml', out / '.github/workflows/firmware.yml')
(out / '.gitignore').write_text('firmware/.pio/\nfirmware/include/secrets.h\n.env*\ndist/\n__pycache__/\n')
(out / 'README.md').write_text('''# Macropad firmware

Universal ESP32-P4 UI runtime and Wi-Fi HTTP relay. The web service supplies
scenes, RGB565 artwork, state bindings and actions. New widgets composed from
supported primitives do not require new firmware. New hardware drivers or UI
primitives do require a firmware update.

## Build and browser installer

```sh
python -m pip install platformio==6.1.19
pio run -d firmware -e esp32p4_release
python firmware/scripts/package-release.py --version 0.2.0-rc1
```

Serve `dist/firmware-release` over HTTPS for ESP Web Tools. Connect the board's
USB-C UART port and close other serial applications before flashing.
The public build ignores local `secrets.h`. Wi-Fi and pairing are configured on
the device and stored in NVS. Flash ranges preserve NVS unless erase is selected.

This is a release candidate: physical display stability, relay operation with
real lamps, and browser flashing still require hardware acceptance testing.
Do not upload local credentials, device tokens, `.pio` or private app sources.
Third-party font license notices are included alongside the font sources.
''')
print(f'Exported {count} firmware files to {out}; review before publishing.')
