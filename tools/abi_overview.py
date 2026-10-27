#!/usr/bin/env python3
"""Generate doc/unit-abi-overview.pdf, an overview of the unit ABI.

Structure layouts (offsets, sizes) are taken from the ARM compiler, not written down by hand,
so the document stays correct when unit_abi.h changes. Needs reportlab (pip install reportlab).

    python3 ccs/units/tools/abi_overview.py [output.pdf]
"""
import collections
import datetime
import os
import re
import subprocess
import sys
import tempfile

from reportlab.graphics.shapes import Drawing, Line, Polygon, Rect, String
from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import mm
from reportlab.platypus import (BaseDocTemplate, CondPageBreak, Frame, KeepTogether, PageTemplate,
                                Paragraph, Preformatted, Spacer, Table, TableStyle)

UNITS_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
HEADER = os.path.join(UNITS_DIR, 'include', 'unit_abi.h')
CC = os.environ.get('CC', '/Applications/ARM/bin/arm-none-eabi-gcc')
NM = CC.replace('gcc', 'nm')

# ---------------------------------------------------------------------------------------------
# Layout from the compiler
# ---------------------------------------------------------------------------------------------

STRUCTS = {
    'UnitMidiPacket': ['header', 'midi0', 'midi1', 'midi2'],
    'UnitParameterChange': ['unitIndex', 'parameterId', 'midiValue', 'value'],
    'UnitTimeInfo': ['microSeconds', 'sampleRate', 'isPlaying', 'barStartPos', 'ppqPos', 'tempo', 'timeSigNumerator'],
    'UnitProcessHost': ['play', 'stop', 'requestTempo', 'sendMIDIEvent', 'timeInfo', 'hostData', 'midiIn', 'midiInCount'],
    'UnitPlugin': ['vendorId', 'getName', 'createInstance', 'getNumInputs', 'getNumOutputs', 'getNumParameters',
                   'getParameterId', 'getParameterName', 'process', 'releaseInstance'],
    'UnitHostApi': ['abiVersion', 'size', 'log', 'allocate', 'fileSize', 'readFile', 'readDirectory', 'processHostSize'],
    'UnitDirectoryEntry': ['name', 'size', 'isDirectory'],
    'UnitImageHeader': ['magic', 'abiVersion', 'name'],
    'UnitFileHeader': ['magic', 'abiVersion', 'headerSize', 'imageSize', 'bssSize', 'relocCount', 'entryOffset',
                       'crc32', 'reserved', 'name'],
}


def compiler_layout():
    src = ['#include <unit_abi.h>']
    for t, fields in STRUCTS.items():
        src.append(f'char size__{t}[sizeof({t}) + 1];')
        for f in fields:
            src.append(f'char off__{t}__{f}[offsetof({t}, {f}) + 1];')
            src.append(f'char fsize__{t}__{f}[sizeof((({t} *)0)->{f}) + 1];')
    with tempfile.TemporaryDirectory() as d:
        c, o = os.path.join(d, 'layout.c'), os.path.join(d, 'layout.o')
        open(c, 'w').write('\n'.join(src) + '\n')
        subprocess.run([CC, '-mcpu=cortex-a8', '-mfpu=neon', '-mfloat-abi=hard', '-marm', '-fno-common',
                        '-I', os.path.dirname(HEADER), '-c', c, '-o', o], check=True)
        nm = subprocess.run([NM, '-S', o], check=True, capture_output=True, text=True).stdout
    size, off, fsize = {}, collections.defaultdict(dict), collections.defaultdict(dict)
    for line in nm.splitlines():
        _, s, _, name = line.split()
        v = int(s, 16) - 1
        kind, rest = name.split('__', 1)
        if kind == 'size':
            size[rest] = v
        else:
            t, f = rest.split('__')
            (off if kind == 'off' else fsize)[t][f] = v
    return {t: dict(size=size[t], fields={f: (off[t][f], fsize[t][f]) for f in STRUCTS[t]}) for t in STRUCTS}


def header_define(name):
    m = re.search(rf'#define\s+{name}\s+(\S+)', open(HEADER).read())
    return m.group(1)

# ---------------------------------------------------------------------------------------------
# Styling
# ---------------------------------------------------------------------------------------------

INK = colors.HexColor('#1d1d1b')
MUTED = colors.HexColor('#6b6b66')
LINE = colors.HexColor('#d9d8d2')
ACCENT = colors.HexColor('#2f5fb3')
ACCENT_SOFT = colors.HexColor('#e7eefa')
WARN_SOFT = colors.HexColor('#fdf3e1')
WARN = colors.HexColor('#a86a00')
CODE_BG = colors.HexColor('#f4f4f1')
ZEBRA = colors.HexColor('#fafaf8')

