#pragma once
#include <algorithm>
#include <deque>
#include <string>

namespace stts {
// Presentation only. Raw recognition events never contain caption history.
class CaptionHistory {
    std::deque<std::string> finals_;
    static size_t limit(int lines){return size_t(std::clamp(lines,1,8));}
    std::string compose(const std::string& partial,int lines) const {
        const auto available=limit(lines)-(partial.empty()?0:1);
        std::string text;
        const auto start=finals_.size()>available?finals_.size()-available:0;
        for(size_t i=start;i<finals_.size();++i){if(!text.empty())text+='\n';text+=finals_[i];}
        if(!partial.empty()){if(!text.empty())text+='\n';text+=partial;}
        return text;
    }
public:
    void clear(){finals_.clear();}
    std::string partial(const std::string& text,int lines) const{return compose(text,lines);}
    std::string final(const std::string& text,int lines){
        if(!text.empty())finals_.push_back(text);
        while(finals_.size()>limit(lines))finals_.pop_front();
        return compose("",lines);
    }
};
}
