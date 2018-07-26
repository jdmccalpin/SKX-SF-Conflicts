#!/bin/bash

# If the root user is going to run the SnoopFilterMapper or SF_test_offsets binaries,
# the LD_LIBRARY_PATH may need to be defined to point to the OpenMP runtime
# library.  
# If the binaries are tagged as "setuid root", the Makefile should be modified
# to include an "rpath" option pointing to the OpenMP runtime library.

NUMTRIALS=100
MAXTRIAL=$(( $NUMTRIALS - 1 ))

# Pin the threads to sequential cores on socket 0 and do not
# let them switch thread contexts (if HyperThreading is enabled).
# Similar to adding "granularity=fine" to the KMP_AFFINITY variable
export KMP_HW_SUBSET=1s,24c,1t
export KMP_AFFINITY=compact
export OMP_NUM_THREADS=24

for TRIAL in `seq 0 $MAXTRIAL`
do
	LABEL=`printf %.3d $TRIAL`
	time ./SnoopFilterMapper > log.2MiB.$LABEL
done
