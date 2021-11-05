//
// Created by Christopher Yarp on 11/2/21.
//

#include <stdlib.h>
#include <memory.h>
#include <stdio.h>

#include "helpers.h"

void* vitis_aligned_alloc(size_t alignment, size_t size){
    size_t allocSize = size + (size%alignment == 0 ? 0 : alignment-(size%alignment));
    //There is a condition on aligned_alloc that the size must be a multiple of the alignment
    void* rtnVal = aligned_alloc(alignment, allocSize);
    return rtnVal;
}

char* bladeRFGainModeToStr(bladerf_gain_mode mode){
    switch(mode) {
        case BLADERF_GAIN_DEFAULT:
            return "BLADERF_GAIN_DEFAULT - Default";
        case BLADERF_GAIN_MGC:
            return "BLADERF_GAIN_MGC - Manual gain control";
        case BLADERF_GAIN_FASTATTACK_AGC:
            return "BLADERF_GAIN_FASTATTACK_AGC - Automatic gain control, fast attack (advanced)";
        case BLADERF_GAIN_SLOWATTACK_AGC:
            return "BLADERF_GAIN_SLOWATTACK_AGC - Automatic gain control, slow attack (advanced)";
        case BLADERF_GAIN_HYBRID_AGC:
            return "BLADERF_GAIN_HYBRID_AGC - Automatic gain control, hybrid attack (advanced)";
        default:
            return "UNKNOWN";
    }
}

char* bladeRFLoopbackModeToStr(bladerf_loopback mode){
    switch(mode) {
        case BLADERF_LB_NONE:
            return "BLADERF_LB_NONE - Disabled (Normal Operation)";
        case BLADERF_LB_FIRMWARE:
            return "BLADERF_LB_FIRMWARE - Firmware Loopback";
        case BLADERF_LB_RFIC_BIST:
            return "BLADERF_LB_RFIC_BIST - RFIC Loopback";
        //There are more for other bladeRFs but we are using bladeRF 2.0 Micro
        default:
            return "UNKNOWN";
    }
}

void reportBladeRFChannelState(struct bladerf *dev, bool tx, int chanNum){
    bladerf_channel chan = tx ? BLADERF_CHANNEL_TX(chanNum) : BLADERF_CHANNEL_RX(chanNum);
    char chanHelpStr[5];
    snprintf(chanHelpStr, 5, tx ? "Tx%d" : "Rx%d", chanNum);

    bladerf_frequency reportedFreq;
    int status = bladerf_get_frequency(dev, chan, &reportedFreq);
    if (status != 0) {
        fprintf(stderr, "Failed to get %s frequency: %s\n", chanHelpStr, bladerf_strerror(status));
        exit(1);
    }

    bladerf_bandwidth reportedBW;
    status = bladerf_get_bandwidth(dev, chan, &reportedBW);
    if (status != 0) {
        fprintf(stderr, "Failed to get %s bandwidth: %s\n", chanHelpStr, bladerf_strerror(status));
        exit(1);
    }

    bladerf_sample_rate reportedSampRate;
    status = bladerf_get_sample_rate(dev, chan, &reportedSampRate);
    if (status != 0) {
        fprintf(stderr, "Failed to get %s sample rate: %s\n", chanHelpStr, bladerf_strerror(status));
        exit(1);
    }

    bladerf_gain_mode reportedGainMode;
    if(!tx){
        status = bladerf_get_gain_mode(dev, chan, &reportedGainMode);
        if (status != 0) {
            fprintf(stderr, "Failed to get %s gain mode: %s\n", chanHelpStr, bladerf_strerror(status));
            exit(1);
        }
    }

    bladerf_gain reportedGain;
    status = bladerf_get_gain(dev, chan, &reportedGain);
    if (status != 0) {
        fprintf(stderr, "Failed to get %s gain: %s\n", chanHelpStr, bladerf_strerror(status));
        exit(1);
    }

    printf("[%s] Freq      (Hz): %10lu\n", chanHelpStr, reportedFreq);
    printf("[%s] BW        (Hz): %10u\n", chanHelpStr, reportedBW);
    printf("[%s] Samp Rate (Hz): %10u\n", chanHelpStr, reportedSampRate);
    if(!tx){
        printf("[%s] AGC Mode      : %s\n", chanHelpStr, bladeRFGainModeToStr(reportedGainMode));
    }
    printf("[%s] Gain      (dB): %10u\n", chanHelpStr, reportedGain);
}