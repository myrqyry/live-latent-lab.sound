import sys, unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from tts_worker import Retention

class RetentionTest(unittest.TestCase):
    def setUp(self):
        self.t=100
        self.r=Retention(lambda:self.t)
    def test_shared_last_release_and_ttl(self):
        self.r.acquire('kokoro','a',0)
        self.r.acquire('kokoro','b',5)
        self.r.release('kokoro','a')
        self.assertFalse(self.r.evictable('kokoro'))
        self.r.release('kokoro','b')
        self.t+=4
        self.assertFalse(self.r.evictable('kokoro'))
        self.t+=1
        self.assertTrue(self.r.evictable('kokoro'))
    def test_renew_expired_and_keep_warm(self):
        self.r.acquire('chatterbox','a',-1)
        self.r.release('chatterbox','a')
        self.t+=500
        self.assertFalse(self.r.evictable('chatterbox'))
        self.r.acquire('chatterbox','a',0)
        self.r.release('chatterbox','a')
        self.assertTrue(self.r.evictable('chatterbox'))
        self.r.acquire('piper','b',3)
        self.t+=1000
        self.assertTrue(self.r.evictable('piper'))
    def test_legacy_and_racing_use(self):
        self.r.used('kokoro',30)
        self.assertFalse(self.r.evictable('kokoro'))
        self.r.acquire('kokoro','a',0)
        self.r.release('kokoro','a')
        self.assertTrue(self.r.evictable('kokoro'))
        self.r.used('kokoro',0)
        self.assertTrue(self.r.evictable('kokoro'))
    def test_refresh_and_crash_expiry(self):
        self.r.acquire('chatterbox','a',-1)
        self.t+=119
        self.r.acquire('chatterbox','a',-1)
        self.t+=119
        self.assertFalse(self.r.evictable('chatterbox'))
        self.t+=1
        self.assertTrue(self.r.evictable('chatterbox'))

    def test_anonymous_idle_cache_refresh(self):
        self.r.used('piper',30)
        self.t+=29
        self.r.used('piper',30)
        self.t+=29
        self.assertFalse(self.r.evictable('piper'))
        self.t+=1
        self.assertTrue(self.r.evictable('piper'))

    def test_other_models_independent(self):
        self.r.acquire('piper','a',0)
        self.r.acquire('kokoro','b',0)
        self.r.release('piper','a')
        self.assertTrue(self.r.evictable('piper'))
        self.assertFalse(self.r.evictable('kokoro'))
if __name__=='__main__':unittest.main()
