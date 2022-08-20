// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2013 Google, Inc
 */

#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <linux/input.h>
#include <SDL2/SDL.h>
#include <asm/state.h>

/**
 * struct buf_info - a data buffer holding audio data
 *
 * @pos:	Current position playing in audio buffer
 * @size:	Size of data in audio buffer (0=empty)
 * @alloced:	Allocated size of audio buffer (max size it can hold)
 * @data:	Audio data
 */
struct buf_info {
	uint pos;
	uint size;
	uint alloced;
	uint8_t *data;
};

/**
 * struct sdl_info - Information about our use of the SDL library
 *
 * @width: Width of simulated LCD display
 * @height: Height of simulated LCD display
 * @vis_width: Visible width (may be larger to allow for scaling up)
 * @vis_height: Visible height (may be larger to allow for scaling up)
 * @depth: Depth of the display in bits per pixel (16 or 32)
 * @pitch: Number of bytes per line of the display
 * @sample_rate: Current sample rate for audio
 * @audio_active: true if audio can be used
 * @inited: true if this module is initialised
 * @cur_buf: Current audio buffer being used by sandbox_sdl_fill_audio (0 or 1)
 * @buf: The two available audio buffers. SDL can be reading from one while we
 *	are setting up the next
 * @running: true if audio is running
 * @stopping: true if audio will stop once it runs out of data
 * @texture: SDL texture to use for U-Boot display contents
 * @renderer: SDL renderer to use
 */
static struct sdl_info {
	int width;
	int height;
	int vis_width;
	int vis_height;
	int depth;
	int pitch;
	uint sample_rate;
	bool audio_active;
	bool inited;
	int cur_buf;
	struct buf_info buf[2];
	bool running;
	bool stopping;
	SDL_Texture *texture;
	SDL_Renderer *renderer;
} sdl;

static void sandbox_sdl_poll_events(void)
{
	/*
	 * We don't want to include common.h in this file since it uses
	 * system headers. So add a declation here.
	 */
	extern void reset_cpu(unsigned long addr);
	SDL_Event event;

	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			puts("LCD window closed - quitting\n");
			reset_cpu(1);
			break;
		}
	}
}

static int sandbox_sdl_ensure_init(void)
{
	if (!sdl.inited) {
		if (SDL_Init(0) < 0) {
			printf("Unable to initialise SDL: %s\n",
			       SDL_GetError());
			return -EIO;
		}

		atexit(SDL_Quit);

		sdl.inited = true;
	}
	return 0;
}

int sandbox_sdl_init_display(int width, int height, int log2_bpp,
			     bool double_size)
{
	struct sandbox_state *state = state_get_current();
	int err;

	if (!width || !state->show_lcd)
		return 0;
	err = sandbox_sdl_ensure_init();
	if (err)
		return err;
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
		printf("Unable to initialise SDL LCD: %s\n", SDL_GetError());
		return -EPERM;
	}
	sdl.width = width;
	sdl.height = height;
	if (double_size) {
		sdl.vis_width = sdl.width * 2;
		sdl.vis_height = sdl.height * 2;
	} else {
		sdl.vis_width = sdl.width;
		sdl.vis_height = sdl.height;
	}

	sdl.depth = 1 << log2_bpp;
	sdl.pitch = sdl.width * sdl.depth / 8;
	SDL_Window *screen = SDL_CreateWindow("U-Boot", SDL_WINDOWPOS_UNDEFINED,
					      SDL_WINDOWPOS_UNDEFINED,
					      sdl.vis_width, sdl.vis_height, 0);
	if (!screen) {
		printf("Unable to initialise SDL screen: %s\n",
		       SDL_GetError());
		return -EIO;
	}
	if (log2_bpp != 4 && log2_bpp != 5) {
		printf("U-Boot SDL does not support depth %d\n", log2_bpp);
		return -EINVAL;
	}
	sdl.renderer = SDL_CreateRenderer(screen, -1,
					  SDL_RENDERER_ACCELERATED |
					  SDL_RENDERER_PRESENTVSYNC);
	if (!sdl.renderer) {
		printf("Unable to initialise SDL renderer: %s\n",
		       SDL_GetError());
		return -EIO;
	}

	sdl.texture = SDL_CreateTexture(sdl.renderer, log2_bpp == 4 ?
					SDL_PIXELFORMAT_RGB565 :
					SDL_PIXELFORMAT_RGB888,
					SDL_TEXTUREACCESS_STREAMING,
					width, height);
	if (!sdl.texture) {
		printf("Unable to initialise SDL texture: %s\n",
		       SDL_GetError());
		return -EBADF;
	}
	sandbox_sdl_poll_events();

	return 0;
}

