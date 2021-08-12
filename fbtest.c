/*	--*- c -*--
 * Copyright (C) 2015 Enrico Scholz <enrico.scholz@sigma-chemnitz.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include <getopt.h>

#ifdef __dietlibc__
#  define dprintf	fdprintf
#endif

#define CMD_HELP	0x1000
#define CMD_VERSION	0x1001
#define CMD_FB		0x1002
#define CMD_SOLID	0x1003
#define CMD_GRAB	0x1004
#define CMD_BARS	0x1005
#define CMD_TEST_XRES	0x1006
#define CMD_TEST_YRES	0x1007
#define CMD_SETPIX	0x1008
#define CMD_X		'x'
#define CMD_Y		'y'
#define CMD_CROSS	0x100b
#define CMD_DSHADE	0x100c

#define CROSS_SZ	50

struct option const
CMDLINE_OPTIONS[] = {
	{ "help",       no_argument,       0, CMD_HELP },
	{ "version",    no_argument,       0, CMD_VERSION },
	{ "fb",         required_argument, 0, CMD_FB },
	{ "solid",	required_argument, 0, CMD_SOLID },
	{ "grab",	required_argument, 0, CMD_GRAB },
#if 0
	{ "test-xres",	required_argument, 0, CMD_TEST_XRES },
	{ "test-yres",	required_argument, 0, CMD_TEST_YRES },
#endif
	{ "setpix",	required_argument, 0, CMD_SETPIX },
	{ "x",		required_argument, 0, CMD_X },
	{ "y",		required_argument, 0, CMD_Y },
	{ "bars",	no_argument,       0, CMD_BARS },
	{ "cross",	no_argument,       0, CMD_CROSS },
	{ "dshade",	no_argument,       0, CMD_DSHADE },
	{ 0,0,0,0 }
};

struct rgb_pix {
	uint8_t		r;
	uint8_t		g;
	uint8_t		b;
	uint8_t		_a;
};

__attribute__((__noreturn__))
static void show_help()
{
	printf("Usage: fbtest [--fb <dev>] [--solid <color>] [--grab <fname>]\n"
	       "       [--bars] [--cross] [--dshade]\n"
	       "       [-x <x> -y <y> -setpix <col>]*\n");
	exit(0);
}

__attribute__((__noreturn__))
static void show_version()
{
	printf("fbtest 0.1 -- framebuffer test utility\n");
	exit(1);
}

static uint32_t ror32_8(uint32_t v)
{
	return ((v & 0xffu) << 24) | (v >> 8);
}

static uint32_t ror32_1(uint32_t v)
{
	return ((v & 0x1u) << 30) | (v >> 1);
}

static void initPalette(int fd, char const *pin_str, struct fb_var_screeninfo const *info)
{
	int const	pos[] = { 0,
				  info->red.length+1,
				  info->red.length + info->green.length+2,
				  info->red.length + info->green.length + info->blue.length+3 };

	int const	min_len = MIN(MIN(info->red.length, info->green.length),
				      info->blue.length)+1;

	uint16_t	red[pos[3]];
	uint16_t	green[pos[3]];
	uint16_t	blue[pos[3]];
	int		i;
	uint16_t	pin_val = pin_str ? atoi(pin_str) : 0;

	struct fb_cmap	cmap = {
		.start = 100,
		.len   = pos[3],
		.red   = red,
		.green = green,
		.blue  = blue,
	};

	memset(red,   0, sizeof red);
	memset(green, 0, sizeof green);
	memset(blue,  0, sizeof blue);

	for (i=0; i<pos[3]; ++i) {
		if      (i>=pos[2]) blue[i]  = (1 << ((i-pos[2]) + (15-info->blue.length)));
		else if (i>=pos[1]) green[i] = (1 << ((i-pos[1]) + (15-info->green.length)));
		else                red[i]   = (1 << ((i-pos[0]) + (15-info->red.length)));
	}

	red  [pos[1]] = 0xffff;
	green[pos[2]] = 0xffff;
	blue [pos[3]] = 0xffff;

	ioctl(fd, FBIOPUTCMAP, &cmap);


	for (i=0; i+1<min_len; ++i)
		red[i] = green[i] = blue[i] = (1 << (i+(16-min_len)));

	red[min_len-1] = green[min_len-1] = blue[min_len-1] = 0xffff;

	cmap.start = 200;
	cmap.len   = min_len;

	ioctl(fd, FBIOPUTCMAP, &cmap);


	red  [0] = green[0] = blue[0] = 0xffff;
	red  [1] = green[1] = blue[1] = 0x0000;
	red  [2] = pin_str ? (pin_val & 0xfc0000) >> 8 : 0;
	green[2] = pin_str ? (pin_val & 0x00fc00)      : 0;
	blue [2] = pin_str ? (pin_val & 0x0000fc) << 8 : 0;

	cmap.start = 210;
	cmap.len   = 3;

	ioctl(fd, FBIOPUTCMAP, &cmap);
}

static inline void *
setPixelPalette(void *buf_v, uint32_t val, int bpp)
{
	switch (bpp) {
	case 8	: {
		uint8_t	*buf = buf_v;
		*buf++ = val;
		return buf;
	}

	default	:
		fprintf(stderr, "Palette mode not supported with %ibpp\n", bpp);
		exit(1);
	}
}

static inline void *
setPixelRGBRaw(void *buf_v, struct fb_var_screeninfo const *info, uint32_t val)
{
#define SET(TYPE)				\
	case sizeof(TYPE)*8 : {			\
		TYPE	*buf = buf_v;		\
		*buf = val;			\
		return buf+1;			\
	}

	switch (info->bits_per_pixel) {
		SET(uint8_t);
		SET(uint16_t);
		SET(uint32_t);
	case 24		: {
		uint8_t	*buf = buf_v;
		buf[0] = val       & 0xff;
		buf[1] = (val>>8)  & 0xff;
		buf[2] = (val>>16) & 0xff;
		return buf+3;
	}
	default		:
		assert(0);
	}
#undef SET
}

static ptrdiff_t get_pix_ofs(unsigned int x, unsigned int y,
			     struct fb_var_screeninfo const *info)
{
	return ((y * info->xres_virtual) + x) * info->bits_per_pixel / 8;
}

static inline void *
setPixelRGB(void *buf_v, struct fb_var_screeninfo const *info, uint8_t r, uint8_t g, uint8_t b)
{
#define S(VAR,FIELD)	(((VAR)==0 ? 0 :				\
			  (VAR)<=info->FIELD.length ? (1<<((VAR)-1)) : ((1<<(info->FIELD.length)) - 1)) << (info->FIELD.offset))

	uint32_t	val = S(r, red) | S(g, green) | S(b, blue);
#undef S

	return setPixelRGBRaw(buf_v, info, val);
}

static inline void *
setPixelRGBCol(void *buf_v, struct fb_var_screeninfo const *info, uint8_t r, uint8_t g, uint8_t b)
{
#define S(VAR,FIELD)							\
	((VAR)==0 ? 0 :							\
	 (((VAR)>=(1u << info->FIELD.length) ? ((1u << info->FIELD.length) - 1u) : (VAR)) << (info->FIELD.offset)))

	uint32_t	val = S(r, red) | S(g, green) | S(b, blue);
#undef S

	return setPixelRGBRaw(buf_v, info, val);
}


static inline uint32_t
getPixelRGB(void const *buf_v_start, struct fb_var_screeninfo const *info, int x, int y)
{
	int const		line_size = info->xres_virtual * info->bits_per_pixel / 8;
	void const *		buf_v     = (char const *)(buf_v_start) + line_size*y + x*info->bits_per_pixel/8;

#define GET(TYPE)				\
	case sizeof(TYPE)*8 : {			\
		TYPE const	*buf = buf_v;	\
		return *buf;			\
	}

	switch (info->bits_per_pixel) {
		GET(uint8_t);
		GET(uint16_t);
		GET(uint32_t);
	case 24		: {
		uint8_t const	*buf = buf_v;
		return (buf[0] | (buf[1]<<8) | (buf[2]<<16));
	}
	default		:
		assert(0);
	}
}

static void
displayPalette(struct fb_var_screeninfo const *info, void *buf_v)
{
	int		x=0,y;
	uint8_t	*ptr = buf_v;
	int const	bpp  = info->bits_per_pixel;
	int const	xres = info->xres;
	int const	yres = info->yres;

	int const	l_len = info->red.length + info->green.length + info->blue.length+3;
	int const	r_len = MIN(MIN(info->red.length, info->green.length),
				    info->blue.length)+1;
	int		i;

	for (y=0; y<yres; ++y) {
		uint8_t	lcol = 100 + ((y*l_len)/yres);
		uint8_t	rcol = 200 + ((y*r_len)/yres);

		for (x=0; x<xres; ++x)
			ptr = setPixelPalette(ptr, x<xres/2 ? lcol : rcol, bpp);

		ptr += (info->xres_virtual - xres) * (bpp/8);
	}

#define P(X,Y,COL)							\
	setPixelPalette((char *)(buf_v) + (((Y)*xres) + (X)) * bpp/8, (COL), bpp)

	for (i=0; i<5; ++i) {
		uint8_t	col = (i%2) ? 211 : 210;

		P(i, 0, col);
		P(0, i, col);

		P(xres-i-1, 0, col);
		P(xres-1,   i, col);

		P(i, yres-1,   col);
		P(0, yres-i-1, col);

		P(xres-i-1, yres-1, col);
		P(xres-1,   yres-i-1, col);
	}

#if 0
	setPixelPalette((char *)(buf_v) +      1 * bpp/8, 210, bpp);
	setPixelPalette((char *)(buf_v) +      2 * bpp/8, 210, bpp);
	setPixelPalette((char *)(buf_v) + xres   * bpp/8, 210, bpp);
	setPixelPalette((char *)(buf_v) + 2*xres * bpp/8, 210, bpp);

	setPixelPalette((char *)(buf_v) + (xres*yres-1)     * bpp/8, 211, bpp);
	setPixelPalette((char *)(buf_v) + (xres*yres-2)     * bpp/8, 211, bpp);
	setPixelPalette((char *)(buf_v) + (xres*yres-3)     * bpp/8, 211, bpp);
	setPixelPalette((char *)(buf_v) + (xres*(yres-1)-1) * bpp/8, 211, bpp);
	setPixelPalette((char *)(buf_v) + (xres*(yres-2)-1) * bpp/8, 211, bpp);
#endif
}

static void *
draw_cross_rgb(void *ptr, struct fb_var_screeninfo const *info, int x, int y)
{
	int		is_black = 1;
	uint8_t		col;

	if (x == y || x == -y ||
	    x == -CROSS_SZ+5 || x+1 == CROSS_SZ-5 ||
	    y == -CROSS_SZ+5 || y+1 == CROSS_SZ-5)
		is_black = !is_black;

	if (y < 0)
		is_black = !is_black;

	col = is_black ? 0 : 255;
	return setPixelRGB(ptr, info,   col,  col, col);
}

static void
displayRGB(struct fb_var_screeninfo const *info, void *buf_v)
{
	int		x=0,y;
	uint8_t	*ptr = buf_v;

	int const	xres = info->xres;
	int const	yres = info->yres;
	int const	pos[] = { 0,
				  info->red.length+1,
				  info->red.length + info->green.length+2,
				  info->red.length + info->green.length + info->blue.length+3 };
	int const	min_len = MIN(MIN(info->red.length, info->green.length),
				      info->blue.length)+1;
	int		old_pos = -1;

	for (y=0; y<yres; ++y) {
		int		cur_pos	= (y*pos[3])/yres;

		uint8_t	r = (pos[0]<=cur_pos && cur_pos<pos[1]) ? cur_pos-pos[0]+1 : 0;
		uint8_t	g = (pos[1]<=cur_pos && cur_pos<pos[2]) ? cur_pos-pos[1]+1 : 0;
		uint8_t	b = (pos[2]<=cur_pos && cur_pos<pos[3]) ? cur_pos-pos[2]+1 : 0;
		uint8_t	grey = (min_len+1) * y/yres + 1;
		int		max_x = xres;

		if (y==0 || y+1==yres) {
			ptr = setPixelRGB(ptr, info, 255, 255, 255);
			ptr = setPixelRGB(ptr, info,   0,   0,   0);
			ptr = setPixelRGB(ptr, info, 255, 255, 255);
			ptr = setPixelRGB(ptr, info,   0,   0,   0);
			ptr = setPixelRGB(ptr, info, 255, 255, 255);
			x      = 5;
			max_x -= x;
		}
		else if (y<5 || y+5>=yres) {
			int	col = ((y<yres/2 && y%2) || (y>yres/2 && (yres-y-1)%2)) ? 0 : 255;
			ptr    = setPixelRGB(ptr, info,  col, col, col);
			x      = 1;
			max_x -= x;
		}
		else if (cur_pos!=old_pos &&
			 (cur_pos==pos[0] || cur_pos==pos[1] || cur_pos==pos[2] || cur_pos==pos[3])) {
			ptr = setPixelRGB(ptr, info, 127, 127, 127);
			ptr = setPixelRGB(ptr, info, 127, 127, 127);
			x      = 2;
		}
		else
			x      = 0;

		for (; x<max_x; ++x) {
			if (y >= yres/2 - CROSS_SZ && y < yres/2 + CROSS_SZ &&
			    x >= xres/2 - CROSS_SZ && x < xres/2 + CROSS_SZ)
				ptr = draw_cross_rgb(ptr, info, xres/2 - x, yres/2 - y);
			else if (y < 4 && x == 4-y)
				ptr = setPixelRGB(ptr, info, 255, 255, 255);
			else if (x<xres/2) ptr = setPixelRGB(ptr, info, r, g, b);
			else if (grey>min_len) ptr = setPixelRGB(ptr, info, 255, 255, 255);
			else          ptr = setPixelRGB(ptr, info,
							grey + info->red.length   - min_len,
							grey + info->green.length - min_len,
							grey + info->blue.length  - min_len);
		}

		if (y==0 || y+1==yres) {
			ptr = setPixelRGB(ptr, info, 255, 255, 255);
			ptr = setPixelRGB(ptr, info,   0,   0,   0);
			ptr = setPixelRGB(ptr, info, 255, 255, 255);
			ptr = setPixelRGB(ptr, info,   0,   0,   0);
			ptr = setPixelRGB(ptr, info, 255, 255, 255);
		}
		else if (y<5 || y+5>=yres) {
			int	col = ((y<yres/2 && y%2) || (y>yres/2 && (yres-y-1)%2)) ? 0 : 255;
			ptr    = setPixelRGB(ptr, info,  col, col, col);
		}

		ptr += ((info->xres_virtual - info->xres) *
			(info->bits_per_pixel / 8));

		if (old_pos!=cur_pos) {
			printf("displayRGB -> ptr=%p, r=%u, g=%u, b=%u, grey=%u, pos=[%u,%u,%u,%u]/%u, *addr=[%08x,%08x]\n",
			       ptr, r,  g,  b, grey,
			       pos[0], pos[1], pos[2], pos[3], cur_pos,
			       getPixelRGB(buf_v, info, 10, y),
			       getPixelRGB(buf_v, info, xres/2 + 10, y));
			old_pos = cur_pos;
		}
	}
}

static void write_all(int fd, void const *buf, size_t len)
{
	char const	*ptr  = buf;
	while (len>0) {
		ssize_t	l = write(fd, ptr, len);
		if (l==0)
			abort();
		else if (l>0) {
			ptr += l;
			len -= l;
		} else {
			perror("write()");
			abort();
		}
	}
}

struct fbinfo {
	struct fb_var_screeninfo	var;
	int				fd;
	void				*buf;
	size_t				buf_size;
	size_t				stride;
};

struct window {
	unsigned int			left;
	unsigned int			right;
	unsigned int			top;
	unsigned int			bottom;
};

static int fb_init(char const *fbdev, struct fbinfo *info)
{
	memset(info, 0, sizeof *info);

	info->fd = open(fbdev, O_RDWR);
	if (info->fd<0) {
		perror("open(<fbdev>)");
		return -1;
	}

	if (ioctl(info->fd, FBIOGET_VSCREENINFO, &info->var)<0) {
		perror("ioctl(FBIOGET_VSCREENINFO)");
		goto err;
		return -1;
	}

	{
		int const	line_size   = (info->var.xres_virtual *
					       info->var.bits_per_pixel) / 8;

		info->stride   = line_size;
		info->buf_size = line_size * info->var.yres_virtual;
		info->buf      = mmap(0, info->buf_size,
				      PROT_READ | PROT_WRITE, MAP_SHARED,
				      info->fd, 0);

		if (!info->buf) {
			perror("mmap()");
			goto err;
		}
	}

	return 0;

err:
	if (info->buf)
		munmap(info->buf, info->buf_size);
	close(info->fd);
	return -1;
}

static void fb_free(struct fbinfo *info)
{
	munmap(info->buf, info->buf_size);
	close(info->fd);
}

static unsigned char normalize_rgb(uint32_t v, struct fb_bitfield const *col)
{
	if (col->msb_right)
		abort();		/* not implemented */

	if (col->length>8)
		abort();

	v >>= col->offset;
	v  &= ((1<<col->length)-1);
	v <<= 8-col->length;

	return v;
}

