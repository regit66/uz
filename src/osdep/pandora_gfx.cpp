#include "sysconfig.h"
#include "sysdeps.h"
#include "config.h"
#include "uae.h"
#include "options.h"
#include "gui.h"
#include "memory-uae.h"
#include "newcpu.h"
#include "custom.h"
#include "xwin.h"
#include "drawing.h"
#include "inputdevice.h"
#include "savestate.h"
#include "picasso96.h"

#include <png.h>
#include <SDL.h>
#include <SDL/SDL_image.h>
#ifndef ANDROID
#include <SDL/SDL_gfxPrimitives.h>
#endif
#include <SDL/SDL_ttf.h>

#ifdef ANDROIDSDL
#include <SDL_screenkeyboard.h>
#endif

#include <linux/fb.h>
#include <sys/ioctl.h>

#ifndef OMAPFB_WAITFORVSYNC
#define OMAPFB_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif
#ifndef OMAPFB_WAITFORVSYNC_FRAME
#define OMAPFB_WAITFORVSYNC_FRAME _IOWR('O', 70, unsigned int)
#endif


/* SDL variable for output of emulation */
SDL_Surface *prSDLScreen = NULL;
static int fbdev = -1;
static unsigned int current_vsync_frame = 0;
unsigned long time_per_frame = 20000; // Default for PAL (50 Hz): 20000 microsecs
static unsigned long last_synctime;

/* Possible screen modes (x and y resolutions) */
#ifdef ANDROID
#define MAX_SCREEN_MODES 11
static int x_size_table[MAX_SCREEN_MODES] = { 640, 640, 720, 800, 800, 960, 1024, 1024, 1280, 1280, 1920 };
static int y_size_table[MAX_SCREEN_MODES] = { 400, 480, 400, 480, 600, 540,  768,  600,  720,  800, 1080 };
#else
#define MAX_SCREEN_MODES 6
static int x_size_table[MAX_SCREEN_MODES] = { 640, 640, 800, 1024, 1152, 1280 };
static int y_size_table[MAX_SCREEN_MODES] = { 400, 480, 480,  768,  864,  960 };
#endif

static int red_bits, green_bits, blue_bits;
static int red_shift, green_shift, blue_shift;

struct PicassoResolution *DisplayModes;
struct MultiDisplay Displays[MAX_DISPLAYS];

int screen_is_picasso = 0;

static SDL_Surface *current_screenshot = NULL;
static char screenshot_filename_default[MAX_DPATH]={
	'/', 't', 'm', 'p', '/', 'n', 'u', 'l', 'l', '.', 'p', 'n', 'g', '\0'
};
char *screenshot_filename=(char *)&screenshot_filename_default[0];
FILE *screenshot_file=NULL;
static void CreateScreenshot(void);
static int save_thumb(char *path);
int delay_savestate_frame = 0;

#define VIDEO_FLAGS_INIT SDL_SWSURFACE|SDL_FULLSCREEN
#ifdef ANDROIDSDL
#define VIDEO_FLAGS VIDEO_FLAGS_INIT
#else
#define VIDEO_FLAGS VIDEO_FLAGS_INIT | SDL_DOUBLEBUF
#endif


static unsigned long next_synctime = 0;

int graphics_setup (void)
{
#ifdef PICASSO96
  picasso_InitResolutions();
  InitPicasso96();
#endif
  return 1;
}


#ifdef WITH_LOGGING

SDL_Surface *liveInfo = NULL;
TTF_Font *liveFont = NULL;
int liveInfoCounter = 0;
void ShowLiveInfo(char *msg)
{
  if(liveFont == NULL)
  {
    TTF_Init();
    liveFont = TTF_OpenFont("data/FreeSans.ttf", 12);
  }
  if(liveInfo != NULL) {
    SDL_FreeSurface(liveInfo);
    liveInfo = NULL;
  }
  SDL_Color col;
  col.r = 0xbf;
  col.g = 0xbf;
  col.b = 0xbf;
  liveInfo = TTF_RenderText_Solid(liveFont, msg, col);
  liveInfoCounter = 50 * 5;
}

void RefreshLiveInfo()
{
  if(liveInfoCounter > 0)
  {
    SDL_Rect dst, src;
    
    dst.x = 0;
    dst.y = 2;
    src.w = liveInfo->w;
    src.h = liveInfo->h;
    src.x = 0;
    src.y = 0;
    if(liveInfo != NULL)
      SDL_BlitSurface(liveInfo, &src, prSDLScreen, &dst);
    liveInfoCounter--;
    if(liveInfoCounter == 0)
    {
      if(liveInfo != NULL) {
        SDL_FreeSurface(liveInfo);
        liveInfo = NULL;
      }
    }
  }
}

