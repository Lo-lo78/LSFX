"""Exercise TEST14 stereo initialization and rendering with installed FluidSynth.
Run: python Tests/FluidStereoLoadTest.py /path/to/bank.sf2
Not a Windows VST3 or JUCE test.
"""
import ctypes as C
import ctypes.util
import math
import sys

P,I,F,D,S=C.c_void_p,C.c_int,C.c_float,C.c_double,C.c_char_p
lib=C.CDLL(ctypes.util.find_library('fluidsynth') or 'libfluidsynth.so.3')
def api(name, result, *args):
    fn=getattr(lib,name);fn.restype=result;fn.argtypes=args;return fn
settings=api('new_fluid_settings',P)
synth=api('new_fluid_synth',P,P)
setint=api('fluid_settings_setint',I,P,S,I)
setnum=api('fluid_settings_setnum',I,P,S,D)
setstr=api('fluid_settings_setstr',I,P,S,S)
load=api('fluid_synth_sfload',I,P,S,I)
select=api('fluid_synth_program_select',I,P,I,I,I,I)
cc=api('fluid_synth_cc',I,P,I,I,I)
note=api('fluid_synth_noteon',I,P,I,I,I)
off=api('fluid_synth_all_sounds_off',I,P,I)
write=api('fluid_synth_write_float',I,P,I,P,I,I,P,I,I)
free=api('delete_fluid_synth',None,P)
free_s=api('delete_fluid_settings',None,P)
rv_on=api('fluid_synth_reverb_on',I,P,I,I)
ch_on=api('fluid_synth_chorus_on',I,P,I,I)
fx=[(api('fluid_synth_set_reverb_group_'+name,I,P,I,D),v) for name,v in [('roomsize',.50),('damp',.30),('width',.80),('level',.70)]]
fx += [(api('fluid_synth_set_chorus_group_nr',I,P,I,I),4)]
fx += [(api('fluid_synth_set_chorus_group_'+name,I,P,I,D),v) for name,v in [('level',.55),('speed',.36),('depth',3.6)]]
for rate in (44100,48000,96000):
    st=settings(); sy=None
    try:
        for k,v in {'synth.midi-channels':16,'synth.audio-channels':1,'synth.audio-groups':1,'synth.effects-groups':1,'synth.polyphony':256,'synth.reverb.active':0,'synth.chorus.active':0,'synth.threadsafe-api':0,'synth.cpu-cores':1}.items():
            assert setint(st,k.encode(),v)==0
        assert setnum(st,b'synth.sample-rate',rate)==0
        assert setnum(st,b'synth.gain',1.)==0
        assert setstr(st,b'synth.midi-bank-select',b'gs')==0
        sy=synth(st); assert sy
        font=load(sy,sys.argv[1].encode(),1); assert font>=0
        for ch in range(16): assert select(sy,ch,font,128 if ch==9 else 0,0)==0
        for mode in (3,0,3):
            assert rv_on(sy,-1,mode&1)==0
            assert ch_on(sy,-1,int(bool(mode&2)))==0
            for fn,v in fx: assert fn(sy,-1,v)==0
            # First callbacks are silent, with odd offsets and irregular sizes.
            for n in (1,63,64,65,127,512,1024,2049):
                l,r=(F*(n+6))(),(F*(n+6))()
                l[0]=r[0]=123.;l[n+5]=r[n+5]=456.
                assert write(sy,n,C.byref(l,4),0,1,C.byref(r,4),0,1)==0
                assert l[0]==r[0]==123. and l[n+5]==r[n+5]==456.
                assert all(math.isfinite(x) for x in list(l)+list(r))
            for ch in range(16):
                assert cc(sy,ch,91,40)==0 and cc(sy,ch,93,40)==0
                assert note(sy,ch,36 if ch==9 else 60,100)==0
            peak=0.
            for n in (1,63,127,511,1024,2049,512):
                l,r=(F*n)(),(F*n)()
                assert write(sy,n,l,0,1,r,0,1)==0
                assert all(math.isfinite(x) for x in list(l)+list(r))
                peak=max(peak,max(map(abs,l)),max(map(abs,r)))
            assert peak>0
            for ch in range(16): assert off(sy,ch)==0
            print(rate, 'FX mode', mode,'peak',peak,flush=True)
    finally:
        if sy: free(sy)
        free_s(st)
print('Stereo load/render tests passed')
