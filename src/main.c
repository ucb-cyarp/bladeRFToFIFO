#define _GNU_SOURCE //Need extra functions from sched.h to set thread affinity
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>

#include <libbladeRF.h>

#include "rxThread.h"
#include "txThread.h"

#define MAX_SERIAL_NUM_STRLEN (100)

volatile bool stop = false; //Shared variable to indicate that the radio should be stopped.  Modified by signal handler

void printHelp(){
    printf("bladeRFToFIFO <-rx rx.pipe> <-tx tx.pipe -txfb tx_feedback.pipe>\n");
    printf("\n");
    printf("Optional Arguments:\n");
    printf("-rx: Path to the Rx Pipe\n");
    printf("-tx: Path to the Tx Pipe\n");
    printf("-txfb: Path to the Tx Feedback Pipe (required if -tx is present)\n");
    printf("-blocklen: Block length in samples (for SharedMemoryFIFO interface)\n");
    printf("-fifosize: Size of the FIFO in blocks (for SharedMemoryFIFO interface)\n");
    printf("-txFreq: Carrier Frequency of the Tx (Hz)\n");
    printf("-rxFreq: Carrier Frequency of the Rx (Hz)\n");
    printf("-txSampRate: Sample Rate of Tx (Hz)\n");
    printf("-rxSampRate: Sample Rate of Rx (Hz)\n");
    printf("-txBW: Bandwidth of Tx (Hz)\n");
    printf("-rxBW: Bandwidth of Rx (Hz)\n");
    printf("-txGain: Gain of the Tx (dB)\n");
    printf("-rxGain: Gain of the Rx (dB)\n");
    printf("-fullScale: The full scale value of samples transacted over the Shared Memory FIFOs\n");
    printf("-saturate: Indicates that Tx values beyond full scale are saturated\n");
    printf("-txCpu: CPU to run this application on (Tx side)\n");
    printf("-rxCpu: CPU to run this application on (Rx side)\n");
    printf("-txSerialNum: Serial Number of BladeRF Board Used for Tx\n");
    printf("-rxSerialNum: Serial Number of BladeRF Board Used for Tx\n");
    printf("-txDCOffsetI: Measured DC Offset for Tx I Channel (DAC Scale [-2048, 2047])\n");
    printf("-txDCOffsetQ: Measured DC Offset for Tx Q Channel (DAC Scale [-2048, 2047])\n");
    printf("-rxDCOffsetI: Measured DC Offset for Rx I Channel (ADC Scale [-2048, 2047])\n");
    printf("-rxDCOffsetQ: Measured DC Offset for Rx Q Channel (ADC Scale [-2048, 2047])\n");
    printf("-txIQGain: Measured IQ Gain Imbalance at the Tx (Ratio)\n");
    printf("-txIQPhase: Measured IQ Phase Imbalance at the Tx (Degree)\n");
    printf("-rxIQGain: Measured IQ Gain Imbalance at the Tx (Ratio)\n");
    printf("-rxIQPhase: Measured IQ Phase Imbalance at the Tx (Degree)\n");
    printf("-v: verbose\n");
}

void signal_handler(int code){
    stop = true;
}

void openBladeRF(struct bladerf **dev, char* serialNum){
    //From BladeRF Boilerplate:

    struct bladerf_devinfo dev_info;
    bladerf_init_devinfo(&dev_info);
    //Can specify the serial number of the bladeRF device here
    //TODO: Remove Sanity Check
    if(sizeof(dev_info.serial) != strlen(serialNum)+1){ //Includes Null Char
        fprintf(stderr, "Invalid Serial Number: %s\n", serialNum);
        exit(1);
    }
    strncpy(dev_info.serial, serialNum, sizeof(dev_info.serial) - 1);

    int status = bladerf_open_with_devinfo(dev, &dev_info);
    if (status != 0) {
        fprintf(stderr, "Unable to open bladeRF device: %s\n", bladerf_strerror(status));
        exit(1);
    }
}