#endif

void InitAmigaVidMode(struct uae_prefs *p)
{
	/* Initialize structure for Amiga video modes */
	struct amigadisplay *ad = &adisplays;
	ad->gfxvidinfo.drawbuffer.pixbytes = 2;
	ad->gfxvidinfo.drawbuffer.bufmem = (uae_u8 *)prSDLScreen->pixels;
  ad->gfxvidinfo.drawbuffer.outwidth = p->gfx_monitor.gfx_size.width;
  ad->gfxvidinfo.drawbuffer.outheight = p->gfx_monitor.gfx_size.height << p->gfx_vresolution;
	ad->gfxvidinfo.drawbuffer.rowbytes = prSDLScreen->pitch;
}


void graphics_subshutdown (void)
{
  if(prSDLScreen != NULL)
  {
    SDL_FreeSurface(prSDLScreen);
    prSDLScreen = NULL;
  }
  if(fbdev != -1)
  {
    close(fbdev);
    fbdev = -1;
  }
}


static int CalcPandoraWidth(struct uae_prefs *p)
{
  int amigaWidth = p->gfx_monitor.gfx_size.width;
  int amigaHeight = p->gfx_monitor.gfx_size.height;
  int pandHeight = 480;
  
  p->gfx_resolution = p->gfx_monitor.gfx_size.width > 600 ? 1 : 0;
  if(amigaWidth > 600)
    amigaWidth = amigaWidth / 2; // Hires selected, but we calc in lores
  int pandWidth = (amigaWidth * pandHeight) / amigaHeight;
  pandWidth = pandWidth & (~1);
  if((pandWidth * amigaHeight) / pandHeight < amigaWidth)
    pandWidth += 2;
  if(pandWidth > 800)
    pandWidth = 800;
  return pandWidth;
}