int sandbox_sdl_sync(void *lcd_base)
{
	SDL_UpdateTexture(sdl.texture, NULL, lcd_base, sdl.pitch);
	SDL_RenderCopy(sdl.renderer, sdl.texture, NULL, NULL);
	SDL_RenderPresent(sdl.renderer);
	sandbox_sdl_poll_events();

	return 0;
}

static const unsigned short sdl_to_keycode[SDL_NUM_SCANCODES] = {
	[SDL_SCANCODE_A]	= KEY_A,
	[SDL_SCANCODE_B]	= KEY_B,
	[SDL_SCANCODE_C]	= KEY_C,
	[SDL_SCANCODE_D]	= KEY_D,
	[SDL_SCANCODE_E]	= KEY_E,
	[SDL_SCANCODE_F]	= KEY_F,
	[SDL_SCANCODE_G]	= KEY_G,
	[SDL_SCANCODE_H]	= KEY_H,
	[SDL_SCANCODE_I]	= KEY_I,
	[SDL_SCANCODE_J]	= KEY_J,
	[SDL_SCANCODE_K]	= KEY_K,
	[SDL_SCANCODE_L]	= KEY_L,
	[SDL_SCANCODE_M]	= KEY_M,
	[SDL_SCANCODE_N]	= KEY_N,
	[SDL_SCANCODE_O]	= KEY_O,
	[SDL_SCANCODE_P]	= KEY_P,
	[SDL_SCANCODE_Q]	= KEY_Q,
	[SDL_SCANCODE_R]	= KEY_R,
	[SDL_SCANCODE_S]	= KEY_S,
	[SDL_SCANCODE_T]	= KEY_T,
	[SDL_SCANCODE_U]	= KEY_U,
	[SDL_SCANCODE_V]	= KEY_V,
	[SDL_SCANCODE_W]	= KEY_W,
	[SDL_SCANCODE_X]	= KEY_X,
	[SDL_SCANCODE_Y]	= KEY_Y,
	[SDL_SCANCODE_Z]	= KEY_Z,

	[SDL_SCANCODE_1]	= KEY_1,
	[SDL_SCANCODE_2]	= KEY_2,
	[SDL_SCANCODE_3]	= KEY_3,
	[SDL_SCANCODE_4]	= KEY_4,
	[SDL_SCANCODE_5]	= KEY_5,
	[SDL_SCANCODE_6]	= KEY_6,
	[SDL_SCANCODE_7]	= KEY_7,
	[SDL_SCANCODE_8]	= KEY_8,
	[SDL_SCANCODE_9]	= KEY_9,
	[SDL_SCANCODE_0]	= KEY_0,

	[SDL_SCANCODE_RETURN]	= KEY_ENTER,
	[SDL_SCANCODE_ESCAPE]	= KEY_ESC,
	[SDL_SCANCODE_BACKSPACE]	= KEY_BACKSPACE,
	[SDL_SCANCODE_TAB]	= KEY_TAB,
	[SDL_SCANCODE_SPACE]	= KEY_SPACE,

	[SDL_SCANCODE_MINUS]	= KEY_MINUS,
	[SDL_SCANCODE_EQUALS]	= KEY_EQUAL,
	[SDL_SCANCODE_BACKSLASH]	= KEY_BACKSLASH,
	[SDL_SCANCODE_SEMICOLON]	= KEY_SEMICOLON,
	[SDL_SCANCODE_APOSTROPHE]	= KEY_APOSTROPHE,
	[SDL_SCANCODE_GRAVE]	= KEY_GRAVE,
	[SDL_SCANCODE_COMMA]	= KEY_COMMA,
	[SDL_SCANCODE_PERIOD]	= KEY_DOT,
	[SDL_SCANCODE_SLASH]	= KEY_SLASH,

	[SDL_SCANCODE_CAPSLOCK]	= KEY_CAPSLOCK,

	[SDL_SCANCODE_F1]	= KEY_F1,
	[SDL_SCANCODE_F2]	= KEY_F2,
	[SDL_SCANCODE_F3]	= KEY_F3,
	[SDL_SCANCODE_F4]	= KEY_F4,
	[SDL_SCANCODE_F5]	= KEY_F5,
	[SDL_SCANCODE_F6]	= KEY_F6,
	[SDL_SCANCODE_F7]	= KEY_F7,
	[SDL_SCANCODE_F8]	= KEY_F8,
	[SDL_SCANCODE_F9]	= KEY_F9,
	[SDL_SCANCODE_F10]	= KEY_F10,
	[SDL_SCANCODE_F11]	= KEY_F11,
	[SDL_SCANCODE_F12]	= KEY_F12,

	[SDL_SCANCODE_PRINTSCREEN]	= KEY_PRINT,
	[SDL_SCANCODE_SCROLLLOCK]	= KEY_SCROLLLOCK,
	[SDL_SCANCODE_PAUSE]	= KEY_PAUSE,
	[SDL_SCANCODE_INSERT]	= KEY_INSERT,
	[SDL_SCANCODE_HOME]	= KEY_HOME,
	[SDL_SCANCODE_PAGEUP]	= KEY_PAGEUP,
	[SDL_SCANCODE_DELETE]	= KEY_DELETE,
	[SDL_SCANCODE_END]	= KEY_END,
	[SDL_SCANCODE_PAGEDOWN]	= KEY_PAGEDOWN,
	[SDL_SCANCODE_RIGHT]	= KEY_RIGHT,
	[SDL_SCANCODE_LEFT]	= KEY_LEFT,
	[SDL_SCANCODE_DOWN]	= KEY_DOWN,
	[SDL_SCANCODE_UP]	= KEY_UP,

	[SDL_SCANCODE_NUMLOCKCLEAR]	= KEY_NUMLOCK,
	[SDL_SCANCODE_KP_DIVIDE]	= KEY_KPSLASH,
	[SDL_SCANCODE_KP_MULTIPLY]	= KEY_KPASTERISK,
	[SDL_SCANCODE_KP_MINUS]	= KEY_KPMINUS,
	[SDL_SCANCODE_KP_PLUS]	= KEY_KPPLUS,
	[SDL_SCANCODE_KP_ENTER]	= KEY_KPENTER,
	[SDL_SCANCODE_KP_1]	= KEY_KP1,
	[SDL_SCANCODE_KP_2]	= KEY_KP2,
	[SDL_SCANCODE_KP_3]	= KEY_KP3,
	[SDL_SCANCODE_KP_4]	= KEY_KP4,
	[SDL_SCANCODE_KP_5]	= KEY_KP5,
	[SDL_SCANCODE_KP_6]	= KEY_KP6,
	[SDL_SCANCODE_KP_7]	= KEY_KP7,
	[SDL_SCANCODE_KP_8]	= KEY_KP8,
	[SDL_SCANCODE_KP_9]	= KEY_KP9,
	[SDL_SCANCODE_KP_0]	= KEY_KP0,
	[SDL_SCANCODE_KP_PERIOD]	= KEY_KPDOT,

	[SDL_SCANCODE_KP_EQUALS]	= KEY_KPEQUAL,
	[SDL_SCANCODE_KP_COMMA]	= KEY_KPCOMMA,

	[SDL_SCANCODE_SYSREQ]	= KEY_SYSRQ,
};