styles = {
    'title': ParagraphStyle('title', fontName='Helvetica-Bold', fontSize=26, leading=30, textColor=INK, spaceAfter=4),
    'subtitle': ParagraphStyle('subtitle', fontName='Helvetica', fontSize=12, leading=16, textColor=MUTED, spaceAfter=14),
    'h1': ParagraphStyle('h1', fontName='Helvetica-Bold', fontSize=15, leading=19, textColor=INK, spaceBefore=14, spaceAfter=6),
    'h2': ParagraphStyle('h2', fontName='Helvetica-Bold', fontSize=11, leading=14, textColor=ACCENT, spaceBefore=10, spaceAfter=4),
    'body': ParagraphStyle('body', fontName='Helvetica', fontSize=9.2, leading=13, textColor=INK, spaceAfter=5, alignment=TA_LEFT),
    'small': ParagraphStyle('small', fontName='Helvetica', fontSize=8, leading=10.5, textColor=MUTED),
    'cell': ParagraphStyle('cell', fontName='Helvetica', fontSize=8.2, leading=10.5, textColor=INK),
    'cellmono': ParagraphStyle('cellmono', fontName='Courier', fontSize=8, leading=10.5, textColor=INK),
    'cellhead': ParagraphStyle('cellhead', fontName='Helvetica-Bold', fontSize=7.6, leading=10, textColor=MUTED),
    'tilelabel': ParagraphStyle('tilelabel', fontName='Helvetica', fontSize=7.5, leading=9.5, textColor=MUTED),
    'tilevalue': ParagraphStyle('tilevalue', fontName='Helvetica-Bold', fontSize=11, leading=13.5, textColor=INK),
    'code': ParagraphStyle('code', fontName='Courier', fontSize=7.8, leading=10, textColor=INK),
    'note': ParagraphStyle('note', fontName='Helvetica', fontSize=8.6, leading=12, textColor=INK),
}


def P(text, style='body'):
    return Paragraph(text, styles[style])


def code(text):
    return f'<font face="Courier" size="8.4">{text}</font>'


def table(rows, widths, header=True, mono_cols=(), zebra=True):
    data = []
    for r, row in enumerate(rows):
        cells = []
        for c, v in enumerate(row):
            if isinstance(v, str):
                st = 'cellhead' if header and r == 0 else ('cellmono' if c in mono_cols else 'cell')
                v = P(v, st)
            cells.append(v)
        data.append(cells)
    t = Table(data, colWidths=widths, repeatRows=1 if header else 0)
    st = [
        ('VALIGN', (0, 0), (-1, -1), 'TOP'),
        ('TOPPADDING', (0, 0), (-1, -1), 3.2),
        ('BOTTOMPADDING', (0, 0), (-1, -1), 3.2),
        ('LEFTPADDING', (0, 0), (-1, -1), 5),
        ('RIGHTPADDING', (0, 0), (-1, -1), 5),
        ('LINEBELOW', (0, 0), (-1, -1), 0.4, LINE),
    ]
    if header:
        st += [('LINEBELOW', (0, 0), (-1, 0), 0.9, INK)]
    if zebra:
        for r in range(1 if header else 0, len(rows)):
            if r % 2 == 0:
                st.append(('BACKGROUND', (0, r), (-1, r), ZEBRA))
    t.setStyle(TableStyle(st))
    return t


def callout(text, kind='info'):
    bg, bar = (ACCENT_SOFT, ACCENT) if kind == 'info' else (WARN_SOFT, WARN)
    t = Table([[P(text, 'note')]], colWidths=[170 * mm])
    t.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, -1), bg),
        ('LINEBEFORE', (0, 0), (0, -1), 2.5, bar),
        ('LEFTPADDING', (0, 0), (-1, -1), 9), ('RIGHTPADDING', (0, 0), (-1, -1), 9),
        ('TOPPADDING', (0, 0), (-1, -1), 6), ('BOTTOMPADDING', (0, 0), (-1, -1), 6),
    ]))
    return t


def code_block(text):
    t = Table([[Preformatted(text, styles['code'])]], colWidths=[170 * mm])
    t.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, -1), CODE_BG),
        ('LEFTPADDING', (0, 0), (-1, -1), 9), ('RIGHTPADDING', (0, 0), (-1, -1), 9),
        ('TOPPADDING', (0, 0), (-1, -1), 7), ('BOTTOMPADDING', (0, 0), (-1, -1), 7),
    ]))
    return t

# ---------------------------------------------------------------------------------------------
# Diagrams
# ---------------------------------------------------------------------------------------------

