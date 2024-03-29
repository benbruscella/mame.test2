/***************************************************************************

  machine.c

  Functions to emulate general aspects of the machine (RAM, ROM, interrupts,
  I/O ports)

***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mame.h"
#include "driver.h"
#include "machine.h"
#include "common.h"
#include "osdepend.h"

#ifdef UNIX
#define uclock_t clock_t
#define	uclock clock
#define UCLOCKS_PER_SEC CLOCKS_PER_SEC
#endif


static struct RunningMachine machine;
struct RunningMachine *Machine = &machine;
static const struct MachineDriver *drv;
unsigned (*opcode_decode)(dword A);

int frameskip;


#define MAX_COLORS 256	/* can't handle more than 256 colors on screen */
#define MAX_COLOR_TUPLE 8	/* no more than 3 bits per pixel, for now */
#define MAX_COLOR_CODES 256	/* no more than 256 color codes, for now */


unsigned char RAM[0x20000];		/* 64k for ROM/RAM and 64k for other uses (gfx, samples...) */

static unsigned char remappedtable[MAX_COLOR_TUPLE*MAX_COLOR_CODES];


/***************************************************************************

  Initialize the emulated machine (load the roms, initialize the various
  subsystems...). Returns 0 if successful.

***************************************************************************/
int init_machine(const char *gamename,int argc,char **argv)
{
	int i;
	const struct MemoryReadAddress *mra;
	const struct MemoryWriteAddress *mwa;


	frameskip = 0;
	for (i = 1;i < argc;i++)
	{
		if (stricmp(argv[i],"-frameskip") == 0)
		{
			i++;
			if (i < argc)
			{
				frameskip = atoi(argv[i]);
				if (frameskip < 0) frameskip = 0;
				if (frameskip > 3) frameskip = 3;
			}
		}
	}

	i = 0;
	while (drivers[i].name && stricmp(gamename,drivers[i].name) != 0)
		i++;

	if (drivers[i].name == 0)
	{
		printf("game \"%s\" not supported\n",gamename);
		return 1;
	}

	drv = drivers[i].drv;
	Machine->drv = drv;
	opcode_decode = drivers[i].opcode_decode;

	if (readroms(RAM,drivers[i].rom,gamename) != 0)
		return 1;

	/* initialize the memory base pointers for memory hooks */
	mra = drv->memory_read;
	while (mra->start != -1)
	{
		if (mra->base) *mra->base = &RAM[mra->start];
		mra++;
	}
	mwa = drv->memory_write;
	while (mwa->start != -1)
	{
		if (mwa->base) *mwa->base = &RAM[mwa->start];
		mwa++;
	}

	if (*drv->init_machine && (*drv->init_machine)(gamename) != 0)
		return 1;

	if (*drv->vh_init && (*drv->vh_init)(gamename) != 0)
		return 1;

	if (*drv->sh_init && (*drv->sh_init)(gamename) != 0)
		return 1;

	return 0;
}



void vh_close(void)
{
	int i;


	for (i = 0;i < MAX_GFX_ELEMENTS;i++) freegfx(Machine->gfx[i]);
	osd_close_display();
}



int vh_open(void)
{
	int i;
	unsigned char pens[MAX_COLORS];
	const unsigned char *palette,*colortable;
	unsigned char convpalette[3 * MAX_COLORS];
	unsigned char convtable[MAX_COLOR_TUPLE*MAX_COLOR_CODES];


	if ((Machine->scrbitmap = osd_create_display(drv->screen_width,drv->screen_height)) == 0)
		return 1;

	if (drv->color_prom && drv->vh_convert_color_prom)
	{
		(*drv->vh_convert_color_prom)(convpalette,convtable,drv->color_prom);
		palette = convpalette;
		colortable = convtable;
	}
	else
	{
		palette = drv->palette;
		colortable = drv->colortable;
	}

	for (i = 0;i < drv->total_colors;i++)
		pens[i] = osd_obtain_pen(palette[3*i],palette[3*i+1],palette[3*i+2]);

	Machine->background_pen = pens[0];

	for (i = 0;i < 4 * drv->color_codes;i++)
		remappedtable[i] = pens[colortable[i]];


	for (i = 0;i < Machine->scrbitmap->height;i++)
		memset(Machine->scrbitmap->line[i],Machine->background_pen,Machine->scrbitmap->width);


	for (i = 0;i < MAX_GFX_ELEMENTS;i++)
		Machine->gfx[i++] = 0;
	for (i = 0;i < MAX_GFX_ELEMENTS && drv->gfxdecodeinfo[i].start != -1;i++)
	{
		if ((Machine->gfx[i] = decodegfx(RAM + drv->gfxdecodeinfo[i].start,
				drv->gfxdecodeinfo[i].gfxlayout)) == 0)
		{
			vh_close();
			return 1;
		}

		Machine->gfx[i]->colortable = &remappedtable[4 * drv->gfxdecodeinfo[i].first_color_code];
		Machine->gfx[i]->total_colors =
				drv->gfxdecodeinfo[i].last_color_code - drv->gfxdecodeinfo[i].first_color_code + 1;
	}

	return 0;
}



