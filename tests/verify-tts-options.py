import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from tts_worker import speech_options

class SpeechOptionsTest(unittest.TestCase):
    def test_defaults_and_types(self):
        self.assertEqual(speech_options('kokoro',{}),dict(speed=1.0,language='auto',is_phonemes=False,trim=True,sentence_pause=.25,clause_pause=.1))
        self.assertEqual(speech_options('piper',{})['noise_w_scale'],.8)
        self.assertEqual(speech_options('chatterbox',{})['top_k'],1000)
        self.assertEqual(speech_options('edge',{}),dict(rate=0,pitch=0,volume=0))
    def test_supported_nondefault_values(self):
        self.assertEqual(speech_options('kokoro',{'speed':1.4,'language':'en-gb','trim':False})['speed'],1.4)
        self.assertEqual(speech_options('piper',{'normalize_audio':False})['normalize_audio'],False)
        self.assertEqual(speech_options('chatterbox',{'temperature':.6,'top_k':50})['top_k'],50)
        self.assertEqual(speech_options('edge',{'rate':-20,'pitch':15,'volume':-30})['rate'],-20)
    def test_native_generation_controls(self):
        self.assertEqual(speech_options('kokoro_native',{'seed':42,'text_chunk_size':80})['seed'],42)
        self.assertEqual(speech_options('kokoro_native',{})['seed'],-1)
        self.assertEqual(speech_options('chatterbox_native',{'seed':42,'max_tokens':512,'text_chunk_size':64})['max_tokens'],512)
        for model,opts in [('kokoro_native',{'seed':-2}),('kokoro_native',{'text_chunk_size':0}),
                           ('chatterbox_native',{'seed':-1}),('chatterbox_native',{'max_tokens':1001}),
                           ('chatterbox_native',{'text_chunk_size':129})]:
            with self.subTest(model=model,opts=opts),self.assertRaises(ValueError):speech_options(model,opts)
    def test_bad_settings_rejected(self):
        for model,opts in [('kokoro',{'speed':2.1}),('kokoro',{'speed':True}),('kokoro',{'language':'xx'}),('kokoro',{'continuous':True}),
                           ('piper',{'speaker_id':2}),('piper',{'noise_scale':float('nan')}),
                           ('chatterbox',{'cfg_weight':.5}),('chatterbox',{'top_k':1.5}),
                           ('edge',{'rate':-51}),('edge',{'pitch':'15Hz'}),('edge',[]),('tiktok',{'rate':10})]:
            with self.subTest(model=model,opts=opts),self.assertRaises(ValueError):speech_options(model,opts)
if __name__=='__main__':unittest.main()
