//
// Created by Christopher Yarp on 11/2/21.
//

#include <stdlib.h>
#include <memory.h>

void* vitis_aligned_alloc(size_t alignment, size_t size){
    size_t allocSize = size + (size%alignment == 0 ? 0 : alignment-(size%alignment));
    //There is a condition on aligned_alloc that the size must be a multiple of the alignment
    void* rtnVal = aligned_alloc(alignment, allocSize);
    return rtnVal;
}