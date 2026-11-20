#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
namespace stts {
inline constexpr size_t MaxSpeechBytes=16384;
inline constexpr size_t MaxChunkBytes=300;
inline bool whitespace(char c){return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'||c=='\v';}
inline bool has_speech(const std::string& text){
    return std::any_of(text.begin(),text.end(),[](char c){return !whitespace(c);});
}
inline bool continuation(unsigned char c){return (c&0xc0)==0x80;}
inline bool valid_utf8(const std::string& text){
    for(size_t i=0;i<text.size();){
        auto c=static_cast<unsigned char>(text[i++]);
        if(c==0)return false;
        if(c<0x80)continue;
        size_t n;uint32_t value,min;
        if(c>=0xc2&&c<=0xdf){n=1;value=c&0x1f;min=0x80;}
        else if(c>=0xe0&&c<=0xef){n=2;value=c&0x0f;min=0x800;}
        else if(c>=0xf0&&c<=0xf4){n=3;value=c&7;min=0x10000;}
        else return false;
        if(text.size()-i<n)return false;
        for(size_t j=0;j<n;++j){
            auto b=static_cast<unsigned char>(text[i++]);
            if(!continuation(b))return false;
            value=(value<<6)|(b&0x3f);
        }
        if(value<min||value>0x10ffff||(value>=0xd800&&value<=0xdfff))return false;
    }
    return true;
}
inline bool valid_speech_text(const std::string& text){
    return text.size()<=MaxSpeechBytes&&has_speech(text)&&valid_utf8(text);
}
// Preserve the complete UTF-8 byte sequence. Prefer a sentence boundary
// in the second half, then a whitespace boundary, then a codepoint.
inline std::vector<std::string> split_speech(const std::string& text,size_t max_chunk=MaxChunkBytes){
    std::vector<std::string> chunks;
    if(!valid_speech_text(text)||max_chunk<4||max_chunk>MaxChunkBytes)return chunks;
    for(size_t start=0;start<text.size();){
        auto end=std::min(start+max_chunk,text.size());
        if(end<text.size()){
            while(end>start&&continuation(static_cast<unsigned char>(text[end])))--end;
            size_t sentence=0,word=0;
            for(size_t i=start+1;i<=end;++i){
                const char last=text[i-1];
                if(whitespace(last))word=i;
                if((last=='.'||last=='!'||last=='?'||last=='\n')&&(i==text.size()||whitespace(text[i])))sentence=i;
            }
            if(sentence>=start+max_chunk/2)end=sentence;
            else if(word>=start+max_chunk/2)end=word;
        }
        chunks.emplace_back(text.substr(start,end-start));start=end;
    }
    return chunks;
}
}
