#ifndef MACHINE_H
#define MACHINE_H


#include "osdepend.h"
#include "common.h"


#define MAX_DIP_SWITCHES 2
#define MAX_GFX_ELEMENTS 10

struct RunningMachine
{
	struct osd_bitmap *scrbitmap;	/* bitmap to draw into */
	struct GfxElement *gfx[MAX_GFX_ELEMENTS];	/* graphic sets (chars, sprites) */
								/* the first one is used by DisplayText() */
	int background_pen;	/* pen to use to clear the bitmap (DON'T use 0) */
	int dsw[MAX_DIP_SWITCHES];	/* dipswitch banks */
	const struct MachineDriver *drv;	/* contains the definition of the machine */
};


extern struct RunningMachine *Machine;


int init_machine(const char *gamename,int argc,char **argv);
int run_machine(const char *gamename);

/* some useful general purpose functions for the memory map */
void interrupt_enable_w(int offset,int data);
int interrupt(void);
int nmi_interrupt(void);


#endif
