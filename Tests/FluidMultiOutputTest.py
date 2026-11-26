"""Native FluidSynth regression, not a JUCE/Windows host test.
Usage: python Tests/FluidMultiOutputTest.py /path/to/GM.sf2
"""
import ctypes as C, ctypes.util, sys, math
P,I,F=C.c_void_p,C.c_int,C.c_float
FP=C.POINTER(F)
lib=C.CDLL(ctypes.util.find_library('fluidsynth') or 'libfluidsynth.so.3')
def api(name,ret,*args):
    f=getattr(lib,name); f.restype=ret; f.argtypes=args; return f
settings=api('new_fluid_settings',P); synth=api('new_fluid_synth',P,P)
si=api('fluid_settings_setint',I,P,C.c_char_p,I); sn=api('fluid_settings_setnum',I,P,C.c_char_p,C.c_double)
load=api('fluid_synth_sfload',I,P,C.c_char_p,I); select=api('fluid_synth_program_select',I,P,I,I,I,I)
cc=api('fluid_synth_cc',I,P,I,I,I); gen=api('fluid_synth_set_gen',I,P,I,I,F); getgen=api('fluid_synth_get_gen',F,P,I,I)
note=api('fluid_synth_noteon',I,P,I,I,I); process=api('fluid_synth_process',I,P,I,I,C.POINTER(FP),I,C.POINTER(FP))
write=api('fluid_synth_write_float',I,P,I,P,I,I,P,I,I)
free=api('delete_fluid_synth',None,P); fs=api('delete_fluid_settings',None,P)
# SF2 generator indices: chorus send 15, reverb send 16.
def run(routes, groups=16, native=False, effects=True):
    st=settings(); sy=None
    try:
        for k,v in {'synth.midi-channels':16,'synth.audio-channels':groups,'synth.audio-groups':groups,'synth.effects-groups':1,'synth.polyphony':256,'synth.reverb.active':int(effects),'synth.chorus.active':int(effects),'synth.threadsafe-api':0,'synth.cpu-cores':1}.items(): assert si(st,k.encode(),v)==0
        assert sn(st,b'synth.sample-rate',44100)==0 and sn(st,b'synth.gain',1)==0
        sy=synth(st); assert sy
        bank=load(sy,sys.argv[1].encode(),1); assert bank>=0
        for ch,route in routes.items():
            assert select(sy,ch,bank,128 if ch==9 else 0,0)==0
            assert cc(sy,ch,91,127)==0 and cc(sy,ch,93,127)==0
            if route:
                for g in (15,16):
                    assert gen(sy,ch,g,-10000)==0
                    assert getgen(sy,ch,g)==-10000
            assert note(sy,ch,36 if ch==9 else 60+ch%12,100)==0
        out=[[] for _ in range(34)]
        for n in (1,63,511,1024,73,1024,1024,1024,1024,1024,1024,1024,1024,1024,1024,1024):
            dry=[(F*n)() for _ in range(32)]; wet=[(F*n)() for _ in range(4)]
            if native:
                assert write(sy,n,dry[0],0,1,dry[1],0,1)==0
                out[0].extend(dry[0]);out[1].extend(dry[1]);continue
            assert process(sy,n,4,(FP*4)(*wet),32,(FP*32)(*dry))==0
            mixed=[[0.0]*n for _ in range(34)]
            for ch in range(16):
                route=routes.get(ch,0)
                for lr in (0,1):
                    for i,x in enumerate(dry[2*ch+lr]): mixed[2*route+lr][i]+=x
            for j in range(4):
                for i,x in enumerate(wet[j]): mixed[j%2][i]+=x
            for dst,src in zip(out,mixed): dst.extend(src)
        assert all(math.isfinite(x) for line in out for x in line)
        return out
    finally:
        if sy:free(sy)
        fs(st)
def peak(out,pair):return max(abs(x) for j in (2*pair,2*pair+1) for x in out[j])
a=run({0:1,1:1,9:16}); print('dry-only main peak',peak(a,0),'shared peak',peak(a,1),'drum 33/34 peak',peak(a,16),flush=True)
assert peak(a,0)<1e-6 and peak(a,1)>0 and peak(a,16)>0
assert all(peak(a,i)==0 for i in range(2,16))
b=run({0:0,9:3}); assert peak(b,0)>0 and peak(b,3)>0 and peak(b,1)==0
base=run({0:0,9:0},1,True); multi=run({0:0,9:0})
for j in (0,1):
    err=max(abs(x-y) for x,y in zip(base[j],multi[j])); print('all-main native difference',err); assert err<1e-5
print('Native routing, isolation, shared destination and stereo parity tests passed')