static int grab_fb(char const *fbdev, char const *fname)
{
	struct fbinfo		fb;
	int			out_fd;
	int			rc = -1;
	unsigned int		x, y;
	uint8_t			*res_buf;
	uint8_t			*res_ptr;

	if (strcmp(fname, "-")==0)
		out_fd = dup(1);
	else
		out_fd = open(fname, O_CREAT|O_WRONLY, 0022);

	if (out_fd<0) {
		fprintf(stderr, "Can not open output file: %m\n");
		return -1;
	}

	if (fb_init(fbdev, &fb)<0)
		goto err;

	fprintf(stderr, "Grabbing from a fb-display with %ux%u (%ibpp) [virtual %ux%u]\n",
		fb.var.xres, fb.var.yres, fb.var.bits_per_pixel,
		fb.var.xres_virtual, fb.var.yres_virtual);


	switch (fb.var.bits_per_pixel) {
	case 8	:
		fprintf(stderr, "grabbing from palette not implemented yet\n");
		break;

	default:
		res_buf = malloc(fb.var.yres * fb.var.xres * 3);
		res_ptr = res_buf;

		dprintf(out_fd, "P6\n%u %u\n255\n", fb.var.xres, fb.var.yres);

		for (y=0; y<fb.var.yres; ++y) {
			for (x=0; x<fb.var.xres; ++x) {
				uint32_t	v = getPixelRGB(fb.buf, &fb.var,
								x, y);

				*res_ptr++ = normalize_rgb(v, &fb.var.red);
				*res_ptr++ = normalize_rgb(v, &fb.var.green);
				*res_ptr++ = normalize_rgb(v, &fb.var.blue);
			}
		}

		write_all(out_fd, res_buf, res_ptr - res_buf);
		free(res_buf);
		break;
	}

	rc = 0;

	fb_free(&fb);
err:
	close(out_fd);
	return rc;
}

