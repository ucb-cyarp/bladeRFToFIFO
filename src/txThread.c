//
// Created by Christopher Yarp on 11/1/21.
//

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <stdatomic.h>

#include <libbladeRF.h>

#include "depends/BerkeleySharedMemoryFIFO.h"
#include "txThread.h"
#include "helpers.h"

void* txThread(void* uncastArgs){
    txThreadArgs_t* args = (txThreadArgs_t*) uncastArgs;
    char *txSharedName = args->txSharedName;
    char *txFeedbackSharedName = args->txFeedbackSharedName;
    volatile bool *stop = args->stop;

    int32_t blockLen = args->blockLen;
    int32_t fifoSizeBlocks = args->fifoSizeBlocks;

    bool print = args->print;

    struct bladerf *dev = args->dev;
    SAMPLE_COMPONENT_DATATYPE fullRangeValue = args->fullRangeValue; //Will scale this to be 2047
    bool saturate = args->saturate;
    uint32_t bladeRFBlockLen = args->bladeRFBlockLen;
    uint32_t bladeRFNumBuffers = args->bladeRFNumBuffers;
    uint32_t bladeRFNumTransfers = args->bladeRFNumTransfers;

    SAMPLE_COMPONENT_DATATYPE scaleFactor = (SAMPLE_COMPONENT_DATATYPE) BLADERF_FULL_RANGE_VALUE / fullRangeValue;

    //---- Constants for opening FIFOs ----
    sharedMemoryFIFO_t txFifo;
    sharedMemoryFIFO_t txfbFifo;

    initSharedMemoryFIFO(&txFifo);
    initSharedMemoryFIFO(&txfbFifo);

    size_t fifoBufferBlockSizeBytes = SAMPLE_SIZE*blockLen;
    size_t fifoBufferSizeBytes = fifoBufferBlockSizeBytes*fifoSizeBlocks;
    size_t txfbFifoBufferBlockSizeBytes = sizeof(FEEDBACK_DATATYPE); //This does not get sent in blocks, it gets sent as a single FEEDBACK_DATATYPE per transaction
    size_t txfbFifoBufferSizeBytes = txfbFifoBufferBlockSizeBytes*fifoSizeBlocks;

    //Initialize Producer FIFOs first to avoid deadlock
    producerOpenInitFIFO(txFeedbackSharedName, txfbFifoBufferSizeBytes, &txfbFifo);
    consumerOpenFIFOBlock(txSharedName, fifoBufferSizeBytes, &txFifo);

    //Allocate Buffers
    SAMPLE_COMPONENT_DATATYPE* sharedMemFIFOSampBuffer = (SAMPLE_COMPONENT_DATATYPE*) vitis_aligned_alloc(MEM_ALIGNMENT, sizeof(SAMPLE_COMPONENT_DATATYPE)*2*blockLen);
    //While this array can be of "any reasonable size" according to https://www.nuand.com/bladeRF-doc/libbladeRF/v2.2.1/sync_no_meta.html,
    //will keep it the same as the requested bladeRF buffer lengths at the underlying bladeRF buffer length has to be filled in order to send samples down to the FPGA
    //The elements are complex 16 bit numbers (32 bits total)
    SAMPLE_COMPONENT_DATATYPE* bladeRFSampBuffer = (SAMPLE_COMPONENT_DATATYPE*) vitis_aligned_alloc(MEM_ALIGNMENT, sizeof(int16_t)*2*bladeRFBlockLen);

    SAMPLE_COMPONENT_DATATYPE *sharedMemFIFO_re = sharedMemFIFOSampBuffer;
    SAMPLE_COMPONENT_DATATYPE *sharedMemFIFO_im = sharedMemFIFOSampBuffer+blockLen;

    //Configure
    //NOTE: This is where SISO (1 Tx) or MIMO (2 Rx) is declared
    //The format is defined in https://www.nuand.com/bladeRF-doc/libbladeRF/v2.2.1/group___s_t_r_e_a_m_i_n_g___f_o_r_m_a_t.html#ga4c61587834fd4de51a8e2d34e14a73b2
    //It is called SC16_Q11, presumably signed complex 16 bit numbers, Q11 fixed point
    //  From some cursory research, it appears that Q formats can be somewhat ambiguous as different conventions have been used over time
    //  Based on the description in the manual (range and that this is the native format of the ADC/DAC,
    //  it looks like 11 bits (not including sign bit) are used with no fractional component (there was no dot in the Q format description).
    //  This would yield the range of [-2048, 2048) and a 12 bit 2's complement number - the number of bits used by the AD9361.
    //  This can be further corroborated by looking at the src for the bladeRF-cli tx command (https://github.com/Nuand/bladeRF/blob/master/host/utilities/bladeRF-cli/src/cmd/tx.c)
    //  Looking at tx_csv_to_sc16q11, the input values are clamped between SC16Q11_IQ_MIN and SC16Q11_IQ_MAX
    //  SC16Q11_IQ_MIN is defined as -2048 and SC16Q11_IQ_MAX is defined as 2047
    //  On the Rx side, the value is masked and sign extended to 16 bits in sc16q11_sample_fixup (https://github.com/Nuand/bladeRF/blob/master/host/utilities/bladeRF-cli/src/cmd/rx.c).
    //  It is, however, not clear if this is actually necessary because the ADI documentation (https://wiki.analog.com/resources/fpga/docs/axi_ad9361#internal_interface_description)
    //  states that the Rx values are sign extended in the ADI IP.  It looks like the ADI IP is instantiated in a Qsys system (the Altera/Intel equivalent of the Xilinx IP Integrator)
    //  https://github.com/Nuand/bladeRF/blob/d1c382779f00c30bac90ca4f993d5d74f899b937/hdl/fpga/platforms/bladerf-micro/build/nios_system.tcl instantiated in
    //  https://github.com/Nuand/bladeRF/blob/master/hdl/fpga/platforms/bladerf-micro/vhdl/bladerf-hosted.vhd.  I believe the IP being instantiated is the ADI IP at
    //  https://github.com/Nuand/bladeRF/blob/master/hdl/fpga/ip/analogdevicesinc/hdl/library/axi_ad9361/axi_ad9361.v
    //
    //  Some descriptions of the Q format descriptor indicate it assumes a sign bit but many systems currently use a 2's
    //  complement representation.  Unfortunately, the datasheet for the AD9361 does not provide much clarity on this point.
    //  A forum post https://ez.analog.com/fpga/f/q-a/31795/ad9361-data-format-digital-interface points
    //  to https://wiki.analog.com/resources/fpga/docs/axi_ad9361#internal_interface_description alludes to 2's complement with the concept
    //  of sign extension.
    //
    //The upper 16 bits are Q
    //The lower 16 bits are I
    //The integer range [-2048, 2048) maps to [-1.0, 1.0)

    //To avoid the asymmetry of the 2's complement representation, I will map to [-2047, 2047] inclusive

    //When in MIMO mode, the samples from the different channels are interleaved.
    int status = bladerf_sync_config(dev, BLADERF_TX_X1, BLADERF_FORMAT_SC16_Q11,
                                     bladeRFNumBuffers, bladeRFBlockLen, bladeRFNumTransfers,
                                 0);
    if (status != 0) {
        fprintf(stderr, "Failed to configure bladeRF Tx: %s\n",
                bladerf_strerror(status));
        return NULL;
    }

    //Start Tx
    status = bladerf_enable_module(dev, BLADERF_TX, true);
    if (status != 0) {
        fprintf(stderr, "Failed to start bladeRF Tx: %s\n", bladerf_strerror(status));
        return NULL;
    }

    //Main Loop
    bool running = true;
    int bladeRFBufferPos = 0;
    while(running && !(*stop)){
        //Get samples from tx FIFO (ok to block)
        // printf("About to read samples\n");
        int samplesRead = readFifo(sharedMemFIFOSampBuffer, fifoBufferBlockSizeBytes, 1, &txFifo);
        if (samplesRead != 1) {
            //Done!
            running = false;
            break;
        }
        // printf("Read Tx FIFO\n");

        //Copy to bladeRF buffer, and sync (if filled a full buffer)
        //Do this until all data from shared memory FIFO has been consumed - keep any remainder
        int sharedMemPos = 0;
        while(sharedMemPos<blockLen) {
            //Find the number of samples to handle
            int remainingSamplesBladeRFSpace = bladeRFBlockLen - bladeRFBufferPos;
            int remainingSharedMemoryToProcess = blockLen - sharedMemPos;
            int numToProcess = remainingSamplesBladeRFSpace < remainingSharedMemoryToProcess ? remainingSamplesBladeRFSpace : remainingSharedMemoryToProcess;

            //Copy to bladeRF buffer and perform interleave
            long scaled_re[numToProcess];
            long scaled_im[numToProcess];
            for (int i = 0; i < numToProcess; i++) {
                scaled_re[i] = SAMPLE_ROUND_FCTN(sharedMemFIFO_re[sharedMemPos+i] * scaleFactor);
                scaled_im[i] = SAMPLE_ROUND_FCTN(sharedMemFIFO_im[sharedMemPos+i] * scaleFactor);
            }

            long scaled_thresh_re[numToProcess];
            long scaled_thresh_im[numToProcess];
            for (int i = 0; i < numToProcess; i++) {
                if (saturate) {
                    if (scaled_re[i] > BLADERF_FULL_RANGE_VALUE) {
                        scaled_thresh_re[i] = BLADERF_FULL_RANGE_VALUE;
                    } else if (scaled_re[i] < -BLADERF_FULL_RANGE_VALUE) {
                        scaled_thresh_re[i] = -BLADERF_FULL_RANGE_VALUE;
                    } else {
                        scaled_thresh_re[i] = scaled_re[i];
                    }

                    if (scaled_im[i] > BLADERF_FULL_RANGE_VALUE) {
                        scaled_thresh_im[i] = BLADERF_FULL_RANGE_VALUE;
                    } else if (scaled_im[i] < -BLADERF_FULL_RANGE_VALUE) {
                        scaled_thresh_im[i] = -BLADERF_FULL_RANGE_VALUE;
                    } else {
                        scaled_thresh_im[i] = scaled_im[i];
                    }
                }
            }

            for (int i = 0; i < blockLen; i++) {
                bladeRFSampBuffer[2 * (bladeRFBufferPos+i)    ] = (int16_t) scaled_thresh_re[i];
                bladeRFSampBuffer[2 * (bladeRFBufferPos+i) + 1] = (int16_t) scaled_thresh_im[i];
            }

            sharedMemPos += numToProcess;
            bladeRFBufferPos += numToProcess;

            if(bladeRFBufferPos>=bladeRFBlockLen){
                //Filled the bladeRF buffer
                status = bladerf_sync_tx(dev, bladeRFSampBuffer, bladeRFBlockLen, NULL, 0);
                if(status != 0){
                    fprintf(stderr, "Failed BladeRF Tx: %s\n", bladerf_strerror(status));
                    return NULL;
                }
                bladeRFBufferPos = 0;
            }

        }//Finished processing block from

        //Send feedback to TX so that it can send more
        FEEDBACK_DATATYPE tokensReturned = 1;
        writeFifo(&tokensReturned, txfbFifoBufferBlockSizeBytes, 1, &txfbFifo);
        // printf("Wrote token\n");
    }

    //Stop Tx
    status = bladerf_enable_module(dev, BLADERF_TX, false);
    if (status != 0) {
        fprintf(stderr, "Failed to stop bladeRF Tx: %s\n", bladerf_strerror(status));
        return NULL;
    }

    free(sharedMemFIFOSampBuffer);
    free(bladeRFSampBuffer);

    return NULL;
}