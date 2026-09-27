/* Regression: an NTSC field must not present stale rows from a larger
 * framebuffer. No ROMs, SDL window or CoCoSDC files required. GPL-3.0-or-later. */
#include <stdbool.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vo.h"
#include "vo_render.h"
#include "ui.h"

/* This renderer test has no UI/message consumers. */
int ui_messenger_preempt_group(int c, int t, messenger_notify_delegate n) {
	(void)c; (void)t; (void)n; return 0;
}
int ui_msg_adjust_value_range(struct ui_state_message *m, int c, int d,
                             int lo, int hi, unsigned flags) {
	(void)m; (void)d; (void)lo; (void)hi; (void)flags; return c;
}
extern inline void vo_vsync(struct vo_interface *, bool);
static int cases, failures, draws;
struct check_frame { struct vo_render *vr; int rendered; size_t size; uint8_t black[4], green[3]; };
static void check_draw(void *p) {
	struct check_frame *c = p;
	struct vo_render *vr = c->vr;
	uint8_t rgb[736*3];
	draws++;
	/* Check *inside draw*, before vsync resets pixel: clearing afterwards
	 * would still show one damaged frame. Check all rows and format alpha. */
	for (int y=0; y<vr->viewport.h; y++) {
		vr->line_to_rgb(vr,y,rgb);
		for (int x=0;x<vr->viewport.w;x++) {
			uint8_t black_rgb[3]={0};
			const uint8_t *expected=y<c->rendered ? c->green : black_rgb;
			if (memcmp(rgb+x*3,expected,3)) {
				fprintf(stderr,"pixel row=%d x=%d rgb=%d,%d,%d expected=%d,%d,%d\n",y,x,rgb[x*3],rgb[x*3+1],rgb[x*3+2],expected[0],expected[1],expected[2]);failures++; return;
			}
			if (y>=c->rendered && memcmp((uint8_t *)vr->buffer+
			    (y*vr->buffer_pitch+x)*c->size,c->black,c->size)) {
				fprintf(stderr,"alpha row=%d\n",y);failures++; return;
			}
		}
	}
}
static void run_case(int fmt, size_t size, int height, int field, bool composite) {
	struct vo_render *vr=vo_render_new(fmt);
	/* Padding lets us check that clearing respects pitch and viewport width. */
	const int width=720, pitch=736;
	uint8_t *buf=malloc(pitch*height*size);
	memset(buf,0xA5,pitch*height*size);
	vr->buffer_pitch=pitch;
	vo_render_set_buffer(vr,buf);
	vo_render_set_viewport(vr,width,height);
	vr->viewport.new_x=100; vr->viewport.new_y=0;
	vo_render_vsync(vr);
	vr->set_palette_entry(vr,VO_RENDER_PALETTE_RGB,0,0,0,0);
	vr->set_palette_entry(vr,VO_RENDER_PALETTE_RGB,1,0,255,0);
	vr->set_palette_entry(vr,VO_RENDER_PALETTE_CMP,0,0,0,0);
	vr->set_palette_entry(vr,VO_RENDER_PALETTE_CMP,1,0,255,0);
	void (*render)(void *,unsigned,unsigned,const uint8_t *) = composite ? vr->render_cmp_palette : vr->render_rgb_palette;
	uint8_t line[1024]={0};
	render(vr,1,1024,line);
	struct check_frame c={.vr=vr,.rendered=field<height?field:height,.size=size};
	memcpy(c.black,buf,size);
	/* Start with real non-black pixels in every row, like a previous longer
	 * field or a changed viewport, rather than relying on allocator contents. */
	vo_render_vsync(vr);
	memset(line,1,sizeof(line));
	for(int y=0;y<height;y++)render(vr,1,1024,line);
	uint8_t expected_rgb[720*3];vr->line_to_rgb(vr,0,expected_rgb);memcpy(c.green,expected_rgb,3);
	vo_render_vsync(vr);
	for(int y=0;y<field;y++)render(vr,1,1024,line);
	struct vo_interface vo={0}; vo.renderer=vr;
	vo.draw=DELEGATE_AS0(void,check_draw,&c);
	int before=draws;
	vo_vsync(&vo,true);
	assert(draws==before+1);
	for(int y=0;y<height;y++)for(int x=width;x<pitch;x++)
		for(size_t b=0;b<size;b++)assert(buf[(y*pitch+x)*size+b]==0xA5);
	/* Skipped frames must not present or erase the retained framebuffer. */
	uint8_t *copy=malloc(pitch*height*size);memcpy(copy,buf,pitch*height*size);
	vo_vsync(&vo,false);assert(draws==before+1);
	assert(!memcmp(copy,buf,pitch*height*size));
	free(copy);free(buf);vo_render_free(vr);cases++;
}
int main(void) {
	for(int fmt=VO_RENDER_FMT_RGBA8;fmt<=VO_RENDER_FMT_RGB565;fmt++)
		for(int h=0;h<3;h++)for(int f=0;f<2;f++)for(int c=0;c<2;c++) {
			int heights[]={240,270,276};
			run_case(fmt,fmt>=VO_RENDER_FMT_RGBA4?2:4,heights[h],f?314:263,c);
		}
	printf("vo_frame_tail: %d cases, %d failures\n",cases,failures);
	return failures?1:0;
}
