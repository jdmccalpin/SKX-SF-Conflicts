CC = icc
CFLAGS = -DIMC_COUNTS -DCHA_COUNTS -sox -g -O -xCORE-AVX512

default: SnoopFilterMapper

SnoopFilterMapper.o: SnoopFilterMapper.c
	icc $(CFLAGS) -qopenmp -c SnoopFilterMapper.c

ssum.o: ssum.c
	icc -sox -g -O -xCORE-AVX512 -qopt-zmm-usage=high -c ssum.c

SnoopFilterMapper: SnoopFilterMapper.o ssum.o va2pa_lib.o low_overhead_timers.c
	icc $(CFLAGS) -qopenmp SnoopFilterMapper.o ssum.o va2pa_lib.o -o SnoopFilterMapper

SF_test_offsets.o: SF_test_offsets.c
	icc -qopenmp -DRANDOMOFFSETS -DMYHUGEPAGE_1GB -DIMC_COUNTS -DCHA_COUNTS -sox -g -O -xCORE-AVX512 -c SF_test_offsets.c 

SF_test_offsets: SF_test_offsets.o ssum.o va2pa_lib.o low_overhead_timers.c
	icc $(CFLAGS) -qopenmp SF_test_offsets.o ssum.o va2pa_lib.o -o SF_test_offsets

SnoopFilterMapper_THP.o: SnoopFilterMapper.c 
	icc $(CFLAGS) -qopenmp -DMYHUGEPAGE_THP -c SnoopFilterMapper.c -o SnoopFilterMapper_THP.o

SnoopFilterMapper_THP: SnoopFilterMapper_THP.o ssum.o va2pa_lib.o low_overhead_timers.c
	icc $(CFLAGS) -qopenmp SnoopFilterMapper_THP.o ssum.o va2pa_lib.o -o SnoopFilterMapper_THP