int sandbox_sdl_scan_keys(int key[], int max_keys)
{
	const Uint8 *keystate;
	int num_keys;
	int i, count;

	sandbox_sdl_poll_events();
	keystate = SDL_GetKeyboardState(&num_keys);
	for (i = count = 0; i < num_keys; i++) {
		if (count < max_keys && keystate[i]) {
			int keycode = sdl_to_keycode[i];

			if (keycode)
				key[count++] = keycode;
		}
	}

	return count;
}

int sandbox_sdl_key_pressed(int keycode)
{
	int key[8];	/* allow up to 8 keys to be pressed at once */
	int count;
	int i;

	count = sandbox_sdl_scan_keys(key, sizeof(key) / sizeof(key[0]));
	for (i = 0; i < count; i++) {
		if (key[i] == keycode)
			return 0;
	}

	return -ENOENT;
}

void sandbox_sdl_fill_audio(void *udata, Uint8 *stream, int len)
{
	struct buf_info *buf;
	int avail;
	bool have_data = false;
	int i;

	for (i = 0; i < 2; i++) {
		buf = &sdl.buf[sdl.cur_buf];
		avail = buf->size - buf->pos;
		if (avail <= 0) {
			sdl.cur_buf = 1 - sdl.cur_buf;
			continue;
		}
		if (avail > len)
			avail = len;
		have_data = true;

		SDL_MixAudio(stream, buf->data + buf->pos, avail,
			     SDL_MIX_MAXVOLUME);
		buf->pos += avail;
		len -= avail;

		/* Move to next buffer if we are at the end */
		if (buf->pos == buf->size)
			buf->size = 0;
		else
			break;
	}
	sdl.stopping = !have_data;
}

