//
// Created by Christopher Yarp on 11/2/21.
//

#ifndef BLADERFTOFIFO_HELPERS_H
#define BLADERFTOFIFO_HELPERS_H

#include <libbladeRF.h>

#include "math.h"

#define MEM_ALIGNMENT (64)
#define BLADERF_FULL_RANGE_VALUE (2047)
#define SAMPLE_COMPONENT_DATATYPE float
#define SAMPLE_SIZE (sizeof(SAMPLE_COMPONENT_DATATYPE)*2)
#define FEEDBACK_DATATYPE int32_t

#if SAMPLE_COMPONENT_DATATYPE == float
#define SAMPLE_ROUND_FCTN(X) (lroundf(X))
#define SAMPLE_STR2_FCTN(X) (strtof(X, NULL))
#elif SAMPLE_COMPONENT_DATATYPE == double
#define SAMPLE_ROUND_FCTN(X) (lround(X))
#define SAMPLE_STR2_FCTN(X) (strtod(X, NULL))
#else
#error "Provide round function for Sample Component Type"
#endif

// #define DEBUG

//Borrowed from Laminar/Vitis emit
void* vitis_aligned_alloc(size_t alignment, size_t size);

char* bladeRFGainModeToStr(bladerf_gain_mode mode);

char* bladeRFLoopbackModeToStr(bladerf_loopback mode);

void reportBladeRFChannelState(struct bladerf *dev, bool tx, int chanNum);

void getIQImbalCorrections(double iqGain, double iqPhase_deg, double* A, double* C, double* D);

#endif //BLADERFTOFIFO_HELPERS_H
