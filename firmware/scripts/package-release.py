"""Package ESP32-P4 public build as separate flash ranges, preserving NVS."""
import argparse, hashlib, json, pathlib, shutil
p=argparse.ArgumentParser();p.add_argument('--version',required=True);p.add_argument('--output',default='dist/firmware-release');args=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[2]
build=root/'firmware/.pio/build/esp32p4_release';out=root/args.output;out.mkdir(parents=True,exist_ok=True)
parts=[]
for name,offset in [('bootloader.bin',0x2000),('partitions.bin',0x8000),('firmware.bin',0x10000)]:
    source=build/name
    if not source.is_file(): raise SystemExit(f'Missing public build: {source}')
    shutil.copyfile(source,out/name);parts.append({'path':name,'offset':offset})
# The factory image includes OTA selection data; don't flash its padded NVS gap.
factory=(build/'firmware.factory.bin').read_bytes()
assert len(factory)>0x10000
(out/'boot_app0.bin').write_bytes(factory[0xe000:0x10000]);parts.insert(2,{'path':'boot_app0.bin','offset':0xe000})
manifest={'name':'Macropad ESP32-P4 7 inch','version':args.version,'new_install_prompt_erase':True,'builds':[{'chipFamily':'ESP32-P4','parts':parts}]}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
shutil.copyfile(root/'firmware/web-installer/index.html',out/'index.html')
(out/'SHA256SUMS').write_text(''.join(f'{hashlib.sha256((out/x["path"]).read_bytes()).hexdigest()}  {x["path"]}\n' for x in parts))
print(f'Public installer packaged: {out}')