#ifdef ANDROIDSDL
void update_onscreen()
{
	SDL_ANDROID_SetScreenKeyboardFloatingJoystick(changed_prefs.floatingJoystick);
	if (changed_prefs.onScreen==0)
	{
	  SDL_ANDROID_SetScreenKeyboardShown(0);
	}
	else
	{
	  SDL_ANDROID_SetScreenKeyboardShown(1);
	    SDL_Rect pos_textinput, pos_dpad, pos_button1, pos_button2, pos_button3, pos_button4, pos_button5, pos_button6;
	    pos_textinput.x = changed_prefs.pos_x_textinput*(SDL_ListModes(NULL, 0)[0]->w/(float)480);
	    pos_textinput.y = changed_prefs.pos_y_textinput*(SDL_ListModes(NULL, 0)[0]->h/(float)360);
	    pos_textinput.h=SDL_ListModes(NULL, 0)[0]->h / (float)10;
	    pos_textinput.w=pos_textinput.h;
	    SDL_ANDROID_SetScreenKeyboardButtonPos(SDL_ANDROID_SCREENKEYBOARD_BUTTON_TEXT, &pos_textinput);
	    pos_dpad.x = changed_prefs.pos_x_dpad*(SDL_ListModes(NULL, 0)[0]->w/(float)480);
	    pos_dpad.y = changed_prefs.pos_y_dpad*(SDL_ListModes(NULL, 0)[0]->h/(float)360);
	    pos_dpad.h=SDL_ListModes(NULL, 0)[0]->h / (float)2.5;
	    pos_dpad.w=pos_dpad.h;
	    SDL_ANDROID_SetScreenKeyboardButtonPos(SDL_ANDROID_SCREENKEYBOARD_BUTTON_DPAD, &pos_dpad);
	    pos_button1.x = changed_prefs.pos_x_button1*(SDL_ListModes(NULL, 0)[0]->w/(float)480);
	    pos_button1.y = changed_prefs.pos_y_button1*(SDL_ListModes(NULL, 0)[0]->h/(float)360);
	    pos_button1.h=SDL_ListModes(NULL, 0)[0]->h / (float)5;
	    pos_button1.w=pos_button1.h;
	    SDL_ANDROID_SetScreenKeyboardButtonPos(SDL_ANDROID_SCREENKEYBOARD_BUTTON_0, &pos_button1);
	    pos_button2.x = changed_prefs.pos_x_button2*(SDL_ListModes(NULL, 0)[0]->w/(float)480);
	    pos_button2.y = changed_prefs.pos_y_button2*(SDL_ListModes(NULL, 0)[0]->h/(float)360);
	    pos_button2.h=SDL_ListModes(NULL, 0)[0]->h / (float)5;
	    pos_button2.w=pos_button2.h;
	    SDL_ANDROID_SetScreenKeyboardButtonPos(SDL_ANDROID_SCREENKEYBOARD_BUTTON_1, &pos_button2);
	    pos_button3.x = changed_prefs.pos_x_button3*(SDL_ListModes(NULL, 0)[0]->w/(float)480);
	    pos_button3.y = changed_prefs.pos_y_button3*(SDL_ListModes(NULL, 0)[0]->h/(float)360);
	    pos_button3.h=SDL_ListModes(NULL, 0)[0]->h / (float)5;
	    pos_button3.w=pos_button3.h;
	    SDL_ANDROID_SetScreenKeyboardButtonPos(SDL_ANDROID_SCREENKEYBOARD_BUTTON_2, &pos_button3);
	    pos_button4.x = changed_prefs.pos_x_button4*(SDL_ListModes(NULL, 0)[0]->w/(float)480);
	    pos_button4.y = changed_prefs.pos_y_button4*(SDL_ListModes(NULL, 0)[0]->h/(float)360);
	    pos_button4.h=SDL_ListModes(NULL, 0)[0]->h / (float)5;
	    pos_button4.w=pos_button4.h;
	    SDL_ANDROID_SetScreenKeyboardButtonPos(SDL_ANDROID_SCREENKEYBOARD_BUTTON_3, &pos_button4);
	    pos_button5.x = changed_prefs.pos_x_button5*(SDL_ListModes(NULL, 0)[0]->w/(float)480);
	    pos_button5.y = changed_prefs.pos_y_button5*(SDL_ListModes(NULL, 0)[0]->h/(float)360);
	    pos_button5.h=SDL_ListModes(NULL, 0)[0]->h / (float)5;
	    pos_button5.w=pos_button5.h;
	    SDL_ANDROID_SetScreenKeyboardButtonPos(SDL_ANDROID_SCREENKEYBOARD_BUTTON_4, &pos_button5);
	    pos_button6.x = changed_prefs.pos_x_button6*(SDL_ListModes(NULL, 0)[0]->w/(float)480);
	    pos_button6.y = changed_prefs.pos_y_button6*(SDL_ListModes(NULL, 0)[0]->h/(float)360);
	    pos_button6.h=SDL_ListModes(NULL, 0)[0]->h / (float)5;
	    pos_button6.w=pos_button6.h;
	    SDL_ANDROID_SetScreenKeyboardButtonPos(SDL_ANDROID_SCREENKEYBOARD_BUTTON_5, &pos_button6);

	    SDL_ANDROID_SetScreenKeyboardButtonShown(SDL_ANDROID_SCREENKEYBOARD_BUTTON_TEXT, changed_prefs.onScreen_textinput);
	    SDL_ANDROID_SetScreenKeyboardButtonShown(SDL_ANDROID_SCREENKEYBOARD_BUTTON_DPAD, changed_prefs.onScreen_dpad);
	    SDL_ANDROID_SetScreenKeyboardButtonShown(SDL_ANDROID_SCREENKEYBOARD_BUTTON_0, changed_prefs.onScreen_button1);
	    SDL_ANDROID_SetScreenKeyboardButtonShown(SDL_ANDROID_SCREENKEYBOARD_BUTTON_1, changed_prefs.onScreen_button2);
	    SDL_ANDROID_SetScreenKeyboardButtonShown(SDL_ANDROID_SCREENKEYBOARD_BUTTON_2, changed_prefs.onScreen_button3);
	    SDL_ANDROID_SetScreenKeyboardButtonShown(SDL_ANDROID_SCREENKEYBOARD_BUTTON_3, changed_prefs.onScreen_button4);
	    SDL_ANDROID_SetScreenKeyboardButtonShown(SDL_ANDROID_SCREENKEYBOARD_BUTTON_4, changed_prefs.onScreen_button5);
	    SDL_ANDROID_SetScreenKeyboardButtonShown(SDL_ANDROID_SCREENKEYBOARD_BUTTON_5, changed_prefs.onScreen_button6);
	}
}
#endif

