"""Create two tiny, original SFZ instruments and their samples for CI."""
import math
import struct
import sys
import wave
from pathlib import Path
root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
for name, frequency in [('sfz-one', 440), ('sfz-two', 660)]:
    with wave.open(str(root / (name + '.wav')), 'wb') as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(44100)
        wav.writeframes(b''.join(struct.pack('<h', int(10000 * math.sin(2 * math.pi * frequency * i / 44100))) for i in range(88200)))
    (root / (name + '.sfz')).write_text(f'<control> label_cc1=Modulation\n<region> sample={name}.wav key=60 amp_veltrack=0\n', encoding='ascii')
