// John D. McCalpin, mccalpin@tacc.utexas.edu
static char const rcsid[] = "$Id: SF_test_offsets.c,v 1.4 2018/05/17 22:20:24 mccalpin Exp mccalpin $";

// include files
#include <stdio.h>				// printf, etc
#include <stdint.h>				// standard integer types, e.g., uint32_t
#include <signal.h>				// for signal handler
#include <stdlib.h>				// exit() and EXIT_FAILURE
#include <string.h>				// strerror() function converts errno to a text string for printing
#include <fcntl.h>				// for open()
#include <errno.h>				// errno support
#include <assert.h>				// assert() function
#include <unistd.h>				// sysconf() function, sleep() function
#include <sys/mman.h>			// support for mmap() function
#include <linux/mman.h>			// required for 1GiB page support in mmap()
#include <math.h>				// for pow() function used in RAPL computations
#include <time.h>
#include <sys/time.h>			// for gettimeofday

# define ARRAYSIZE 2147483648L

#ifdef MYHUGEPAGE_1GB
// 1 GiB pages
#define MYPAGESIZE 1073741824UL
#define NUMPAGES 32L
#define PAGES_MAPPED 32L			// this code is not working correctly for 1GiB pages, but I already know the answers....
#else
#define MYPAGESIZE 2097152L
#define NUMPAGES 1024L
#define PAGES_MAPPED 12L
#endif

#define SPECIAL_VALUE (-1)

// interfaces for va2pa_lib.c
void print_pagemap_entry(unsigned long long pagemap_entry);
unsigned long long get_pagemap_entry( void * va );

int dumpall;			// when set to 1, will cause dump of lots of stuff for debugging
int report;
int nwraps;				// track number of performance counter wraps

double *array;					// array pointer to mmap on 1GiB pages
double *page_pointers[NUMPAGES];		// one pointer for each page allocated
uint64_t pageframenumber[NUMPAGES];	// one PFN entry for each page allocated

// constant value defines
# define NUM_SOCKETS 2				// 
# define NUM_IMC_CHANNELS 6			// includes channels on all IMCs in a socket
# define NUM_IMC_COUNTERS 5			// 0-3 are the 4 programmable counters, 4 is the fixed-function DCLK counter
# define NUM_CHA_BOXES 28
# define NUM_CHA_USED 24
# define NUM_CHA_COUNTERS 4

long imc_counts[NUM_SOCKETS][NUM_IMC_CHANNELS][NUM_IMC_COUNTERS][2];	// including the fixed-function (DCLK) counter as the final entry
long imc_pkg_sums[NUM_SOCKETS][NUM_IMC_COUNTERS];						// sum across channels for each chip
char imc_event_name[NUM_SOCKETS][NUM_IMC_CHANNELS][NUM_IMC_COUNTERS][32];		// reserve 32 characters for the IMC event names for each socket, channel, counter
uint32_t imc_perfevtsel[NUM_IMC_COUNTERS];			// expected control settings for the counters
uint32_t imc_vid_did[3];							// PCIe configuration space vendor and device IDs for the IMC blocks 
long cha_counts[NUM_SOCKETS][NUM_CHA_BOXES][NUM_CHA_COUNTERS][2];		// 2 sockets, 28 tiles per socket, 4 counters per tile, 2 times (before and after)
uint32_t cha_perfevtsel[NUM_CHA_COUNTERS];
long cha_pkg_sums[NUM_SOCKETS][NUM_CHA_COUNTERS];

#define MAXCORES 96
#define CORES_USED 24
// New feature -- core counters.
// upgrade to include counters for all cores 
long core_counters[MAXCORES][4][2];					// 24 cores & 24 threads on one socket, 4 counters, before and after
long fixed_counters[MAXCORES][4][2];				// 24 cores with 4 fixed-function core counters (Instr, CoreCyc, RefCyc, TSC)
long core_pkg_sums[NUM_SOCKETS][4];					// four core counters
long fixed_pkg_sums[NUM_SOCKETS][4];				// four fixed-function counters per core (Instr, CoreCyc, RefCyc, TSC)

int8_t cha_by_page[PAGES_MAPPED][32768];				// L3 numbers for each of the 32,768 cache lines in each of the first PAGES_MAPPED 2MiB pages
uint64_t paddr_by_page[PAGES_MAPPED];					// physical addresses of the base of each of the first PAGES_MAPPED 2MiB pages used
long lines_by_cha[NUM_CHA_USED];			// bulk count of lines assigned to each CHA

#ifdef DEBUG
FILE *log_file;					// log file for debugging -- should not be needed in production
#endif
unsigned int *mmconfig_ptr;         // must be pointer to 32-bit int so compiler will generate 32-bit loads and stores

struct timeval tp;		// seconds and microseconds from gettimeofday
struct timezone tzp;	// required, but not used here.

double ssum(double *a, long vl);

double mysecond()
{
        struct timeval tp;
        struct timezone tzp;
        int i;

        i = gettimeofday(&tp,&tzp);
        return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif


#include "low_overhead_timers.c"


#include "SKX_IMC_BusDeviceFunctionOffset.h"
#include "MSR_defs.h"
// ===========================================================================================================================================================================
// Convert PCI(bus:device.function,offset) to uint32_t array index
uint32_t PCI_cfg_index(unsigned int Bus, unsigned int Device, unsigned int Function, unsigned int Offset)
{
    uint32_t byteaddress;
    uint32_t index;
    assert (Device >= 0);
    assert (Function >= 0);
    assert (Offset >= 0);
    assert (Device < (1<<5));
    assert (Function < (1<<3));
    assert (Offset < (1<<12));
    byteaddress = (Bus<<20) | (Device<<15) | (Function<<12) | Offset;
    index = byteaddress / 4;
    return ( index );
}

// Compute the difference of 48-bit counter values, correcting
// for a single overflow of the counter if necessary
long corrected_delta48(int tag, long end, long start)
{
    long result;
	int i;
    if (end >= start) {
        result = (long) (end - start);
    } else {
        // result = (long) ((end + (1UL<<48)) - start);
        result = 0;
		if (report == 1) {
			nwraps++;
			dumpall = 1;
			i = gettimeofday(&tp,&tzp);
			printf("DEBUG: wrap detected at %ld.%.6ld tag %d end %ld (0x%lx) start %ld (0x%lx) result %ld (0x%lx)\n",tp.tv_sec,tp.tv_usec,tag,end,end,start,start,result,result);
		}
    }
    return (result);
}

// ===========================================================================================================================================================================
int main(int argc, char *argv[])
{
	// local declarations
	// int cpuid_return[4];
	int i;
	int retries;
	int zeros;
	int tag;
	int rc;
	ssize_t rc64;
	char description[100];
	size_t len;
	long arraylen;
	long l2_contained_size, inner_repetitions;
	unsigned long pagemapentry;
	unsigned long paddr, basephysaddr;
	unsigned long pagenum, basepagenum;
	uint32_t bus, device, function, offset, ctl_offset, ctr_offset, value, index;
	uint32_t socket, imc, channel, counter, controller;
	long count,delta;
	long j,k,page_number,page_base_index,line_number;
	long jstart[CORES_USED], jend[CORES_USED], mycore, vl[CORES_USED];
	uint32_t low_0, high_0, low_1, high_1;
	char filename[100];
	int pkg, tile;
	int nr_cpus;
	uint64_t msr_val, msr_num;
	int mem_fd;
	int msr_fd[2];				// one for each socket
	int proc_in_pkg[2];			// one Logical Processor number for each socket
	uid_t my_uid;
	gid_t my_gid;
	double sum,expected;
	double t0, t1;
	double avg_cycles;
	unsigned long tsc_start, tsc_end;
	double tsc_rate = 2.1e9;
	double sf_evict_rate;
	double bandwidth;
    unsigned long mmconfig_base=0x80000000;		// DOUBLE-CHECK THIS ON NEW SYSTEMS!!!!!   grep MMCONFIG /proc/iomem | awk -F- '{print $1}'
    unsigned long mmconfig_size=0x10000000;
	double private_sum,partial_sums[CORES_USED];
	long iters,iteration_counts[CORES_USED];
	long BaseOffset;

	BaseOffset = 0;
#ifdef RANDOMOFFSETS
	if (argc != 2) {
		printf("Must Provide a Random Offset cache line offset value (an integer between 0 and 2^24-375000 (16,402,216))\n");
		exit(1);
	} else {
		BaseOffset = atol(argv[1]);
		printf("Random Cache Line Offset is %ld\n",BaseOffset);
		BaseOffset = BaseOffset*8;
		printf("Starting index for summation is %ld\n",BaseOffset);
	}
#endif

	retries = 0;
	zeros = 0;
	report = 1;
	dumpall = 0;
	nwraps = 0;
	// l2_contained_size = 125000 * CORES_USED;		// about 95% of the L2 space in the cores used
	l2_contained_size = 87380 * CORES_USED;		// with 24 cores, this gives almost exactly 16 MiB
	for (i=0; i<CORES_USED; i++) {
		iters = 0;
		jstart[i] = BaseOffset + i*l2_contained_size/CORES_USED;
		jend[i] = jstart[i] + l2_contained_size/CORES_USED;
		vl[i] = jend[i]-jstart[i];
		printf("thread %d jstart %ld jend %ld vl %ld\n",i,jstart[i],jend[i],vl[i]);

		partial_sums[i] = 0.0;
		iteration_counts[i] = 0;
		for (counter=0; counter<4; counter++) {
			core_counters[i][counter][0] = SPECIAL_VALUE;
			core_counters[i][counter][1] = SPECIAL_VALUE;
			fixed_counters[i][counter][0] = SPECIAL_VALUE;
			fixed_counters[i][counter][1] = SPECIAL_VALUE;
		}
	}
	// initialize the array that will hold the L3 numbers for each cache line for each of the first PAGES_MAPPED 2MiB pages
	for (i=0; i<PAGES_MAPPED; i++) {
		for (line_number=0; line_number<32768; line_number++) {
			cha_by_page[i][line_number] = -1; 	// special value -- if set properly, all values should be in the range of 0..23
		}
	}

	// allocate working array on a huge pages -- either 1GiB or 2MiB
	len = NUMPAGES * MYPAGESIZE;
#ifdef MYHUGEPAGE_1GB
	array = (double*) mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0 );
#else
	array = (double*) mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0 );
