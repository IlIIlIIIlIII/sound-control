import sys,time,json
import numpy as np
import openvino as ov
from scipy.signal import resample_poly
from scipy.io.wavfile import write
import importlib.util
spec=importlib.util.spec_from_file_location('aec_compare',__file__.replace('compare-windows-dtln.py','compare-windows-aec.py'))
shared=importlib.util.module_from_spec(spec);spec.loader.exec_module(shared)
ROOT,fixtures,score=shared.ROOT,shared.fixtures,shared.score

e,n,r=[resample_poly(x,1,3).astype(np.float32) for x in fixtures()]
core=ov.Core(); results=[]
sizes=[int(x) for x in sys.argv[1:]] or [128,256,512]
for size in sizes:
    models=[core.compile_model(core.read_model(str(ROOT/f'DTLN-aec/pretrained_models/dtln_aec_{size}_{i}.tflite')),'NPU') for i in [1,2]]
    print('compiled',size,[(str(m.get_property('EXECUTION_DEVICES')),[(p.any_name,list(p.shape)) for p in m.inputs]) for m in models],flush=True)
    requests=[m.create_infer_request() for m in models]
    for gain in [0,0.3,1,3]:
        mic=np.pad(e+n*gain,(384,512));ref=np.pad(r,(384,512));out=np.zeros_like(mic)
        s=[np.zeros(m.inputs[1].shape,np.float32) for m in models]
        ib=np.zeros(512,np.float32);rb=ib.copy();ob=ib.copy();timings=[]
        start=time.perf_counter()
        for pos in range(0,len(mic)-128,128):
            t=time.perf_counter()
            ib[:-128]=ib[128:];ib[-128:]=mic[pos:pos+128]
            rb[:-128]=rb[128:];rb[-128:]=ref[pos:pos+128]
            spec=np.fft.rfft(ib);rspec=np.fft.rfft(rb)
            o=requests[0].infer({0:np.abs(spec).reshape(1,1,257).astype(np.float32),1:s[0],2:np.abs(rspec).reshape(1,1,257).astype(np.float32)})
            mask=o[models[0].output(0)].copy();s[0]=o[models[0].output(1)].copy()
            estimated=np.fft.irfft(spec*mask.ravel()).reshape(1,1,512).astype(np.float32)
            o=requests[1].infer({0:estimated,1:s[1],2:rb.reshape(1,1,512)})
            s[1]=o[models[1].output(1)].copy()
            ob[:-128]=ob[128:];ob[-128:]=0;ob+=o[models[1].output(0)].ravel()
            out[pos:pos+128]=ob[:128]
            timings.append((time.perf_counter()-t)*1000)
        out=out[768:768+len(e)]
        np.save(ROOT/f'dtln{size}-g{gain}.npy',out);write(ROOT/f'dtln{size}-g{gain}.wav',16000,out)
        result={'model':f'DTLN-AEC {size}','device':'NPU','gain':gain,'seconds':time.perf_counter()-start,'frame_p50_ms':float(np.percentile(timings[10:],50)),'frame_p99_ms':float(np.percentile(timings[10:],99)),**score(out,e,n*gain)}
        results.append(result);print(json.dumps(result),flush=True)
        (ROOT/'comparison-dtln.json').write_text(json.dumps(results,indent=2))
