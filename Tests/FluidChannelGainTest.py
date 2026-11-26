"""TEST15: independent dry buffers, shared native FX, no aliasing.
Usage: python Tests/FluidChannelGainTest.py /path/to/bank.sf2
Exercises the local FluidSynth ABI, not the VST3 host or Windows build.
"""
import ctypes as C, ctypes.util, math, sys
P,I,F,D,S=C.c_void_p,C.c_int,C.c_float,C.c_double,C.c_char_p
FP=C.POINTER(F)
lib=C.CDLL(ctypes.util.find_library('fluidsynth') or 'libfluidsynth.so.3')
def api(name,result,*args):
    fn=getattr(lib,name);fn.restype=result;fn.argtypes=args;return fn
settings=api('new_fluid_settings',P); synth=api('new_fluid_synth',P,P)
setint=api('fluid_settings_setint',I,P,S,I);setnum=api('fluid_settings_setnum',I,P,S,D)
load=api('fluid_synth_sfload',I,P,S,I);select=api('fluid_synth_program_select',I,P,I,I,I,I)
cc=api('fluid_synth_cc',I,P,I,I,I);getcc=api('fluid_synth_get_cc',I,P,I,I,C.POINTER(I));note=api('fluid_synth_noteon',I,P,I,I,I)
process=api('fluid_synth_process',I,P,I,I,C.POINTER(FP),I,C.POINTER(FP));write=api('fluid_synth_write_float',I,P,I,P,I,I,P,I,I)
free=api('delete_fluid_synth',None,P);free_s=api('delete_fluid_settings',None,P)
rv=api('fluid_synth_reverb_on',I,P,I,I);chorus=api('fluid_synth_chorus_on',I,P,I,I)
countaudio=api('fluid_synth_count_audio_channels',I,P);countgroups=api('fluid_synth_count_audio_groups',I,P)
countfx=api('fluid_synth_count_effects_channels',I,P);countfxg=api('fluid_synth_count_effects_groups',I,P)

def render(rate,groups,active,fx=False,gains=None):
    st=settings();sy=None;gains=gains or [1.]*16
    try:
        for k,v in {'synth.midi-channels':16,'synth.audio-channels':groups,'synth.audio-groups':groups,'synth.effects-groups':1,'synth.polyphony':256,'synth.reverb.active':0,'synth.chorus.active':0,'synth.threadsafe-api':0,'synth.cpu-cores':1}.items():assert setint(st,k.encode(),v)==0
        assert setnum(st,b'synth.sample-rate',rate)==0;assert setnum(st,b'synth.gain',1.)==0
        sy=synth(st);assert sy and countaudio(sy)==groups and countgroups(sy)==groups and countfx(sy)*countfxg(sy)==2
        font=load(sy,sys.argv[1].encode(),1);assert font>=0
        for ch in range(16):
            assert select(sy,ch,font,128 if ch==9 else 0,0)==0
            assert cc(sy,ch,91,40)==0 and cc(sy,ch,93,40)==0
        assert rv(sy,-1,int(fx))==0 and chorus(sy,-1,int(fx))==0
        for ch in active: assert note(sy,ch,36 if ch==9 else 60+(ch%12),100)==0
        result=[];wet_result=[];dry_result=[[] for _ in range(16)]
        for n in [1,63,65,127,511,1024,17,1024,1024,1024,1024,1024,1024,1024,1024]:
            if groups==1:
                l,r=(F*n)(),(F*n)();assert write(sy,n,l,0,1,r,0,1)==0
                result.extend(zip(l,r));continue
            # Distinct buffers with guard words before/after the actual sample area.
            arrays=[(F*(n+2))() for _ in range(36)]
            for a in arrays:a[0]=123.;a[-1]=456.
            ptr=[C.cast(C.byref(a,C.sizeof(F)),FP) for a in arrays]
            dry=(FP*32)(*ptr[:32]);wet=(FP*4)(*ptr[32:]);assert process(sy,n,4,wet,32,dry)==0
            assert all(a[0]==123. and a[-1]==456. for a in arrays)
            for i in range(1,n+1):
                w=tuple(arrays[32+k][i]+arrays[34+k][i] for k in (0,1));wet_result.append(w)
                for ch in range(16):dry_result[ch].append((arrays[ch*2][i],arrays[ch*2+1][i]))
                result.append(tuple(w[k]+sum(arrays[ch*2+k][i]*gains[ch] for ch in range(16)) for k in (0,1)))
        for ch in range(16):
            for number,expected in [(7,100),(11,127),(91,40),(93,40)]:
                v=I();assert getcc(sy,ch,number,C.byref(v))==0 and v.value==expected
        assert all(math.isfinite(v) for pair in result for v in pair)
        return result,dry_result,wet_result
    finally:
        if sy:free(sy)
        free_s(st)

def peak(x):return max(abs(v) for pair in x for v in pair)
def diff(x,y):return max(abs(a-b) for p,q in zip(x,y) for a,b in zip(p,q))
for rate in (44100,48000,96000):
    for fx in (False,True):
        base,_,_=render(rate,1,range(16),fx)
        mixed,dry,wet=render(rate,16,range(16),fx)
        # Floating-point summation order differs across routing modes.
        error=diff(base,mixed);assert error<2e-5,(rate,fx,error)
        assert all(peak(x)>0 for x in dry)
        print('unity',rate,'FX',fx,'max error',error,flush=True)
    for ch in (0,9):
        normal,dry,wet=render(rate,16,[ch],False)
        gs=[1.]*16;gs[ch]=.5
        changed,d2,w2=render(rate,16,[ch],False,gs)
        assert peak(normal)>0 and diff(changed,[tuple(.5*v for v in p) for p in normal])<2e-6
        assert all(peak(d)==0 for i,d in enumerate(dry) if i!=ch)
        gs=[1.]*16;gs[(ch+1)%16]=.5
        unaffected,_,_=render(rate,16,[ch],False,gs)
        assert diff(normal,unaffected)<2e-6
        print('independent gain',rate,'CH',ch+1,'OK',flush=True)
print('All independent channel routing checks passed')