static void open_screen(struct uae_prefs *p)
{
  char layersize[20];

  graphics_subshutdown();
  
  if(!screen_is_picasso)
  {
    int layerwidth = CalcPandoraWidth(p);
	  snprintf(layersize, 19, "%dx480", layerwidth);
  }
  else
  {
    if(picasso_vidinfo.height < 480)
	    snprintf(layersize, 19, "%dx480", picasso_vidinfo.width);
	  else
	    snprintf(layersize, 19, "%dx%d", picasso_vidinfo.width, picasso_vidinfo.height);
  }
#ifndef WIN32
	setenv("SDL_OMAP_LAYER_SIZE", layersize, 1);
	setenv("SDL_OMAP_VSYNC", "0", 1);
#endif
  
#ifdef ANDROIDSDL
	update_onscreen();
#endif

  if(!screen_is_picasso)
  {
    if(prSDLScreen == NULL || prSDLScreen->w != p->gfx_monitor.gfx_size.width || prSDLScreen->h != p->gfx_monitor.gfx_size.height << p->gfx_vresolution)
    {
#ifdef ANDROIDSDL
  	  prSDLScreen = SDL_SetVideoMode(p->gfx_monitor.gfx_size.width, p->gfx_monitor.gfx_size.height << p->gfx_vresolution, 16, SDL_SWSURFACE);
#else
  	  prSDLScreen = SDL_SetVideoMode(p->gfx_monitor.gfx_size.width, p->gfx_monitor.gfx_size.height << p->gfx_vresolution, 16, SDL_SWSURFACE|SDL_DOUBLEBUF);
#endif
    }
  }
  else
  {
    if(picasso_vidinfo.width != 0 && picasso_vidinfo.height != 0)
#ifdef ANDROIDSDL
  	    prSDLScreen = SDL_SetVideoMode(picasso_vidinfo.width, picasso_vidinfo.height, 16, SDL_SWSURFACE|SDL_FULLSCREEN);
#else
    	prSDLScreen = SDL_SetVideoMode(picasso_vidinfo.width, picasso_vidinfo.height, 16, SDL_HWSURFACE|SDL_FULLSCREEN|SDL_DOUBLEBUF);
#endif
  }
  if(prSDLScreen != NULL)
  {
    InitAmigaVidMode(p);
    init_row_map();
  }
  
  current_vsync_frame = 0;

  fbdev = open("/dev/fb0", O_RDWR);
  if(fbdev != -1)
  {
    // Check if we have vsync with frame counter...
    current_vsync_frame = 0;
    ioctl(fbdev, OMAPFB_WAITFORVSYNC_FRAME, &current_vsync_frame);
    if(current_vsync_frame != 0)
      current_vsync_frame += 2;
  }
}


void update_display(struct uae_prefs *p)
{
	struct amigadisplay *ad = &adisplays;

  open_screen(p);
    
  SDL_ShowCursor(SDL_DISABLE);

  ad->framecnt = 1; // Don't draw frame before reset done
}


static int currVSyncRate = 0;
static bool SetVSyncRate(int hz)
{
	char cmd[64];

  if(currVSyncRate != hz && (hz == 50 || hz == 60))
  {
    snprintf((char*)cmd, 63, "sudo /usr/pandora/scripts/op_lcdrate.sh %d", hz);
    system(cmd);
    currVSyncRate = hz;
    return true;
  }
  return false;
}


