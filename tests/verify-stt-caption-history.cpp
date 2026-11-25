#include "framework/stt_caption_history.hpp"
#include <cassert>
#include <iostream>
int main(){
 stts::CaptionHistory h;
 assert(h.partial("first part",1)=="first part");
 assert(h.final("first",1)=="first");
 assert(h.partial("second par",1)=="second par");
 assert(h.final("second",3)=="first\nsecond");
 assert(h.partial("third par",3)=="first\nsecond\nthird par");
 assert(h.partial("third revised",3)=="first\nsecond\nthird revised");
 assert(h.final("third",3)=="first\nsecond\nthird");
 assert(h.partial("fourth",3)=="second\nthird\nfourth");
 assert(h.final("fourth",3)=="second\nthird\nfourth");
 h.clear();
 assert(h.partial("new epoch",3)=="new epoch");
 assert(h.final("new",3)=="new");
 assert(h.final("last",1)=="last");
 assert(h.partial("again",3)=="last\nagain");
 std::cout<<"STT_CAPTION_HISTORY_PASS=1\n";
}