#endif
	if (array == (void *)(-1)) {
        perror("ERROR: mmap of array a failed! ");
        exit(1);
    }
	// initialize working array
	arraylen = NUMPAGES * MYPAGESIZE/sizeof(double);
#pragma omp parallel for
	for (j=0; j<arraylen; j++) {
		array[j] = 1.0;
	}
	// initialize page_pointers to point to the beginning of each page in the array
	// then get and print physical addresses for each
#ifdef VERBOSE
	printf(" Page    ArrayIndex            VirtAddr        PagemapEntry         PFN           PhysAddr\n");
#endif
	for (j=0; j<NUMPAGES; j++) {
		k = j*MYPAGESIZE/sizeof(double);
		page_pointers[j] = &array[k];
		pagemapentry = get_pagemap_entry(&array[k]);
		pageframenumber[j] = (pagemapentry & (unsigned long) 0x007FFFFFFFFFFFFF);
#ifdef VERBOSE
		printf(" %.5ld   %.10ld  %#18lx  %#18lx  %#18lx  %#18lx\n",j,k,&array[k],pagemapentry,pageframenumber[j],(pageframenumber[j]<<12));
#endif
	}
	printf("PAGE_ADDRESSES ");
	for (j=0; j<PAGES_MAPPED; j++) {
		basephysaddr = pageframenumber[j] << 12;
		paddr_by_page[j] = basephysaddr;
		printf("0x%.12lx ",paddr_by_page[j]);
	}
	printf("\n");


	// initialize arrays for counter data
	for (socket=0; socket<NUM_SOCKETS; socket++) {
		for (channel=0; channel<NUM_IMC_CHANNELS; channel++) {
			for (counter=0; counter<NUM_IMC_COUNTERS; counter++) {
				imc_counts[socket][channel][counter][0] = 0;
				imc_counts[socket][channel][counter][1] = 0;
			}
		}
		for (tile=0; tile<NUM_CHA_USED; tile++) {
			lines_by_cha[tile] = 0;
			for (counter=0; counter<4; counter++) {
				cha_counts[socket][tile][counter][0] = 0;
				cha_counts[socket][tile][counter][1] = 0;
			}
		}
	}

	// get the host name, assume that it is of the TACC standard form, and use this as part
	// of the log file name....  Standard form is "c263-109.stampede2.tacc.utexas.edu", so
	// truncating at the first "." is done by writing \0 to character #8.
	len = 100;	
	rc = gethostname(description, len);
	if (rc != 0) {
		fprintf(stderr,"ERROR when trying to get hostname\n");
		exit(-1);
	}
	description[8] = 0;		// assume hostname of the form c263-109.stampede2.tacc.utexas.edu -- truncate after first period

	my_uid = getuid();
	my_gid = getgid();

#ifdef DEBUG
	sprintf(filename,"log.%s.perf_counters",description);
	// sprintf(filename,"log.perf_counters");
	log_file = fopen(filename,"w+");
	if (log_file == 0) {
		fprintf(stderr,"ERROR %s when trying to open log file %s\n",strerror(errno),filename);
		exit(-1);
	}

	fprintf(log_file,"DEBUG: my uid is %d, my gid is %d\n",my_uid,my_gid);

	rc = chown(filename,my_uid,my_gid);
	if (rc == 0) {
		fprintf(log_file,"DEBUG: Successfully changed ownership of log file to %d %d\n",my_uid,my_gid);
	} else {
		fprintf(stderr,"ERROR: Attempt to change ownership of log file failed -- bailing out\n");
		exit(-1);
	}
