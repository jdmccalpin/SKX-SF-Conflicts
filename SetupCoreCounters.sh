#!/bin/bash

# set this to point to the msrtools distribution if it is not 
# in the default path
WRMSR=./wrmsr
RDMSR=./rdmsr

echo "Are the performance counters globally enabled? Should be 0x70000000f or 0x7000000ff"
echo -n "  global perf counter enable MSR on core 0 is set to "
$RDMSR -p 0 -c 0x38f
echo "Are the fixed-function counters enabled? Should be 0x333 -- if any values include 0xb, need to disable the NMI watchdog"
echo -n "  fixed counter config MSR on core 0 is set to "
$RDMSR -p 0 -c 0x38d



# some useful events
MEM_LOAD_RETIRED_L1_HIT=0x004301d1
MEM_LOAD_RETIRED_L2_HIT=0x004302d1
MEM_LOAD_RETIRED_L3_HIT=0x004304d1
MEM_LOAD_RETIRED_L1_MISS=0x004308d1
MEM_LOAD_RETIRED_L2_MISS=0x004310d1
MEM_LOAD_RETIRED_L3_MISS=0x004320d1
MEM_LOAD_RETIRED_FB_HIT=0x004340d1
MEM_INST_RETIRED_ALL_LOADS=0x004381d0

L2_RQSTS_MISS=0x00433f24
L1D_REPLACEMENTS=0x00430151
L2_LINES_IN_ALL=0x00431ff1
IDI_MISC_WB_DOWNGRADE=0x004304fe

# program these events on all cores
echo "Setting up programmable counters on all cores...."
echo "  Counter 0 MEM_INST_RETIRED_ALL_LOADS"
echo "  Counter 1 L1D_REPLACEMENTS"
echo "  Counter 2 L2_RQSTS_MISS"
echo "  Counter 3 L2_LINES_IN_ALL"

$WRMSR -a 0x186 $MEM_INST_RETIRED_ALL_LOADS
$WRMSR -a 0x187 $L1D_REPLACEMENTS
$WRMSR -a 0x188 $L2_RQSTS_MISS
$WRMSR -a 0x189 $L2_LINES_IN_ALL
