/*
 * PSn00bSDK basic graphics example
 * (C) 2020-2023 Lameguy64, spicyjpeg - MPL licensed
 *
 * A comprehensive "advanced hello world" example showing how to set up the
 * screen with double buffering, draw basic graphics (a bouncing square) and use
 * PSn00bSDK's debug font API to quickly print some text, all while following
 * best practices. This is not necessarily the simplest hello world example and
 * may look daunting at first glance, but it is a good starting point for more
 * complex programs.
 *
 * In order to avoid cluttering the program with global variables (as many Sony
 * SDK examples and other PSn00bSDK examples written before this one do) two
 * custom structures are employed:
 *
 * - a RenderBuffer structure containing the DISPENV and DRAWENV objects that
 *   represent the location of the framebuffer in VRAM, as well as the ordering
 *   table (OT) used to sort GPU commands/primitives by their Z index and the
 *   actual buffer commands will be written to;
 * - a RenderContext structure holding two RenderBuffer instances plus some
 *   variables to keep track of which buffer is currently being drawn and how
 *   much of its primitive buffer has been filled up so far.
 *
 * A C++ version of this example is also available (see examples/hellocpp).
 */

#include "hwregs_c.h"
#include "psxapi.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <psxgpu.h>
#include <psxcd.h>
#include <psxspu.h>
#include <psxetc.h>
#include <stdlib.h>


// Length of the ordering table, i.e. the range Z coordinates can have, 0-15 in
// this case. Larger values will allow for more granularity with depth (useful
// when drawing a complex 3D scene) at the expense of RAM usage and performance.
#define OT_LENGTH 16

// Size of the buffer GPU commands and primitives are written to. If the program
// crashes due to too many primitives being drawn, increase this value.
#define BUFFER_LENGTH 8192

/* Framebuffer/display list class */

typedef struct {
	DISPENV disp_env;
	DRAWENV draw_env;

	uint32_t ot[OT_LENGTH];
	uint8_t  buffer[BUFFER_LENGTH];
} RenderBuffer;

typedef struct {
	RenderBuffer buffers[2];
	uint8_t      *next_packet;
	int          active_buffer;
} RenderContext;

void setup_context(RenderContext *ctx, int w, int h, int r, int g, int b) {
	// Place the two framebuffers vertically in VRAM.
	SetDefDrawEnv(&(ctx->buffers[0].draw_env), 0, 0, w, h);
	SetDefDispEnv(&(ctx->buffers[0].disp_env), 0, 0, w, h);
	SetDefDrawEnv(&(ctx->buffers[1].draw_env), 0, h, w, h);
	SetDefDispEnv(&(ctx->buffers[1].disp_env), 0, h, w, h);

	// Set the default background color and enable auto-clearing.
	setRGB0(&(ctx->buffers[0].draw_env), r, g, b);
	setRGB0(&(ctx->buffers[1].draw_env), r, g, b);
	ctx->buffers[0].draw_env.isbg = 1;
	ctx->buffers[1].draw_env.isbg = 1;

	// Initialize the first buffer and clear its OT so that it can be used for
	// drawing.
	ctx->active_buffer = 0;
	ctx->next_packet   = ctx->buffers[0].buffer;
	ClearOTagR(ctx->buffers[0].ot, OT_LENGTH);

	// Turn on the video output.
	SetDispMask(1);
}

void flip_buffers(RenderContext *ctx) {
	// Wait for the GPU to finish drawing, then wait for vblank in order to
	// prevent screen tearing.
	DrawSync(0);
	VSync(0);

	RenderBuffer *draw_buffer = &(ctx->buffers[ctx->active_buffer]);
	RenderBuffer *disp_buffer = &(ctx->buffers[ctx->active_buffer ^ 1]);

	// Display the framebuffer the GPU has just finished drawing and start
	// rendering the display list that was filled up in the main loop.
	PutDispEnv(&(disp_buffer->disp_env));
	DrawOTagEnv(&(draw_buffer->ot[OT_LENGTH - 1]), &(draw_buffer->draw_env));

	// Switch over to the next buffer, clear it and reset the packet allocation
	// pointer.
	ctx->active_buffer ^= 1;
	ctx->next_packet    = disp_buffer->buffer;
	ClearOTagR(disp_buffer->ot, OT_LENGTH);
}

void *new_primitive(RenderContext *ctx, int z, size_t size) {
	// Place the primitive after all previously allocated primitives, then
	// insert it into the OT and bump the allocation pointer.
	RenderBuffer *buffer = &(ctx->buffers[ctx->active_buffer]);
	uint8_t      *prim   = ctx->next_packet;

	addPrim(&(buffer->ot[z]), prim);
	ctx->next_packet += size;

	// Make sure we haven't yet run out of space for future primitives.
	assert(ctx->next_packet <= &(buffer->buffer[BUFFER_LENGTH]));

	return (void *) prim;
}