static unsigned int init_color(struct fbinfo *fb, char const *opt)
{
	unsigned int	res;

	switch (fb->var.bits_per_pixel) {
	case 8:
		initPalette(fb->fd, opt, &fb->var);

		if (opt[0]=='#')
			res = atoi(opt+1);
		else if (opt[0]=='g')
			res = atoi(opt+1)+200;
		else if (opt[0]=='p')
			res = 211;
		else
			res = atoi(opt)+100;

		break;

	case 16	:
	case 24:
	case 32	:
		res = strtoul(opt, NULL, 0);
		break;

	default:
		abort();
	}

	return res;
}

static void dshade_next_color(struct rgb_pix *col,
			      struct fb_var_screeninfo const *info)
{
	uint8_t		*this_c;
	uint8_t		*next_c;
	uint8_t		max;

	if (col->r != 0 || (col->g == 0 && col->b == 0)) {
		this_c = &col->r;
		next_c = &col->g;
		max    = (1u << info->red.length) - 1;
	} else if (col->g != 0) {
		this_c = &col->g;
		next_c = &col->b;
		max    = (1u << info->green.length) - 1;
	} else if (col->b != 0) {
		this_c = &col->b;
		next_c = &col->r;
		max    = (1u << info->blue.length) - 1;
	} else {
		abort();
	}

