"""Optional Linux regression: python Tests/FluidRoutingTest.py /path/to/bank.sf2
Uses the installed FluidSynth shared library, not the Windows VST3 binary.
"""
import ctypes as C
import ctypes.util
import math
import sys
import time
P, I, F = C.c_void_p, C.c_int, C.c_float
FP = C.POINTER(F)
lib = C.CDLL(ctypes.util.find_library('fluidsynth') or 'libfluidsynth.so.3')
def api(n, restype, *args):
    f = getattr(lib,n); f.restype=restype; f.argtypes=args; return f
settings=api('new_fluid_settings',P); synth=api('new_fluid_synth',P,P)
setint=api('fluid_settings_setint',I,P,C.c_char_p,I)
setnum=api('fluid_settings_setnum',I,P,C.c_char_p,C.c_double)
load=api('fluid_synth_sfload',I,P,C.c_char_p,I)
select=api('fluid_synth_program_select',I,P,I,I,I,I)
cc=api('fluid_synth_get_cc',I,P,I,I,C.POINTER(I))
note=api('fluid_synth_noteon',I,P,I,I,I)
process=api('fluid_synth_process',I,P,I,I,C.POINTER(FP),I,C.POINTER(FP))
write=api('fluid_synth_write_float',I,P,I,P,I,I,P,I,I)
free=api('delete_fluid_synth',None,P); free_s=api('delete_fluid_settings',None,P)
N=1024

def render(groups, drum, melodic, split, trim=1.0, gain=0.2):
    st=settings()
    for k,v in {'synth.midi-channels':16,'synth.audio-channels':groups,'synth.audio-groups':groups,'synth.effects-groups':1,'synth.reverb.active':0,'synth.chorus.active':0,'synth.cpu-cores':1}.items():
        assert setint(st,k.encode(),v)!=-1
    setnum(st,b'synth.sample-rate',44100.)
    assert setnum(st,b'synth.gain',gain)!=-1
    sy=synth(st)
    assert sy
    try:
        font=load(sy,sys.argv[1].encode(),1); assert font>=0
        assert select(sy,0,font,0,0)==0 and select(sy,9,font,128,0)==0
        for ch in (0,9):
            for number,expected in ((7,100),(11,127),(10,64),(91,0),(93,0)):
                value=I(); assert cc(sy,ch,number,C.byref(value))==0 and value.value==expected
        if drum: assert note(sy,9,36,100)==0
        if melodic: assert note(sy,0,60,100)==0
        result=[]; t0=time.perf_counter()
        for _ in range(16):
            l,r,dl,dr=[(F*N)() for _ in range(4)]
            if groups==1:
                assert write(sy,N,l,0,1,r,0,1)==0
            else:
                wet=(FP*2)(l,r)
                if split:
                    out=(FP*32)(*[p for ch in range(16) for p in ((dl,dr) if ch==9 else (l,r))])
                    assert process(sy,N,2,wet,32,out)==0
                else:
                    assert process(sy,N,2,wet,2,wet)==0
            result.extend(x+trim*y for x,y in zip(l,dl))
        print('render',groups,drum,melodic,split,trim, 'seconds',round(time.perf_counter()-t0,4))
        return result
    finally: free(sy); free_s(st)

def rms(x):return math.sqrt(sum(v*v for v in x)/len(x))
def eq(a,b):return max(abs(x-y) for x,y in zip(a,b))<2e-6
base=render(1,True,True,False)
neutral=render(16,True,True,False)
split=render(16,True,True,True)
assert eq(base,neutral) and eq(base,split)
for drum,melodic in [(True,False),(False,True)]:
    normal=render(16,drum,melodic,True)
    boosted=render(16,drum,melodic,True,10**(3/20))
    assert rms(normal)>0
    delta=20*math.log10(rms(boosted)/rms(normal))
    print('channel',10 if drum else 1,'delta dB',delta)
    assert abs(delta-(3 if drum else 0))<0.001
print('FluidR3 routing and dry gain tests passed')

# TEST13: confirm the new unity gain with the real SF2, on both kinds of channel.
# Voice termination can vary with gain; allow a small RMS tolerance.
for drum,melodic in [(True,False),(False,True)]:
    low=render(16,drum,melodic,False,gain=.2)
    unity=render(16,drum,melodic,False,gain=1.)
    delta=20*math.log10(rms(unity)/rms(low))
    print('Unity gain channel',10 if drum else 1,'delta dB',delta,'peak',max(map(abs,unity)))
    assert abs(delta-20*math.log10(5))<.1
print('Unity gain calibration passed on melodic and percussion channels')