def arrow(d, x1, y, x2, color=MUTED):
    d.add(Line(x1, y, x2 - 4, y, strokeColor=color, strokeWidth=1))
    d.add(Polygon([x2, y, x2 - 5, y + 2.6, x2 - 5, y - 2.6], fillColor=color, strokeColor=color))


def box(d, x, y, w, h, title, sub, fill=colors.white, stroke=LINE, title_color=INK):
    d.add(Rect(x, y, w, h, rx=4, ry=4, fillColor=fill, strokeColor=stroke, strokeWidth=0.8))
    d.add(String(x + w / 2, y + h - 14, title, fontName='Helvetica-Bold', fontSize=8.3, fillColor=title_color, textAnchor='middle'))
    for i, line in enumerate(sub):
        d.add(String(x + w / 2, y + h - 26 - i * 9.5, line, fontName='Helvetica', fontSize=6.9, fillColor=MUTED, textAnchor='middle'))


def pipeline_diagram():
    W, H = 170 * mm, 68 * mm
    d = Drawing(W, H)
    # lane labels
    d.add(String(0, H - 9, 'ON THE LAPTOP', fontName='Helvetica-Bold', fontSize=7, fillColor=MUTED))
    d.add(String(0, 64, 'ON THE DEVICE', fontName='Helvetica-Bold', fontSize=7, fillColor=MUTED))
    d.add(Line(0, 88, W, 88, strokeColor=LINE, strokeWidth=0.6, strokeDashArray=[2, 2]))
    bw, bh, gap = 86, 54, 22
    y1 = H - 72
    xs = [i * (bw + gap) for i in range(5)]
    laptop = [
        ('unit.c', ['#include <unit_abi.h>', 'UNIT_DECLARE("Name")', 'unit_entry()']),
        ('arm-none-eabi-gcc', ['-fpie -ffreestanding', 'cortex-a8, hard float', 'ARM mode']),
        ('ld + unit.ld', ['-pie, linked at 0', '+ unitrt.c, libgcc', 'header section first']),
        ('mkunit.py', ['flatten image', 'collect R_ARM_RELATIVE', 'reject anything else']),
        ('name.unt', ['64-byte header', 'image + relocations', 'CRC-32'], ACCENT_SOFT, ACCENT, ACCENT),
    ]
    for i, b in enumerate(laptop):
        box(d, xs[i], y1, bw, bh, b[0], b[1], *(b[2:] if len(b) > 2 else ()))
        if i:
            arrow(d, xs[i - 1] + bw + 2, y1 + bh / 2, xs[i] - 2)
    y2 = 6
    device = [
        ('web UI upload', ['unitput + base64 lines', 'or copy to the SD card']),
        ('/units/Name.unt', ['saved next to', 'config.txt; loaded', 'at every boot']),
        ('unitloader.c', ['validate, copy to pool', 'relocate, clean caches', 'call unit_entry(api)']),
        ('plugin table', ['UnitPlugin registered', 'like built-in plugins', '[units] creates it']),
        ('audio interrupt', ['process() once per', 'block, with params', 'and MIDI out']),
    ]
    for i, b in enumerate(device):
        box(d, xs[i], y2, bw, bh, b[0], b[1])
        if i:
            arrow(d, xs[i - 1] + bw + 2, y2 + bh / 2, xs[i] - 2)
    # .unt -> upload (down and back to the left)
    x_from = xs[4] + bw / 2
    d.add(Line(x_from, y1 - 2, x_from, 80, strokeColor=ACCENT, strokeWidth=1))
    x_to = xs[0] + bw - 14                      # right of the lane label
    d.add(Line(x_from, 80, x_to, 80, strokeColor=ACCENT, strokeWidth=1))
    d.add(Line(x_to, 80, x_to, y2 + bh + 5, strokeColor=ACCENT, strokeWidth=1))
    d.add(Polygon([x_to, y2 + bh + 1, x_to - 2.6, y2 + bh + 6, x_to + 2.6, y2 + bh + 6], fillColor=ACCENT, strokeColor=ACCENT))
    return d