	if (*this_c == max) {
		*this_c = 0;
		*next_c = 1;
	} else {
		*this_c += 1;
	}
}

static int dshade(char const *fbdev)
{
	struct fbinfo		fb;

	if (fb_init(fbdev, &fb)<0)
		return -1;

	fprintf(stderr, "Assuming a fb-display with %ux%u (%ibpp) [virtal %ux%u]\n",
		fb.var.xres, fb.var.yres, fb.var.bits_per_pixel,
		fb.var.xres_virtual, fb.var.yres_virtual);

	switch (fb.var.bits_per_pixel) {
	case 8	: {
		/* TODO: implement me! */
		abort();
		break;
	}

	case 16:
	case 24:
	case 32: {
		struct rgb_pix	col = { 0,0,0,0 };

		for (unsigned int x = 0; x < fb.var.xres; ++x) {
			void		*ptr;
			struct rgb_pix	cur_col = col;

			ptr = fb.buf + get_pix_ofs(x, 0, &fb.var);

			for (unsigned int y = 0; y < fb.var.yres; ++y) {
				setPixelRGBCol(ptr, &fb.var, col.r, col.g, col.b);
				dshade_next_color(&col, &fb.var);

				ptr += fb.stride;
			}

			col = cur_col;
			dshade_next_color(&col, &fb.var);
		}
		break;
	}


	default:
		abort();
	}

	return 0;
}

