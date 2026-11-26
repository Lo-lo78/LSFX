#pragma once
#include <algorithm>
namespace lsfx {
enum class GridMove { up, down, left, right, pageUp, pageDown, home, end };
inline int gridDestination(int i,int count,int rows,GridMove move,bool wholeGrid=false) {
    if(count<=0||rows<=0) return 0;
    i=std::clamp(i,0,count-1);
    const int first=i-i%rows,last=(std::min)(first+rows-1,count-1);
    switch(move) {
        case GridMove::up: return (std::max)(first,i-1);
        case GridMove::down: return (std::min)(last,i+1);
        case GridMove::left: return first>0?i-rows:i;
        case GridMove::right: return first+rows<count?(std::min)(i+rows,count-1):i;
        case GridMove::pageUp: return (std::max)(first,i-5);
        case GridMove::pageDown: return (std::min)(last,i+5);
        case GridMove::home: return wholeGrid?0:first;
        case GridMove::end: return wholeGrid?count-1:last;
    }
    return i;
}
}