/***************************************************************************

  Run the emulation. Start the various subsystems and the CPU emulation.
  Returns non zero in case of error.

***************************************************************************/
int run_machine(const char *gamename)
{
	int res = 1;


	if (vh_open() == 0)
	{
		if (*drv->vh_start == 0 || (*drv->vh_start)() == 0)	/* start the video hardware */
		{
			if (*drv->sh_start == 0 || (*drv->sh_start)() == 0)	/* start the audio hardware */
			{
				FILE *f;
				char name[100];
				int i;


				for (i = 0;i < MAX_DIP_SWITCHES;i++)
					Machine->dsw[i] = drv->defaultdsw[i];

				/* read dipswitch settings from disk */
				sprintf(name,"%s/%s.dsw",gamename,gamename);
				if ((f = fopen(name,"rb")) != 0)
				{
					fread(Machine->dsw,1,MAX_DIP_SWITCHES,f);
					fclose(f);
				}

				Z80_IPeriod = drv->cpu_clock / drv->frames_per_second;	/* Number of T-states per interrupt */
				Z80_Reset();
				Z80();		/* start the CPU emulation */

				if (*drv->sh_stop) (*drv->sh_stop)();
				if (*drv->vh_stop) (*drv->vh_stop)();

				/* write dipswitch settings to disk */
				sprintf(name,"%s/%s.dsw",gamename,gamename);
				if ((f = fopen(name,"wb")) != 0)
				{
					fwrite(Machine->dsw,1,MAX_DIP_SWITCHES,f);
					fclose(f);
				}

				res = 0;
			}
			else printf("Unable to start audio emulation\n");
		}
		else printf("Unable to start video emulation\n");

		vh_close();
	}
	else printf("Unable to initialize display\n");

	return res;
}



/* some functions commonly used by emulators */

/* start with interrupts enabled, so the generic routine will work even if */
/* the machine doesn't have an interrupt enable port */
static int interrupt_enable = 1;

void interrupt_enable_w(int offset,int data)
{
	interrupt_enable = data;
}



/* this generic interrupt function assumes that the emulated game uses */
/* interrupt mode 1, which doesn't require an interrupt vector. If the */
/* game you are emulating is different (like Pac Man), or it doesn't */
/* have vertical blank interrupts (like Lady Bug) you'll have to provide */
/* your own. */
int interrupt(void)
{
	if (interrupt_enable == 0) return Z80_IGNORE_INT;
	else return 0xff;
}



int nmi_interrupt(void)
{
	if (interrupt_enable == 0) return Z80_IGNORE_INT;
	else return Z80_NMI_INT;
}



/***************************************************************************

  Perform a memory read. This function is called by the CPU emulation.

***************************************************************************/
unsigned Z80_RDMEM (dword A)
{
	const struct MemoryReadAddress *mra;


	mra = drv->memory_read;
	while (mra->start != -1)
	{
		if (A >= mra->start && A <= mra->end)
		{
			switch ((int)mra->handler)
			{
				case MRA_NOP:
					return 0;
					break;
				case MRA_RAM:
				case MRA_ROM:
					return RAM[A];
					break;
				default:
					return (*mra->handler)(A - mra->start);
					break;
			}
		}

		mra++;
	}

	if (errorlog) fprintf(errorlog,"%04x: warning - read unmapped memory address %04x\n",Z80_GetPC(),A);
	return RAM[A];
}