static int cross(char const *fbdev)
{
	struct fbinfo		fb;

	if (fb_init(fbdev, &fb)<0)
		return -1;

	fprintf(stderr, "Assuming a fb-display with %ux%u (%ibpp) [virtal %ux%u]\n",
		fb.var.xres, fb.var.yres, fb.var.bits_per_pixel,
		fb.var.xres_virtual, fb.var.yres_virtual);

	switch (fb.var.bits_per_pixel) {
	case 8	: {
		/* TODO: implement me! */
		abort();
		break;
	}

	case 16:
	case 24:
	case 32: {
		unsigned int	y = 0;
		int		dir = 1;
		uint32_t	col0 = 0xff00ffff;
		uint32_t	col1 = 0xfff00fff;

		for (unsigned int x = 0; x < fb.var.xres; ++x) {
			void		*ptr;

			ptr = fb.buf + get_pix_ofs(x, y, &fb.var);
			setPixelRGBRaw(ptr, &fb.var,
				       x % 2 ? col0 : ror32_1(col0));

			ptr = fb.buf + get_pix_ofs(x, fb.var.yres - y - 1, &fb.var);
			setPixelRGBRaw(ptr, &fb.var,
				       x % 2 ? col1 : ror32_1(col1));

			if (dir > 0 && y + dir >= fb.var.yres) {
				dir = -1;
				col1 = ror32_8(col1);
				printf("x=%u, y=%u -> col1=%08x\n", x, y, col1);
			} else if (dir < 0 && y < (unsigned int)(-dir)) {
				dir = +1;
				col0 = ror32_8(col0);
				printf("x=%u, y=%u -> col0=%08x\n", x, y, col0);
			}

			y += dir;
		}
		break;
	}

	default:
		abort();
	}

	return 0;
}