int check_prefs_changed_gfx (void)
{
  int changed = 0;

	if(changed_prefs.ntscmode != currprefs.ntscmode)
	{
		if(changed_prefs.ntscmode)
			SetVSyncRate(60);
		else
			SetVSyncRate(50);
		fix_apmodes(&changed_prefs);
	}
  
  if(currprefs.gfx_monitor.gfx_size.height != changed_prefs.gfx_monitor.gfx_size.height ||
     currprefs.gfx_monitor.gfx_size.width != changed_prefs.gfx_monitor.gfx_size.width ||
     currprefs.gfx_resolution != changed_prefs.gfx_resolution ||
		 currprefs.gfx_vresolution != changed_prefs.gfx_vresolution)
  {
    currprefs.gfx_monitor.gfx_size.height = changed_prefs.gfx_monitor.gfx_size.height;
    currprefs.gfx_monitor.gfx_size.width = changed_prefs.gfx_monitor.gfx_size.width;
    currprefs.gfx_resolution = changed_prefs.gfx_resolution;
		currprefs.gfx_vresolution = changed_prefs.gfx_vresolution;
    update_display(&currprefs);
    changed = 1;
  }
  if (currprefs.leds_on_screen != changed_prefs.leds_on_screen ||
	    currprefs.leds_on_screen_mask[0] != changed_prefs.leds_on_screen_mask[0] ||
	    currprefs.leds_on_screen_mask[1] != changed_prefs.leds_on_screen_mask[1] ||
      currprefs.gfx_monitor.gfx_size.y != changed_prefs.gfx_monitor.gfx_size.y)	
  {
    currprefs.leds_on_screen = changed_prefs.leds_on_screen;
		currprefs.leds_on_screen_mask[0] = changed_prefs.leds_on_screen_mask[0];
		currprefs.leds_on_screen_mask[1] = changed_prefs.leds_on_screen_mask[1];
    currprefs.gfx_monitor.gfx_size.y = changed_prefs.gfx_monitor.gfx_size.y;
    changed = 1;
  }
  if (currprefs.chipset_refreshrate != changed_prefs.chipset_refreshrate) 
  {
  	currprefs.chipset_refreshrate = changed_prefs.chipset_refreshrate;
	  init_hz_normal ();
	  changed = 1;
  }

	// Not the correct place for this...
	currprefs.filesys_limit = changed_prefs.filesys_limit;
	currprefs.harddrive_read_only = changed_prefs.harddrive_read_only;
  
  if(changed) {
    inputdevice_unacquire();
		init_custom ();
		inputdevice_acquire(TRUE);
  }

  return changed;
}


int lockscr (void)
{
  if(SDL_LockSurface(prSDLScreen) == -1)
    return 0;
  init_row_map();
  return 1;
}


void unlockscr (void)
{
  SDL_UnlockSurface(prSDLScreen);
}

void wait_for_vsync(void)
{
  if(fbdev != -1)
  {
    unsigned int dummy;
    ioctl(fbdev, OMAPFB_WAITFORVSYNC, &dummy);
  }
}

void reset_sync(void)
{
  wait_for_vsync();
}


bool render_screen (bool immediate)
{
	if (savestate_state == STATE_DOSAVE)
	{
    if(delay_savestate_frame > 0)
      --delay_savestate_frame;
    else
    {
	    CreateScreenshot();
		  save_thumb(screenshot_filename);
	    savestate_state = 0;
    }
  }

#ifdef WITH_LOGGING
  RefreshLiveInfo();
#endif

	return true;
}


void show_screen (int mode)
{
  unsigned long start = read_processor_time();
  if(current_vsync_frame == 0) 
  {
    // Old style for vsync and idle time calc
    if(start < next_synctime && next_synctime - start > time_per_frame - 1000)
      usleep((next_synctime - start) - 750);
    ioctl(fbdev, OMAPFB_WAITFORVSYNC, &current_vsync_frame);
  } 
  else 
  {
    // New style for vsync and idle time calc
    int wait_till = current_vsync_frame;
    do 
    {
      ioctl(fbdev, OMAPFB_WAITFORVSYNC_FRAME, &current_vsync_frame);
    } while (wait_till >= current_vsync_frame);
    
    if(wait_till + 1 != current_vsync_frame) 
    {
      // We missed a vsync...
      next_synctime = 0;
    }
    current_vsync_frame += currprefs.gfx_framerate;
  }

  last_synctime = read_processor_time();
  SDL_Flip(prSDLScreen);

  idletime += last_synctime - start;

  if(!screen_is_picasso)
  	adisplays.gfxvidinfo.drawbuffer.bufmem = (uae_u8 *)prSDLScreen->pixels;

  if (last_synctime - next_synctime > time_per_frame - 5000)
    next_synctime = last_synctime + time_per_frame * (1 + currprefs.gfx_framerate);
  else
    next_synctime = next_synctime + time_per_frame * (1 + currprefs.gfx_framerate);
}


unsigned long target_lastsynctime(void)
{
  return last_synctime;
}


void black_screen_now(void)
{
	SDL_FillRect(prSDLScreen,NULL,0);
	SDL_Flip(prSDLScreen);
}


static void graphics_subinit (void)
{
	if (prSDLScreen == NULL)
	{
		fprintf(stderr, "Unable to set video mode: %s\n", SDL_GetError());
		return;
	}
	else
	{
		SDL_Flip(prSDLScreen);
		SDL_ShowCursor(SDL_DISABLE);
    InitAmigaVidMode(&currprefs);
	}
}


