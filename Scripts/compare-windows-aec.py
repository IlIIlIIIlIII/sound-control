import sys, struct, json, time
from pathlib import Path
import numpy as np
from scipy.signal import resample_poly
from scipy.io.wavfile import write

ROOT=Path(__file__).resolve().parents[1]/'build'
def packets(path):
    with open(path,'rb') as f:
        while header:=f.read(20):
            k,n,t,flags=struct.unpack('<iIQI',header)
            a=np.zeros((n,2),np.float32) if flags&2 else np.frombuffer(f.read(n*8),'<f4').reshape(n,2).copy()
            yield k,t,a

def fixtures():
    ps=list(packets(ROOT/'aec-replay-music-only.bin'))
    def channel(k):
        a=[(t,x) for j,t,x in ps if j==k]
        return np.concatenate([t+np.arange(len(x))*1e7/48000 for t,x in a]),np.concatenate([x for t,x in a])
    tm,m=channel(0); tr,r=channel(2)
    echo=m[:,0]*2
    ref=np.interp(tm,tr,r.mean(axis=1)).astype(np.float32)
    voice=np.concatenate([a[:,0]*2 for k,t,a in packets(ROOT/'aec-replay-voice-only.bin') if k==0])[35*48000:]
    near=np.zeros_like(echo); n=min(len(voice),len(echo)-480000); near[480000:480000+n]=voice[:n]
    np.savez(ROOT/'aec-comparison-fixtures.npz',echo=echo,near=near,ref=ref)
    return echo,near,ref

def score(y,echo,near):
    # Known independently captured near speech and echo mixed digitally.
    # Projections are diagnostic, not perceptual MOS or an independent live recording.
    sl=slice(14*16000,min(len(y),len(echo),len(near)))
    y=y[sl].astype(float); e=echo[sl].astype(float); n=near[sl].astype(float)
    if np.dot(n,n)>1e-12:
        gain=np.linalg.lstsq(np.stack([n,e],axis=1),y,rcond=None)[0]
        target=gain[0]*n
        return {'voice_gain_db':float(20*np.log10(abs(gain[0])+1e-15)),
                'echo_projection_reduction_db':float(-20*np.log10(abs(gain[1])+1e-15)),
                'voice_si_sdr_db':float(10*np.log10((np.dot(target,target)+1e-15)/(np.sum((y-target)**2)+1e-15)))}
    return {'echo_energy_reduction_db':float(10*np.log10((np.dot(e,e)+1e-15)/(np.dot(y,y)+1e-15)))}

if __name__=='__main__':
    echo,near,ref=fixtures()
    e,n,r=[resample_poly(x,1,3).astype(np.float32) for x in [echo,near,ref]]
    sys.path.insert(0,str(ROOT/'LocalVQE/pytorch'))
    import torch
    from localvqe.model import LocalVQE
    torch.set_num_threads(4)
    ck=torch.load(ROOT/'localvqe-v1.3-4.8M.pt',map_location='cpu',weights_only=True)
    cfg={k:v for k,v in ck.get('model_config',{}).items() if k!='transform'}
    print('config',cfg,flush=True)
    model=LocalVQE(**cfg).eval()
    model.load_state_dict(ck['model_state_dict'],strict=True);model.align.fold_temperature()
    results=[]
    for gain in [0,0.3,1,3]:
        mic=e+n*gain
        with torch.inference_mode():
            t=time.perf_counter(); out=model.decoder(model(torch.from_numpy(mic[None]),torch.from_numpy(r[None])),length=len(mic))[0].numpy(); elapsed=time.perf_counter()-t
        np.save(ROOT/f'localvqe13-g{gain}.npy',out)
        write(ROOT/f'localvqe13-g{gain}.wav',16000,np.clip(out,-1,1))
        result={'model':'LocalVQE 1.3','gain':gain,'offline_seconds':elapsed,**score(out,e,n*gain)}
        print(json.dumps(result),flush=True);results.append(result)
    (ROOT/'comparison-localvqe13.json').write_text(json.dumps(results,indent=2))