// A simple helper for drawing text using PSn00bSDK's debug font API. Note that
// FntSort() requires the debug font texture to be uploaded to VRAM beforehand
// by calling FntLoad().
void draw_text(RenderContext *ctx, int x, int y, int z, const char *text) {
	RenderBuffer *buffer = &(ctx->buffers[ctx->active_buffer]);

	ctx->next_packet = (uint8_t *)
		FntSort(&(buffer->ot[z]), ctx->next_packet, x, y, text);

	assert(ctx->next_packet <= &(buffer->buffer[BUFFER_LENGTH]));
}

/* Main */

#define SCREEN_XRES 320
#define SCREEN_YRES 240

// converts pcm into adpcm (28 -> 16 uint8_t)
void encode_block(int16_t *samples, uint8_t *out)
{
    int peak = 0;

    for (int i = 0; i < 28; i++) {
        int s = abs(samples[i]);
        if (s > peak)
            peak = s;
    }

    int shift = 0;
    while (shift < 12 && (peak >> shift) > 7)
        shift++;

	// i think the spu gets the shift and makes it quieter when shfit is larger.
    out[0] = (12 - shift);   // filter 0
    out[1] = 0x00;

    for (int i = 0; i < 14; i++) {
        int s0 = samples[i * 2] >> shift;
        int s1 = samples[i * 2 + 1] >> shift;

        if (s0 < -8) s0 = -8;
        if (s0 >  7) s0 =  7;
        if (s1 < -8) s1 = -8;
        if (s1 >  7) s1 =  7;

        out[2 + i] = (s1 << 4) | (s0 & 0xf);
    }
}

const int16_t sine_table[256] = {
     0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
  6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
 12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
 18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
 23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
 27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
 30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
 32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
 32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
 32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
 30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
 27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
 23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
 18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
 12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
  6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
     0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
 -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
-12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
-18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
-23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
-27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
-30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
-32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
-32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
-32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
-30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
-27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
-23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
-18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
-12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
 -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804,
};

#define WAVEFORM_SIZE 128
#define WAVEFORM_X 50
#define WAVEFORM_Y 120
#define WAVEFORM_SCALE 8

int16_t waveform[WAVEFORM_SIZE] = {0};
LINE_F2 waveform_lines[WAVEFORM_SIZE - 1];
int waveformpointer = 0;

static uint32_t phase = 0;

int16_t sine_sample(int freq)
{
    int index = phase >> 24;
    phase += ((uint64_t)freq << 32) / 44100;
    return sine_table[index];
}

#define BLOCKS_PER_CHUNK 8
#define CHUNK_SIZE (BLOCKS_PER_CHUNK * 16) // 128 bytes

#define BUFFER0_ADDR 0x1010
#define BUFFER1_ADDR (BUFFER0_ADDR + CHUNK_SIZE)

static int active_buffer = 0;

#define SECTOR_SIZE 2048

uint8_t audio_buffer[2][SECTOR_SIZE] __attribute__((aligned(4)));
volatile uint16_t buffer_pos = 0x40; // adpcm starts at 0x40 for the file
uint8_t buffer_idx = 0; // when the spu is reading from buffer_idx, we write into the other buffer.

uint8_t need_cd_read = 0;

void spu_irq_handler() {
	// acknowledge the interrupt
	SPU_CTRL &= ~(1 << 6);

	active_buffer ^= 1;

	uint32_t playing_buffer = active_buffer ? BUFFER0_ADDR : BUFFER1_ADDR;
	uint32_t write_buffer   = active_buffer ? BUFFER1_ADDR : BUFFER0_ADDR;

	SPU_IRQ_ADDR        = getSPUAddr(write_buffer);
	SPU_CH_LOOP_ADDR(0) = getSPUAddr(write_buffer);
	SpuSetTransferStartAddr(write_buffer);

	static int sampleidx = 0;
	static uint8_t chunk[CHUNK_SIZE] __attribute__((aligned(4)));

	for (int b = 0; b < BLOCKS_PER_CHUNK; b++) {
		for (int i = 0; i < 16; i++) {
			chunk[(b * 16) + i] = audio_buffer[buffer_idx][buffer_pos++];
		}

		uint8_t *block = &chunk[b * 16];

		block[1] = 0x02;

		uint8_t shift = block[0] & 0x0F;

		for (int i = 0; i < 28; i++) {
			int8_t nibble;

			if (!(i & 1))
				nibble = block[2 + i / 2] & 0x0F;
			else
				nibble = block[2 + i / 2] >> 4;

			if (nibble & 8)
				nibble -= 16;

			int16_t sample = nibble << (12 - shift);

			waveform[waveformpointer] = sample;

			waveformpointer++;
			if (waveformpointer >= WAVEFORM_SIZE)
				waveformpointer = 0;
		}

		
		// int16_t samples[28];
		// for (int i = 0; i < 28; i++) {
		// 	samples[i] = audio_buffer[buffer_idx][buffer_pos++];

		// 	waveform[waveformpointer] = samples[i];

		// 	waveformpointer++;
		// 	if (waveformpointer >= WAVEFORM_SIZE)
		// 		waveformpointer = 0;
		// }

		// encode_block(samples, &chunk[b * 16]);
	}

	chunk[(BLOCKS_PER_CHUNK - 1) * 16 + 1] = 0x03;

	if (buffer_pos >= SECTOR_SIZE) {
		need_cd_read = 1;
        buffer_idx ^= 1;
        buffer_pos = 0;
    }

    SpuSetTransferStartAddr(write_buffer);
    SpuWrite((uint32_t *)chunk, CHUNK_SIZE);
}

