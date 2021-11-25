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
#include "rxThread.h"

// #define WRITE_RX_CSV

void* rxThread(void* uncastArgs){
    rxThreadArgs_t* args = (rxThreadArgs_t*) uncastArgs;
    char *rxSharedName = args->rxSharedName;

    int32_t blockLen = args->blockLen;
    int32_t fifoSizeBlocks = args->fifoSizeBlocks;
    bool print = args->print;

    volatile bool *stop = args->stop;

    struct bladerf *dev = args->dev;
    SAMPLE_COMPONENT_DATATYPE fullRangeValue = args->fullRangeValue;
    uint32_t bladeRFBlockLen = args->bladeRFBlockLen;
    uint32_t bladeRFNumBuffers = args->bladeRFNumBuffers;
    uint32_t bladeRFNumTransfers = args->bladeRFNumTransfers;

    SAMPLE_COMPONENT_DATATYPE scaleFactor = (SAMPLE_COMPONENT_DATATYPE) fullRangeValue / BLADERF_FULL_RANGE_VALUE;

    //Get the correction parameters
    //Scale down to a float
    SAMPLE_COMPONENT_DATATYPE dc_I = (SAMPLE_COMPONENT_DATATYPE) args->dcOffsetI;
    SAMPLE_COMPONENT_DATATYPE dc_Q = (SAMPLE_COMPONENT_DATATYPE) args->dcOffsetQ;
    double iq_A_dbl, iq_C_dbl, iq_D_dbl;
    getIQImbalCorrections(args->iqGain, args->iqPhase_deg, &iq_A_dbl, &iq_C_dbl, &iq_D_dbl);
    SAMPLE_COMPONENT_DATATYPE iq_A = (SAMPLE_COMPONENT_DATATYPE) iq_A_dbl;
    SAMPLE_COMPONENT_DATATYPE iq_C = (SAMPLE_COMPONENT_DATATYPE) iq_C_dbl;
    SAMPLE_COMPONENT_DATATYPE iq_D = (SAMPLE_COMPONENT_DATATYPE) iq_D_dbl;
    printf("Rx: DC Offset (I, Q)=(%5.2f, %5.2f), I/Q Imbalance (Gain, Phase.deg)=(%5.3f, %5.3f), Correction (A, C, D)=(%5.2f, %5.2f, %5.2f)\n", dc_I, dc_Q, args->iqGain, args->iqPhase_deg, iq_A, iq_C, iq_D);
	
    #ifdef WRITE_RX_CSV
        printf("Writing to ./bladeRF_rx.csv\n");
        FILE *rxCSV = fopen("./bladeRF_rx.csv", "w");
        fprintf(rxCSV, "re,im\n");
    #endif
	
    //---- Constants for opening FIFOs ----
    sharedMemoryFIFO_t rxFifo;

    initSharedMemoryFIFO(&rxFifo);

    size_t fifoBufferBlockSizeBytes = SAMPLE_SIZE*blockLen;
    size_t fifoBufferSizeBytes = fifoBufferBlockSizeBytes*fifoSizeBlocks;

    // printf("FIFO Block Size (Samples): %d\n", blockLen);
    // printf("FIFO Block Size (Bytes): %d\n", fifoBufferBlockSizeBytes);
    // printf("FIFO Buffer Size (Samples): %d\n", fifoSizeBlocks);
    // printf("FIFO Buffer Size (Bytes): %d\n", fifoBufferSizeBytes);

    //Initialize Producer FIFOs first to avoid deadlock
    producerOpenInitFIFO(rxSharedName, fifoBufferSizeBytes, &rxFifo);

    //Allocate Buffers
    SAMPLE_COMPONENT_DATATYPE* sharedMemFIFOSampBuffer = (SAMPLE_COMPONENT_DATATYPE*) vitis_aligned_alloc(MEM_ALIGNMENT, fifoBufferBlockSizeBytes);
    //While this array can be of "any reasonable size" according to https://www.nuand.com/bladeRF-doc/libbladeRF/v2.2.1/sync_no_meta.html,
    //will keep it the same as the requested bladeRF buffer lengths at the underlying bladeRF buffer length has to be filled in order to send samples down to the FPGA
    //The elements are complex 16 bit numbers (32 bits total)
    int16_t* bladeRFSampBuffer = (int16_t*) vitis_aligned_alloc(MEM_ALIGNMENT, sizeof(int16_t)*2*bladeRFBlockLen);

    SAMPLE_COMPONENT_DATATYPE *sharedMemFIFO_re = sharedMemFIFOSampBuffer;
    SAMPLE_COMPONENT_DATATYPE *sharedMemFIFO_im = sharedMemFIFOSampBuffer+blockLen;

    int status = bladerf_sync_config(dev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11,
                                     bladeRFNumBuffers, bladeRFBlockLen, bladeRFNumTransfers,
                                     1000);
    if (status != 0) {
        fprintf(stderr, "Failed to configure bladeRF Rx: %s\n",
                bladerf_strerror(status));
        exit(1);
    }

    //Start Rx
    status = bladerf_enable_module(dev, BLADERF_RX, true);
    if (status != 0) {
        fprintf(stderr, "Failed to enable bladeRF Rx: %s\n", bladerf_strerror(status));
        return NULL;
    }
    
    if(print){
        printf("Configured Rx\n");
        reportBladeRFChannelState(dev, false, 0);
    }
    //Main Loop

    //Get a block of samples from the bladeRF.  Process them by copying them to the shared memory buffer.  Write
    //to the FIFO as the buffer fills.  Do this until the bladeRF block is processed.  Save any remaining samples in the
    //shared memory buffer.
    int sharedMemPos = 0;
    while(!(*stop)){
        #ifdef DEBUG
        printf("About to read Rx samples from BladeRf\n");
        #endif
        //Get samples from bladeRF
        status = bladerf_sync_rx(dev, bladeRFSampBuffer, bladeRFBlockLen, NULL, 0);
        if (status != 0) {
            fprintf(stderr, "Failed bladeRF Rx: %s\n",
                    bladerf_strerror(status));
            return NULL;
        }
        #ifdef DEBUG
        printf("Read Rx samples from BladeRf\n");
        #endif

        int bladeRFBufferPos = 0;
        while(bladeRFBufferPos < bladeRFBlockLen) {
            //Find the number of samples to handle
            int remainingSamplesBladeRFToProcess = bladeRFBlockLen - bladeRFBufferPos;
            int remainingSharedMemorySpace = blockLen - sharedMemPos;
            int numToProcess = remainingSamplesBladeRFToProcess < remainingSharedMemorySpace ? remainingSamplesBladeRFToProcess : remainingSharedMemorySpace;
            #ifdef DEBUG
            printf("Rx Samples Being Processed: %d\n", numToProcess);
            #endif

            SAMPLE_COMPONENT_DATATYPE dcCorrectScaled_re[numToProcess];
            SAMPLE_COMPONENT_DATATYPE dcCorrectScaled_im[numToProcess];
            for(int i = 0; i<numToProcess; i++){
                dcCorrectScaled_re[i] = (((SAMPLE_COMPONENT_DATATYPE) bladeRFSampBuffer[2 * (bladeRFBufferPos + i)    ]) - dc_I) * scaleFactor;
                dcCorrectScaled_im[i] = (((SAMPLE_COMPONENT_DATATYPE) bladeRFSampBuffer[2 * (bladeRFBufferPos + i) + 1]) - dc_Q) * scaleFactor;
            }

            //IQ Correct & copy to shared memory buffer
            for(int i = 0; i<numToProcess; i++){
                // printf("Rx: %5d, %5d\n", bladeRFSampBuffer[2 * (bladeRFBufferPos+i)    ], bladeRFSampBuffer[2 * (bladeRFBufferPos+i) + 1]);
                sharedMemFIFO_re[sharedMemPos+i] = iq_A*dcCorrectScaled_re[i];
                sharedMemFIFO_im[sharedMemPos+i] = iq_C*dcCorrectScaled_re[i] + iq_D*dcCorrectScaled_im[i];
                // printf("Rx: %15.10f, %15.10f\n", sharedMemFIFO_re[sharedMemPos+i], sharedMemFIFO_im[sharedMemPos+i]);
            }

            sharedMemPos += numToProcess;
            bladeRFBufferPos += numToProcess;

            if(sharedMemPos >= blockLen) {
                //Write samples to rx pipe (ok to block)
                // printf("About to write samples to rx FIFO\n");
                #ifdef DEBUG
                printf("Sending Rx samples to Shared Memory FIFO\n");
                #endif
                writeFifo(sharedMemFIFOSampBuffer, fifoBufferBlockSizeBytes, 1, &rxFifo);
                sharedMemPos = 0;
                #ifdef DEBUG
                printf("Sent Rx samples to Shared Memory FIFO\n");
                #endif

                #ifdef WRITE_RX_CSV
                //Write to CSV too
                for(int i = 0; i<blockLen; i++){
                    fprintf(rxCSV, "%f,%f\n", sharedMemFIFO_re[i], sharedMemFIFO_im[i]);
                }
                #endif
            }
        }

        //Done processing bladeRF buffer
    }

    //Stop Rx
    status = bladerf_enable_module(dev, BLADERF_RX, false);
    if (status != 0) {
        fprintf(stderr, "Failed to stop bladeRF Rx: %s\n", bladerf_strerror(status));
        return NULL;
    }
    if(print){
        printf("BladeRF Rx Stopped");
    }

    #ifdef WRITE_RX_CSV
    fclose(rxCSV);
    #endif

    free(sharedMemFIFOSampBuffer);
    free(bladeRFSampBuffer);

    return NULL;
}