def file_map_diagram(layout):
    W, H = 170 * mm, 34 * mm
    d = Drawing(W, H)
    parts = [
        ('UnitFileHeader', '64 bytes', 0.16, ACCENT_SOFT),
        ('UnitImageHeader', '40 bytes', 0.16, colors.HexColor('#eef3e6')),
        ('image: code, rodata, data, GOT', 'imageSize bytes, linked at address 0', 0.46, colors.HexColor('#f4f4f1')),
        ('relocations', 'relocCount x uint32', 0.22, WARN_SOFT),
    ]
    x, y, h = 0, 12, 34
    for name, sub, frac, fill in parts:
        w = W * frac
        d.add(Rect(x, y, w, h, fillColor=fill, strokeColor=INK, strokeWidth=0.6))
        d.add(String(x + 5, y + h - 13, name, fontName='Helvetica-Bold', fontSize=7.8, fillColor=INK))
        d.add(String(x + 5, y + h - 24, sub, fontName='Helvetica', fontSize=6.9, fillColor=MUTED))
        x += w
    d.add(String(0, 2, 'file offset 0', fontName='Helvetica', fontSize=6.5, fillColor=MUTED))
    d.add(String(W * 0.16, 2, '64', fontName='Helvetica', fontSize=6.5, fillColor=MUTED))
    d.add(String(W * 0.78, 2, '64 + imageSize', fontName='Helvetica', fontSize=6.5, fillColor=MUTED))
    d.add(String(W * 0.16 + 2, y + h + 6, 'CRC-32 covers everything from offset 64 to the end', fontName='Helvetica-Oblique', fontSize=6.8, fillColor=ACCENT))
    d.add(Line(W * 0.16, y + h + 3, W, y + h + 3, strokeColor=ACCENT, strokeWidth=0.8))
    return d

# ---------------------------------------------------------------------------------------------
# Content
# ---------------------------------------------------------------------------------------------

FIELD_INFO = {
    'UnitPlugin': {
        'vendorId': ('uint32_t', 'free for the unit; not used by the firmware'),
        'getName': ('void (*)(char *name, size_t max)', 'type name used in [units]; must equal the name in UNIT_DECLARE'),
        'createInstance': ('void *(*)(void)', 'one per [units] line; NULL if none left. Instances are never destroyed'),
        'getNumInputs': ('uint8_t (*)(void)', 'audio inputs unit:1..N'),
        'getNumOutputs': ('uint8_t (*)(void)', 'audio outputs unit:1..N'),
        'getNumParameters': ('uint8_t (*)(void)', 'parameters reachable from [midimap]'),
        'getParameterId': ('uint16_t (*)(size_t index)', 'stable id, <b>must be non-zero</b>; arrives in UnitParameterChange'),
        'getParameterName': ('void (*)(size_t, char *, size_t)', 'name used in [midimap] (unit:name)'),
        'process': ('void (*)(...)', 'once per audio block, in the audio interrupt (see below)'),
    },
    'UnitProcessHost': {
        'play': ('void (*)(void)', 'start the transport'),
        'stop': ('void (*)(void)', 'stop the transport'),
        'requestTempo': ('void (*)(float bpm)', 'change the tempo'),
        'sendMIDIEvent': ('void (*)(hostData, UnitMidiPacket)', 'queue MIDI on the unit\'s output ([midiroutes]); max 16 per block, all units together'),
        'timeInfo': ('UnitTimeInfo', 'timing for this block'),
        'hostData': ('struct UnitHostData *', 'opaque; pass to sendMIDIEvent'),
        'midiIn': ('const UnitMidiPacket *', 'MIDI routed to the unit by [midiroutes] since the last block; check UNIT_PROCESS_HOST_HAS'),
        'midiInCount': ('size_t', 'number of packets in midiIn'),
    },
    'UnitTimeInfo': {
        'microSeconds': ('uint64_t', 'device clock at the start of the block'),
        'sampleRate': ('float', 'Hz (48000)'),
        'isPlaying': ('int', 'transport running (Ableton Link / MIDI clock timeline)'),
        'barStartPos': ('double', 'start of the current bar, in quarter notes'),
        'ppqPos': ('double', 'position in quarter notes'),
        'tempo': ('float', 'BPM'),
        'timeSigNumerator': ('int', 'quantum (beats per bar)'),
    },
    'UnitParameterChange': {
        'unitIndex': ('uint8_t', 'index of the receiving unit'),
        'parameterId': ('uint16_t', 'as returned by getParameterId()'),
        'midiValue': ('uint8_t', 'raw 0..127 (CC value or note velocity)'),
        'value': ('float', 'midiValue / 127'),
    },
    'UnitMidiPacket': {
        'header': ('uint8_t', '(cable << 4) | code index number; cable 0 = default jack'),
        'midi0': ('uint8_t', 'status byte (e.g. 0x90 note on, ch 1)'),
        'midi1': ('uint8_t', 'data 1'),
        'midi2': ('uint8_t', 'data 2'),
    },
    'UnitHostApi': {
        'abiVersion': ('uint32_t', 'UNIT_ABI_VERSION of the firmware', 'first'),
        'size': ('uint32_t', 'sizeof(UnitHostApi) in the firmware', 'first'),
        'log': ('void (*)(const char *fmt, ...)', 'queued console log; safe from process()', 'first'),
        'allocate': ('void *(*)(size_t)', 'memory for the unit\'s life, 8-byte aligned, not zeroed; NULL when the 64 MB heap is full', 'added'),
        'fileSize': ('int32_t (*)(const char *path)', 'size of a file, &lt; 0 if missing or a directory', 'added'),
        'readFile': ('int32_t (*)(path, offset, buf, len)', 'bytes read, &lt; 0 on error', 'added'),
        'readDirectory': ('int32_t (*)(path, index, entry *)', '1 = entry filled, 0 = no more, &lt; 0 = error', 'added'),
        'processHostSize': ('uint32_t', 'sizeof(UnitProcessHost) in the firmware (see UNIT_PROCESS_HOST_HAS)', 'added'),
    },
    'UnitDirectoryEntry': {
        'name': ('char[64]', 'file or folder name (longer names are skipped)'),
        'size': ('uint32_t', 'file size in bytes'),
        'isDirectory': ('uint8_t', 'non-zero for folders'),
    },
    'UnitFileHeader': {
        'magic': ('uint32_t', '"UNT1" (0x31544E55)'),
        'abiVersion': ('uint16_t', 'must equal the firmware\'s UNIT_ABI_VERSION'),
        'headerSize': ('uint16_t', '64'),
        'imageSize': ('uint32_t', 'bytes of image that follow, a multiple of 4'),
        'bssSize': ('uint32_t', 'zeroed bytes the loader adds after the image'),
        'relocCount': ('uint32_t', 'uint32 offsets after the image'),
        'entryOffset': ('uint32_t', 'offset of unit_entry in the image'),
        'crc32': ('uint32_t', 'IEEE CRC-32 (zlib.crc32) of everything after this header'),
        'reserved': ('uint32_t', '0'),
        'name': ('char[32]', 'type name, NUL-terminated, [A-Za-z0-9_]{1,31}'),
    },
    'UnitImageHeader': {
        'magic': ('uint32_t', '"UNIT" (0x54494E55)'),
        'abiVersion': ('uint32_t', 'UNIT_ABI_VERSION the unit was built with'),
        'name': ('char[32]', 'type name; written by UNIT_DECLARE("Name")'),
    },
}