#endif

	//========================================================================================================================
	// initial checks
	// 		is this a supported core?  (CPUID Family/Model)
	//      Every processor that I am going to see will be Family 0x06 (no ExtFamily needed).
	//      The DisplayModel field is (ExtModel<<4)+Model and should be 0x3F for all Xeon E5 v3 systems
	int leaf = 1;
	int subleaf = 0;
	uint32_t eax, ebx, ecx, edx;
	__asm__ __volatile__ ("cpuid" : \
		  "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx) : "a" (leaf), "c" (subleaf));

	// Alternate form: 
	// 		The compiler cpuid intrinsics are not documented by Intel -- they use the Microsoft format
	// 			described at https://msdn.microsoft.com/en-us/library/hskdteyh.aspx
	// 			__cpuid(array to hold eax,ebx,ecx,edx outputs, initial eax value)
	// 			__cpuidex(array to hold eax,ebx,ecx,edx outputs, initial eax value, initial ecx value)
	//      CPUID function 0x01 returns the model info in eax.
	//      		27:20 ExtFamily	-- expect 0x00
	//      		19:16 ExtModel	-- expect 0x3 for HSW, 0x5 for SKX
	//      		11:8  Family	-- expect 0x6
	//      		7:4   Model		-- expect 0xf for HSW, 0x5 for SKX
	// __cpuid(&cpuid_return[0], 1);
	// uint32_t ModelInfo = cpuid_return[0] & 0x0fff0ff0;	// mask out the reserved and "stepping" fields, leaving only the based and extended Family/Model fields

	uint32_t ModelInfo = eax & 0x0fff0ff0;	// mask out the reserved and "stepping" fields, leaving only the based and extended Family/Model fields
	if (ModelInfo != 0x00050650) {				// expected values for Skylake Xeon
		fprintf(stderr,"ERROR -- this does not appear to be the correct processor type!!!\n");
		fprintf(stderr,"ERROR -- Expected CPUID(0x01) Family/Model bits = 0x%x, but found 0x%x\n",0x00050650,ModelInfo);
		exit(1);
	}

#ifdef IMC_COUNTS
	// ===================================================================================================================
	// ------------------ REQUIRES ROOT PERMISSIONS ------------------
	// open /dev/mem for PCI device access and mmap() a pointer to the beginning
	// of the 256 MiB PCI Configuration Space.
	// 		check VID/DID for uncore bus:device:function combinations
	//   Note that using /dev/mem for PCI configuration space access is required for some devices on KNL.
	//   It is not required on other systems, but it is not particularly inconvenient either.
	sprintf(filename,"/dev/mem");
#ifdef DEBUG
	fprintf(log_file,"opening %s\n",filename);
#endif
	mem_fd = open(filename, O_RDWR);
	if (mem_fd == -1) {
		fprintf(stderr,"ERROR %s when trying to open %s\n",strerror(errno),filename);
		exit(-1);
	}
	int map_prot = PROT_READ | PROT_WRITE;
	mmconfig_ptr = mmap(NULL, mmconfig_size, map_prot, MAP_SHARED, mem_fd, mmconfig_base);
    if (mmconfig_ptr == MAP_FAILED) {
        fprintf(stderr,"cannot mmap base of PCI configuration space from /dev/mem: address %lx\n", mmconfig_base);
        exit(2);
#ifdef DEBUG
    } else {
		fprintf(log_file,"Successful mmap of base of PCI configuration space from /dev/mem at address %lx\n", mmconfig_base);
#endif
	}
    close(mem_fd);      // OK to close file after mmap() -- the mapping persists until unmap() or program exit

	// New simple test that does not need to know the uncore bus numbers here...
	// Skylake bus 0, Function 5, offset 0 -- Sky Lake-E MM/Vt-d Configuration Registers
	//
	// simple test -- should return "20248086" on Skylake Xeon EP -- DID 0x2024, VID 0x8086
	bus = 0x00;
	device = 0x5;
	function = 0x0;
	offset = 0x0;
	index = PCI_cfg_index(bus, device, function, offset);
    value = mmconfig_ptr[index];
	if (value != 0x20248086) {
		fprintf(stderr,"ERROR: Bus %x device %x function %x offset %x expected %x, found %x\n",bus,device,function,offset,0x20248086,value);
		exit(3);
#ifdef DEBUG
	} else {
		fprintf(log_file,"DEBUG: Well done! Bus %x device %x function %x offset %x returns expected value of %x\n",bus,device,function,offset,value);
#endif
	}
#endif

#ifdef CHA_COUNTS
	// ===================================================================================================================
	// open the MSR driver using one core in socket 0 and one core in socket 1
	nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    proc_in_pkg[0] = 0;                 // logical processor 0 is in socket 0 in all TACC systems
    proc_in_pkg[1] = nr_cpus-1;         // logical processor N-1 is in socket 1 in all TACC 2-socket systems
	for (pkg=0; pkg<2; pkg++) {
		sprintf(filename,"/dev/cpu/%d/msr",proc_in_pkg[pkg]);
		msr_fd[pkg] = open(filename, O_RDWR);
		if (msr_fd[pkg] == -1) {
			fprintf(stderr,"ERROR %s when trying to open %s\n",strerror(errno),filename);
			exit(-1);
		}
	}
	for (pkg=0; pkg<2; pkg++) {
		pread(msr_fd[pkg],&msr_val,sizeof(msr_val),IA32_TIME_STAMP_COUNTER);
		fprintf(stdout,"DEBUG: TSC on core %d socket %d is %ld\n",proc_in_pkg[pkg],pkg,msr_val);
	}

	pread(msr_fd[0],&msr_val,sizeof(msr_val),0x186);
	printf("Core PerfEvtSel0 0x%lx\n",msr_val);
	pread(msr_fd[0],&msr_val,sizeof(msr_val),0x187);
	printf("Core PerfEvtSel1 0x%lx\n",msr_val);
	pread(msr_fd[0],&msr_val,sizeof(msr_val),0x188);
	printf("Core PerfEvtSel2 0x%lx\n",msr_val);
	pread(msr_fd[0],&msr_val,sizeof(msr_val),0x189);
	printf("Core PerfEvtSel3 0x%lx\n",msr_val);


	// Program the CHA mesh counters
	//   Each CHA has a block of 16 MSRs reserved, of which 12 are used
	//   The base for each CHA is 0xE00 + 0x10*CHA
	//   Within each block:
	//   	Unit Control is at offset 0x00
	//   	CTL0, 1, 2, 3 are at offsets 0x01, 0x02, 0x03, 0x04
	//   	CTR0, 1, 2, 3 are at offsets 0x08, 0x09, 0x0a, 0x0b
	//   For the moment I think I can ignore the filter registers at offsets 0x05 and 0x06
	//     and the status register at offset 0x07
	//   The control register needs bit 22 set to enabled, then bits 15:8 as Umask and 7:0 as EventSelect
	//   Mesh Events:
	//   	HORZ_RING_BL_IN_USE = 0xab
	//   		LEFT_EVEN = 0x01
	//   		LEFT_ODD = 0x02
	//   		RIGHT_EVEN = 0x04
	//   		RIGHT_ODD = 0x08
	//   	VERT_RING_BL_IN_USE = 0xaa
	//   		UP_EVEN = 0x01
	//   		UP_ODD = 0x02
	//   		DN_EVEN = 0x04
	//   		DN_ODD = 0x08
	//   For starters, I will combine even and odd and create 4 events
	//   	0x004003ab	HORZ_RING_BL_IN_USE.LEFT
	//   	0x00400cab	HORZ_RING_BL_IN_USE.RIGHT
	//   	0x004003aa	VERT_RING_BL_IN_USE.UP
	//   	0x00400caa	VERT_RING_BL_IN_USE.DN

	// first set to try....
	cha_perfevtsel[0] = 0x004003ab;		// HORZ_RING_BL_IN_USE.LEFT
	cha_perfevtsel[1] = 0x00400cab;		// HORZ_RING_BL_IN_USE.RIGHT
	cha_perfevtsel[2] = 0x004003aa;		// VERT_RING_BL_IN_USE.UP
	cha_perfevtsel[3] = 0x00400caa;		// VERT_RING_BL_IN_USE.DN

	// second set to try....
//	cha_perfevtsel[0] = 0x004001ab;		// HORZ_RING_BL_IN_USE.LEFT_EVEN
//	cha_perfevtsel[1] = 0x004002ab;		// HORZ_RING_BL_IN_USE.LEFT_ODD
//	cha_perfevtsel[2] = 0x004004ab;		// HORZ_RING_BL_IN_USE.RIGHT_EVEN
//	cha_perfevtsel[3] = 0x004008ab;		// HORZ_RING_BL_IN_USE.RIGHT_ODD

	// Snoop Filter Eviction counters
	cha_perfevtsel[0] = 0x0040073d;		// SF_EVICTION S,E,M states
	cha_perfevtsel[1] = 0x00400334;		// LLC_LOOKUP.DATA_READ	<-- requires CHA_FILTER0[26:17]
	cha_perfevtsel[2] = 0x00400534;		// LLC_LOOKUP.DATA_WRITE (WB from L2) <-- requires CHA_FILTER0[26:17]
	cha_perfevtsel[3] = 0x0040af37;		// LLC_VICTIMS.TOTAL (MESF) (does not count clean victims)
	uint64_t cha_filter0 = 0x01e20000;		// set bits 24,23,22,21,17 FMESI -- all LLC lookups, no SF lookups

	printf("CHA PerfEvtSel0 0x%lx\n",cha_perfevtsel[0]);
	printf("CHA PerfEvtSel1 0x%lx\n",cha_perfevtsel[1]);
	printf("CHA PerfEvtSel2 0x%lx\n",cha_perfevtsel[2]);
	printf("CHA PerfEvtSel3 0x%lx\n",cha_perfevtsel[3]);
	printf("CHA FILTER0 0x%lx\n",cha_filter0);

#ifdef VERBOSE
	printf("VERBOSE: programming CHA counters\n");
#endif

	for (pkg=0; pkg<2; pkg++) {
		for (tile=0; tile<NUM_CHA_USED; tile++) {
			msr_num = 0xe00 + 0x10*tile;		// box control register -- set enable bit
			msr_val = 0x00400000;
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
			msr_num = 0xe00 + 0x10*tile + 1;	// ctl0
			msr_val = cha_perfevtsel[0];
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
			msr_num = 0xe00 + 0x10*tile + 2;	// ctl1
			msr_val = cha_perfevtsel[1];
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
			msr_num = 0xe00 + 0x10*tile + 3;	// ctl2
			msr_val = cha_perfevtsel[2];
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
			msr_num = 0xe00 + 0x10*tile + 4;	// ctl3
			msr_val = cha_perfevtsel[3];
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
			msr_num = 0xe00 + 0x10*tile + 5;	// filter0
			msr_val = cha_filter0;				// bits 24:21,17 FMESI -- all LLC lookups, not not SF lookups
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
		}
	}
#ifdef VERBOSE
	printf("VERBOSE: finished programming CHA counters\n");
#endif
#endif

#ifdef IMC_COUNTS
	// ===================================================================================================================
	// Read the current programming of the IMC counters and look for the standard values (in this order)
	//     CAS_COUNT.READS		Event 0x04, Umask 0x03
	//     CAS_COUNT.WRITES		Event 0x04, Umask 0x0C
	//     ACT.ALL				Event 0x01, Umask 0x0B
	//     PRE_COUNT.MISS		Event 0x02, Umask 0x01
	//     DCLK

#ifdef VERBOSE
	printf("Preparing to program IMC counters\n");
#endif
	// expected values of IMC performance counter event select control registers
	imc_perfevtsel[0] = 0x00400304;		// CAS_COUNT.READS
	imc_perfevtsel[1] = 0x00400C04;		// CAS_COUNT.WRITES
	imc_perfevtsel[2] = 0x00400B01;		// ACT_COUNT.ALL
	imc_perfevtsel[3] = 0x00400102;		// PRE_COUNT.MISS
	imc_perfevtsel[4] = 0x00400000;		// DCLK

	imc_vid_did[0] = 0x20428086;		// all channel 0 devices are 2042
	imc_vid_did[1] = 0x20468086;		// all channel 1 devices are 2046
	imc_vid_did[2] = 0x204a8086;		// all channel 2 devices are 204a

	printf("IMC PerfEvtSel0 0x%lx\n",imc_perfevtsel[0]);
	printf("IMC PerfEvtSel1 0x%lx\n",imc_perfevtsel[1]);
	printf("IMC PerfEvtSel2 0x%lx\n",imc_perfevtsel[2]);
	printf("IMC PerfEvtSel3 0x%lx\n",imc_perfevtsel[3]);
	printf("IMC PerfEvtSel4 0x%lx\n",imc_perfevtsel[4]);

	// print the full wall-clock time in seconds and microseconds
	// assume both components of tp struct are longs.
	fprintf(stdout,"# %s\n", rcsid);
    i = gettimeofday(&tp,&tzp);
	fprintf(stdout,"%ld %ld\n", tp.tv_sec,tp.tv_usec);

	for (socket=0; socket<NUM_SOCKETS; socket++) {
		bus = IMC_BUS_Socket[socket];
#ifdef VERBOSE
		printf("VERBOSE: socket %d bus %d\n",socket,bus);
#endif
		for (channel=0; channel<NUM_IMC_CHANNELS; channel++) {
			device = IMC_Device_Channel[channel];
			function = IMC_Function_Channel[channel];
#ifdef VERBOSE
			printf("VERBOSE: channel %d device %d function %d\n",channel, device, function);
#endif
			// check to make sure this is the correct device
			offset = 0x0;
			index = PCI_cfg_index(bus, device, function, offset);
			value = mmconfig_ptr[index];
			if ( value != imc_vid_did[channel%3]) {
				fprintf(stderr,"WARNING!!!! socket %d, channel %d has vid_did %x but should be %x\n",socket,channel,value,imc_vid_did[channel%3]);
			}
			for (counter=0; counter<NUM_IMC_COUNTERS; counter++) {
				// check to see if this unit is programmed correctly and reprogram if needed
				offset = IMC_PmonCtl_Offset[counter];
				index = PCI_cfg_index(bus, device, function, offset);
				value = mmconfig_ptr[index];
				if ( value != imc_perfevtsel[counter]) {
					fprintf(stderr,"WARNING!!!! socket %d, channel %d has perfevtsel %x but should be %x -- reprogramming\n",socket,channel,value,imc_perfevtsel[counter]);
					mmconfig_ptr[index] = imc_perfevtsel[counter];
				}

			}
		}
	}
#endif

// ========= END OF PERFORMANCE COUNTER SETUP ========================================================================

// ============== BEGIN L3 MAPPING TESTS ==============================
// For each of the PAGES_MAPPED 2MiB pages:
//   1. Use "access()" to see if the mapping file already exists.
//		If exists:
//   		2. Use "stat()" to make sure the file is the correct size
//   		   If right size:
//   		   	3. Read the contents into the 32768-element int8_t array of L3 numbers.
//   		   Else (wrong size):
//   		   	4. Abort and tell the user to fix it manually.
//   	Else (not exists):
//   		4. Call the mapping function to re-compute the map
//   		5. Create mapping file
//   		6. Save data in mapping file
//   		7. Close output file

	FILE *ptr_mapping_file;
	int needs_mapping;
	int good, good_old, good_new, pass1, pass2, pass3, found, numtries;
	int min_count, max_count, sum_count, old_cha;
	double avg_count, goodness1, goodness2, goodness3;
	int globalsum = 0;
	long totaltries = 0;
	int NFLUSHES = 1000;
	for (page_number=0; page_number<PAGES_MAPPED; page_number++) {
		needs_mapping=0;
		sprintf(filename,"PADDR_0x%.12lx.map",paddr_by_page[page_number]);
		i = access(filename, F_OK);
		if (i == -1) {								// file does not exist
			printf("DEBUG: Mapping file %s does not exist -- will create file after mapping cache lines\n",filename);
			needs_mapping = 1;
		} else {									// file exists
			i = access(filename, R_OK);
			if (i == -1) {							// file exists without read permissions
				printf("ERROR: Mapping file %s exists, but without read permission\n",filename);
				exit(1);
			} else {								// file exists with read permissions
				ptr_mapping_file = fopen(filename,"r");
				if (!ptr_mapping_file) {
					printf("ERROR: Failed to open Mapping File %s, should not happen\n",filename);
					exit(2);
				}
				k = fread(&cha_by_page[page_number][0],(size_t) 32768,(size_t) 1,ptr_mapping_file);
				if (k != 1) {					// incorrect read length
					printf("ERROR: Read from Mapping File %s, returned the wrong record count %ld expected 1\n",filename,k);
					exit(3);
				} else {							// correct read length
					printf("DEBUG: Mapping File read for %s succeeded -- skipping mapping for this page\n",filename);
					needs_mapping = 0;
				}
			}
		}
		if (needs_mapping == 1) {
			// code imported from SystemMirrors/Hikari/MemSuite/InterventionLatency/L3_mapping.c
#ifdef VERBOSE
			printf("DEBUG: here I need to perform the mapping for paddr 0x%.12lx, and then save the file\n",paddr_by_page[page_number]);
#endif
			page_base_index = page_number*262144;		// index of element at beginning of current 2MiB page
			for (line_number=0; line_number<32768; line_number++) {
				good = 0;
				good_old = 0;
				good_new = 0;
				numtries = 0;
#ifdef VERBOSE
				if (line_number%64 == 0) {
					pagemapentry = get_pagemap_entry(&array[page_base_index+line_number*8]);
					printf("DEBUG: page_base_index %ld line_number %ld index %ld pagemapentry 0x%lx\n",page_base_index,line_number,page_base_index+line_number*8,pagemapentry);
				}
#endif
				do  {               // -------------- Inner Repeat Loop until results pass "goodness" tests --------------
					numtries++;
					if (numtries > 100) {
						printf("ERROR: No good results for line %d after %d tries\n",line_number,numtries);
						exit(101);
					}
					totaltries++;
				// 1. read L3 counters before starting test
				for (tile=0; tile<NUM_CHA_USED; tile++) {
					msr_num = 0xe00 + 0x10*tile + 0x8 + 1;				// counter 1 is the LLC_LOOKUPS.READ event
					pread(msr_fd[0],&msr_val,sizeof(msr_val),msr_num);
					cha_counts[0][tile][1][0] = msr_val;					//  use the array I have already declared for cha counts
					// printf("DEBUG: page %ld line %ld msr_num 0x%x msr_val %ld cha_counter1 %lu\n",
					//		page_number,line_number,msr_num,msr_val,cha_counts[0][tile][1][0]);
				}

				// 2. Access the line many times
				sum = 0;
				for (i=0; i<NFLUSHES; i++) {
					sum += array[page_base_index+line_number*8];
					_mm_mfence();
					_mm_clflush(&array[page_base_index+line_number*8]);
					_mm_mfence();
				}
				globalsum += sum;

				// 3. read L3 counters after loads are done
				for (tile=0; tile<NUM_CHA_USED; tile++) {
					msr_num = 0xe00 + 0x10*tile + 0x8 + 1;				// counter 1 is the LLC_LOOKUPS.READ event
					pread(msr_fd[0],&msr_val,sizeof(msr_val),msr_num);
					cha_counts[0][tile][1][1] = msr_val;					//  use the array I have already declared for cha counts
				}

#ifdef VERBOSE
				for (tile=0; tile<NUM_CHA_USED; tile++) {
					printf("DEBUG: page %ld line %ld cha_counter1_after %lu cha_counter1 before %lu delta %lu\n",
							page_number,line_number,cha_counts[0][tile][1][1],cha_counts[0][tile][1][0],cha_counts[0][tile][1][1]-cha_counts[0][tile][1][0]);
				}
#endif

				//   CHA counter 1 set to LLC_LOOKUP.READ
				//
				//  4. Determine which L3 slice owns the cache line and
				//  5. Save the CHA number in the cha_by_page[page][line] array

				// first do a rough quantitative checks of the "goodness" of the data
				//		goodness1 = max/NFLUSHES (pass if >95%)
				// 		goodness2 = min/NFLUSHES (pass if <20%)
				//		goodness3 = avg/NFLUSHES (pass if <40%)
				max_count = 0;
				min_count = 1<<30;
				sum_count = 0;
				for (tile=0; tile<NUM_CHA_USED; tile++) {
					max_count = MAX(max_count, cha_counts[0][tile][1][1]-cha_counts[0][tile][1][0]);
					min_count = MIN(min_count, cha_counts[0][tile][1][1]-cha_counts[0][tile][1][0]);
					sum_count += cha_counts[0][tile][1][1]-cha_counts[0][tile][1][0];
				}
				avg_count = (double)(sum_count - max_count) / (double)(NUM_CHA_USED);
				goodness1 = (double) max_count / (double) NFLUSHES;
				goodness2 = (double) min_count / (double) NFLUSHES;
				goodness3 =          avg_count / (double) NFLUSHES;
				// compare the goodness parameters with manually chosen limits & combine into a single pass (good=1) or fail (good=0)
				pass1 = 0;
				pass2 = 0;
				pass3 = 0;
				if ( goodness1 > 0.95 ) pass1 = 1;
				if ( goodness2 < 0.20 ) pass2 = 1;
				if ( goodness3 < 0.40 ) pass3 = 1;
				good_new = pass1 * pass2 * pass3;
#ifdef VERBOSE
				printf("GOODNESS: line_number %ld max_count %d min_count %d sum_count %d avg_count %f goodness1 %f goodness2 %f goodness3 %f pass123 %d %d %d\n",
								  line_number, max_count, min_count, sum_count, avg_count, goodness1, goodness2, goodness3, pass1, pass2, pass3);
				if (good_new == 0) printf("DEBUG: one or more of the sanity checks failed for line=%ld: %d %d %d goodness values %f %f %f\n",
					line_number,pass1,pass2,pass3,goodness1,goodness2,goodness3);
#endif

				// test to see if more than one CHA reports > 0.95*NFLUSHES events
				found = 0;
				old_cha = -1;
				int min_counts = (NFLUSHES*19)/20;
				for (tile=0; tile<NUM_CHA_USED; tile++) {
					if (cha_counts[0][tile][1][1]-cha_counts[0][tile][1][0] >= min_counts) {
						old_cha = cha_by_page[page_number][line_number];
						cha_by_page[page_number][line_number] = tile;
						found++;
#ifdef VERBOSE
						if (found > 1) {
							printf("WARNING: Multiple (%d) CHAs found using counter 1 for cache line %ld, index %ld: old_cha %d new_cha %d\n",found,line_number,page_base_index+line_number*8,old_cha,cha_by_page[page_number][line_number]);
						}
#endif
					}
				}
				if (found == 0) {
					good_old = 0;
#ifdef VERBOSE
					printf("WARNING: no CHA entry has been found for line %ld!\n",line_number);
					printf("DEBUG dump for no CHA found\n");
					for (tile=0; tile<NUM_CHA_USED; tile++) {
						printf("CHA %d LLC_LOOKUP.READ          delta %ld\n",tile,(cha_counts[0][tile][1][1]-cha_counts[0][tile][1][0]));
					}
#endif
				} else if (found == 1) {
					good_old = 1;
				} else {
					good_old = 0;
#ifdef VERBOSE
					printf("DEBUG dump for multiple CHAs found\n");
					for (tile=0; tile<NUM_CHA_USED; tile++) {
						printf("CHA %d LLC_LOOKUP.READ          delta %ld\n",tile,(cha_counts[0][tile][1][1]-cha_counts[0][tile][1][0]));
					}
#endif
				}
				good = good_new * good_old;         // trigger a repeat if either the old or new tests failed
				}
				while (good == 0);
#if 0
				// 6. save the cache line number in the appropriate the cbo_indices[cbo][#lines] array
				// 7. increment the corresponding cbo_num_lines[cbo] array entry
				this_cbo = cha_by_page[page_number][line_number];
				if (this_cbo == -1) {
					printf("ERROR: cha_by_page[%ld][%ld] has not been set!\n",page_number,line_number);
					exit(80);
				}
				cbo_indices[this_cbo][cbo_num_lines[this_cbo]] = line_number;
				cbo_num_lines[this_cbo]++;
#endif
			}
			// I have not overwritten the filename, but I will rebuild it here just in case I add something stupid in between....
			sprintf(filename,"PADDR_0x%.12lx.map",paddr_by_page[page_number]);
			ptr_mapping_file = fopen(filename,"w");
			if (!ptr_mapping_file) {
				printf("ERROR: Failed to open Mapping File %s for writing -- aborting\n",filename);
				exit(4);
			}
			// first try -- write one record of 32768 bytes
			rc64 = fwrite(&cha_by_page[page_number][0],(size_t) 32768, (size_t) 1, ptr_mapping_file);
			if (rc64 != 1) {
				printf("ERROR: failed to write one 32768 Byte record to  %s -- return code %ld\n",filename,rc64);
				exit(5);
			} else {
				printf("SUCCESS: wrote mapping file %s\n",filename);
			}
		}
	}
	printf("DUMMY: globalsum %d\n",globalsum);
	printf("VERBOSE: L3 Mapping Complete in %ld tries for %d cache lines ratio %f\n",totaltries,32768*PAGES_MAPPED,(double)totaltries/(double)(32768*PAGES_MAPPED));

#ifndef MYHUGEPAGE_1GB
	// now that the mapping is complete, I can add up the number of lines mapped to each CHA
	// be careful to count only the lines that are used, not the full 24MiB
	// 3 million elements is ~11.44 2MiB pages, so count all lines in each of the first 11 pages
	// If I did the arithmetic correctly, the 3 million elements uses 931328 Bytes of the 12th 2MiB page
	// which is 116416 elements or 14552 cache lines.

	// first accumulate the first 11 full pages
	for (page_number=0; page_number<11; page_number++) {
		for (line_number=0; line_number<32768; line_number++) {
			lines_by_cha[cha_by_page[page_number][line_number]]++;
		}
	}
	// then accumulate the partial 12th page
	for (line_number=0; line_number<14552; line_number++) {
		lines_by_cha[cha_by_page[11][line_number]]++;
	}
	// output
	long lines_accounted = 0;
	printf("LINES_BY_CHA");
	for (i=0; i<NUM_CHA_USED; i++) {
		printf(" %ld",lines_by_cha[i]);
		lines_accounted += lines_by_cha[i];
	}
	printf("\n");
	printf("ACCCOUNTED FOR %ld lines expected %ld lines\n",lines_accounted,l2_contained_size/8);
#endif

// ============== END L3 MAPPING TESTS ==============================


	// NEW LOOP STRUCTURE -- MCCALPIN
	// I want to run the test at various offsets within each of the 1GiB
	// pages allocated.
	// Start with repeating the test for the beginning of each 1GiB page.
	// I can simply add 134,217,728 to the jstart and jend values to 
	// move to the next 1GiB page

	printf("DEBUG: jstart[0] = %ld\n",jstart[0]);
	long current_page;
	for (current_page=0; current_page < NUMPAGES; current_page++) {
		if (current_page > 0) {
			for (i=0; i<CORES_USED; i++) {
				jstart[i] += 134217728;
				jend[i] += 134217728;
			}
			printf("DEBUG: jstart[0] = %ld\n",jstart[0]);
		}


	// For the snoop filter tests, I want to repeatedly read 
	// some number of arrays per core with an aggregate footprint
	// close to 1MiB per core 
	// 24 cores = 24 MiB = 3 Mi elements, so 
	// using an array length of 3 million should be just about right 95.3674%

	// l2_contained_size = arraylen;			// only use if I want a large memory-contained version
	inner_repetitions = 1000;
	int stride = 1;		// used in thread binding checks: use 2 for Dell nodes, 1 for Intel nodes

	// try to pre-load the working data into the L2 caches before the initial performance counter reads
	sum = 0.0;
#pragma omp parallel for reduction(+:sum)
	for (j=jstart[0]; j<jstart[0]+l2_contained_size; j++) sum += array[j];

	// While I am at it, I need to warm up the cores using AVX-512 code to get them to full frequency
	// This may take up to 100 microseconds, or maybe 400,000 AVX512 instructions per thread.
	// This is a pain because I can't trust the compiler to generate AVX512 code at any given time,
	// so I have to resort to inline assembly.
	tsc_start = rdtsc();
#pragma omp parallel for 
	for (i=0; i<CORES_USED; i++) {
		for (j=0; j<10*1000*1000; j++) {
			__asm__ __volatile__ (
				"vpaddq %%zmm0, %%zmm1, %%zmm2\n\t"
				"vpaddq %%zmm1, %%zmm2, %%zmm3\n\t"
				"vpaddq %%zmm2, %%zmm3, %%zmm0\n\t"
				"vpaddq %%zmm3, %%zmm0, %%zmm1"
			: : : "zmm0","zmm1","zmm2","zmm3");
		}
	}
	tsc_end = rdtsc();
	printf("DEBUG: WARMUP LOOP took %lu TSC cycles\n",tsc_end - tsc_start);


// =================== BEGINNING OF PERFORMANCE COUNTER READS BEFORE KERNEL TESTING ==============================
#ifdef IMC_COUNTS
	// read the initial values of the IMC counters
    for (socket=0; socket<NUM_SOCKETS; socket++) {
        bus = IMC_BUS_Socket[socket];
        for (channel=0; channel<NUM_IMC_CHANNELS; channel++) {
            device = IMC_Device_Channel[channel];
            function = IMC_Function_Channel[channel];
            for (counter=0; counter<NUM_IMC_COUNTERS; counter++) {
                offset = IMC_PmonCtr_Offset[counter];
                index = PCI_cfg_index(bus, device, function, offset);

                // read each counter twice to identify rare cases where the low-order bits
                // overflow and increment the high-order bits between the two reads.
                // Use the second set of values unless (( high_1 != high_0 ) && ( low_1 > low_0))
                //   (this indicates that the counter rolled between the 3rd and 4th reads).
                low_0 = mmconfig_ptr[index];
                high_0 = mmconfig_ptr[index+1];

                low_1 = mmconfig_ptr[index];
                high_1 = mmconfig_ptr[index+1];

                if ( (high_1 != high_0) && (low_1 > low_0) ) {
                    count = ((uint64_t) high_0) << 32 | (uint64_t) low_0;
                } else {
                    count = ((uint64_t) high_1) << 32 | (uint64_t) low_1;
                }
                imc_counts[socket][channel][counter][0] = count;
            }
        }
    }
#if 0
	// for debugging only: print initial values of IMC counts
	for (socket=0; socket<NUM_SOCKETS; socket++) {
		for (channel=0; channel<NUM_IMC_CHANNELS; channel++) {
			fprintf(stdout,"%d %d",socket,channel);
			for (counter=0; counter<NUM_IMC_COUNTERS; counter++) {
				fprintf(stdout," %ld",imc_counts[socket][channel][counter][0]);
			}
			fprintf(stdout,"\n");
		}
	}
#endif
#endif

#ifdef CHA_COUNTS
	// read the initial values of the CHA mesh counters
	for (pkg=0; pkg<2; pkg++) {
		for (tile=0; tile<NUM_CHA_USED; tile++) {
			for (counter=0; counter<4; counter++) {
				msr_num = 0xe00 + 0x10*tile + 0x8 + counter;
				pread(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
				cha_counts[pkg][tile][counter][0] = msr_val;
			}
		}
	}
#if 0
	// for debugging only: print initial values of CHA counters
	for (pkg=0; pkg<2; pkg++) {
		for (tile=0; tile<NUM_CHA_USED; tile++) {
			for (counter=0; counter<4; counter++) {
				printf("Package %d, tile %d, counter %d, value %lu\n",pkg,tile,counter,cha_counts[pkg][tile][counter][0]);
			}
		}
	}
#endif
#endif

	// ===================================== CODE TO TEST BEGINS HERE =======================================================================

	tsc_start = rdtsc();

#pragma omp parallel for private(counter)
	for (i=0; i<CORES_USED; i++) {
		if (get_core_number() != stride*i) {
			printf("ERROR: thread %d is in the wrong place %d\n",i,get_core_number());
		}
		for (counter=0; counter<4; counter++) {
			core_counters[i][counter][0] = rdpmc(counter);
		}
	}


#ifdef SIMPLE_OMP_LOOP
	for (k=0; k<inner_repetitions; k++) {
#pragma omp parallel for, reduction(+:sum)
		for (j=jstart[0]; j<jstart[0]+l2_contained_size; j++) {
			sum += array[j];
		}
	}
#ifdef CHECK_START_STOP
	printf("CHECK_START_STOP: SIMPLE_OMP_LOOP: start %ld end %ld vl %ld\n",jstart[0],jstart[0]+l2_contained_size,l2_contained_size);
#endif
#else
#pragma omp parallel for private(j,k,iters,private_sum)
	for (i=0; i<CORES_USED; i++) {
		iters = 0;
		partial_sums[i] = 0.0;
		fixed_counters[i][0][0] = rdpmc_instructions();
		fixed_counters[i][1][0] = rdpmc_actual_cycles();
		fixed_counters[i][2][0] = rdpmc_reference_cycles();
		fixed_counters[i][3][0] = rdtsc();
		for (k=0; k<inner_repetitions; k++) {
			private_sum = ssum(&array[jstart[i]],vl[i]);
			partial_sums[i] += private_sum;
			iters++;
		}
		fixed_counters[i][0][1] = rdpmc_instructions();
		fixed_counters[i][1][1] = rdpmc_actual_cycles();
		fixed_counters[i][2][1] = rdpmc_reference_cycles();
		fixed_counters[i][3][1] = rdtsc();
		iteration_counts[i] = iters;
	}
#ifdef CHECK_START_STOP
	for (i=0; i<CORES_USED; i++) {
		printf("CHECK_START_STOP: PER-THREAD-INDICES: thread %d jstart %ld jstop %ld vl %ld\n",i,jstart[i],jend[i],vl[i]);
	}
#endif
#endif
	for (i=0; i<CORES_USED; i++) {
		sum += partial_sums[i];
	}

#pragma omp parallel for private(counter)
	for (i=0; i<CORES_USED; i++) {
		if (get_core_number() != stride*i) {
			printf("ERROR: thread %d is in the wrong place %d\n",i,get_core_number());
		}
		for (counter=0; counter<4; counter++) {
			core_counters[i][counter][1] = rdpmc(counter);
			if (core_counters[i][counter][1] == SPECIAL_VALUE) {
				printf("BADNESS: SPECIAL_VALUE value returned on thread %d counter %d\n",i,counter);
			}
#ifdef RETRIES
			// if the counter returns zero, read it one more time....
			if (core_counters[i][counter][1] == SPECIAL_VALUE) {
				core_counters[i][counter][1] = rdpmc(counter);
#pragma omp atomic update 
				retries++;
			}
#endif
		}
	}
	tsc_end = rdtsc();

	for (i=0; i<CORES_USED; i++) {
		for (counter=0; counter<4; counter++) {
			if (core_counters[i][counter][0] == SPECIAL_VALUE) {
				printf("DEBUG: SPECIAL_VALUE found after loop in start count on thread %d counter %d\n",i,counter);
				zeros++;
			}
			if (core_counters[i][counter][1] == SPECIAL_VALUE) {
				printf("DEBUG: SPECIAL_VALUE found after loop in end count on thread %d counter %d\n",i,counter);
				zeros++;
			}
		}
	}

// ===================================== END OF CODE UNDER TEST  ========================================================
#ifdef CHA_COUNTS
	// read the final values of the CHA mesh counters
	for (pkg=0; pkg<2; pkg++) {
		for (tile=0; tile<NUM_CHA_USED; tile++) {
			for (counter=0; counter<4; counter++) {
				msr_num = 0xe00 + 0x10*tile + 0x8 + counter;
				pread(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
				cha_counts[pkg][tile][counter][1] = msr_val;
			}
		}
	}
#endif

#ifdef IMC_COUNTS
	for (socket=0; socket<NUM_SOCKETS; socket++) {
		bus = IMC_BUS_Socket[socket];
		for (channel=0; channel<NUM_IMC_CHANNELS; channel++) {
			device = IMC_Device_Channel[channel];
			function = IMC_Function_Channel[channel];
			for (counter=0; counter<NUM_IMC_COUNTERS; counter++) {
				offset = IMC_PmonCtr_Offset[counter];
				index = PCI_cfg_index(bus, device, function, offset);

				// read each counter twice to identify rare cases where the low-order bits
				// overflow and increment the high-order bits between the two reads.
				// Use the second set of values unless (( high_1 != high_0 ) && ( low_1 > low_0))
				//   (this indicates that the counter rolled between the 3rd and 4th reads).
				low_0 = mmconfig_ptr[index];
				high_0 = mmconfig_ptr[index+1];

				low_1 = mmconfig_ptr[index];
				high_1 = mmconfig_ptr[index+1];

				if ( (high_1 != high_0) && (low_1 > low_0) ) {
					count = ((uint64_t) high_0) << 32 | (uint64_t) low_0;
				} else {
					count = ((uint64_t) high_1) << 32 | (uint64_t) low_1;
				}
				imc_counts[socket][channel][counter][1] = count;
			}
		}
	}
#endif
// ================================== END OF PERFORMANCE COUNTER READS AFTER TEST  ==============================================

	t0 = 0.0;
	t1 = (double) (tsc_end - tsc_start) / tsc_rate;
	printf("Instrumented code required %f seconds to execute\n",t1-t0);
	bandwidth = sizeof(double)*(double)l2_contained_size*(double)inner_repetitions / (t1-t0) / 1e9;
	printf("Bandwidth %f GB/s\n",bandwidth);
	printf("Bandwidth per core %f GB/s\n",bandwidth/(double)CORES_USED);
	printf("Approx Bytes/cycle per core %f\n",bandwidth/(double)CORES_USED/2.0);

	expected = (double)l2_contained_size * (double)(inner_repetitions) / (double)CORES_USED;
	avg_cycles = (double)(tsc_end - tsc_start) / expected;
	printf("Average TSC cycles per element %f\n",avg_cycles);

	// clear the arrays for the package-level sums
	for (pkg=0; pkg<2; pkg++) {
		for (counter=0; counter<4; counter++) {			// no point in summing the cycle counts, so exclude counter 4
			core_pkg_sums[pkg][counter] = 0;
			fixed_pkg_sums[pkg][counter] = 0;
			imc_pkg_sums[pkg][counter] = 0;
			cha_pkg_sums[pkg][counter] = 0;
		}
	}

	// compute core package sums and optional print
	for (i=0; i<CORES_USED; i++) {
		for (counter=0; counter<4; counter++) {
			tag = 10*i + counter;
			delta = corrected_delta48(tag,fixed_counters[i][counter][1],fixed_counters[i][counter][0]);
			fixed_pkg_sums[0][counter] += delta;
		}
		for (counter=0; counter<4; counter++) {
			tag = 1000 + 10*i + counter;
			if (core_counters[i][counter][0] == SPECIAL_VALUE) {
				printf("DEBUG: SPECIAL_VALUE found in post-processing in start count on thread %d counter %d\n",i,counter);
			}
			if (core_counters[i][counter][1] == SPECIAL_VALUE) {
				printf("DEBUG: SPECIAL_VALUE found in post-processing in end count on thread %d counter %d\n",i,counter);
			}
			delta = corrected_delta48(tag,core_counters[i][counter][1],core_counters[i][counter][0]);
#ifdef VERBOSE
			printf("CORE %d counter %d end %ld start %ld delta %ld\n",i,counter,core_counters[i][counter][1],core_counters[i][counter][0],delta);
#endif
			core_pkg_sums[0][counter] += delta;
		}


	}

	if (dumpall == 1) {
		report = 0;
		for (i=0; i<CORES_USED; i++) {
			for (counter=0; counter<4; counter++) {
				delta = corrected_delta48(tag,core_counters[i][counter][1],core_counters[i][counter][0]);
				printf("CORE %d counter %d end %ld start %ld delta %ld\n",i,counter,core_counters[i][counter][1],core_counters[i][counter][0],delta);
			}
		}
	}
	report = 1;
	dumpall = 0;

#ifdef CHA_COUNTS
	// print out the differences and compute sums of differences
	for (pkg=0; pkg<2; pkg++) {
		for (tile=0; tile<NUM_CHA_USED; tile++) {
			for (counter=0; counter<4; counter++) {
				tag = 2000 + 10*tile + counter;
				delta = corrected_delta48(tag,cha_counts[pkg][tile][counter][1],cha_counts[pkg][tile][counter][0]);
#ifdef VERBOSE
				printf("CHA pkg %d tile %d counter %d delta %ld\n",pkg,tile,counter,delta);
#endif
				cha_pkg_sums[pkg][counter] += delta;
			}
		}
	}
#endif
#ifdef IMC_COUNTS
	for (pkg=0; pkg<2; pkg++) {
		for (channel=0; channel<NUM_IMC_CHANNELS; channel++) {
			for (counter=0; counter<NUM_IMC_COUNTERS; counter++) {
				tag = 3000 + 10*channel + counter;
				delta = corrected_delta48(tag,imc_counts[pkg][channel][counter][1],imc_counts[pkg][channel][counter][0]);
#ifdef VERBOSE
				printf("IMC pkg %d channel %d counter %d delta %ld\n",pkg,channel,counter,delta);
#endif
				imc_pkg_sums[pkg][counter] += delta;
			}
		}
	}
#endif

	int max_display_pkg = 1;

	for (pkg=0; pkg<max_display_pkg; pkg++) {
		for (counter=0; counter<4; counter++) {
			printf("CORE_PKG_SUMS pkg %d counter %d sum_delta %ld\n",pkg,counter,core_pkg_sums[pkg][counter]);
		}
	}

	for (pkg=0; pkg<max_display_pkg; pkg++) {
		for (counter=0; counter<4; counter++) {
			printf("FIXED_PKG_SUMS pkg %d counter %d sum_delta %ld\n",pkg,counter,fixed_pkg_sums[pkg][counter]);
		}
	}

	// the fixed-function counters are measured inside the OpenMP loop, so they should not be contaminated by 
	// spin-waiting....
	// Compute per-core metrics here -- note that the fixed-function counter set is (Instr, CoreCyc, RefCyc, TSC)
	// 		Utilization = RefCyc/TSC (fixed2/fixed3)
	// 		AvgGHz_unhalted = CoreCyc/RefCyc * 2.1  (fixed1/fixed2 * 2.1)
	// 		AvgGHz_wall = CoreCyc/TSC * 2.1 (fixed1/fixed3 * 2.1)
	// 		IPC = Instr/CoreCyc (fixed0/fixed1)
	long delta_inst, delta_core, delta_ref, delta_tsc;
	double utilization, avg_ghz, ipc;

	printf("CORE_UTILIZATION ");
	for (i=0; i<CORES_USED; i++) {
		tag = 0;
		delta_ref  = corrected_delta48(tag,fixed_counters[i][2][1],fixed_counters[i][2][0]);
		delta_tsc  = corrected_delta48(tag,fixed_counters[i][3][1],fixed_counters[i][3][0]);
		utilization = (double)delta_ref / (double)delta_tsc;
		printf("%6.4f ",utilization);
	}
	printf("\n");

	float TSC_GHz;
	TSC_GHz = get_TSC_frequency()/1.0e9;
	printf("CORE_GHZ ");
	for (i=0; i<CORES_USED; i++) {
		tag = 0;
		delta_core = corrected_delta48(tag,fixed_counters[i][1][1],fixed_counters[i][1][0]);
		delta_ref  = corrected_delta48(tag,fixed_counters[i][2][1],fixed_counters[i][2][0]);
		avg_ghz = (double)delta_core / (double)delta_ref * TSC_GHz;
		printf("%6.4f ",avg_ghz);
	}
	printf("\n");

	printf("CORE_IPC ");
	for (i=0; i<CORES_USED; i++) {
		tag = 0;
		delta_inst = corrected_delta48(tag,fixed_counters[i][0][1],fixed_counters[i][0][0]);
		delta_core = corrected_delta48(tag,fixed_counters[i][1][1],fixed_counters[i][1][0]);
		ipc = (double)delta_inst / (double)delta_core;
		printf("%6.4f ",ipc);
	}
	printf("\n");

	printf("THREAD_EXECUTION_TIME ");
	for (i=0; i<CORES_USED; i++) {
		tag = 0;
		delta_tsc  = corrected_delta48(tag,fixed_counters[i][3][1],fixed_counters[i][3][0]);
		t0 = (double)delta_tsc / (TSC_GHz*1.0e9);
		printf("%f ",t0);
	}
	printf("\n");



#ifdef CHA_COUNTS
	for (pkg=0; pkg<max_display_pkg; pkg++) {
		for (counter=0; counter<4; counter++) {
			printf("CHA_PKG_SUMS pkg %d counter %d sum_delta %ld\n",pkg,counter,cha_pkg_sums[pkg][counter]);
		}
	}
#endif
#ifdef IMC_COUNTS
	for (pkg=0; pkg<max_display_pkg; pkg++) {
		for (counter=0; counter<4; counter++) {			// no point in summing the cycle counts, so exclude counter 4
			printf("IMC_PKG_SUMS pkg %d counter %d sum_delta %ld\n",pkg,counter,imc_pkg_sums[pkg][counter]);
		}
	}
#endif

	


	// for the Snoop Filter set 
	// 	expected = expected number of cache lines loaded from L2
	// 	sf_evict_rate = #evictions / expected number of loads
	expected = 8.0/64.0* (double)l2_contained_size * (double) inner_repetitions;
	sf_evict_rate = (double) cha_pkg_sums[0][0] / expected;
	printf("SnoopFilterEvictionRate %f\n",sf_evict_rate);

	expected = (double)l2_contained_size * (double) (inner_repetitions+1); // adjusted for pre-load of data
	printf("Dummy Sum value is %f, expected value %f\n",sum,expected);

	expected = (double)l2_contained_size * (double) inner_repetitions;
	printf("Expected number of cache lines loaded from L2 %f\n",expected/8.0);
	printf("Number of performance counter wraprounds detected %d\n",nwraps);
#ifdef RETRIES
	printf("Number of core performance counter reads retried %d\n",retries);
#endif
	printf("Number of zero values found in the inner loop %d\n",zeros);
	// printf("Expected Number of Loads for AVX2 code %ld\n",arraylen/4);	
	// printf("Expected Number of Cache Lines loaded %ld\n",arraylen/8);	

	for (i=0; i<CORES_USED; i++) {
		if (iteration_counts[i] != inner_repetitions) {
			printf("ERROR: thread %d iteration_counts %ld expected %ld\n",i,iteration_counts[i],inner_repetitions);
		}
	}

	// per-core performance counter values
	for (counter=0; counter<4; counter++) {
		printf("CORE_counter %d ",counter);
		for (i=0; i<CORES_USED; i++) {
			delta = corrected_delta48(tag,core_counters[i][counter][1],core_counters[i][counter][0]);
			printf("%ld ",delta);
		}
		printf("\n");
	}
	// per-CHA performance counter values -- socket 0 only
	for (counter=0; counter<4; counter++) {
		printf("CHA_counter %d ",counter);
		for (i=0; i<NUM_CHA_USED; i++) {
			delta = corrected_delta48(tag,cha_counts[0][i][counter][1],cha_counts[0][i][counter][0]);
			printf("%ld ",delta);
		}
		printf("\n");
	}

	printf("Double-check physical address of first element used in array\n");
	pagemapentry = get_pagemap_entry(&array[jstart[0]]);
	printf("  array[%ld] va 0x%.16lx pa 0x%.16lx\n",jstart[0],&array[jstart[0]],pagemapentry);
	}
}