void configBladeRFChannel(struct bladerf *dev, bool tx, int chanNum, bladerf_frequency carrierFreqHz, bladerf_bandwidth bandwidthHz, bladerf_sample_rate sampleRateHz, bladerf_gain gainDB, bool verbose){
    bladerf_channel chan = tx ? BLADERF_CHANNEL_TX(chanNum) : BLADERF_CHANNEL_RX(chanNum);
    char chanHelpStr[5];
    snprintf(chanHelpStr, 5, tx ? "Tx%d" : "Rx%d", chanNum);

    //According to https://www.nuand.com/bladeRF-doc/libbladeRF/v2.2.1/group___f_n___t_u_n_i_n_g.html#ga4e9b635f18a9531bcd3c6b4d2dd8a4e0
    //  changing one of them will change the other
    int status = bladerf_set_frequency(dev, chan, carrierFreqHz);
    if (status != 0) {
        fprintf(stderr, "Failed to set %s frequency = %lu: %s\n", chanHelpStr, carrierFreqHz, bladerf_strerror(status));
        exit(1);
    }

    bladerf_frequency reportedFreq;
    status = bladerf_get_frequency(dev, chan, &reportedFreq);
    if (status != 0) {
        fprintf(stderr, "Failed to get %s frequency: %s\n", chanHelpStr, bladerf_strerror(status));
        exit(1);
    }

    bladerf_bandwidth actualBW;
    status = bladerf_set_bandwidth(dev, chan, bandwidthHz, &actualBW);
    if (status != 0) {
        fprintf(stderr, "Failed to set %s bandwidth = %u: %s\n", chanHelpStr, bandwidthHz, bladerf_strerror(status));
        exit(1);
    }

    bladerf_sample_rate actualSampRate;
    status = bladerf_set_sample_rate(dev, chan, sampleRateHz, &actualSampRate);
    if (status != 0) {
        fprintf(stderr, "Failed to set %s sample rate = %u: %s\n", chanHelpStr, sampleRateHz, bladerf_strerror(status));
        exit(1);
    }

    //Turn AGC Off
    bladerf_gain_mode gainMode;
    bladerf_gain_mode reportedGainMode;
    if(!tx){
        gainMode = BLADERF_GAIN_MGC;
        status = bladerf_set_gain_mode(dev, chan, gainMode);
        if (status != 0) {
            fprintf(stderr, "Failed to set %s AGC = %s: %s\n", chanHelpStr, bladeRFGainModeToStr(gainMode), bladerf_strerror(status));
            exit(1);
        }

        status = bladerf_get_gain_mode(dev, chan, &reportedGainMode);
        if (status != 0) {
            fprintf(stderr, "Failed to get %s gain mode: %s\n", chanHelpStr, bladerf_strerror(status));
            exit(1);
        }
    }

    status = bladerf_set_gain(dev, chan, gainDB);
    if (status != 0) {
        fprintf(stderr, "Failed to set %s gain = %u: %s\n", chanHelpStr, gainDB, bladerf_strerror(status));
        exit(1);
    }

    bladerf_gain reportedGain;
    status = bladerf_get_gain(dev, chan, &reportedGain);
    if (status != 0) {
        fprintf(stderr, "Failed to get %s gain: %s\n", chanHelpStr, bladerf_strerror(status));
        exit(1);
    }


    if(verbose){
        printf("[%s] Freq      Requested: %10lu, Reported:  %10lu\n", chanHelpStr, carrierFreqHz, reportedFreq);
        printf("[%s] BW        Requested: %10u, Currently: %10u\n", chanHelpStr, bandwidthHz, actualBW);
        printf("[%s] Samp Rate Requested: %10u, Currently: %10u\n", chanHelpStr, sampleRateHz, actualSampRate);
        if(!tx){
            printf("[%s] AGC: \n\tRequested %s\n\tReported: %s\n", chanHelpStr, bladeRFGainModeToStr(gainMode), bladeRFGainModeToStr(reportedGainMode));
        }
        printf("[%s] Gain      Requested: %10u, Reported:  %10u\n", chanHelpStr, gainDB, reportedGain);
    }
}

