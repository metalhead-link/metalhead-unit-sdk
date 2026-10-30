#!/usr/bin/env python3
"""Rebuild every unit and upload them to the device over its USB serial port (the console's
unitput, as the web UI's Units tab does; see ../README.md).

Usage: python3 ccs/units/tools/upload_units.py [--bbb] [--build-only] [--reboot] [unit ...]

Every unit directory is built (`make aarch64`, or `make arm32` with --bbb). Then the units
named on the command line are uploaded, by bundle or directory name (Stock, mutable, ...).
Without names, Stock and Mutable are uploaded, and the unit files already on the device again.
Not every unit: Stock holds the same plugins as VUMeter, Drums, YAFR and ClipGrid, and a plugin
can only be loaded once. So Stock is left out when the device has one of those files.

A unit that was loaded already is only replaced at the next boot: --reboot reboots the device
when that's needed. The web UI must not have the port open at the same time.
"""
import argparse
import base64
import os
import re
import subprocess
import sys

UNITS = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.join(UNITS, '..', '..', 'rpi5', 'tools'))
from upload_image import LINE, answer, command, open_port     # noqa: E402

DEFAULT = ['Stock', 'Mutable']

# @unitput ok <name> <state>
MEANING = {
    'loaded': 'saved and loaded',
    'unchanged': 'saved; the same as the loaded one',
    'reboot': 'saved; replaces the loaded one at the next boot',
}


def unit_dirs():
    """{directory: (NAME, BUNDLE, {plugin entry functions})} of every directory with a unit
    Makefile; only the literal PLUGINS list (mutable/ computes its own, they're unique anyway)"""
    found = {}
    for d in sorted(os.listdir(UNITS)):
        makefile = os.path.join(UNITS, d, 'Makefile')
        if not os.path.isfile(makefile):
            continue
        text = open(makefile).read()
        name = re.search(r'^NAME\s*:=\s*(\S+)', text, re.M)
        bundle = re.search(r'^BUNDLE\s*:=\s*(\S+)', text, re.M)
        plugins = re.search(r'^PLUGINS\s*:=(.*)$', text, re.M)
        if name and bundle:
            entries = {p for p in plugins.group(1).split() if '$' not in p} if plugins else set()
            found[d] = (name.group(1), bundle.group(1), entries)
    return found


def default_units(dirs, built, on_device):
    """The units on the device and DEFAULT, without those sharing a plugin with one on the device"""
    plugins = {bundle: entries for _, bundle, entries in dirs.values()}
    wanted = [u for u in on_device if u in built]
    for u in DEFAULT:
        if u in built and u not in wanted and not any(plugins[u] & plugins.get(o, set()) for o in on_device):
            wanted.append(u)
        elif u in built and u not in wanted:
            print(f'{u}: left out, it has plugins of a unit on the device')
    return wanted


def build(dirs, machine):
    built = {}
    for d, (name, bundle, _) in dirs.items():
        if d == 'mutable' and not os.path.isdir(os.path.join(UNITS, d, 'eurorack', 'stmlib')):
            print(f'{d}: skipped, no eurorack sources (git submodule update --init --recursive)')
            continue
        print(f'{d}: make {machine}', flush=True)
        result = subprocess.run(['make', '-s', f'-j{os.cpu_count()}', '-C', os.path.join(UNITS, d), machine],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            sys.exit(f'{result.stdout}\n{d}: build failed')
        built[bundle] = os.path.join(UNITS, d, 'build', machine, f'{name}.unt')
    return built


def list_units(fd):
    """(abi, {bundle name: state}) on the device"""
    out = command(fd, 'unitls')
    abi = answer(out, '@unitabi ')
    units = {}
    for line in out.splitlines():
        m = re.match(r'@unit (\S+) \d+ (\S+)', line.strip())
        if m:
            units[m.group(1)] = m.group(2)
    return int(abi.split()[1]) if abi else None, units


def put_unit(fd, path):
    data = open(path, 'rb').read()
    ready = command(fd, f'unitput {len(data)}')
    if not answer(ready, '@unitput ready'):
        return answer(ready, '@unitput') or '@unitput error no answer to unitput'
    b64 = base64.b64encode(data).decode()
    for i in range(0, len(b64), LINE):
        if not command(fd, b64[i:i + LINE]).rstrip().endswith('>'):
            command(fd, 'cfgabort')
            return f'@unitput error the console stopped answering at byte {i // 4 * 3}'
    return answer(command(fd, 'unitend', timeout=30), '@unitput') or '@unitput error no answer to unitend'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('units', nargs='*', help='bundle or directory names (default: the ones on the device)')
    parser.add_argument('--bbb', action='store_true', help='build for the BeagleBone Black (default: Raspberry Pi 5)')
    parser.add_argument('--build-only', action='store_true', help="build, don't upload")
    parser.add_argument('--reboot', action='store_true', help='reboot the device when a replaced unit needs it')
    args = parser.parse_args()
    machine = 'arm32' if args.bbb else 'aarch64'

    dirs = unit_dirs()
    by_dir = {d: bundle for d, (_, bundle, _) in dirs.items()}
    wanted = [by_dir.get(u, u) for u in args.units]
    unknown = [u for u in wanted if u not in by_dir.values()]
    if unknown:
        sys.exit(f'unknown unit {", ".join(unknown)}; there are {", ".join(by_dir.values())}')

    built = build(dirs, machine)
    if args.build_only:
        return

    fd = open_port()
    os.read(fd, 65536)
    command(fd, 'cfgabort')
    abi, on_device = list_units(fd)
    with open(os.path.join(UNITS, 'include', 'unit_abi.h')) as f:
        ours = int(re.search(r'#define UNIT_ABI_VERSION\s+(\d+)', f.read()).group(1))
    if abi != ours:
        sys.exit(f'the firmware has unit ABI {abi}, these units are ABI {ours}')
    if not wanted:
        wanted = default_units(dirs, built, on_device)
        unbuilt = [u for u in on_device if u not in built]
        if unbuilt:
            print(f'on the device but not built here, left alone: {", ".join(unbuilt)}')

    failed = False
    states = {}
    for bundle in wanted:
        if bundle not in built:
            print(f'{bundle}: not built')
            failed = True
            continue
        print(f'{bundle}: {os.path.getsize(built[bundle])} bytes ... ', end='', flush=True)
        result = put_unit(fd, built[bundle])
        m = re.match(r'@unitput ok \S+ (\S+)', result)
        if m:
            states[bundle] = m.group(1)
            print(MEANING.get(m.group(1), m.group(1)))
        else:
            print(result.replace('@unitput ', ''))
            failed = True

    stale = [u for u, state in states.items() if state == 'reboot']
    if stale and args.reboot:
        print('rebooting')
        os.write(fd, b'reboot\r')
    elif stale:
        print(f'replaced at the next boot: {", ".join(stale)} (--reboot, or type reboot in the console)')
    os.close(fd)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
