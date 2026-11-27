"""Model-free checks for the resident PCM contract."""
import importlib.util
import io
import socket
import threading
from pathlib import Path
import unittest
from unittest.mock import patch
import numpy as np
import soundfile as sf

path=Path(__file__).resolve().parents[1]/'tools/live_stem_sidecar.py'
spec=importlib.util.spec_from_file_location('live_stem_sidecar',path)
sidecar=importlib.util.module_from_spec(spec)
spec.loader.exec_module(sidecar)

class ResidentTests(unittest.TestCase):
    def test_pcm16_matches_reference(self):
        rng=np.random.default_rng(21)
        samples=rng.uniform(-1.2,1.2,(2,10000)).astype('float32')
        samples[:,0:5]=[-1.,-.5,0.,.5,1.]
        buf=io.BytesIO()
        sf.write(buf,samples.T,44100,format='WAV',subtype='PCM_16')
        buf.seek(0)
        expected,sr=sf.read(buf,dtype='float32',always_2d=True)
        np.testing.assert_array_equal(sidecar.pcm16_compatible(samples),expected.T)

    def test_resident_window_no_file_io_and_named_channels(self):
        class Backend:
            def __init__(self):self.calls=0
            def separate_pcm(self,pcm):
                self.calls+=1
                self.last=pcm.shape
                return {name:np.stack((np.full(pcm.shape[1],.025*(i+1),np.float32),
                                       np.full(pcm.shape[1],-.0375*(i+1),np.float32)))
                        for i,name in enumerate(reversed(sidecar.STEMS))}
        backend=Backend()
        for rate in (44100,48000):
            with patch('soundfile.write',side_effect=AssertionError('disk write')):
                y=sidecar.separate_window(backend,np.zeros((2,rate*8),np.float32),rate,None)
            self.assertEqual(y.shape,(4,2,rate*8))
            self.assertEqual(y.dtype,np.dtype('<f4'))
            self.assertEqual(backend.last,(2,44100*8))
            np.testing.assert_allclose(y[:,0,rate], [.1,.075,.05,.025],atol=2e-4)
            np.testing.assert_allclose(y[:,1,rate], [-.15,-.1125,-.075,-.0375],atol=2e-4)
        self.assertEqual(backend.calls,2)

    def test_resident_socket_does_not_allocate_temp_directory(self):
        class Backend:
            def separate_pcm(self,pcm):
                return {name:pcm*(i+1)*.1 for i,name in enumerate(sidecar.STEMS)}
        a,b=socket.socketpair()
        with a,b,patch('tempfile.TemporaryDirectory',side_effect=AssertionError('temp directory')),patch('soundfile.write',side_effect=AssertionError('file write')):
            a.settimeout(5)
            worker=threading.Thread(target=sidecar.serve_client,args=(b,Backend(),None),daemon=True)
            worker.start()
            rate=44100;frames=rate*8
            pcm=np.stack((np.full(frames,.25,np.float32),np.full(frames,-.125,np.float32)))
            a.sendall(sidecar.REQUEST.pack(sidecar.MAGIC,1,rate,frames,2,42,0,0)+pcm.tobytes())
            self.assertEqual(sidecar.REQUEST.unpack(sidecar.read_exact(a,sidecar.REQUEST.size)),
                             (sidecar.MAGIC,1,rate,frames,2,42,4,0))
            y=np.frombuffer(sidecar.read_exact(a,frames*2*4*4),dtype='<f4').reshape(4,2,frames)
            np.testing.assert_allclose(y[3,:,0],[.1,-.05],atol=1/32768)
            worker.join(5)
            self.assertFalse(worker.is_alive())

    def test_reject_bad_resident_output(self):
        class Backend:
            def __init__(self,kind):self.kind=kind
            def separate_pcm(self,pcm):
                result={name:pcm.copy() for name in sidecar.STEMS}
                if self.kind=='missing':del result['Bass']
                if self.kind=='shape':result['Bass']=pcm[:,:-1]
                if self.kind=='nonfinite':result['Bass'][0,0]=np.nan
                return result
        for kind in ('missing','shape','nonfinite'):
            with self.subTest(kind=kind),self.assertRaises(ValueError):
                sidecar.separate_window(Backend(kind),np.zeros((2,44100*8),np.float32),44100,None)

if __name__=='__main__':unittest.main()
