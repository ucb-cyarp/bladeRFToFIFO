//
// Created by Christopher Yarp on 11/1/21.
//

#ifndef BLADERFTOFIFO_RXTHREAD_H
#define BLADERFTOFIFO_RXTHREAD_H

#include <stdint.h>
#include <stdbool.h>

#include "helpers.h"

typedef struct{
    char *rxSharedName;

    int32_t blockLen;
    int32_t fifoSizeBlocks;

    volatile bool *stop; //Used to stop ADC/DAC in the event that the program is signaled (for orderly shutdown)
    bool print;

    //BladeRFParams
    struct bladerf *dev;
    SAMPLE_COMPONENT_DATATYPE fullRangeValue; //Will scale this to be 2047
    uint32_t bladeRFBlockLen; //Needs to be a multiple of 1024, example gives 8192
    uint32_t bladeRFNumBuffers; //Example gives 16
    uint32_t bladeRFNumTransfers;
} rxThreadArgs_t;

void* rxThread(void* uncastArgs);

#endif //BLADERFTOFIFO_RXTHREAD_H
