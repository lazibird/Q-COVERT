//
//
//

#ifndef DOUBLE_ORAM_RAND_INTERFACE_H
#define DOUBLE_ORAM_RAND_INTERFACE_H

using namespace std;

namespace double_oram {

class RandForOramInterface {
public:
    virtual int getRandomLeaf() {return 0;};
    virtual int getRandomLeafWithBound(int b){return 0;};

    virtual void setBound(int num_leaves) {};
};

} // double_oram 命名空间


#endif 
