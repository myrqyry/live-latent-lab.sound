"""Built-in-voice Chatterbox Turbo using the same private native transport."""
from pathlib import Path
import sys
from native_kokoro import Worker as NativeWorker, Codec as NativeCodec, serve
from tts_worker import speech_options

def validate(voice,text,options):
    if voice != 'default' or not isinstance(text,str) or not text.strip() or '\x00' in text or len(text.encode())>128:
        raise ValueError('Native Turbo requires the built-in voice and nonempty text')
    return speech_options('chatterbox_native',options)

class Worker(NativeWorker):
    entrypoint = Path(__file__)
    model_id = 'chatterbox_native'
    device = "cpu"

    def synthesize(self,voice,text,options):
        return self.request([voice,text,validate(voice,text,options)])

class Codec(NativeCodec):
    device = "cpu"
    threads = 4
    model_bytes = 699102368
    model_sha = 'acf02bed2b9ac94e271a01f556526818161a28009120f2337b9e8e10035c42b6'

    def synthesize(self,voice,text,options):
        return self.run(None,text,b'en',validate(voice,text,options))

if __name__=='__main__':
    serve(int(sys.argv[1]),sys.argv[2],sys.argv[3],Codec)