/***************************************************************************

  Perform a memory write. This function is called by the CPU emulation.

***************************************************************************/
void Z80_WRMEM (dword A,byte V)
{
	const struct MemoryWriteAddress *mwa;


	mwa = drv->memory_write;
	while (mwa->start != -1)
	{
		if (A >= mwa->start && A <= mwa->end)
		{
			switch ((int)mwa->handler)
			{
				case MWA_NOP:
					break;
				case MWA_RAM:
					RAM[A] = V;
					break;
				case MRA_ROM:
					if (errorlog) fprintf(errorlog,"%04x: warning - write %02x to ROM address %04x\n",Z80_GetPC(),V,A);
					break;
				default:
					(*mwa->handler)(A - mwa->start,V);
					break;
			}

			return;
		}

		mwa++;
	}

	if (errorlog) fprintf(errorlog,"%04x: warning - write %02x to unmapped memory address %04x\n",Z80_GetPC(),V,A);
	RAM[A] = V;
}



static void drawfps(void)
{
	#define MEMORY 20
	static uclock_t prev[MEMORY];
	uclock_t curr;
	static int i;


	curr = uclock();
	if (prev[i])
	{
		int fps;
		uclock_t mtpf;

		mtpf = ((curr - prev[i])/MEMORY)/2;
		fps = (UCLOCKS_PER_SEC+mtpf)/2/mtpf;

		drawgfx(Machine->scrbitmap,Machine->gfx[0],fps/10 + drv->numbers_start,drv->white_text,0,0,0,0,0,TRANSPARENCY_NONE,0);
		drawgfx(Machine->scrbitmap,Machine->gfx[0],fps%10 + drv->numbers_start,drv->white_text,0,0,8,0,0,TRANSPARENCY_NONE,0);
	}

	prev[i] = curr;
	i = (i+1) % MEMORY;
}



int Z80_IRQ = Z80_IGNORE_INT;	/* needed by the CPU emulation */

int Z80_Interrupt(void)
{
	static int showfps;
	static uclock_t prev;
	uclock_t curr;
	static int framecount = 0;


	/* if the user pressed ESC, stop the emulation */
	if (osd_key_pressed(OSD_KEY_ESC)) Z80_Running = 0;

	/* if F3, reset the machine */
	if (osd_key_pressed(OSD_KEY_F3))
	{
		Z80_Reset();
		return Z80_IGNORE_INT;
	}

	if (osd_key_pressed(OSD_KEY_P)) /* pause the game */
	{
		struct DisplayText dt[] =
		{
			{ "PAUSED", drv->paused_color, drv->paused_x, drv->paused_y },
			{ 0 }
		};
		int key;


		displaytext(dt,0);

		while (osd_key_pressed(OSD_KEY_P));	/* wait for key release */
		do
		{
			key = osd_read_key();

			if (key == OSD_KEY_ESC) Z80_Running = 0;
			else if (key == OSD_KEY_TAB)
			{
				setdipswitches(Machine->dsw,drv->dswsettings);	/* might set CPURunning to 0 */
				displaytext(dt,0);
			}
		} while (Z80_Running && key != OSD_KEY_P);
		while (osd_key_pressed(key));	/* wait for key release */
	}

	/* if the user pressed TAB, go to dipswitch setup menu */
	if (osd_key_pressed(OSD_KEY_TAB)) setdipswitches(Machine->dsw,drv->dswsettings);

	(*drv->sh_update)();	/* update sound */
	osd_update_audio();

	if (++framecount > frameskip)
	{
		framecount = 0;

		(*drv->vh_screenrefresh)(Machine->scrbitmap);	/* update screen */

		if (osd_key_pressed(OSD_KEY_F11)) showfps = 1;
		if (showfps) drawfps();

		osd_update_display();

		osd_poll_joystick();

		/* now wait until it's time to trigger the interrupt */
		do
		{
			curr = uclock();
		} while ((curr - prev) < (frameskip+1) * UCLOCKS_PER_SEC/drv->frames_per_second);

		prev = curr;
	}

	return (*drv->interrupt)();
}



void Z80_Out(byte Port,byte Value)
{
	if (*drv->out) (*drv->out)(Port,Value);
	if (*drv->sh_out) (*drv->sh_out)(Port,Value);
}



byte Z80_In(byte Port)
{
	if (*drv->sh_in) return (*drv->sh_in)(Port);
	else return 0;
}



void Z80_Patch (Z80_Regs *Regs)
{
}



void Z80_Reti (void)
{
}



void Z80_Retn (void)
{
}
