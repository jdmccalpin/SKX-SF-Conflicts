# SKX\_SF\_Conflicts -- READ.ME file

This is remarkably messy code, including way too much processor-specific customization, but it does provide a very compact way to demonstrate Snoop Filter Conflicts on Intel Skylake Xeon processors.

There are two main programs here:

1. SnoopFilterMapper.c
1. SF\_Test\_Offsets.c

These are very similar codes that are set up to repeatedly sum a contiguous, nominally
L2-containable array, with extensive performance counter monitoring, to look for evidence
of Snoop Filter Conflicts on Intel Xeon Scalable processors (a.k.a., Skylake Xeon, or Skylake Server).

* **SnoopFilterMapper** is specialized for use with 2MiB large pages (either pre-allocated
	or Transparent Huge Pages).  It is intended to be run hundreds or thousands of times
	(getting a different set of physical addresses each time), allowing post-processing 
	of results to investigate the relationship between L2 miss rates (which should be near
	zero) and Snoop Filter Evictions.

* **SF\_Test\_Offsets** is specialized for use with 1 GiB large pages (which must be pre-allocated
	at system boot time).  It is intended to be run many times with different offsets
	provided on the command line.  The offset determines the start of the contiguous, L2-
	containable array relative to the beginning of each of the 1 GiB pages used.
	In this case, ensembles of runs are not needed, since the largest L2-containable array
	size is much smaller than a 1 GiB page (1 MiB per core times 28-cores = 28 MiB), 
	ensuring that the contiguous virtual address range used corresponds to a contiguous
	physical address range.  

## SnoopFilterMapper
This outline lists the major operations executed by the **SnoopFilterMapper** code

