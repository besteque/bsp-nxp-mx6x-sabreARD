/*
*
* Copyright (c) 2014, QNX Software Systems.
*
* Licensed under the Apache License, Version 2.0 (the "License"). You
* may not reproduce, modify or distribute this software except in
* compliance with the License. You may obtain a copy of the License
* at: http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" basis,
* WITHOUT WARRANTIES OF ANY KIND, either express or implied.
*
*/

/*
 * Test program for minidrivers, when the min driver handler is used in startup.
 *
 * Opens the minidriver data area and reads the data from it for display.
 *
 */

#ifdef __USAGE
%C - utility to display all registered minidrivers

%C:  Display a list of all registered minidrivers.
#endif

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syspage.h>

int main(int argc, char *argv[])
{
	int i, num_drivers = 0;
	struct mdriver_entry	*mdriver;

	mdriver = (struct mdriver_entry *) SYSPAGE_ENTRY(mdriver);
	num_drivers = _syspage_ptr->mdriver.entry_size/sizeof(*mdriver);

	printf("Number of Installed minidrivers = %d\n\n", num_drivers);
	for (i = 0; i < num_drivers; i++)
	{
		printf("Minidriver entry .. %d\n", i);
		printf("Name .............. %s\n", SYSPAGE_ENTRY(strings)->data +
			mdriver[i].name);
		printf("Interrupt ......... 0x%X\n", mdriver[i].intr);
		printf("Data size ......... %d\n", mdriver[i].data_size);
		printf("\n");
	}

	return EXIT_SUCCESS;
}



#if defined(__QNXNTO__) && defined(__USESRCVERSION)
#include <sys/srcversion.h>
__SRCVERSION("$URL: http://svn.ott.qnx.com/product/branches/7.0.0/trunk/hardware/support/mdriver/utils/mdriver-info/mdriver-info.c $ $Rev: 823226 $")
#endif
