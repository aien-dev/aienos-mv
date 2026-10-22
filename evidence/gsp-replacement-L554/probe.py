#!/usr/bin/env python3
"""Read-only GB10 replacement prerequisite evidence, NOT firmware.

Runs existing C host inspection oracles in this task-owned worktree. Never
opens a device node, sends MMIO, changes modules, installs firmware or resets
hardware. nvidia-smi/NVML and the existing /v1/models endpoint query live state.
Python is evidence orchestration, not part of an Omega construction closure.
"""
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
assert ROOT.name == 'aienux-gsp-replacement-L554', ROOT
OUT = ROOT / 'target/gsp-replacement-L554'
EV = ROOT / 'evidence/gsp-replacement-L554'
SRC = Path.home() / 'workspace/_ext/open-gpu-kernel-modules-580.173.02'
FW = Path('/lib/firmware/nvidia/580.173.02/gsp_ga10x.bin')
PIN = '20e4e6e19cc26ba47b5cbe23130a396be100c427'
BASE = '18e276590424593647cd7c39c6d16c2f9f8d6027'
records = []

def run(label, argv, expected=0, timeout=60):
    p = subprocess.run(argv, cwd=ROOT, text=True, capture_output=True, timeout=timeout)
    row = {'label': label, 'argv': list(map(str, argv)), 'exit_code': p.returncode,
           'expected_exit_code': expected, 'stdout': p.stdout, 'stderr': p.stderr}
    records.append(row)
    print(label, 'RC=' + str(p.returncode), flush=True)
    assert p.returncode == expected, row
    return p.stdout

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

receipt = {'utc_start': datetime.datetime.now(datetime.timezone.utc).isoformat(),
           'scope': 'host/source inspection only; no custom-firmware boot attempted',
           'probe_sha256': sha(Path(__file__)), 'records': records}
try:
    assert run('branch', ['git', 'branch', '--show-current']).strip() == 'research/gsp-replacement-L554'
    run('base_present', ['git', 'merge-base', '--is-ancestor', BASE, 'HEAD'])
    assert run('nvidia_source_pin', ['git', '-C', str(SRC), 'rev-parse', 'HEAD']).strip() == PIN
    assert run('nvidia_source_clean', ['git', '-C', str(SRC), 'status', '--porcelain']) == ''
    run('uname', ['uname', '-a'])
    run('gpu_before', ['nvidia-smi', '--query-gpu=name,driver_version,pci.bus_id', '--format=csv'])
    run('gsp_before', ['nvidia-smi', '-q', '-d', 'GSP_FIRMWARE_VERSION'])
    run('service_before', ['curl', '--fail', '--silent', '--show-error', '--max-time', '10', 'http://127.0.0.1:18091/v1/models'])
    receipt['firmware_before'] = {'path': str(FW), 'bytes': FW.stat().st_size, 'sha256': sha(FW)}
    run('inspector_selftest_and_real_pin', [str(Path.home()/'.local/bin/quiet-build'), 'make', '-C', 'native/kernel', 'OUT='+str(OUT), 'gsp-fw-inspect-test'], timeout=180)
    tool = OUT / 'host/ck_gsp_fw_inspect'
    receipt['oracle'] = {'language': 'pre-existing C, external inspection oracle only', 'source_commit': BASE,
                         'binary_sha256': sha(tool),
                         'source_sha256': sha(ROOT/'native/kernel/tools/ck_gsp_fw_inspect.c'),
                         'sha256_source_sha256': sha(ROOT/'native/argus/sha256.c')}
    run('real_firmware', [str(tool), 'inspect', str(FW), '--family', 'gb10y', '--expect-version', '580.173.02'])
    run('negative_missing_family', [str(tool), 'inspect', str(FW), '--family', 'l554-nonexistent', '--expect-version', '580.173.02'], expected=2)
    run('negative_wrong_version', [str(tool), 'inspect', str(FW), '--family', 'gb10y', '--expect-version', 'l554-wrong-version'], expected=2)
    spans = {
      'src/nvidia/generated/g_kern_fsp_nvoc.c': [(220,240)],
      'src/nvidia/generated/g_kernel_sec2_nvoc.c': [(438,454)],
      'src/nvidia/src/kernel/gpu/sec2/arch/blackwell/kernel_sec2_gb10b.c': [(216,237),(285,345),(438,535)],
      'src/nvidia/src/kernel/gpu/gsp/arch/hopper/kernel_gsp_gh100.c': [(775,808),(845,900)],
      'src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c': [(4740,4905)],
      'src/nvidia/src/kernel/gpu/gsp/arch/blackwell/kernel_gsp_gb100.c': [(90,110)],
      'src/nvidia/arch/nvalloc/common/inc/nv-firmware-chip-family-select.h': [(47,67)],
      'kernel-open/common/inc/nv-firmware.h': [(50,68),(85,109)],
    }
    excerpt = ['NVIDIA source '+PIN+'; git status empty; line-numbered excerpts (trailing whitespace omitted).']
    for rel,ranges in spans.items():
        p=SRC/rel
        lines=p.read_text().splitlines()
        excerpt.append('\nFILE '+rel+' sha256 '+sha(p))
        for a,b in ranges:
            assert 1 <= a <= b <= len(lines)
            excerpt.extend((str(i+1)+': '+lines[i]).rstrip() for i in range(a-1,b))
    (EV/'source-excerpts.txt').write_text('\n'.join(excerpt)+'\n')
    (EV/'NVIDIA-COPYING.txt').write_text((SRC/'COPYING').read_text().rstrip()+'\n')
    receipt['source_excerpts_sha256']=sha(EV/'source-excerpts.txt')
    run('gpu_after', ['nvidia-smi', '--query-gpu=name,driver_version,pci.bus_id', '--format=csv'])
    run('gsp_after', ['nvidia-smi', '-q', '-d', 'GSP_FIRMWARE_VERSION'])
    run('service_after', ['curl', '--fail', '--silent', '--show-error', '--max-time', '10', 'http://127.0.0.1:18091/v1/models'])
    receipt['firmware_after'] = {'path': str(FW), 'bytes': FW.stat().st_size, 'sha256': sha(FW)}
    assert receipt['firmware_before'] == receipt['firmware_after']
    receipt['result'] = 'INSPECTION_PASS; CUSTOM_FIRMWARE_NOT_BUILT_OR_BOOTED; CUTOVER_BLOCKED'
finally:
    receipt['utc_end']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    (EV/'probe.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(receipt['result'])