int main(int argc, char **argv) {
    //--- Parse the arguments ---
    char *txSharedName = NULL;
    char *txFeedbackSharedName = NULL;
    char *rxSharedName = NULL;

    int32_t blockLen = 1;
    int32_t fifoSize = 8;

    int txCpu = -1;
    int rxCpu = -1;

    bool print;

    //RF Params
    int txGain = 0;
    int rxGain = 0;

    unsigned long txFreq = 2400000000;
    unsigned long rxFreq = 2400000000;

    unsigned int txSampRate = 61440000;
    unsigned int rxSampRate = 61440000;

    unsigned int txBW = 56000000;
    unsigned int rxBW = 56000000;

    SAMPLE_COMPONENT_DATATYPE fullScaleValue = 1;
    bool saturate = false;

    char txSerial[MAX_SERIAL_NUM_STRLEN+1] = "";
    char rxSerial[MAX_SERIAL_NUM_STRLEN+1] = "";

    //I/Q and DC Offset Corrections
    double txDCOffsetI = 0;
    double txDCOffsetQ = 0;
    double rxDCOffsetI = 0;
    double rxDCOffsetQ = 0;
    double txIQGain = 1;
    double txIQPhase_deg = 0;
    double rxIQGain = 1;
    double rxIQPhase_deg = 0;

    if (argc < 2) {
        printHelp();
    }

    for (int i = 1; i < argc; i++) {
        //#### Shared Memory FIFO Params
        if (strcmp("-rx", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxSharedName = argv[i];
            } else {
                printf("Missing argument for -rx\n");
                exit(1);
            }
        } else if (strcmp("-tx", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txSharedName = argv[i];
            } else {
                printf("Missing argument for -tx\n");
                exit(1);
            }
        } else if (strcmp("-txfb", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txFeedbackSharedName = argv[i];
            } else {
                printf("Missing argument for -txfb\n");
                exit(1);
            }
        } else if (strcmp("-blocklen", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                blockLen = strtol(argv[i], NULL, 10);
                if (blockLen <= 1) {
                    printf("-blocklen must be positive\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -blocklen\n");
                exit(1);
            }
        } else if (strcmp("-fifosize", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                fifoSize = strtol(argv[i], NULL, 10);
                if (blockLen <= 1) {
                    printf("-fifosize must be positive\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -fifosize\n");
                exit(1);
            }
        //#### RF Properties
        } else if (strcmp("-txGain", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txGain = strtol(argv[i], NULL, 10);
            } else {
                printf("Missing argument for -txGain\n");
                exit(1);
            }
        } else if (strcmp("-rxGain", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxGain = strtol(argv[i], NULL, 10);
            } else {
                printf("Missing argument for -rxGain\n");
                exit(1);
            }
        } else if (strcmp("-txFreq", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txFreq = strtol(argv[i], NULL, 10);
                if (txFreq <= 1) {
                    printf("-txFreq must be positive\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -txFreq\n");
                exit(1);
            }
        } else if (strcmp("-rxFreq", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxFreq = strtol(argv[i], NULL, 10);
                if (txFreq <= 1) {
                    printf("-rxFreq must be positive\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -rxFreq\n");
                exit(1);
            }
        } else if (strcmp("-txBW", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txBW = strtol(argv[i], NULL, 10);
                if (txBW <= 1) {
                    printf("-txBW must be positive\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -txBW\n");
                exit(1);
            }
        } else if (strcmp("-rxBW", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxBW = strtol(argv[i], NULL, 10);
                if (rxBW <= 1) {
                    printf("-rxBW must be positive\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -rxBW\n");
                exit(1);
            }
        } else if (strcmp("-txSampRate", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txSampRate = strtol(argv[i], NULL, 10);
                if (txSampRate <= 1) {
                    printf("-txSampRate must be positive\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -txSampRate\n");
                exit(1);
            }
        } else if (strcmp("-rxSampRate", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxSampRate = strtol(argv[i], NULL, 10);
                if (rxSampRate <= 1) {
                    printf("-rxSampRate must be positive\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -rxSampRate\n");
                exit(1);
            }
        } else if (strcmp("-fullScale", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                fullScaleValue = SAMPLE_STR2_FCTN(argv[i]);
                if (fullScaleValue <= 0) {
                    printf("-fullScale must be positive\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -fullScale\n");
                exit(1);
            }
        } else if (strcmp("-saturate", argv[i]) == 0) {
            saturate = true;
            //#### CPUs
        } else if (strcmp("-txCpu", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txCpu = strtol(argv[i], NULL, 10);
                if (txCpu <= 0) {
                    printf("-txCpu must be non-negative\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -txCpu\n");
                exit(1);
            }
        } else if (strcmp("-rxCpu", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxCpu = strtol(argv[i], NULL, 10);
                if (rxCpu <= 0) {
                    printf("-rxCpu must be non-negative\n");
                    exit(1);
                }
            } else {
                printf("Missing argument for -rxCpu\n");
                exit(1);
            }
        //#### Serial Numbers
        } else if (strcmp("-txSerialNum", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                strncpy(txSerial, argv[i], MAX_SERIAL_NUM_STRLEN);
            } else {
                printf("Missing argument for -txSerialNum\n");
                exit(1);
            }
        } else if (strcmp("-rxSerialNum", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                strncpy(rxSerial, argv[i], MAX_SERIAL_NUM_STRLEN);
            } else {
                printf("Missing argument for -rxSerialNum\n");
                exit(1);
            }
        //#### DC Offset
        } else if (strcmp("-txDCOffsetI", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txDCOffsetI = strtod(argv[i], NULL);
            } else {
                printf("Missing argument for -txDCOffsetI\n");
                exit(1);
            }
        } else if (strcmp("-txDCOffsetQ", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txDCOffsetQ = strtod(argv[i], NULL);
            } else {
                printf("Missing argument for -txDCOffsetQ\n");
                exit(1);
            }
        } else if (strcmp("-rxDCOffsetI", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxDCOffsetI = strtod(argv[i], NULL);
            } else {
                printf("Missing argument for -rxDCOffsetI\n");
                exit(1);
            }
        } else if (strcmp("-rxDCOffsetQ", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxDCOffsetQ = strtod(argv[i], NULL);
            } else {
                printf("Missing argument for -rxDCOffsetQ\n");
                exit(1);
            }
        //#### I/Q Imbalance Properties
        } else if (strcmp("-txIQGain", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txIQGain = strtod(argv[i], NULL);
            } else {
                printf("Missing argument for -txIQGain\n");
                exit(1);
            }
        } else if (strcmp("-txIQPhase", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                txIQPhase_deg = strtod(argv[i], NULL);
            } else {
                printf("Missing argument for -txIQGain\n");
                exit(1);
            }
        } else if (strcmp("-rxIQGain", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxIQGain = strtod(argv[i], NULL);
            } else {
                printf("Missing argument for -rxIQGain\n");
                exit(1);
            }
        } else if (strcmp("-rxIQPhase", argv[i]) == 0) {
            i++; //Get the actual argument

            if (i < argc) {
                rxIQPhase_deg = strtod(argv[i], NULL);
            } else {
                printf("Missing argument for -rxIQGain\n");
                exit(1);
            }
        } else if (strcmp("-v", argv[i]) == 0) {
            print = true;
        } else {
            printf("Unknown CLI option: %s\n", argv[i]);
        }
    }

    if (txSharedName == NULL || txFeedbackSharedName == NULL || rxSharedName == NULL) {
        printf("must supply tx, rx, and txfb share names\n");
        exit(1);
    }

    if(strlen(txSerial) == 0 && strlen(rxSerial) != 0){
        fprintf(stderr, "Rx Serial Specified but Tx Serial Unspecified");
        exit(1);
    }
    if(strlen(txSerial) != 0 && strlen(rxSerial) == 0){
        fprintf(stderr, "Tx Serial Specified but Rx Serial Unspecified");
        exit(1);
    }
    if(strlen(txSerial) == 0 && strlen(rxSerial) == 0){
        fprintf(stderr, "Tx & Rx Serial Unspecified");
        exit(1);
    }


    //### Setup bladeRF
    //For info on how to use libbladeRF see the documentation at https://www.nuand.com/bladeRF-doc/libbladeRF/v2.2.1/
    //The boilerplate for usage is at https://www.nuand.com/bladeRF-doc/libbladeRF/v2.2.1/boilerplate.html
    //Examples for Tx & Rx are at https://www.nuand.com/bladeRF-doc/libbladeRF/v2.2.1/sync_no_meta.html

    printf("Tx BladeRF Serial Num: %s\nRx BladeRF Serial Num: %s\n", txSerial, rxSerial);

    struct bladerf *txDev = NULL;
    openBladeRF(&txDev, txSerial);

    struct bladerf *rxDev = NULL;
    if(strcmp(txSerial, rxSerial) == 0){
        //Tx and Rx board are the same
        //Do not re-open the board
        rxDev = txDev;
    }else{
        openBladeRF(&rxDev, rxSerial);
    }

    //Config bladeRF settings
    //Will configure Tx0 and Rx 0
    configBladeRFChannel(txDev, true,  0, txFreq, txBW, txSampRate, txGain, false);
    configBladeRFChannel(rxDev, false, 0, rxFreq, rxBW, rxSampRate, rxGain, false);

    //**** For Debugging Interface, Can Enable Loopback ****
    bool enableLoopBack = false;
    if(enableLoopBack && print){
        printf("********** RFIC LOOPBACK MODE *********\n");
    }

    bladerf_loopback loopbackMode = enableLoopBack ? BLADERF_LB_RFIC_BIST : BLADERF_LB_NONE;

    int statusLB = bladerf_set_loopback(txDev, loopbackMode);
    if (statusLB != 0) {
        fprintf(stderr, "Failed to configure bladeRF Loopback: %s\n",
                bladerf_strerror(statusLB));
        exit(1);
    }

    bladerf_loopback txLoopbackModeReported;
    statusLB = bladerf_get_loopback(txDev, &txLoopbackModeReported);
    if (statusLB != 0) {
        fprintf(stderr, "Failed to get bladeRF Loopback: %s\n",
                bladerf_strerror(statusLB));
        exit(1);
    }

    if(print){
        char* loopbackModeDescr = bladeRFLoopbackModeToStr(txLoopbackModeReported);
        printf("%s BladeRF Loopback Mode: %s\n", txDev==rxDev ? "Tx/Rx" : "Tx", loopbackModeDescr);
    }

    if(rxDev != txDev){
        int statusLB = bladerf_set_loopback(rxDev, loopbackMode);
        if (statusLB != 0) {
            fprintf(stderr, "Failed to configure bladeRF Loopback: %s\n",
                    bladerf_strerror(statusLB));
            exit(1);
        }

        bladerf_loopback rxLoopbackModeReported;
        statusLB = bladerf_get_loopback(rxDev, &rxLoopbackModeReported);
        if (statusLB != 0) {
            fprintf(stderr, "Failed to get bladeRF Loopback: %s\n",
                    bladerf_strerror(statusLB));
            exit(1);
        }

        if(print){
            char* loopbackModeDescr = bladeRFLoopbackModeToStr(rxLoopbackModeReported);
            printf("Rx BladeRF Loopback Mode: %s\n", loopbackModeDescr);
        }
    }

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
    //TODO: Make args?
    //int bladeRFBlockLen = 8192;
    int bladeRFBlockLen = 16384;
    //int bladeRFNumBuffers = 16;
    int bladeRFNumBuffers = 32;
    //int bladeRFNumTransfers = 8;
    int bladeRFNumTransfers = 16;

    //Configure before opening threads and enabling Tx or Rx.  Streams need to be configured before any call to sync

    // bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_VERBOSE);
    bladerf_log_set_verbosity(BLADERF_LOG_LEVEL_DEBUG);

    signal(SIGABRT, &signal_handler);
    signal(SIGTERM, &signal_handler);
    signal(SIGINT, &signal_handler);

    //Create Thread Args
    txThreadArgs_t txThreadArgs;
    txThreadArgs.txSharedName = txSharedName;
    txThreadArgs.txFeedbackSharedName = txFeedbackSharedName;
    txThreadArgs.blockLen = blockLen;
    txThreadArgs.fifoSizeBlocks = fifoSize;
    txThreadArgs.stop = &stop;
    txThreadArgs.print = print;
    txThreadArgs.dev = txDev;
    txThreadArgs.fullRangeValue = fullScaleValue;
    txThreadArgs.saturate = saturate;
    txThreadArgs.bladeRFBlockLen = bladeRFBlockLen;
    txThreadArgs.bladeRFNumBuffers = bladeRFNumBuffers;
    txThreadArgs.bladeRFNumTransfers = bladeRFNumTransfers;
    txThreadArgs.dcOffsetI = txDCOffsetI;
    txThreadArgs.dcOffsetQ = txDCOffsetQ;
    txThreadArgs.iqGain = txIQGain;
    txThreadArgs.iqPhase_deg = txIQPhase_deg;

    rxThreadArgs_t rxThreadArgs;
    rxThreadArgs.rxSharedName = rxSharedName;
    rxThreadArgs.blockLen = blockLen;
    rxThreadArgs.fifoSizeBlocks = fifoSize;
    rxThreadArgs.stop = &stop;
    rxThreadArgs.print = print;
    rxThreadArgs.dev = rxDev;
    rxThreadArgs.fullRangeValue = fullScaleValue;
    rxThreadArgs.bladeRFBlockLen = bladeRFBlockLen;
    rxThreadArgs.bladeRFNumBuffers = bladeRFNumBuffers;
    rxThreadArgs.bladeRFNumTransfers = bladeRFNumTransfers;
    rxThreadArgs.dcOffsetI = rxDCOffsetI;
    rxThreadArgs.dcOffsetQ = rxDCOffsetQ;
    rxThreadArgs.iqGain = rxIQGain;
    rxThreadArgs.iqPhase_deg = rxIQPhase_deg;

    //Create Thread
    cpu_set_t cpuset_tx, cpuset_rx;
    pthread_t thread_tx, thread_rx;
    pthread_attr_t attr_tx, attr_rx;

    int status = pthread_attr_init(&attr_tx);
    if (status != 0) {
        printf("Could not create Tx pthread attributes ... exiting");
        exit(1);
    }
    status = pthread_attr_init(&attr_rx);
    if (status != 0) {
        printf("Could not create Rx pthread attributes ... exiting");
        exit(1);
    }

    //Set Thread CPU
    if (txCpu >= 0) {
        CPU_ZERO(&cpuset_tx); //Clear cpuset
        CPU_SET(txCpu, &cpuset_tx); //Add CPU to cpuset
        status = pthread_attr_setaffinity_np(&attr_tx, sizeof(cpu_set_t), &cpuset_tx);//Set thread CPU affinity
        if (status != 0) {
            printf("Could not set Tx thread core affinity ... exiting");
            exit(1);
        }

        //The worker threads in libbladeRF should inherit the affinity mask of this thread
        // "A new thread created by pthread_create(3) inherits a copy of its creator's CPU affinity mask." (https://linux.die.net/man/3/pthread_setaffinity_np)
        //Will not set it to use a real time scheduler, especially SCHED_FIFO as that could potentially
        //cause things to block

        //Note that the worker thread is created in the call to sync_worker_init in src/streaming/sync_worker.c and
        //no thread attributes are supplied to the call to pthread_create (NULL passed).  This should create the thread
        //with the default attributes which I think should include the inherited affinity mask
        // "If attr is NULL, then the thread is created with default attributes" (https://linux.die.net/man/3/pthread_create)

        //Because of this, we don't need to
    }
    if (rxCpu >= 0) {
        CPU_ZERO(&cpuset_tx); //Clear cpuset
        CPU_SET(rxCpu, &cpuset_rx); //Add CPU to cpuset
        status = pthread_attr_setaffinity_np(&attr_rx, sizeof(cpu_set_t), &cpuset_rx);//Set thread CPU affinity
        if (status != 0) {
            printf("Could not set Rx thread core affinity ... exiting");
            exit(1);
        }
    }

    //Start Threads
    status = pthread_create(&thread_tx, &attr_tx, txThread, &txThreadArgs);
    if (status != 0) {
        printf("Could not create Tx thread ... exiting");
        errno = status;
        perror(NULL);
        exit(1);
    }
    status = pthread_create(&thread_rx, &attr_rx, rxThread, &rxThreadArgs);
    if (status != 0) {
        printf("Could not create Rx thread ... exiting");
        errno = status;
        perror(NULL);
        exit(1);
    }

    //Wait for threads to exit
    void *res;
    status = pthread_join(thread_tx, &res);
    if (status != 0) {
        printf("Could not join Tx thread ... exiting");
        errno = status;
        perror(NULL);
        exit(1);
    }
    status = pthread_join(thread_rx, &res);
    if (status != 0) {
        printf("Could not join Rx thread ... exiting");
        errno = status;
        perror(NULL);
        exit(1);
    }

    pthread_attr_destroy(&attr_tx);
    pthread_attr_destroy(&attr_rx);

    bladerf_close(txDev);
    if(rxDev != txDev) {
        bladerf_close(rxDev);
    }

    return 0;
}