static int solid_fb(char const *fbdev, char const *opt)
{
	struct fbinfo		fb;
	unsigned int		col;

	if (fb_init(fbdev, &fb)<0)
		return -1;

	col = init_color(&fb, opt);

	switch (fb.var.bits_per_pixel) {
	case 8	: {
		fprintf(stderr, "Filling fb-display with %ux%u (%ibpp) with solid color of %d[%s]\n",
			fb.var.xres, fb.var.yres, fb.var.bits_per_pixel, col, opt);

		memset(fb.buf, col, fb.var.xres_virtual*fb.var.yres_virtual);
		break;
	}

	case 16	:
	case 24:
	case 32	: {
		size_t		i   = fb.var.xres_virtual*fb.var.yres_virtual;
		void		*buf = fb.buf;

		fprintf(stderr, "Filling fb-display with %ux%u (%ibpp) with solid color of %08x\n",
			fb.var.xres, fb.var.yres, fb.var.bits_per_pixel, col);

		while (i-->0)
			buf = setPixelRGBRaw(buf, &fb.var, col);
		break;
	}
	}

	fb_free(&fb);
	return 0;
}

static int bars_fb(char const *fbdev)
{
	struct fbinfo		fb;

	if (fb_init(fbdev, &fb)<0)
		return -1;

	fprintf(stderr, "Assuming a fb-display with %ux%u (%ibpp) [virtal %ux%u]\n",
		fb.var.xres, fb.var.yres, fb.var.bits_per_pixel,
		fb.var.xres_virtual, fb.var.yres_virtual);

	switch (fb.var.bits_per_pixel) {
	case 8	:
		initPalette(fb.fd, NULL, &fb.var);
		displayPalette(&fb.var, fb.buf);
		break;
	default	:
		displayRGB(&fb.var, fb.buf);
		break;
	}

	fb_free(&fb);
	return 0;
}

