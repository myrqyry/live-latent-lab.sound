#include "framework/tts_text.hpp"
#include <cassert>
#include <iostream>
int main(){
    using namespace stts;
    assert(!valid_speech_text(""));
    assert(!valid_speech_text(" \n\t"));
    assert(!valid_speech_text(std::string(MaxSpeechBytes+1,'a')));
    assert(!valid_speech_text("\xc0\x80"));
    assert(!valid_speech_text("\xed\xa0\x80"));
    assert(!valid_speech_text("\xf4\x90\x80\x80"));
    auto verify=[](const std::string& text){
        assert(valid_speech_text(text));
        auto chunks=split_speech(text);
        assert(!chunks.empty());
        std::string combined;
        for(const auto& chunk:chunks){
            assert(!chunk.empty()&&chunk.size()<=MaxChunkBytes);
            assert(valid_utf8(chunk));combined+=chunk;
        }
        assert(combined==text);
        return chunks;
    };
    assert(verify("Hello there.").size()==1);
    assert(verify(std::string(673,'a')).size()==3);
    std::string mixed;
    while(mixed.size()<2400)mixed+="Hola, señor. 日本語。 🫂  ";
    verify(mixed);
    auto sentences=verify(std::string(200,'a')+". "+std::string(200,'b')+"!");
    assert(sentences.front().back()=='.'||sentences.front().back()==' ');
    verify("hello"+std::string(600,' ')+"world");
    verify(std::string(MaxSpeechBytes,'x'));
    std::cout<<"SOUND_TTS_TEXT_PASS=1\n";
}
