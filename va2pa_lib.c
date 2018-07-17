#define _GNU_SOURCE

#define _XOPEN_SOURCE 500	// required by pread
#include <stdio.h>			// required by printf
#include <unistd.h>			// required by pread, close, getpid
#include <sys/types.h>		// required by open, getpid
#include <sys/stat.h>		// required by open
#include <fcntl.h>			// required by open

// declarations that calling routines will need
void print_pagemap_entry(unsigned long long pagemap_entry);
unsigned long long get_pagemap_entry( void * va );

// -----------------------------------------------------------------------------------------
// Function to take any pointer and look up the entry in /proc/$pid/pagemap
//   No error handling -- caller should check errno on a 0 return value.
//   Does not attempt to interpret bits -- returns them all in an unsigned long long
//   Returns 0 if the page is not currently mapped, or if an error occurs.
//   Note that the page shift bits are wrong in 2.6.32 kernels (and 2.6.34 on MIC).
//     I have not been able to figger out if these bits are correct in any Linux version.
//
// John D. McCalpin, mccalpin@tacc.utexas.edu
// Revised to 2013-04-18

unsigned long long get_pagemap_entry( void * va )
{
	ssize_t ret;
	off_t myoffset;

	pid_t mypid;
	char filename[32];		// needs 15 characters for the "/proc/" and "/pagemap", plus enough for the PID
	unsigned long long result;
	static int pagemap_fd;
	static int initialized=0;

	// on first call: get process pid, open /proc/$pid/pagemap, and save the file descriptor for subsequent calls
	if (initialized == 0) {
		mypid = getpid();
		sprintf(filename,"/proc/%d/pagemap",mypid);
		pagemap_fd = open(filename, O_RDONLY);
		if (pagemap_fd == 0) {
			return(0UL);		// user must check errno if a zero value is returned
		}
		initialized = 1;
	}

	myoffset = ((long long) va >> 12) << 3;	 // required to cast void pointer before using it

	ret = pread(pagemap_fd, &result, 8, myoffset);
	if (ret != 8) {
		return (0UL);			// user must check errno if a zero value is returned
	}
	return(result);
}
// -----------------------------------------------------------------------------------------


// -----------------------------------------------------------------------------------------
// McCalpin's function to print the PFN (Page Frame Number) from entries returned by get_pagemap_entry()
//   Warnings are printed if
//     a. the page is not present (bit 63 != 1), or
//     b. the page is swapped (bit 62 == 1)
//   Note that the page shift bits are wrong in 2.6.25 through 2.6.32 kernels, fixed in 2.6.33
//     Therefore this only prints the page frame number (bits 0..55)
//   The PFN value will not make sense if the page is swapped (bit 62 set)
//   The value should be all 0's if the page is unmapped, or if get_pagemap_entry() returned an error.
//
// John D. McCalpin, mccalpin@tacc.utexas.edu
// Revised to 2013-04-18

#define BIT_IS_SET(x, n)   (((x) & (1UL<<(n)))?1:0)	// more convenient 0/1 result

void print_pagemap_entry(unsigned long long pagemap_entry)
{
	int logpagesize;
	int pagesize;
	unsigned long framenumber;
	unsigned long tmp;

	tmp = BIT_IS_SET(pagemap_entry, 63);		// could also use ( (pagemap_entry >> 63) != 1 ) as the test
	if (tmp == 0) {
		printf("WARNING in print_pagemap_entry: page is not present.  Result = %.16lx\n",pagemap_entry);
	}
	tmp = BIT_IS_SET(pagemap_entry, 62);
	if (tmp != 0) {
		printf("WARNING in print_pagemap_entry: page is swapped.  Result = %.16lx\n",pagemap_entry);
	}

	framenumber = ( (pagemap_entry<<9) >> 9);	// clear upper 9 bits -- only works for unsigned types

#ifdef OLDKERNEL
	// Page size bits are broken in 2.6.32 kernels (Stampede) and 2.6.34 kernels (MIC)
	printf("print_pagemap_entry: argument = 0x%.16lx, framenumber = 0x%.16lx\n",pagemap_entry,framenumber);
#else
	// Dunno if this is fixed in newer kernels -- clearly broken in 2.6.32 and 2.6.34 (MIC)
	logpagesize = ( (pagemap_entry<<3) >> 58);	// clear bits 61-63, then shift (original) bit 55 down to 0;
	pagesize = 1 << logpagesize;
	printf("print_pagemap_entry: logpagesize = %d, pagesize = %d, framenumber = 0x%.16lx\n",logpagesize,pagesize,framenumber);
#endif
}