static int set_pix(char const *fbdev, unsigned int x, unsigned int y,
		    char const *opt)
{
	struct fbinfo	fb;
	unsigned int	col;
	void		*ptr;

	if (fb_init(fbdev, &fb)<0)
		return -1;

	col = init_color(&fb, opt);

	ptr = fb.buf + get_pix_ofs(x, y, &fb.var);

	switch (fb.var.bits_per_pixel) {
	case 8:
		setPixelPalette(ptr, col, 8);
		break;

	default:
		setPixelRGBRaw(ptr, &fb.var, col);
		break;
	}

	return 0;
}

int main (int argc, char *argv[])
{
	struct {
		char const	*fb;
		unsigned int	x;
		unsigned int	y;
	}	options = {
		.fb = "/dev/fb0",
	};
	int			done = 0;

	while (1) {
		int		c = getopt_long(argc, argv, "",
						CMDLINE_OPTIONS, 0);
		if (c==-1)
			break;

		switch (c) {
		case CMD_HELP:		show_help();
		case CMD_VERSION:	show_version();
		case CMD_FB:		options.fb = optarg; break;
		case CMD_GRAB:
			done = 1;
			grab_fb(options.fb, optarg);
			break;
		case CMD_SOLID:
			done = 1;
			solid_fb(options.fb, optarg);
			break;
		case CMD_BARS:
			done = 1;
			bars_fb(options.fb);
			break;
		case CMD_CROSS:
			done = 1;
			cross(options.fb);
			break;
		case CMD_DSHADE:
			done = 1;
			dshade(options.fb);
			break;
#if 0
		case CMD_TEST_XRES:
			done = 1;
			test_xres(options.fb, optarg);
			break;
		case CMD_TEST_YRES:
			done = 1;
			test_yres(options.fb, optarg);
			break;
#endif
		case CMD_X:
			options.x = atoi(optarg);
			break;
		case CMD_Y:
			options.y = atoi(optarg);
			break;
		case CMD_SETPIX:
			done = 1;
			set_pix(options.fb, options.x, options.y, optarg);
			break;
		default:
			fprintf(stderr, "invalid option; try '--help' for more information\n");
			return EXIT_FAILURE;
		}
	}

	if (!done)
		bars_fb(options.fb);
}
