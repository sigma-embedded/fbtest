#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include <getopt.h>

struct option const
CMDLINE_OPTIONS[] = {
	{ "help",       no_argument,       0, CMD_HELP },
	{ "version",    no_argument,       0, CMD_VERSION },
	{ "fb",         required_argument, 0, CMD_FB },
	{ "solid",	required_argument, 0, CMD_SOLID },
	{ "grab",	required_argument, 0, CMD_GRAB },
	{ "bars",	no_argument,       0, CMD_BARS },
	{ 0,0,0,0 }
};

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
#define SET(TYPE)		\
  case sizeof(TYPE)*8 : {	\
    TYPE	*buf = buf_v;	\
    *buf = val;			\
    return buf+1;		\
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


static inline void *
setPixelRGB(void *buf_v, struct fb_var_screeninfo const *info, uint8_t r, uint8_t g, uint8_t b)
{
#define S(VAR,FIELD)	(((VAR)==0 ? 0 :				\
			  (VAR)<=info->FIELD.length ? (1<<((VAR)-1)) : ((1<<(info->FIELD.length+1)) - 1)) << (info->FIELD.offset))

  uint32_t	val = S(r, red) | S(g, green) | S(b, blue);
#undef S

  return setPixelRGBRaw(buf_v, info, val);
}


static inline uint32_t
getPixelRGB(void const *buf_v_start, struct fb_var_screeninfo const *info, int x, int y)
{
  int const		line_size = info->xres * info->bits_per_pixel / 8;
  void const *		buf_v     = (char const *)(buf_v_start) + line_size*y + x*info->bits_per_pixel/8;

#define GET(TYPE)		\
  case sizeof(TYPE)*8 : {	\
    TYPE const	*buf = buf_v;	\
    return *buf;		\
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
  }

#define P(X,Y,COL)	\
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
      if (x<xres/2) ptr = setPixelRGB(ptr, info, r, g, b);
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

int main (int argc, char *argv[])
{
	struct {
		char const	*fb;
	}	options = {
		.fb = "/dev/fb0";
	};
	
  int				fd;
  uint8_t			*buf;
  struct fb_var_screeninfo	var_info;

  while (1) {
    int		c = getopt_long(argc, argv, "",
				CMDLINE_OPTIONS, 0);
    if (c==-1) break;

    switch (c) {
    case CMD_HELP	:  show_help();
    case CMD_VERSION	:  show_version();
    case CMD_FB		:  
    default:
	    fprintf(stderr, "invalid option; try '--help' for more information\n");
	    return EXIT_FAILURE;
    }
  }


  fd  = open ("/dev/fb0", O_RDWR);

  ioctl(fd, FBIOGET_VSCREENINFO, &var_info);

  int const		line_size = var_info.xres * var_info.bits_per_pixel / 8;
  int const		buffer_size = line_size * var_info.yres;

  printf("Assuming fb-display with %ux%u (%ibpp)\n",
	 var_info.xres, var_info.yres, var_info.bits_per_pixel);

  buf = mmap (0, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  switch (var_info.bits_per_pixel) {
    case 8	:
      initPalette(fd, argc>1 ? argv[1] : 0, &var_info);
      break;
  }

  if (argc==1) {
    switch (var_info.bits_per_pixel) {
      case 8	:
	displayPalette(&var_info, buf);
	break;
      default	:
	displayRGB(&var_info, buf);
	break;
    }
  }
  else {
    switch (var_info.bits_per_pixel) {
      case 8	: {
	uint8_t	val;

	if      (argv[1][0]=='#') val = atoi(argv[1]+1);
	else if (argv[1][0]=='g') val = atoi(argv[1]+1)+200;
	else if (argv[1][0]=='p') val = 211;
	else                      val = atoi(argv[1])+100;

	memset(buf, val, var_info.xres*var_info.yres);
	break;
      }

      case 16	:
      case 32	: {
	size_t		i   = var_info.xres*var_info.yres;
	uint32_t	val = strtol(argv[1], 0, 0);

	while (i-->0)
	  buf = setPixelRGBRaw(buf, &var_info, val);
	break;
      }
    }
  }
}

  // Local Variables:
  // compile-command: "make fbtest CC='arm-xscale-linux-gnu-diet -Os arm-xscale-linux-gnu-gcc' CFLAGS='-Wall -W -std=c99'"
  // End:
