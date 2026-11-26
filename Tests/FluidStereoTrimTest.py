"""Local native SF2 stereo trim regression (not a Windows VST3 test).
Usage: python Tests/FluidStereoTrimTest.py /path/to/bank.sf2
Requires FluidR3 GM or a compatible bank with melodic and percussion programs.
"""
import ctypes as C, ctypes.util, math, sys
P,I,F,D,S=C.c_void_p,C.c_int,C.c_float,C.c_double,C.c_char_p
lib=C.CDLL(ctypes.util.find_library('fluidsynth') or 'libfluidsynth.so.3')
def api(name,result,*args):
    fn=getattr(lib,name);fn.restype=result;fn.argtypes=args;return fn
settings=api('new_fluid_settings',P); synth=api('new_fluid_synth',P,P)
setint=api('fluid_settings_setint',I,P,S,I);setnum=api('fluid_settings_setnum',I,P,S,D)
load=api('fluid_synth_sfload',I,P,S,I);select=api('fluid_synth_program_select',I,P,I,I,I,I)
cc=api('fluid_synth_cc',I,P,I,I,I);getcc=api('fluid_synth_get_cc',I,P,I,I,C.POINTER(I))
note=api('fluid_synth_noteon',I,P,I,I,I);gen=api('fluid_synth_set_gen',I,P,I,I,F)
getgen=api('fluid_synth_get_gen',F,P,I,I);write=api('fluid_synth_write_float',I,P,I,P,I,I,P,I,I)
free=api('delete_fluid_synth',None,P);free_s=api('delete_fluid_settings',None,P)
ATTENUATION=48

def render(ch,trim_ch=None,db=0.):
    st=settings();sy=None
    try:
        for k,v in {'synth.midi-channels':16,'synth.audio-channels':1,'synth.audio-groups':1,'synth.effects-groups':1,'synth.polyphony':256,'synth.reverb.active':0,'synth.chorus.active':0,'synth.threadsafe-api':0,'synth.cpu-cores':1}.items():assert setint(st,k.encode(),v)==0
        assert setnum(st,b'synth.sample-rate',44100.)==0 and setnum(st,b'synth.gain',1.)==0
        sy=synth(st);assert sy
        font=load(sy,sys.argv[1].encode(),1);assert font>=0
        for i in range(16):
            assert select(sy,i,font,128 if i==9 else 0,0)==0
            assert cc(sy,i,7,100)==0 and cc(sy,i,11,127)==0
        if trim_ch is not None:
            original=getgen(sy,trim_ch,ATTENUATION)
            assert gen(sy,trim_ch,ATTENUATION,original-10.*db)==0
            assert abs(getgen(sy,trim_ch,ATTENUATION)-(original-10.*db))<1e-5
        assert note(sy,ch,36 if ch==9 else 60,100)==0
        l,r=(F*8192)(),(F*8192)();assert write(sy,8192,l,0,1,r,0,1)==0
        for i in range(16):
            for k,v in ((7,100),(11,127)):
                read=I();assert getcc(sy,i,k,C.byref(read))==0 and read.value==v
        assert all(math.isfinite(x) for x in list(l)+list(r))
        return math.sqrt(sum(x*x+y*y for x,y in zip(l,r))/(2*len(l)))
    finally:
        if sy:free(sy)
        free_s(st)

for ch in (0,9):
    native=render(ch);zero=render(ch,ch,0);quiet=render(ch,ch,-6);other=render(ch,(ch+1)%16,-6)
    assert native>0 and quiet<native
    # New instances avoid residual releases and reused voices. The note and bank
    # are fixed; an unrelated channel offset must not affect the rendered one.
    assert abs(zero/native-1)<.001 and abs(other/native-1)<.001
    print('CH',ch+1,'native RMS',native,'trim RMS',quiet,'observed dB',20*math.log10(quiet/native),flush=True)
print('Stereo channel trim and unchanged CC checks passed')