int sandbox_sdl_sound_init(int rate, int channels)
{
	SDL_AudioSpec wanted, have;
	int i;

	if (sandbox_sdl_ensure_init())
		return -1;

	if (sdl.audio_active)
		return 0;

	/* Set the audio format */
	wanted.freq = rate;
	wanted.format = AUDIO_S16;
	wanted.channels = channels;
	wanted.samples = 1024;  /* Good low-latency value for callback */
	wanted.callback = sandbox_sdl_fill_audio;
	wanted.userdata = NULL;

	for (i = 0; i < 2; i++) {
		struct buf_info *buf = &sdl.buf[i];

		buf->alloced = sizeof(uint16_t) * wanted.freq * wanted.channels;
		buf->data = malloc(buf->alloced);
		if (!buf->data) {
			printf("%s: Out of memory\n", __func__);
			if (i == 1)
				free(sdl.buf[0].data);
			return -1;
		}
		buf->pos = 0;
		buf->size = 0;
	}

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		printf("Unable to initialise SDL audio: %s\n", SDL_GetError());
		goto err;
	}

	/* Open the audio device, forcing the desired format */
	if (SDL_OpenAudio(&wanted, &have) < 0) {
		printf("Couldn't open audio: %s\n", SDL_GetError());
		goto err;
	}
	if (have.format != wanted.format) {
		printf("Couldn't select required audio format\n");
		goto err;
	}
	sdl.audio_active = true;
	sdl.sample_rate = wanted.freq;
	sdl.cur_buf = 0;
	sdl.running = false;

	return 0;

err:
	for (i = 0; i < 2; i++)
		free(sdl.buf[i].data);
	return -1;
}

int sandbox_sdl_sound_play(const void *data, uint size)
{
	struct buf_info *buf;

	if (!sdl.audio_active)
		return 0;

	buf = &sdl.buf[0];
	if (buf->size)
		buf = &sdl.buf[1];
	while (buf->size)
		usleep(1000);

	if (size > buf->alloced)
		return -E2BIG;

	memcpy(buf->data, data, size);
	buf->size = size;
	buf->pos = 0;
	if (!sdl.running) {
		SDL_PauseAudio(0);
		sdl.running = true;
		sdl.stopping = false;
	}

	return 0;
}

int sandbox_sdl_sound_stop(void)
{
	if (sdl.running) {
		while (!sdl.stopping)
			SDL_Delay(100);

		SDL_PauseAudio(1);
		sdl.running = 0;
		sdl.stopping = false;
	}

	return 0;
}