1.		Allocates a 2GiB array
	*		Options for 1GiB pages (#ifdef MYHUGEPAGE\_1GB) or pre-allocated 2MiB pages (default) or 2MiB Transparent Huge Pages (#ifdef MYHUGEPAGE_THP).
1.		Initialize/instantiate full 2GiB array.
1.		Grab the physical addresses of each page in the array (either 2 values or 2048 values)
			and save in an array.
1.		Check CPUID to see if the processor model is correct.
1.		Optionally prepare to use the IMC counters (#ifdef IMC\_COUNTS)
	1.		mmap /dev/mem and check for the correct SKX VID/DID for bus 0, device 5, function 0
1.		Optionally set up the CHA counters (#ifdef CHA\_COUNTS)
	1.		open one /dev/cpu/*/msr device in each socket
	1.		read and print the four programmable core performance counters for the core in socket 0
	1.		program the counters and filter in each of the CHAs (NUM\_CHA\_USED is hardcoded)
1.		Optionally program the IMC counters (#ifdef IMC\_COUNTS)
1.		Optionally determines the mapping of addresses to L3 numbers (#ifdef MAP\_L3)
	1.		Mostly written for 2MiB pages.
	1.		For the first PAGES\_MAPPED pages:
		1.		Check to see if mapping file already exists for the 2MiB page physical address
		1.		If exists, read the file
		1.		else, for each cache line
			1.		Read the L3 counts, access the line many times, read the L3 counts
			1.		Sanity check results -- if good, store L3 mapping, if bad, repeat.
		1.		After mapping all lines in the page, write the mapping file for later use.
	1.		After all pages are mapped, add up the number of lines mapped to each CHA.
				(Not needed once it is shown that short contiguous ranges cover all the CHAs
				almost-uniformly.)
1.		Run an OpenMP parallel "warm-up" loop of AVX512 instructions to try to make sure the
			cores have spun up the AVX512 units and boosted the cores to the correct frequency.
1.		Optionally read the initial values of the IMC counters (#ifdef IMC\_COUNTS)
1.		Optionally read the initial values of the CHA counters (#ifdef CHA\_COUNTS)
1.		Code Under Test
	1.		save start\_tsc() (in OpenMP master thread)
	1.		First OpenMP loop: 
		1.		Check core number for each thread using RDTSCP TSC_AUX value 
			1.		NOTE: implies KMP_AFFINITY="granularity=fine"
		1.		Read initial values of programmable core counters on each core used.
	1.		Second OpenMP loop:
		1.		read initial value of fixed-function counters on each core in use
		1.		Repeat call to ssum() "inner\_repetitions" times (with individualized
					array start/stop values per thread).
		1.		read final value of fixed-function counters on each core in use
		1.		NOTE: these counter reads are inside the OpenMP barrier, so they
					can be used to detect load imbalance.
	1.		Third OpenMP loop:
		1.		Check core number for each thread using RDTSCP TSC\_AUX value 
		1.		Read final values of programmable core counters on each core used.
	1.		save end\_tsc() (in OpenMP master thread)
1.		Optionally read the final values of the CHA counters (#ifdef CHA\_COUNTS)
1.		Optionally read the final values of the IMC counters (#ifdef IMC\_COUNTS)
1.		Post-Processing
	1.		Compute package sums of core counters
	1.		Optionally compute package sums of CHA counters
	1.		Optionally compute package sums of IMC counters
	1.		Compute utilization, average frequency, and IPC for each thread (inside the OpenMP barriers).
	1.		Compute snoop filter eviction rate (assumes SF EVICTS are in CHA counter 0)

## SF\_Test\_Offsets
The **SF\_test\_offsets** code is very similar to **SnoopFilterMapper**, but is specialized to run the code under test using contiguous memory at various offsets from the base of each 1GiB page.

* This is probably similar enough to merge with SnoopFilterMapper.c.
* The code includes "#ifdef SIMPLE\_OMP\_LOOP" to run the reduction in scalar mode instead of using the external AVX512 ssum() routine.
	* The OpenMP scalar reduction mode loses the "between the barriers" fixed-function core counter data, but retains all the other performance counter data.

## Porting Notes

1. The main functionality of the codes is enabled/disabled through preprocessor variables
	* MAP\_L3 -- if defined, causes the code to use CHA counters to attempt to map each cache line in the contiguous array to one of the L3 cache slices.   This code is complex and slow (about 6 seconds for the 32768 cache lines in a 2MiB page), and is not needed when using this code to look for snoop filter conflicts.
		* Requires CHA\_COUNTS to be defined, which requires root privileges for access to the /dev/cpu/*/msr device drivers.
	* CHA\_COUNTS -- if defined, causes the code to program the hardware performance counters in each CHA to measure four specific events, and to read these counts before and after the code under test.
		* This must be defined to directly measure Snoop Filter Evictions.
		* Requires root privileges for access to the /dev/cpu/*/msr device drivers.
		* The events are:
			1. Snoop Filter Evictions: SF\_EVICTION.ALL (sum of M, E, S states)
			2. L3 Data Read Lookups: LLC\_LOOKUP.DATA\_READ (requires CHA\_FILTER0[26:17])
			3. L2 Writebacks to L3: LLC\_LOOKUP.DATA\_WRITE (requires CHA\_FILTER0[26:17])
			4. L3 Writebacks to Memory: LLC\_VICTIMS.TOTAL (MESF) (does not count clean victims)
		* The CHA\_FILTER0 in each CHA is programmed to count all L3 lookups (hit or miss, any state), but not to count SF lookups.
	* IMC\_COUNTS -- if defined, causes the code to program the IMC counters in each DRAM channel to measure four events, and to read these counts before and after the code under test.
		* This is not required to measure snoop filter conflicts.
		* With the L2-contained array access kernel, these counters don't provide any useful information, but they are still in the code for historical reasons.
		* If this variable is defined, more portability checks include:
			* The file SKX\_IMC\_BusDeviceFunctionOffset.h contains (potentially) machine-specific bus numbers for the PCI configuration space used to access the memory controller performance counters that will need to be checked and/or updated.
			* The variable `mmconfig_base` is set to 0x80000000 in the main program.  This value is used to map all of PCI configuration space to a local pointer for access to the memory controller performance counters.
				* The easiest way to find the correct value for your system is `grep MMCONFIG /proc/iomem`
		* The events are:
			1. DRAM cache line reads: CAS\_COUNT.READS
			2. DRAM cache line writes: CAS\_COUNT.WRITES
			3. DRAM bank "activate" operations: ACT.ALL
			4. DRAM bank "precharge" operations due to bank conflicts: PRE\_COUNT.MISS
2. There are a stupid number of machine-specific defines in the code:
	* The ARRAYSIZE is set to 2GiB by default.
		* Only a fraction of this is typically used for the contiguous, L2-resident array accesses, but the large size allows for two 1 GiB pages or 1024 2 MiB pages.  These large sizes are useful when the code is being used to determine the mapping of physical addresses to L3 slices.
	* MYPAGESIZE is typically set to 2097152L for **SnoopFilterMapper** and to 1073741824UL for **SF\_Test\_offsets**.
	* PAGES\_MAPPED tells the code how many pages to look at when mapping physical addresses to L3 slices (assuming MAP\_L3 is defined).  This is typically set to 1/2 the number of cores in use, so that the code will only be mapping the cache lines that are used in the contiguous L2-contained summation kernel.
	* NUM\_SOCKETS is set to two for measurement on 2-socket systems, but the typical use case only uses socket 0.
	* NUM\_IMC\_CHANNELS is set to 6, which is the correct number of channels per socket for all of the Xeon Scalable Processor models.  This would need to be reduced to 4 for testing the "Xeon W-21xx" processors.
	* NUM\_CHA\_BOXES is set to 28.  All Xeon Scalable Processors have MSR addresses for all 28 CHAs, even if some (or many) are disabled, so there should be no need to change this.
	* NUM\_CHA\_USED is set to 24, which is the number of "active" CHAs in the Xeon Platinum 8160 processor.   This should be changed to the correct number of active L3 slices for other SKX processor models.  
		* Inactive CHAs will return zero on all performance counts, so both the individual results and the socket-wide sums should be correct as long as NUM\_CHA\_USED is at least as large as the number of CHA/L3 slices actually active.)
	* MAXCORES is set to 96, which is the number of logical processors on a two-socket Xeon Platinum 8160 system with HyperThreading enabled (2 sockets * 24 physical cores/socket * 2 logical processors/physical core = 96).  This is used for array sizing, so it only needs to be changed if the actual number of cores used is larger than this value.
	* CORES\_USED is set to 24, which is the number of physical cores in a Xeon Platinum 8160 processor.  The code assumes that this variable matches the OpenMP thread count, with loop structures set up to execute one iteration per OpenMP thread.  
		* This will need to be changed for testing other core counts.
		* The runtime environment must be consistent with the value used in the compilation!!!
		* More notes on the runtime environment are included below....
	* RANDOM\_OFFSETS does not really mean what it says.... It is not used in **SnoopFilterMapper**, but is used in **SF\_Test\_offsets**.  When defined, the code expects an integer argument on the command line.  The argument is interpreted as the number of 64-bit array elements above the base of each 1 GiB page to start the contiguous, L2-containable array accesses.    This ensures that the contiguous virtual address range used maps to a contiguous physical address range.
	* SIMPLE\_OMP\_LOOP in **SF\_Test\_offsets** switches the code under test from an AVX512-optimized external summation routine (ssum.c) to a simple OpenMP sum reduction. 
		* With the AVX-512 code, the 512-bit loads ensure that each cache line is consumed by a single load operation, so there is no possibility that the cache line can be evicted from the cache while it is still in use.
		* With the simple OpenMP reduction, the Intel 18 compiler generates scalar code, so 8 load operations are required to process each cache line.  In this case, it is possible for a line to be evicted (by a Snoop Filter Eviction) before it has been completely processed.  When this happens, there will be more than one L2 miss associated with processing that cache line one time, and the overall L2 miss rate will increase.
3. Run time environment
	* I use the Intel OpenMP runtime environment variable `KMP_HW_SUBSET=1s,24c,1t` to limit the execution to 24 cores on 1 socket.  
		* With this environment variable definition, there is no need to set `OMP_NUM_THREADS`.
		* The code assumes that threads are not allowed to migrate between the two thread contexts on each logical processor (when HyperThreading is enabled).  This can be enforced by the `1t` option to KMP\_HW\_SUBSET, or by adding the `granularity=fine` to the KMP\_AFFINITY environment variable.
	* The code expects the fixed-function and programmable core performance counters to be enabled and configured correctly on each core that is used:
		* IA32\_PERF\_GLOBAL\_CTRL (MSR 0x38f) should be set to 0x70000000f to enable the three fixed-function counters and four programmable counters per core.  (With HyperThreading disabled, setting this MSR to 0x7000000ff enables all eight programmable counters on each core).
		* IA32\_FIXED\_CTR\_CTRL (MSR 0x38d) should be set to 0x333 to enable the three fixed-function counters to count in both user and kernel mode.  This may require disabling the NMI watchdog, which typically uses one of these counters.
		* The code expects the first four core counters on each core to be programmed to count:
			1. MEM\_INST\_RETIRED\_ALL\_LOADS (0x004381d0)
			2. MEM\_LOAD\_RETIRED\_L2_HIT (0x004302d1)
			3. MEM\_LOAD\_RETIRED\_FB\_HIT (0x004340d1)
			4. MEM_LOAD_RETIRED_L2_MISS (0x004310d1)
		* If CHA\_COUNTS is defined, the code will print out the core performance counter event select registers on core 0, but does not check to see if the values are the expected ones.


