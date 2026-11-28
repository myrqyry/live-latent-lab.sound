"""A real-liveness bridge must reject malformed and fabricated stem replies."""
import importlib.util
import os
import socket
import struct
import tempfile
import threading
import unittest
from pathlib import Path

import numpy as np

path=Path(__file__).resolve().parents[1]/'tools/live_stem_sidecar.py'
spec=importlib.util.spec_from_file_location('live_stem_sidecar',path)
sidecar=importlib.util.module_from_spec(spec)
spec.loader.exec_module(sidecar)

class FakeSeparator:
    def __init__(self,complete=True):
        self.complete=complete
        self.called=0
    def separate(self,path):
        import soundfile as sf
        self.called+=1
        x,sr=sf.read(path,dtype='float32',always_2d=True)
        outputs=[]
        for index,name in enumerate(['Drums','Bass','Other','Vocals'][:4 if self.complete else 3]):
            dst=Path(path).with_name('window_('+name+')_htdemucs.wav')
            sf.write(dst,x*float(index+1)/4,sr)
            outputs.append(dst.name)
        return outputs

class TestLiveStemProtocol(unittest.TestCase):
    def test_v1_documented_wire_vectors(self):
        # Read the independently specified golden bytes, not a second
        # calculation using the implementation's own format constants.
        import re
        doc=(path.parents[1]/"docs/stem-api.md").read_text()
        vectors=re.findall(r"```text\n([0-9a-f ]+)\n```",doc)
        self.assertEqual(len(vectors),3)
        request,success,error=map(bytes.fromhex,vectors)
        expected=(b"LLSTEM01",1,48000,384000,2,45)
        self.assertEqual(sidecar.REQUEST.size,36)
        self.assertEqual(sidecar.REQUEST.pack(*expected,0,0),request)
        self.assertEqual(sidecar.REQUEST.pack(*expected,4,0),success)
        self.assertEqual(sidecar.REQUEST.pack(*expected,0,1),error)
        self.assertEqual(sidecar.parse_request(request),(1,48000,384000,45))
        self.assertEqual(sidecar.STEMS,("Drums","Bass","Other","Vocals"))
        for offset in (0,8,12,16,20,28,32):
            malformed=bytearray(request)
            malformed[offset]^=1
            with self.subTest(offset=offset),self.assertRaises(ValueError):
                sidecar.parse_request(malformed)

    def test_v1_error_header_no_payload(self):
        import re
        vectors=re.findall(r"```text\n([0-9a-f ]+)\n```",(path.parents[1]/"docs/stem-api.md").read_text())
        request,_,error=map(bytes.fromhex,vectors)
        class NeverInfer:
            def separate_pcm(self,pcm):
                raise AssertionError("Invalid input reached model")
        for invalid_header in (False,True):
            a,b=socket.socketpair()
            def serve():
                with b:sidecar.serve_client(b,NeverInfer(),None)
            with a:
                a.settimeout(5)
                worker=threading.Thread(target=serve,daemon=True)
                worker.start()
                if invalid_header:
                    a.sendall(b"BADMAGIC"+request[8:])
                    expected=sidecar.REQUEST.pack(b"LLSTEM01",1,0,0,2,0,0,1)
                else:
                    a.sendall(request)
                    pcm=np.zeros((2,384000),dtype="<f4")
                    pcm[0,0]=np.nan
                    a.sendall(pcm.tobytes())
                    expected=error
                self.assertEqual(sidecar.read_exact(a,36),expected)
                self.assertEqual(a.recv(1),b"")
                worker.join(5)
                self.assertFalse(worker.is_alive())

    def test_request_header_is_bounded_and_exact(self):
        h=sidecar.REQUEST.pack(sidecar.MAGIC,1,48000,48000*8,2,21,0,0)
        self.assertEqual(len(h),sidecar.REQUEST.size)
        self.assertEqual(sidecar.parse_request(h)[1:],(48000,48000*8,21))
        for rate,frames,channels in [(96000,768000,2),(48000,10,2),
                                      (48000,48000*8,1),(48000,48000*7,2)]:
            h=sidecar.REQUEST.pack(sidecar.MAGIC,1,rate,frames,channels,21,0,0)
            with self.assertRaises(ValueError):sidecar.parse_request(h)

    def test_complete_stems_use_stable_order_and_preserve_stereo(self):
        sep=FakeSeparator()
        with tempfile.TemporaryDirectory() as tmp:
            a,b=socket.socketpair()
            try:
                worker=threading.Thread(target=lambda:sidecar.serve_client(b,sep,Path(tmp)),daemon=True)
                worker.start()
                rate=44100;frames=rate*8
                x=np.vstack((np.full(frames,.08,dtype='<f4'),
                             np.full(frames,-.19,dtype='<f4')))
                a.sendall(sidecar.REQUEST.pack(sidecar.MAGIC,1,rate,frames,2,7,0,0))
                a.sendall(x.tobytes())
                header=sidecar.read_exact(a,sidecar.REQUEST.size)
                magic,version,r,n,c,seq,stems,status=sidecar.REQUEST.unpack(header)
                self.assertEqual((magic,version,r,n,c,seq,stems,status),
                                 (sidecar.MAGIC,1,rate,frames,2,7,4,0))
                raw=sidecar.read_exact(a,4*2*frames*4)
                y=np.frombuffer(raw,dtype='<f4').reshape(4,2,frames)
                self.assertLess(float(np.max(np.abs(y[0]-y[1]))),.1)
                self.assertTrue(np.all(y[0,0]>0))
                self.assertTrue(np.all(y[0,1]<0))
                self.assertEqual(sep.called,1)
                worker.join(timeout=2)
                self.assertFalse(worker.is_alive())
            finally:a.close();b.close()

    def test_missing_stem_returns_error_and_no_audio(self):
        sep=FakeSeparator(complete=False)
        with tempfile.TemporaryDirectory() as tmp:
            a,b=socket.socketpair()
            try:
                worker=threading.Thread(target=lambda:sidecar.serve_client(b,sep,Path(tmp)),daemon=True)
                worker.start()
                frames=44100*8
                a.sendall(sidecar.REQUEST.pack(sidecar.MAGIC,1,44100,frames,2,0,0,0))
                a.sendall(np.zeros((2,frames),dtype='<f4').tobytes())
                magic,version,r,n,c,seq,stems,status=sidecar.REQUEST.unpack(sidecar.read_exact(a,sidecar.REQUEST.size))
                self.assertNotEqual(status,0)
                self.assertEqual(stems,0)
                worker.join(timeout=2)
            finally:a.close();b.close()

    def test_warmup_exercises_separation_in_ephemeral_directory(self):
        sep=FakeSeparator()
        with tempfile.TemporaryDirectory() as tmp:
            elapsed=sidecar.warmup(sep,Path(tmp))
            self.assertGreaterEqual(elapsed,0)
            self.assertEqual(sep.called,1)
            self.assertEqual(list(Path(tmp).iterdir()),[])

    def test_shared_demucs_quality_settings(self):
        self.assertEqual(sidecar.demucs_options(),{'segment_size':'Default','shifts':1,'overlap':.25,'segments_enabled':True})
        self.assertEqual(sidecar.demucs_options(0,.1,4),{'segment_size':'4','shifts':0,'overlap':.1,'segments_enabled':True})
        for values in [(3,.25,4),(1,.8,4),(1,float('nan'),4),(1,.25,8),(1,.25,0),(True,.25,4)]:
            with self.subTest(values=values),self.assertRaises(ValueError):sidecar.demucs_options(*values)

    def test_requested_device_policy(self):
        sidecar.require_device('auto','cpu')
        sidecar.require_device('cuda','cuda:0')
        sidecar.require_device('cpu','cpu')
        with self.assertRaises(RuntimeError):sidecar.require_device('cuda','cpu')

    def test_precision_is_explicit_and_rejects_unverified_integer_quantization(self):
        self.assertEqual(sidecar.precision_options('fp32'),
                         {'use_autocast': False, 'use_native_fp16': False})
        self.assertEqual(sidecar.precision_options('autocast'),
                         {'use_autocast': True, 'use_native_fp16': False})
        for mode in ('int8', 'int4', 'native_fp16', 'bogus'):
            with self.assertRaises(ValueError):
                sidecar.precision_options(mode)

    def test_requested_precision_fails_closed_if_runtime_cannot_honor_it(self):
        sidecar.require_effective_precision('fp32', 'fp32', 'cpu')
        sidecar.require_effective_precision('autocast', 'autocast', 'cuda')
        with self.assertRaises(RuntimeError):
            sidecar.require_effective_precision('autocast', 'fp32', 'cuda')
        with self.assertRaises(RuntimeError):
            sidecar.require_effective_precision('autocast', 'autocast', 'cpu')

if __name__=='__main__':unittest.main()
