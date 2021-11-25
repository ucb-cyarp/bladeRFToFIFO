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

    //Get the pre-distortion parameters
    //Scale down to a float
    float dc_I = (float) args->dcOffsetI;
    float dc_Q = (float) args->dcOffsetQ;
    double iq_A_dbl, iq_C_dbl, iq_D_dbl;
    getIQImbalCorrections(args->iqGain, args->iqPhase_deg, &iq_A_dbl, &iq_C_dbl, &iq_D_dbl);
    float iq_A = (float) iq_A_dbl;
    float iq_C = (float) iq_C_dbl;
    float iq_D = (float) iq_D_dbl;
    printf("Tx: DC Offset (I, Q)=(%5.2f, %5.2f), I/Q Imbalance (Gain, Phase.deg)=(%5.3f, %5.3f), Correction (A, C, D)=(%5.2f, %5.2f, %5.2f)\n", dc_I, dc_Q, args->iqGain, args->iqPhase_deg, iq_A, iq_C, iq_D);

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
    SAMPLE_COMPONENT_DATATYPE* sharedMemFIFOSampBuffer = (SAMPLE_COMPONENT_DATATYPE*) vitis_aligned_alloc(MEM_ALIGNMENT, fifoBufferBlockSizeBytes);
    //While this array can be of "any reasonable size" according to https://www.nuand.com/bladeRF-doc/libbladeRF/v2.2.1/sync_no_meta.html,
    //will keep it the same as the requested bladeRF buffer lengths at the underlying bladeRF buffer length has to be filled in order to send samples down to the FPGA
    //The elements are complex 16 bit numbers (32 bits total)
    int16_t* bladeRFSampBuffer = (int16_t*) vitis_aligned_alloc(MEM_ALIGNMENT, sizeof(int16_t)*2*bladeRFBlockLen);

    SAMPLE_COMPONENT_DATATYPE *sharedMemFIFO_re = sharedMemFIFOSampBuffer;
    SAMPLE_COMPONENT_DATATYPE *sharedMemFIFO_im = sharedMemFIFOSampBuffer+blockLen;

    int status = bladerf_sync_config(dev, BLADERF_TX_X1, BLADERF_FORMAT_SC16_Q11,
                                     bladeRFNumBuffers, bladeRFBlockLen, bladeRFNumTransfers,
                                 0);
    if (status != 0) {
        fprintf(stderr, "Failed to configure bladeRF Tx: %s\n",
                bladerf_strerror(status));
        exit(1);
    }

    //Start Tx
    status = bladerf_enable_module(dev, BLADERF_TX, true);
    if (status != 0) {
        fprintf(stderr, "Failed to start bladeRF Tx: %s\n", bladerf_strerror(status));
        return NULL;
    }

    //Main Loop
    if(print){
        printf("Configured Tx\n");
        reportBladeRFChannelState(dev, true, 0);
    }

    bool running = true;
    int bladeRFBufferPos = 0;
    while(running && !(*stop)){
        //Get samples from tx FIFO (ok to block)
        #ifdef DEBUG
        printf("About to read Tx samples from Shared Memory FIFO\n");
        #endif
        int samplesRead = readFifo(sharedMemFIFOSampBuffer, fifoBufferBlockSizeBytes, 1, &txFifo);
        if (samplesRead != 1) {
            //Done!
            running = false;
            break;
        }
        #ifdef DEBUG
        printf("Read Tx samples from Shared Memory FIFO\n");
        #endif

        //Copy to bladeRF buffer, and sync (if filled a full buffer)
        //Do this until all data from shared memory FIFO has been consumed - keep any remainder
        int sharedMemPos = 0;
        while(sharedMemPos<blockLen) {
            //Find the number of samples to handle
            int remainingSamplesBladeRFSpace = bladeRFBlockLen - bladeRFBufferPos;
            int remainingSharedMemoryToProcess = blockLen - sharedMemPos;
            int numToProcess = remainingSamplesBladeRFSpace < remainingSharedMemoryToProcess ? remainingSamplesBladeRFSpace : remainingSharedMemoryToProcess;
            #ifdef DEBUG
            printf("Tx Samples Being Processed: %d\n", numToProcess);
            #endif

            //Predistort Here for I/Q Imbalance
            float iqPredistort_re[numToProcess];
            float iqPredistort_im[numToProcess];
            for (int i = 0; i < numToProcess; i++) {
                iqPredistort_re[i] = iq_A*sharedMemFIFO_re[sharedMemPos+i];
                iqPredistort_im[i] = iq_C*sharedMemFIFO_re[sharedMemPos+i] + iq_D*sharedMemFIFO_im[sharedMemPos+i];
            }

            //Scale and Subtract DC Offset, then Round
            //Copy to bladeRF buffer and perform interleave
            long scaled_re[numToProcess];
            long scaled_im[numToProcess];
            for (int i = 0; i < numToProcess; i++) {
                scaled_re[i] = SAMPLE_ROUND_FCTN(iqPredistort_re[i] * scaleFactor - dc_I);
                scaled_im[i] = SAMPLE_ROUND_FCTN(iqPredistort_im[i] * scaleFactor - dc_Q);
            }

            long scaled_thresh_re[numToProcess];
            long scaled_thresh_im[numToProcess];
            for (int i = 0; i < numToProcess; i++) {
                scaled_thresh_re[i] = scaled_re[i];
                scaled_thresh_im[i] = scaled_im[i];
                if (saturate) {
                    if (scaled_thresh_re[i] > BLADERF_FULL_RANGE_VALUE) {
                        scaled_thresh_re[i] = BLADERF_FULL_RANGE_VALUE;
                    } else if (scaled_thresh_re[i] < -BLADERF_FULL_RANGE_VALUE) {
                        scaled_thresh_re[i] = -BLADERF_FULL_RANGE_VALUE;
                    }

                    if (scaled_thresh_im[i] > BLADERF_FULL_RANGE_VALUE) {
                        scaled_thresh_im[i] = BLADERF_FULL_RANGE_VALUE;
                    } else if (scaled_thresh_im[i] < -BLADERF_FULL_RANGE_VALUE) {
                        scaled_thresh_im[i] = -BLADERF_FULL_RANGE_VALUE;
                    }
                }
            }

            for (int i = 0; i < numToProcess; i++) {
                bladeRFSampBuffer[2 * (bladeRFBufferPos+i)    ] = (int16_t) scaled_thresh_re[i];
                bladeRFSampBuffer[2 * (bladeRFBufferPos+i) + 1] = (int16_t) scaled_thresh_im[i];
                // printf("Tx: %5d, %5d\n", bladeRFSampBuffer[2 * (bladeRFBufferPos+i)    ], bladeRFSampBuffer[2 * (bladeRFBufferPos+i) + 1]);
            }

            sharedMemPos += numToProcess;
            bladeRFBufferPos += numToProcess;

            if(bladeRFBufferPos>=bladeRFBlockLen){
                #ifdef DEBUG
                printf("Tx Samples Being Sent to BladeRF, bladeRFBlockLen: %d\n", bladeRFBlockLen);
                #endif
                //Filled the bladeRF buffer
                status = bladerf_sync_tx(dev, bladeRFSampBuffer, bladeRFBlockLen, NULL, 0);
                if(status != 0){
                    fprintf(stderr, "Failed BladeRF Tx: %s\n", bladerf_strerror(status));
                    return NULL;
                }
                #ifdef DEBUG
                printf("Tx Samples Sent to BladeRF\n");
                #endif

                bladeRFBufferPos = 0;
            }

        }//Finished processing block from

        #ifdef DEBUG
        printf("Sending Feedback Token for Tx\n");
        #endif
        //Send feedback to TX so that it can send more
        FEEDBACK_DATATYPE tokensReturned = 1;
        writeFifo(&tokensReturned, txfbFifoBufferBlockSizeBytes, 1, &txfbFifo);
        #ifdef DEBUG
        printf("Sent Feedback Token for Tx\n");
        #endif
    }

    //Stop Tx
    status = bladerf_enable_module(dev, BLADERF_TX, false);
    if (status != 0) {
        fprintf(stderr, "Failed to stop bladeRF Tx: %s\n", bladerf_strerror(status));
        return NULL;
    }
    if(print){
        printf("BladeRF Tx Stopped");
    }

    free(sharedMemFIFOSampBuffer);
    free(bladeRFSampBuffer);

    return NULL;
}