void spu_dma_handler() {
	// re-enable the spu irq once the new chunk has been written
	SPU_CTRL |= 1 << 6;
}

uint8_t cd_read_pending = 0;
uint32_t next_sector = 0;

void update_buffers() {
    if (need_cd_read && !cd_read_pending) {
        need_cd_read    = 0;
        cd_read_pending = 1;
        CdlLOC loc;
        CdIntToPos(next_sector, &loc);
        CdControl(CdlSetloc, &loc, 0);
        CdRead(1, (uint32_t *)audio_buffer[buffer_idx ^ 1], CdlModeSpeed);
    }
}

void read_callback(enum _CdlIntrResult status, uint8_t* result) {
	(void)result; // unused, doc's say its something to do with a internal library

	FntPrint(-1, "CD CALLBACK: %d\n", status);
	cd_read_pending = 0;
	next_sector++;
	if (status == CdlComplete) {
	}
}

int main(int argc, const char **argv) {
	// Initialize the GPU and load the default font texture provided by
	// PSn00bSDK at (960, 0) in VRAM.
	ResetGraph(0);
	FntLoad(960, 0);

	CdInit();
	SpuInit();

	EnterCriticalSection();
	{
		InterruptCallback(IRQ_SPU, &spu_irq_handler);
		DMACallback(DMA_SPU, &spu_dma_handler);
	}
	ExitCriticalSection();

	CdReadCallback(read_callback);

	CdlFILE file;

	CdSearchFile(&file, "\\AUDIO.VAG;1");
	next_sector = CdPosToInt(&file.pos);

	// seek to it
	CdControl(CdlSetloc, &file.pos, 0);

	CdRead(
		1,
		(uint32_t *)audio_buffer[buffer_idx ^ 1],
		CdlModeSpeed
	);
	next_sector++;

	CdReadSync(0, 0);
	buffer_idx ^= 1;
	buffer_pos = 0x30; // skip 48-byte VAG header

	SPU_CTRL &= ~(1 << 6);

	spu_irq_handler();
	SpuIsTransferCompleted(1); // block untill dma done

	// set up channel
	SPU_CH_ADDR(0) 		= getSPUAddr(0x1010);
	SPU_CH_LOOP_ADDR(0) = getSPUAddr(0x1010);
	SPU_CH_FREQ(0)		= 0x0800; // 44100hz
	SPU_CH_ADSR1(0)     = 0x00ff;
	SPU_CH_ADSR2(0)		= 0x0000;
	SPU_CH_VOL_L(0)		= 0x3fff;
	SPU_CH_VOL_R(0)		= 0x3fff;

	SPU_KEY_ON1 = 1;
	spu_irq_handler();

	// Set up our rendering context.
	RenderContext ctx;
	setup_context(&ctx, SCREEN_XRES, SCREEN_YRES, 0, 0, 0);

	int x  = 0, y  = 0;
	int dx = 1, dy = 1;


	FntOpen(0, 16, 320, 240, 0, 512);

	for (;;) {
		update_buffers();
		ClearOTagR(ctx.buffers[ctx.active_buffer].ot, OT_LENGTH);
		for (int i = 0; i < WAVEFORM_SIZE - 1; i++) {
			int index0 = (waveformpointer + i) & (WAVEFORM_SIZE - 1);
			int index1 = (waveformpointer + i + 1) & (WAVEFORM_SIZE - 1);

			int x0 = WAVEFORM_X + i;
			int y0 = WAVEFORM_Y - (waveform[index0] >> WAVEFORM_SCALE);

			int x1 = WAVEFORM_X + i + 1;
			int y1 = WAVEFORM_Y - (waveform[index1] >> WAVEFORM_SCALE);

			LINE_F2 *line = &waveform_lines[i];

			setLineF2(line);
			setXY2(line, x0, y0, x1, y1);
			setRGB0(line, 255, 255, 255);

			AddPrim(ctx.buffers[ctx.active_buffer].ot, line);
		}

		DrawOTag(ctx.buffers[ctx.active_buffer].ot);

		FntPrint(-1, "CUR TRACK: %s\n", file.name);
		FntPrint(-1, "TRACK POS: %08x\n", file.pos);
		FntPrint(-1, "TRACK SIZE: %u\n", file.size/2048);

		FntPrint(-1, "BUF POS: %04x\n", buffer_pos);
		FntPrint(-1, "BUF IDX: %u\n", buffer_idx);
		FntPrint(-1, "SECTOR: %u\n", next_sector);
		FntPrint(-1, "READING?: %u\n", cd_read_pending);
		FntFlush(-1);

		flip_buffers(&ctx);
	}

	return 0;
}