"""Generate a tiny original SF2 test bank; no external samples or downloads."""
import math
import struct
import sys
from pathlib import Path


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + (b'\0' if len(data) % 2 else b'')


def listing(tag, *chunks):
    return chunk(b'LIST', tag + b''.join(chunks))


def name(s):
    return s.encode('ascii').ljust(20, b'\0')


# Periodic original waveform, with 46 terminal guard samples per SF2.
period = 128
length = period * 32
samples = [int(12000 * math.sin(2 * math.pi * i / period)) for i in range(length)] + [0] * 46
info = listing(b'INFO', chunk(b'ifil', struct.pack('<HH', 2, 1)),
               chunk(b'isng', b'EMU8000\0'), chunk(b'INAM', b'LSFX smoke test\0'))
sdta = listing(b'sdta', chunk(b'smpl', struct.pack('<' + 'h' * len(samples), *samples)))
phdr = b''.join(struct.pack('<20sHHHIII', name(n), p, b, i, 0, 0, 0)
                for i, (n, p, b) in enumerate([('Tone', 0, 0), ('Drum test', 0, 128), ('EOP', 0, 0)]))
pbag = b''.join(struct.pack('<HH', i, 0) for i in range(3))
pgen = struct.pack('<HHHHHH', 41, 0, 41, 0, 0, 0)
inst = struct.pack('<20sH20sH', name('Tone'), 0, name('EOI'), 1)
ibag = struct.pack('<HHHH', 0, 0, 2, 0)
igen = struct.pack('<HHHHHH', 54, 1, 53, 0, 0, 0)
shdr = struct.pack('<20sIIIIIBbHH', name('Tone'), 0, length, period, length-period, 44100, 60, 0, 0, 1)
shdr += struct.pack('<20sIIIIIBbHH', name('EOS'), 0, 0, 0, 0, 0, 0, 0, 0, 0)
pdta = listing(b'pdta', chunk(b'phdr', phdr), chunk(b'pbag', pbag), chunk(b'pmod', bytes(10)),
               chunk(b'pgen', pgen), chunk(b'inst', inst), chunk(b'ibag', ibag), chunk(b'imod', bytes(10)),
               chunk(b'igen', igen), chunk(b'shdr', shdr))
Path(sys.argv[1]).write_bytes(chunk(b'RIFF', b'sfbk' + info + sdta + pdta))
