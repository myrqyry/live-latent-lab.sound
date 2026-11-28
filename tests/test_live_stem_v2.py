"""Model-free v2 contract tests; no model or network downloads."""
import socket
import sys
import threading
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import live_stem_v2 as v2

class TestV2(unittest.TestCase):
    def test_geometry_and_profile(self):
        for rate in (44100,48000):
            n=rate*28//10
            request=v2.HEADER.pack(b"LLSTEM02",2,rate,n,2,45,0,0,1)
            self.assertEqual(v2.parse_request(request),(rate,n,45))
            self.assertEqual(v2.STEMS,("Vocals","Instrumental"))
            for offset in (0,8,12,16,20,28,32,36):
                bad=bytearray(request);bad[offset]^=1
                with self.assertRaises(ValueError):v2.parse_request(bad)
    def exchange(self,rate,invalid=False,wrong_shape=False):
        n=rate*28//10
        x=np.stack([np.full(n,.25,dtype="<f4"),np.full(n,-.125,dtype="<f4")])
        if invalid:x[0,0]=np.nan
        calls=[]
        def model(pcm,r):
            calls.append(r)
            np.testing.assert_array_equal(pcm,x)
            return pcm[:1] if wrong_shape else pcm*.25
        a,b=socket.socketpair()
        def serve():
            with b:v2.serve_client(b,model)
        thread=threading.Thread(target=serve,daemon=True)
        thread.start()
        with a:
            a.settimeout(5)
            a.sendall(v2.HEADER.pack(b"LLSTEM02",2,rate,n,2,45,0,0,1))
            a.sendall(x.tobytes())
            reply=v2.HEADER.unpack(v2.read_exact(a,40))
            self.assertEqual(reply,(b"LLSTEM02",2,rate,n,2,45,0 if invalid or wrong_shape else 2,1 if invalid or wrong_shape else 0,1))
            if not invalid and not wrong_shape:
                y=np.frombuffer(v2.read_exact(a,4*n*4),dtype="<f4").reshape(2,2,n)
                np.testing.assert_array_equal(y[0],x*.25)
                np.testing.assert_array_equal(y[1],x*.75)
                np.testing.assert_array_equal(y.sum(axis=0),x)
            self.assertEqual(a.recv(1),b"")
        thread.join(5);self.assertFalse(thread.is_alive())
        self.assertEqual(len(calls),0 if invalid else 1)
    def test_both_rates(self):
        for rate in (44100,48000):self.exchange(rate)
    def test_reject_nonfinite(self):self.exchange(48000,invalid=True)
    def test_reject_shape(self):self.exchange(44100,wrong_shape=True)
if __name__=="__main__":unittest.main()