static int init_colors (void)
{
  int red_bits, green_bits, blue_bits;
  int red_shift, green_shift, blue_shift;

	/* Truecolor: */
	red_bits = bits_in_mask(prSDLScreen->format->Rmask);
	green_bits = bits_in_mask(prSDLScreen->format->Gmask);
	blue_bits = bits_in_mask(prSDLScreen->format->Bmask);
	red_shift = mask_shift(prSDLScreen->format->Rmask);
	green_shift = mask_shift(prSDLScreen->format->Gmask);
	blue_shift = mask_shift(prSDLScreen->format->Bmask);
	alloc_colors64k (red_bits, green_bits, blue_bits, red_shift, green_shift, blue_shift);
	notice_new_xcolors();

	return 1;
}


/*
 * Find the colour depth of the display
 */
static int get_display_depth (void)
{
  const SDL_VideoInfo *vid_info;
  int depth = 0;

  if ((vid_info = SDL_GetVideoInfo())) {
  	depth = vid_info->vfmt->BitsPerPixel;

	  /* Don't trust the answer if it's 16 bits; the display
	   * could actually be 15 bits deep. We'll count the bits
	   * ourselves */
	  if (depth == 16)
	    depth = bits_in_mask (vid_info->vfmt->Rmask) + bits_in_mask (vid_info->vfmt->Gmask) + bits_in_mask (vid_info->vfmt->Bmask);
  }
  return depth;
}


int GetSurfacePixelFormat(void)
{
  int depth = get_display_depth();
  int unit = (depth + 1) & 0xF8;

  return (unit == 8 ? RGBFB_CHUNKY
		: depth == 15 && unit == 16 ? RGBFB_R5G5B5
		: depth == 16 && unit == 16 ? RGBFB_R5G6B5
		: unit == 24 ? RGBFB_B8G8R8
		: unit == 32 ? RGBFB_R8G8B8A8
		: RGBFB_NONE);
}


int graphics_init (bool mousecapture)
{
	inputdevice_unacquire();

	graphics_subinit ();

  if (!init_colors ())
		return 0;
    
	inputdevice_acquire(TRUE);

  return 1;
}

void graphics_leave (void)
{
  graphics_subshutdown ();
}


#define  systemRedShift      (prSDLScreen->format->Rshift)
#define  systemGreenShift    (prSDLScreen->format->Gshift)
#define  systemBlueShift     (prSDLScreen->format->Bshift)
#define  systemRedMask       (prSDLScreen->format->Rmask)
#define  systemGreenMask     (prSDLScreen->format->Gmask)
#define  systemBlueMask      (prSDLScreen->format->Bmask)

static int save_png(SDL_Surface* surface, char *path)
{
  int w = surface->w;
  int h = surface->h;
  unsigned char * pix = (unsigned char *)surface->pixels;
  unsigned char writeBuffer[1024 * 3];
  FILE *f  = fopen(path,"wb");
  if(!f) return 0;
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                                NULL,
                                                NULL,
                                                NULL);
  if(!png_ptr) {
    fclose(f);
    return 0;
  }

  png_infop info_ptr = png_create_info_struct(png_ptr);

  if(!info_ptr) {
    png_destroy_write_struct(&png_ptr,NULL);
    fclose(f);
    return 0;
  }

  png_init_io(png_ptr,f);

  png_set_IHDR(png_ptr,
               info_ptr,
               w,
               h,
               8,
               PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);

  png_write_info(png_ptr,info_ptr);

  unsigned char *b = writeBuffer;

  int sizeX = w;
  int sizeY = h;
  int y;
  int x;

  unsigned short *p = (unsigned short *)pix;
  for(y = 0; y < sizeY; y++) 
  {
     for(x = 0; x < sizeX; x++) 
     {
       unsigned short v = p[x];
  
       *b++ = ((v & systemRedMask  ) >> systemRedShift  ) << 3; // R
       *b++ = ((v & systemGreenMask) >> systemGreenShift) << 2; // G 
       *b++ = ((v & systemBlueMask ) >> systemBlueShift ) << 3; // B
     }
     p += surface->pitch / 2;
     png_write_row(png_ptr,writeBuffer);
     b = writeBuffer;
  }

  png_write_end(png_ptr, info_ptr);

  png_destroy_write_struct(&png_ptr, &info_ptr);

  fclose(f);
  return 1;
}