EXAMPLE_UNIT = """#include <unit_abi.h>

UNIT_DECLARE("Gain");                                // type name used in [units]

static float gains[4];
static int used;

static void copy(char * d, const char * s, size_t max) {
    size_t i = 0;
    for (; s[i] && i + 1 < max; i++) d[i] = s[i];
    if (max) d[i] = 0;
}

static void getName(char * n, size_t max) { copy(n, __unit_header.name, max); }
static void * createInstance(void) { if (used == 4) return 0; gains[used] = 1.0F; return &gains[used++]; }
static uint8_t numIn(void) { return 1; }
static uint8_t numOut(void) { return 1; }
static uint8_t numParams(void) { return 1; }
static uint16_t paramId(size_t i) { return 1; }                      // never 0
static void paramName(size_t i, char * n, size_t max) { copy(n, "gain", max); }

static void process(void * inst, UnitProcessHost * host, size_t nIn, float ** in,
                    size_t nOut, float ** out, size_t frames,
                    UnitParameterChange * changes, size_t nChanges) {
    float * gain = inst;
    for (size_t c = 0; c < nChanges; c++) *gain = changes[c].midiValue / 127.0F;
    for (size_t i = 0; i < frames; i++) out[0][i] = in[0][i] * *gain;
}

static const UnitPlugin plugin = { .getName = getName, .createInstance = createInstance,
    .getNumInputs = numIn, .getNumOutputs = numOut, .getNumParameters = numParams,
    .getParameterId = paramId, .getParameterName = paramName, .process = process };

const UnitPlugin * unit_entry(const UnitHostApi * api) {
    api->log("Gain unit ready");
    return &plugin;
}
"""


def struct_table(layout, name, extra_col=None):
    info = FIELD_INFO[name]
    head = ['Offset', 'Size', 'Field', 'Type', 'Meaning']
    widths = [13 * mm, 11 * mm, 34 * mm, 44 * mm, 68 * mm]
    if extra_col:
        head.insert(0, extra_col)
        widths = [11 * mm, 12 * mm, 10 * mm, 30 * mm, 44 * mm, 63 * mm]
    rows = [head]
    for f, (off, size) in sorted(layout[name]['fields'].items(), key=lambda kv: kv[1][0]):
        t = info[f]
        row = [str(off), str(size), f, t[0], t[1]]
        if extra_col:
            row.insert(0, 'v1' if t[2] == 'first' else 'v1+')
        rows.append(row)
    mono = (2, 3) if not extra_col else (3, 4)
    return table(rows, widths, mono_cols=mono)