static void CreateScreenshot(void)
{
	int w, h;

  if(current_screenshot != NULL)
  {
    SDL_FreeSurface(current_screenshot);
    current_screenshot = NULL;
  }

  if(prSDLScreen != NULL) {
	  w=prSDLScreen->w;
	  h=prSDLScreen->h;
	  current_screenshot = SDL_CreateRGBSurfaceFrom(prSDLScreen->pixels, w, h, prSDLScreen->format->BitsPerPixel, prSDLScreen->pitch,
	    prSDLScreen->format->Rmask, prSDLScreen->format->Gmask, prSDLScreen->format->Bmask, prSDLScreen->format->Amask);
  }
}


static int save_thumb(char *path)
{
	int ret = 0;
	if(current_screenshot != NULL)
	{
	  ret = save_png(current_screenshot, path);
	  SDL_FreeSurface(current_screenshot);
	  current_screenshot = NULL;
	}
	return ret;
}


bool vsync_switchmode (int hz)
{
	int changed_height = changed_prefs.gfx_monitor.gfx_size.height;
	struct amigadisplay *ad = &adisplays;
	
	if (hz >= 55)
		hz = 60;
	else
		hz = 50;

  if(hz == 50 && currVSyncRate == 60)
  {
    // Switch from NTSC -> PAL
    switch(changed_height) {
      case 200: changed_height = 240; break;
      case 216: changed_height = 262; break;
      case 240: changed_height = 270; break;
      case 256: changed_height = 270; break;
      case 262: changed_height = 270; break;
      case 270: changed_height = 270; break;
    }
  }
  else if(hz == 60 && currVSyncRate == 50)
  {
    // Switch from PAL -> NTSC
    switch(changed_height) {
      case 200: changed_height = 200; break;
      case 216: changed_height = 200; break;
      case 240: changed_height = 200; break;
      case 256: changed_height = 216; break;
      case 262: changed_height = 216; break;
      case 270: changed_height = 240; break;
    }
  }
  
  if(hz != currVSyncRate) 
  {
    SetVSyncRate(hz);
  	black_screen_now();
    fpscounter_reset();
    time_per_frame = 1000 * 1000 / (hz);
  }
  
  if(!ad->picasso_on && !ad->picasso_requested_on)
    changed_prefs.gfx_monitor.gfx_size.height = changed_height;
  
  return true;
}


bool target_graphics_buffer_update (void)
{
  bool rate_changed = false;
  
  if(currprefs.gfx_monitor.gfx_size.height != changed_prefs.gfx_monitor.gfx_size.height)
  {
    update_display(&changed_prefs);
    rate_changed = true;
  }

	if(rate_changed)
  {
  	black_screen_now();
    fpscounter_reset();
    time_per_frame = 1000 * 1000 / (currprefs.chipset_refreshrate);
  }

  return true;
}


#ifdef PICASSO96


int picasso_palette (struct MyCLUTEntry *CLUT)
{
  int i, changed;

  changed = 0;
  for (i = 0; i < 256; i++) {
    int r = CLUT[i].Red;
    int g = CLUT[i].Green;
    int b = CLUT[i].Blue;
    int value = (r << 16 | g << 8 | b);
  	uae_u32 v = CONVERT_RGB(value);
	  if (v !=  picasso_vidinfo.clut[i]) {
	     picasso_vidinfo.clut[i] = v;
	     changed = 1;
	  } 
  }
  return changed;
}

static int resolution_compare (const void *a, const void *b)
{
  struct PicassoResolution *ma = (struct PicassoResolution *)a;
  struct PicassoResolution *mb = (struct PicassoResolution *)b;
  if (ma->res.width < mb->res.width)
  	return -1;
  if (ma->res.width > mb->res.width)
  	return 1;
  if (ma->res.height < mb->res.height)
  	return -1;
  if (ma->res.height > mb->res.height)
  	return 1;
  return ma->depth - mb->depth;
}
static void sortmodes (void)
{
  int	i = 0, idx = -1;
  int pw = -1, ph = -1;
  while (DisplayModes[i].depth >= 0)
  	i++;
  qsort (DisplayModes, i, sizeof (struct PicassoResolution), resolution_compare);
  for (i = 0; DisplayModes[i].depth >= 0; i++) {
  	if (DisplayModes[i].res.height != ph || DisplayModes[i].res.width != pw) {
	    ph = DisplayModes[i].res.height;
	    pw = DisplayModes[i].res.width;
	    idx++;
	  }
	  DisplayModes[i].residx = idx;
  }
}

static void modesList (void)
{
  int i, j;

  i = 0;
  while (DisplayModes[i].depth >= 0) {
  	write_log ("%d: %s (", i, DisplayModes[i].name);
  	j = 0;
  	while (DisplayModes[i].refresh[j] > 0) {
	    if (j > 0)
	    	write_log (",");
	    write_log ("%d", DisplayModes[i].refresh[j]);
	    j++;
	  }
	  write_log (")\n");
	  i++;
  }
}