def struct_section(layout, name, intro, extra_col=None):
    return KeepTogether([
        P(f'{name} <font color="#6b6b66" size="9">  {layout[name]["size"]} bytes</font>', 'h2'),
        P(intro),
        struct_table(layout, name, extra_col),
        Spacer(1, 4),
    ])


def build(out):
    layout = compiler_layout()
    abi = header_define('UNIT_ABI_VERSION')
    today = datetime.date.today().strftime('%-d %B %Y')
    story = []

    # --- Cover ---------------------------------------------------------------------------
    story += [
        Spacer(1, 4 * mm),
        P('Metalhead unit ABI', 'title'),
        P(f'Version {abi} &nbsp;·&nbsp; loadable units for the metalhead firmware &nbsp;·&nbsp; {today}', 'subtitle'),
        P('A <b>unit</b> is an audio/MIDI plugin built on the laptop, separately from the firmware, and loaded by the '
          'running device. It is uploaded from the web UI (or copied to the SD card), stored as '
          f'{code("/units/&lt;Name&gt;.unt")} next to {code("config.txt")}, loaded at every boot and then used in '
          f'{code("[units]")} like a built-in plugin. This document summarises the binary interface between the two: '
          f'what a unit file contains, how it is loaded, which functions run where, and the exact layout of every '
          f'structure. The source of truth is {code("ccs/units/include/unit_abi.h")}; offsets and sizes below come '
          'from the ARM compiler.'),
        Spacer(1, 3 * mm),
    ]
    tiles = [
        ('ABI version', f'{abi}', 'exact match required'),
        ('Target', 'Cortex-A8, ARM mode', 'AAPCS, hard float, NEON'),
        ('Code', 'Position independent', '-fpie, freestanding, libgcc only'),
        ('Relocations', 'R_ARM_RELATIVE only', 'base address added per word'),
        ('Host services', f'{len(STRUCTS["UnitHostApi"]) - 2} functions', 'log, allocate, 3 file reads'),
        ('Plugin interface', f'{layout["UnitPlugin"]["size"]}-byte vtable', 'identical to the firmware\'s'),
    ]
    cells = [[Table([[P(label.upper(), 'tilelabel')], [P(value, 'tilevalue')], [P(sub, 'small')]],
                    colWidths=[54 * mm], style=[('LEFTPADDING', (0, 0), (-1, -1), 0), ('TOPPADDING', (0, 0), (-1, -1), 1),
                                                ('BOTTOMPADDING', (0, 0), (-1, -1), 1)])
              for label, value, sub in tiles[i:i + 3]] for i in (0, 3)]
    grid = Table(cells, colWidths=[57 * mm] * 3)
    grid.setStyle(TableStyle([
        ('BOX', (0, 0), (-1, -1), 0.6, LINE), ('INNERGRID', (0, 0), (-1, -1), 0.6, LINE),
        ('LEFTPADDING', (0, 0), (-1, -1), 7), ('TOPPADDING', (0, 0), (-1, -1), 6), ('BOTTOMPADDING', (0, 0), (-1, -1), 6),
    ]))
    story += [grid, Spacer(1, 6 * mm)]

    story += [P('From source to sound', 'h1'), pipeline_diagram(), Spacer(1, 2 * mm),
              P('There is no dynamic linker. A unit cannot call firmware or C library functions by name: everything it '
                'needs from the host comes through two tables of function pointers, <b>UnitHostApi</b> (passed once to '
                'unit_entry) and <b>UnitProcessHost</b> (passed to every process call). mkunit.py fails the build if the '
                'unit references anything outside its own image, so a missing symbol never reaches the device.')]

    # --- Lifecycle ---------------------------------------------------------------------------
    story += [CondPageBreak(90 * mm), P('Lifecycle and calling contexts', 'h1'),
              P('The loader validates the file, copies the image into a 2 MB code pool in DDR, zeroes its bss, adds the '
                'load address to every relocated word, cleans the D-cache, invalidates the I-cache and branch predictor, '
                'calls unit_entry and registers the returned plugin. Units are never unloaded.')]
    story.append(table([
        ['Call', 'When', 'Runs in', 'Allowed'],
        ['unit_entry(api)', 'once, at boot (before config.txt) or right after an upload', 'main loop',
         'everything: log, allocate, file access, slow set-up work (e.g. loading samples)'],
        ['createInstance()', 'for each [units] line when config.txt is loaded', 'main loop',
         'log, allocate, file access; return NULL when out of instances'],
        ['getName, getNum*, getParameter*', 'while loading the unit and resolving config.txt', 'main loop',
         'must be pure and return the same answers every time'],
        ['process(...)', 'once per audio block (one USB frame of samples)', '<b>audio interrupt</b>',
         'log and sendMIDIEvent only. No file access (refused), no allocate, no blocking or waiting'],
    ], [38 * mm, 48 * mm, 22 * mm, 62 * mm], mono_cols=(0,)))
    story += [Spacer(1, 3 * mm), P('process() signature', 'h2'), code_block(
        'void process(void * instance, UnitProcessHost * host,\n'
        '             size_t nrInChannels,  float ** inSamples,    // unconnected inputs contain silence\n'
        '             size_t nrOutChannels, float ** outSamples,   // outputs start zeroed; add into them\n'
        '             size_t frameCount,                           // samples per channel in this block\n'
        '             UnitParameterChange * parameterChanges, size_t parameterChangeCount);')]
    story += [Spacer(1, 3 * mm), callout(
        '<b>Things that bite.</b> Parameter ids must be non-zero (0 means "no such parameter" to the firmware). '
        f'UnitParameterChange is {layout["UnitParameterChange"]["size"]} bytes, not 8: it is a packed union of an '
        'unpacked struct, so <i>value</i> sits at offset 8. At most 16 MIDI events are delivered per block for all units '
        'together. Floating point and NEON are fine in process(): the IRQ handler saves the full VFP/NEON state.',
        'warn')]

    # --- Structures ---------------------------------------------------------------------------
    story += [CondPageBreak(120 * mm), P('Structures', 'h1'),
              P('All structures are little endian with natural ARM EABI alignment unless noted. The firmware '
                f'static-asserts that the size and every field offset match its own plugin structures '
                f'({code("unitloader.c")}), so a unit\'s UnitPlugin goes straight into the plugin table.')]
    story.append(struct_section(layout, 'UnitPlugin', 'Returned by unit_entry. Every function pointer must point into the unit\'s own image (the loader checks).'))
    story.append(struct_section(layout, 'UnitProcessHost', 'Passed to every process() call; only valid during that call.'))
    story.append(struct_section(layout, 'UnitTimeInfo', 'Embedded in UnitProcessHost at offset %d.' % layout['UnitProcessHost']['fields']['timeInfo'][0]))
    story.append(struct_section(layout, 'UnitParameterChange', 'Produced from [midimap]: a CC or note on a device mapped to unit:parameter.'))
    story.append(struct_section(layout, 'UnitMidiPacket', 'A USB-MIDI event packet. SysEx is sent as CIN 0x4 packets followed by a 0x5-0x7 end packet.'))
    story.append(struct_section(layout, 'UnitHostApi',
                                'Passed to unit_entry; stays valid and may be kept. Fields marked v1+ were appended to ABI 1 after its first '
                                'release: check <font face="Courier">UNIT_HOST_HAS(api, field)</font> before calling them, so the unit still '
                                'loads on older firmware.', extra_col='Since'))
    story.append(struct_section(layout, 'UnitDirectoryEntry', 'Filled by readDirectory. Entries are returned in on-disk order; sort if order matters.'))

    # --- File format ---------------------------------------------------------------------------
    story += [CondPageBreak(110 * mm), P('The .unt file', 'h1'), file_map_diagram(layout), Spacer(1, 2 * mm),
              P('The image is linked at address 0 and starts with the UnitImageHeader that UNIT_DECLARE places in the '
                '.unit_header section. After loading at address <i>base</i>, the loader adds <i>base</i> to the 32-bit word at '
                'each relocation offset. The firmware allocates imageSize + bssSize bytes, 64-byte aligned.')]
    story.append(struct_section(layout, 'UnitFileHeader', 'Written by mkunit.py.'))
    story.append(struct_section(layout, 'UnitImageHeader', 'First bytes of the image; checked against the file header.'))

    # --- Loader, versioning, limits ------------------------------------------------------------
    checks = [
        'magic "UNT1", headerSize 64 and an exact ABI version match',
        'sizes consistent with the file length; image a multiple of 4 bytes',
        'CRC-32 over image and relocations',
        'entry point and every relocation inside the image, 4-byte aligned',
        'image header magic, ABI version and name match the file header',
        'after unit_entry: the plugin and all its function pointers lie inside the unit\'s image',
        'getName() returns the name in the file; no built-in plugin or loaded unit has that name',
    ]
    story.append(KeepTogether([P('What the loader checks', 'h1'),
                               table([[f'{i + 1}', c] for i, c in enumerate(checks)], [8 * mm, 162 * mm], header=False)]))
    story += [Spacer(1, 3 * mm), KeepTogether([P('Versioning rules', 'h1'), table([
        ['Change', 'Version', 'Effect on existing units'],
        ['Append a service to the end of UnitHostApi', f'stays {abi}', 'none; new units check UNIT_HOST_HAS before using it'],
        ['Change, reorder or remove any field; change a calling convention or a meaning', 'bump UNIT_ABI_VERSION', 'refused by the loader and the web UI until rebuilt'],
        ['Change the firmware\'s plugin structures (plugins.h)', 'n/a', 'the firmware stops compiling (static asserts) until the ABI is updated'],
    ], [78 * mm, 32 * mm, 60 * mm])])]
    story += [Spacer(1, 3 * mm), KeepTogether([P('Limits', 'h1'), table([
        ['Resource', 'Limit'],
        ['Unit code and static data', '2 MB pool for all units; 256 KB per .unt file'],
        ['Heap (allocate)', '64 MB shared by all units (0x8C000000 to 0x90000000), never freed'],
        ['Units / plugins', '16 loaded units; 32 plugins including the built-in ones'],
        ['MIDI out', '16 events per audio block, all units together'],
        ['Files', 'read only, and only from unit_entry / createInstance'],
        ['Replacing a unit', 'saved immediately; the new version loads at the next reboot'],
    ], [50 * mm, 120 * mm])])]

    # --- Writing a unit --------------------------------------------------------------------------
    story += [CondPageBreak(120 * mm), P('A minimal unit', 'h1'), code_block(EXAMPLE_UNIT), Spacer(1, 2 * mm),
        P(f'Build with a three-line Makefile ({code("NAME := gain")}, {code("SRCS := gain.c")}, '
          f'{code("include ../unit.mk")}) to get {code("build/gain.unt")}. Then use it in config.txt:'),
        code_block('[units]\namp Gain\n[audioroutes]\n.Scarlett_2i2_USB:1 amp:1\namp:1 .device:1\n[midimap]\n.nanoKONTROL2:cc0 amp:gain'),
        Spacer(1, 2 * mm),
        P('Units in the repository: <b>vumeter</b> (level meters on a Launchpad Mini MK3, MIDI out) and <b>drums</b> '
          '(sampler that loads WAV files from /samples/drums with readDirectory/readFile/allocate).')]

    # --- Upload protocol ---------------------------------------------------------------------------
    story += [KeepTogether([P('Upload protocol', 'h1'),
              P('The web UI talks to the device console over the USB CDC serial port (or the debug UART). '
                'Machine-readable replies start with @; the console prints "&gt; " after each command, which the UI '
                'uses as the acknowledgement for every line.'),
              table([
                  ['Command', 'Reply'],
                  ['unitls', '@unitabi &lt;version&gt;, then @unit &lt;name&gt; &lt;bytes&gt; &lt;loaded|reboot|failed|new&gt; per file in /units'],
                  ['unitput &lt;bytes&gt;', '@unitput ready; then base64 lines (768 characters, a multiple of 4) until unitend'],
                  ['unitend', '@unitput ok &lt;name&gt; &lt;loaded|unchanged|reboot&gt;  or  @unitput error &lt;reason&gt;'],
                  ['unitrm &lt;name&gt;', '@unitrm ok (the unit stays loaded until the next reboot)'],
                  ['cfgabort', 'leaves unitput (or cfgput) mode without saving'],
              ], [34 * mm, 136 * mm], mono_cols=(0, 1))])]

    # --- Page template ---------------------------------------------------------------------------
    def on_page(canvas, doc):
        canvas.saveState()
        canvas.setStrokeColor(LINE)
        canvas.setLineWidth(0.5)
        canvas.line(20 * mm, 14 * mm, A4[0] - 20 * mm, 14 * mm)
        canvas.setFont('Helvetica', 7.5)
        canvas.setFillColor(MUTED)
        canvas.drawString(20 * mm, 9.5 * mm, f'Metalhead unit ABI {abi}  ·  ccs/units/include/unit_abi.h')
        canvas.drawRightString(A4[0] - 20 * mm, 9.5 * mm, f'{doc.page}')
        canvas.setFillColor(ACCENT)
        canvas.rect(0, A4[1] - 4, A4[0], 4, stroke=0, fill=1)
        canvas.restoreState()

    doc = BaseDocTemplate(out, pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm, topMargin=16 * mm, bottomMargin=20 * mm,
                          title=f'Metalhead unit ABI {abi}', author='metalhead', subject='Binary interface for loadable units')
    frame = Frame(doc.leftMargin, doc.bottomMargin, doc.width, doc.height, id='body')
    doc.addPageTemplates([PageTemplate(id='page', frames=[frame], onPage=on_page)])
    doc.build(story)


if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(UNITS_DIR, 'doc', 'unit-abi-overview.pdf')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    build(out)
    print(out)