void picasso_InitResolutions (void)
{
  struct MultiDisplay *md1;
  int i, count = 0;
  char tmp[200];
  int bit_idx;
  int bits[] = { 8, 16, 32 };
  
  Displays[0].primary = 1;
  Displays[0].disabled = 0;
  Displays[0].rect.left = 0;
  Displays[0].rect.top = 0;
  Displays[0].rect.right = 800;
  Displays[0].rect.bottom = 480;
  sprintf (tmp, "%s (%d*%d)", "Display", Displays[0].rect.right, Displays[0].rect.bottom);
  Displays[0].name = my_strdup(tmp);
  Displays[0].name2 = my_strdup("Display");

  md1 = Displays;
  DisplayModes = md1->DisplayModes = xmalloc (struct PicassoResolution, MAX_PICASSO_MODES);
  for (i = 0; i < MAX_SCREEN_MODES && count < MAX_PICASSO_MODES; i++) {
    for(bit_idx = 0; bit_idx < 3; ++bit_idx) {
      int bitdepth = bits[bit_idx];
      int bit_unit = (bitdepth + 1) & 0xF8;
      int rgbFormat = (bitdepth == 8 ? RGBFB_CLUT : (bitdepth == 16 ? RGBFB_R5G6B5 : RGBFB_R8G8B8A8));
      int pixelFormat = 1 << rgbFormat;
  	  pixelFormat |= RGBFF_CHUNKY;
      
  	  if (SDL_VideoModeOK (x_size_table[i], y_size_table[i], 16, SDL_SWSURFACE))
  	  {
  	    DisplayModes[count].res.width = x_size_table[i];
  	    DisplayModes[count].res.height = y_size_table[i];
  	    DisplayModes[count].depth = bit_unit >> 3;
        DisplayModes[count].refresh[0] = 50;
        DisplayModes[count].refresh[1] = 60;
        DisplayModes[count].refresh[2] = 0;
        DisplayModes[count].colormodes = pixelFormat;
        sprintf(DisplayModes[count].name, "%dx%d, %d-bit",
  	      DisplayModes[count].res.width, DisplayModes[count].res.height, DisplayModes[count].depth * 8);
  
  	    count++;
      }
    }
  }
  DisplayModes[count].depth = -1;
  sortmodes();
  modesList();
  DisplayModes = Displays[0].DisplayModes;
}


void gfx_set_picasso_state (int on)
{
	if (on == screen_is_picasso)
		return;

	screen_is_picasso = on;
  open_screen(&currprefs);
  if(prSDLScreen != NULL)
    picasso_vidinfo.rowbytes	= prSDLScreen->pitch;
}

void gfx_set_picasso_modeinfo (uae_u32 w, uae_u32 h, uae_u32 depth, RGBFTYPE rgbfmt)
{
  depth >>= 3;
  if( ((unsigned)picasso_vidinfo.width == w ) &&
    ( (unsigned)picasso_vidinfo.height == h ) &&
    ( (unsigned)picasso_vidinfo.depth == depth ) &&
    ( picasso_vidinfo.selected_rgbformat == rgbfmt) )
  	return;

  picasso_vidinfo.selected_rgbformat = rgbfmt;
  picasso_vidinfo.width = w;
  picasso_vidinfo.height = h;
  picasso_vidinfo.depth = 2; // Native depth
  picasso_vidinfo.extra_mem = 1;

  picasso_vidinfo.pixbytes = 2; // Native bytes
  if (screen_is_picasso)
  {
  	open_screen(&currprefs);
  	if(prSDLScreen != NULL)
      picasso_vidinfo.rowbytes	= prSDLScreen->pitch;
    picasso_vidinfo.rgbformat = RGBFB_R5G6B5;
  }
}

uae_u8 *gfx_lock_picasso (void)
{
  if(prSDLScreen == NULL || screen_is_picasso == 0)
    return NULL;
  SDL_LockSurface(prSDLScreen);
  picasso_vidinfo.rowbytes = prSDLScreen->pitch;
  return (uae_u8 *)prSDLScreen->pixels;
}

void gfx_unlock_picasso (bool dorender)
{
  SDL_UnlockSurface(prSDLScreen);
  if(dorender)
  {
    render_screen(true);
    show_screen(0);
  }
}

#endif // PICASSO96
