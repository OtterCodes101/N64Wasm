#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include "neil.h"

#ifndef NO_LIBCO
#include <libco.h>
#endif

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
// #include <glsm/glsmsym.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

//NEILTODO - handle wasm
#include <GL/glew.h>
#include <glsm/glsm.h>

#endif

#ifdef _WIN32
#else
#include "../glide2gl/src/Glitch64/glide.h"
#endif

#include "api/m64p_frontend.h"
#include "plugin/plugin.h"
#include "api/m64p_types.h"
#include "r4300/r4300.h"
#include "memory/memory.h"
#include "main/main.h"
#include "main/cheat.h"
#include "main/version.h"
#include "main/savestates.h"
#include "dd/dd_disk.h"
#include "pi/pi_controller.h"
#include "si/pif.h"
#include "libretro_memory.h"

/* Cxd4 RSP */
#include "../mupen64plus-rsp-cxd4/config.h"
#include "plugin/audio_libretro/audio_plugin.h"
#include "../Graphics/plugin.h"

#ifdef HAVE_THR_AL
#include "../mupen64plus-video-angrylion/vdac.h"
#endif

#ifndef PRESCALE_WIDTH
#define PRESCALE_WIDTH  640
#endif

#ifndef PRESCALE_HEIGHT
#define PRESCALE_HEIGHT 625
#endif

#if defined(NO_LIBCO) && defined(DYNAREC)
#error cannot currently use dynarecs without libco
#endif

/* forward declarations */
int InitGfx(void);
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
int glide64InitGfx(void);
void gles2n64_reset(void);
#endif

#if defined(HAVE_PARALLEL)
#include "../mupen64plus-video-paraLLEl/parallel.h"

static struct retro_hw_render_callback hw_render;
static struct retro_hw_render_context_negotiation_interface_vulkan hw_context_negotiation;
static const struct retro_hw_render_interface_vulkan* vulkan;
#endif

#define ISHEXDEC ((codeLine[cursor]>='0') && (codeLine[cursor]<='9')) || ((codeLine[cursor]>='a') && (codeLine[cursor]<='f')) || ((codeLine[cursor]>='A') && (codeLine[cursor]<='F'))

//libretro callbacks
//struct retro_perf_callback perf_cb;
//retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;
//retro_log_printf_t log_cb = NULL;
//retro_video_refresh_t video_cb = NULL;
//retro_input_poll_t poll_cb = NULL;
//retro_input_state_t input_cb = NULL;
//retro_audio_sample_batch_t audio_batch_cb = NULL;
//retro_environment_t environ_cb = NULL;

struct retro_rumble_interface rumble;

#define SUBSYSTEM_CART_DISK 0x0101

static const struct retro_subsystem_rom_info n64_cart_disk[] = {
   { "Cartridge", "n64|z64|v64|bin", false, false, false, NULL, 0 },
   { "Disk",      "ndd|bin",         false, false, false, NULL, 0 },
   { NULL }
};

static const struct retro_subsystem_info subsystems[] = {
   { "Cartridge and Disk", "n64_cart_disk", n64_cart_disk, 2, SUBSYSTEM_CART_DISK},
   { NULL }
};

save_memory_data saved_memory;

#ifdef NO_LIBCO
static bool stop_stepping;
#else
cothread_t main_thread;
static cothread_t game_thread;
#endif

float polygonOffsetFactor = 0.0f;
float polygonOffsetUnits = 0.0f;

static bool vulkan_inited = false;
static bool gl_inited = false;

int astick_deadzone = 1000;
int astick_sensitivity = 100;
int first_time = 1;
bool flip_only = false;

static uint8_t* cart_data = NULL;
static uint32_t cart_size = 0;
static uint8_t* disk_data = NULL;
static uint32_t disk_size = 0;

static bool     emu_initialized = false;
static unsigned initial_boot = true;
static unsigned audio_buffer_size = 2048;

static unsigned retro_filtering = 0;
static unsigned retro_dithering = 0;
static bool     reinit_screen = false;
static bool     first_context_reset = false;
static bool     pushed_frame = false;

bool frame_dupe = false;

uint32_t gfx_plugin_accuracy = 2;
static enum rsp_plugin_type
rsp_plugin;
uint32_t screen_width = 640;
uint32_t screen_height = 480;
uint32_t screen_pitch = 0;
uint32_t screen_aspectmodehint;
uint32_t send_allist_to_hle_rsp = 0;

unsigned int BUFFERSWAP = 0;
unsigned int FAKE_SDL_TICKS = 0;

bool alternate_mapping;

static bool initializing = true;

extern int g_vi_refresh_rate;

/* after the controller's CONTROL* member has been assigned we can update
 * them straight from here... */
extern struct
{
    CONTROL* control;
    BUTTONS buttons;
} controller[4];

/* ...but it won't be at least the first time we're called, in that case set
 * these instead for input_plugin to read. */
int pad_pak_types[4];
int pad_present[4] = { 1, 0, 0, 0 };

void log_cb(int type, char* message)
{
    printf("%s\n", message);
}

static void n64DebugCallback(void* aContext, int aLevel, const char* aMessage)
{
    char buffer[1024];

    sprintf(buffer, "mupen64plus: %s\n", aMessage);

    switch (aLevel)
    {
    case M64MSG_ERROR:
        log_cb(RETRO_LOG_ERROR, buffer);
        break;
    case M64MSG_INFO:
        log_cb(RETRO_LOG_INFO, buffer);
        break;
    case M64MSG_WARNING:
        log_cb(RETRO_LOG_WARN, buffer);
        break;
    case M64MSG_VERBOSE:
    case M64MSG_STATUS:
        log_cb(RETRO_LOG_DEBUG, buffer);
        break;
    default:
        break;
    }
}

extern m64p_rom_header ROM_HEADER;

static void core_settings_autoselect_gfx_plugin(void)
{
    if (gl_inited)
    {
#if defined(HAVE_OPENGL)
        gfx_plugin = GFX_GLIDE64;
#endif
#if defined(HAVE_GLN64)
        gfx_plugin = GFX_GLN64;
#endif
        return;
    }
}

static void core_settings_autoselect_rsp_plugin(void)
{
    rsp_plugin = RSP_HLE;
}

unsigned libretro_get_gfx_plugin(void)
{
    return gfx_plugin;
}

static void core_settings_set_defaults(void)
{
    core_settings_autoselect_gfx_plugin();

    /* Load RSP plugin core option */

    //if (rsp_var.value && !strcmp(rsp_var.value, "auto"))
    //    core_settings_autoselect_rsp_plugin();
    //if (rsp_var.value && !strcmp(rsp_var.value, "hle") && !vulkan_inited)
        rsp_plugin = RSP_HLE;
    //if (rsp_var.value && !strcmp(rsp_var.value, "cxd4"))
    //    rsp_plugin = RSP_CXD4;
    //if (rsp_var.value && !strcmp(rsp_var.value, "parallel"))
    //    rsp_plugin = RSP_PARALLEL;
}

bool is_cartridge_rom(const uint8_t* data)
{
    return (data != NULL && *((uint32_t*)data) != 0x16D348E8 && *((uint32_t*)data) != 0x56EE6322);
}

static bool emu_step_load_data()
{
    const char* dir = "empty";
    bool loaded = false;
    char slash;

#if defined(_WIN32)
    slash = '\\';
#else
    slash = '/';
#endif

    if (CoreStartup(FRONTEND_API_VERSION, ".", ".", "Core", n64DebugCallback, 0, 0) && log_cb)
        log_cb(RETRO_LOG_ERROR, "mupen64plus: Failed to initialize core\n");

    if (cart_data != NULL && cart_size != 0)
    {
        /* N64 Cartridge loading */
        loaded = true;

        if (log_cb)
            log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_ROM_OPEN\n");

        if (CoreDoCommand(M64CMD_ROM_OPEN, cart_size, (void*)cart_data))
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "mupen64plus: Failed to load ROM\n");
            goto load_fail;
        }

        free(cart_data);
        cart_data = NULL;

        if (log_cb)
            log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_ROM_GET_HEADER\n");

        if (CoreDoCommand(M64CMD_ROM_GET_HEADER, sizeof(ROM_HEADER), &ROM_HEADER))
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "mupen64plus; Failed to query ROM header information\n");
            goto load_fail;
        }
    }
    if (disk_data != NULL && disk_size != 0)
    {
        /* 64DD Disk loading */
        char disk_ipl_path[256];
        FILE* fPtr;
        long romlength = 0;
        uint8_t* ipl_data = NULL;

        loaded = true;
        //NEILTODO - if I want to use DD
        // if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || !dir)
        //     goto load_fail;

        /* connect saved_memory.disk to disk */
        g_dd_disk = saved_memory.disk;

        if (log_cb)
            log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_DISK_OPEN\n");
        printf("M64CMD_DISK_OPEN\n");

        if (CoreDoCommand(M64CMD_DISK_OPEN, disk_size, (void*)disk_data))
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "mupen64plus: Failed to load DISK\n");
            goto load_fail;
        }

        free(disk_data);
        disk_data = NULL;

        /* 64DD IPL LOAD - assumes "64DD_IPL.bin" is in system folder */
        sprintf(disk_ipl_path, "%s%c64DD_IPL.bin", dir, slash);


        fPtr = fopen(disk_ipl_path, "rb");
        if (fPtr == NULL)
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "mupen64plus: Failed to load DISK IPL\n");
            goto load_fail;
        }

        fseek(fPtr, 0L, SEEK_END);
        romlength = ftell(fPtr);
        fseek(fPtr, 0L, SEEK_SET);

        ipl_data = malloc(romlength);
        if (ipl_data == NULL)
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "mupen64plus: couldn't allocate DISK IPL buffer\n");
            fclose(fPtr);
            free(ipl_data);
            ipl_data = NULL;
            goto load_fail;
        }

        if (fread(ipl_data, 1, romlength, fPtr) != romlength)
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "mupen64plus: couldn't read DISK IPL file to buffer\n");
            fclose(fPtr);
            free(ipl_data);
            ipl_data = NULL;
            goto load_fail;
        }
        fclose(fPtr);

        if (log_cb)
            log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_DDROM_OPEN\n");
        printf("M64CMD_DDROM_OPEN\n");

        if (CoreDoCommand(M64CMD_DDROM_OPEN, romlength, (void*)ipl_data))
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "mupen64plus: Failed to load DDROM\n");
            free(ipl_data);
            ipl_data = NULL;
            goto load_fail;
        }

        if (log_cb)
            log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_ROM_GET_HEADER\n");

        if (CoreDoCommand(M64CMD_ROM_GET_HEADER, sizeof(ROM_HEADER), &ROM_HEADER))
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "mupen64plus; Failed to query ROM header information\n");
            goto load_fail;
        }
    }
    return loaded;

load_fail:
    free(cart_data);
    cart_data = NULL;
    free(disk_data);
    disk_data = NULL;
    stop = 1;

    return false;
}

void neil_invoke_linker()
{
    emu_step_load_data();
}

bool emu_step_render(void);

int retro_return(bool just_flipping)
{
    if (stop)
        return 0;

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
    vbo_disable();
#endif

#ifdef NO_LIBCO
    if (just_flipping)
    {
        /* HACK: in case the VI comes before the render? is that possible?
         * remove this when we totally remove libco */
        flip_only = 1;
        emu_step_render();
        flip_only = 0;
    }
    else
        flip_only = just_flipping;

    stop_stepping = true;
#else
    flip_only = just_flipping;
    co_switch(main_thread);
#endif

    return 0;
}

int retro_stop_stepping(void)
{
#ifdef NO_LIBCO
    return stop_stepping;
#else
    return false;
#endif
}

#ifdef HAVE_THR_AL
extern struct rgba prescale[PRESCALE_WIDTH * PRESCALE_HEIGHT];
#endif

int ready_to_swap = 0;

bool emu_step_render(void)
{
    if (flip_only)
    {
        switch (gfx_plugin)
        {
        case GFX_ANGRYLION:
#ifdef HAVE_THR_AL
            video_cb(prescale, screen_width, screen_height, screen_pitch);
#endif
            break;

        case GFX_PARALLEL:
#if defined(HAVE_PARALLEL)
            parallel_profile_video_refresh_begin();
            video_cb(parallel_frame_is_valid() ? RETRO_HW_FRAME_BUFFER_VALID : NULL,
                parallel_frame_width(), parallel_frame_height(), 0);
            parallel_profile_video_refresh_end();
#endif
            break;

        default:
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
            ready_to_swap = 1;
            //NEILTODO video output
            //video_cb(RETRO_HW_FRAME_BUFFER_VALID, screen_width, screen_height, 0);
#else
            video_cb((screen_pitch == 0) ? NULL : prescale, screen_width, screen_height, screen_pitch);
#endif
            break;
        }

        pushed_frame = true;
        return true;
    }

    //if (!pushed_frame && frame_dupe) /* Dupe. Not duping violates libretro API, consider it a speedhack. */
    //    video_cb(NULL, screen_width, screen_height, screen_pitch);

    return false;
}

int getReadyToSwap()
{
    return ready_to_swap;
}

void resetReadyToSwap()
{
    ready_to_swap = 0;
}

static void emu_step_initialize(void)
{
    if (emu_initialized)
        return;

    emu_initialized = true;

    core_settings_set_defaults();
    core_settings_autoselect_gfx_plugin();
    core_settings_autoselect_rsp_plugin();

    plugin_connect_all(gfx_plugin, rsp_plugin);

    if (log_cb)
        log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_EXECUTE.\n");

    CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
}

void reinit_gfx_plugin(void)
{
    if (first_context_reset)
    {
        first_context_reset = false;
#ifndef NO_LIBCO
        co_switch(game_thread);
#endif
    }

    switch (gfx_plugin)
    {
    case GFX_GLIDE64:
#ifdef HAVE_GLIDE64
        glide64InitGfx();
#endif
        break;
    case GFX_GLN64:
#if defined(HAVE_GLN64) || defined(HAVE_GLIDEN64)
        gles2n64_reset();
#endif
        break;
    case GFX_RICE:
#ifdef HAVE_RICE
        /* TODO/FIXME */
#endif
        break;
    case GFX_ANGRYLION:
        /* Stub */
        break;
    case GFX_PARALLEL:
#ifdef HAVE_PARALLEL
        if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &vulkan) || !vulkan)
        {
            if (log_cb)
                log_cb(RETRO_LOG_ERROR, "Failed to obtain Vulkan interface.\n");
        }
        else
            parallel_init(vulkan);
#endif
        break;
    }
}

void deinit_gfx_plugin(void)
{
    switch (gfx_plugin)
    {
    case GFX_PARALLEL:
#if defined(HAVE_PARALLEL)
        parallel_deinit();
#endif
        break;
    default:
        break;
    }
}

static void EmuThreadInit(void)
{
    emu_step_initialize();

    initializing = false;

    main_pre_run();
}

static void EmuThreadStep(void)
{
    stop_stepping = false;
    main_run();
}

/* Get the system type associated to a ROM country code. */
static m64p_system_type rom_country_code_to_system_type(char country_code)
{
    switch (country_code)
    {
        /* PAL codes */
    case 0x44:
    case 0x46:
    case 0x49:
    case 0x50:
    case 0x53:
    case 0x55:
    case 0x58:
    case 0x59:
        return SYSTEM_PAL;

        /* NTSC codes */
    case 0x37:
    case 0x41:
    case 0x45:
    case 0x4a:
    default: /* Fallback for unknown codes */
        return SYSTEM_NTSC;
    }
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
    m64p_system_type region = rom_country_code_to_system_type(ROM_HEADER.destination_code);

    info->geometry.base_width = screen_width;
    info->geometry.base_height = screen_height;
    info->geometry.max_width = screen_width;
    info->geometry.max_height = screen_height;
    info->geometry.aspect_ratio = 4.0 / 3.0;
    info->timing.fps = (region == SYSTEM_PAL) ? 50.0 : (60.13);                /* TODO: Actual timing  */
    info->timing.sample_rate = 44100.0;
}

static void context_reset(void)
{
    switch (gfx_plugin)
    {
    case GFX_ANGRYLION:
    case GFX_PARALLEL:
        break;
    default:
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
    {
        static bool first_init = true;
        printf("context_reset.\n");
        glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);

        if (first_init)
        {
            glsm_ctl(GLSM_CTL_STATE_SETUP, NULL);
            first_init = false;
        }
    }
#endif
    break;
    }

    reinit_gfx_plugin();
}

static void context_destroy(void)
{
    deinit_gfx_plugin();
}

static bool context_framebuffer_lock(void* data)
{
    if (!stop)
        return false;
    return true;
}

static bool retro_init_gl(void)
{
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
    glsm_ctx_params_t params = { 0 };

    params.context_reset = context_reset;
    params.context_destroy = context_destroy;
    //params.environ_cb = environ_cb;
    params.stencil = false;

    params.framebuffer_lock = context_framebuffer_lock;

    if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
    {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "mupen64plus: libretro frontend doesn't have OpenGL support.\n");
        return false;
    }

    context_reset();

    return true;
#else
    return false;
#endif
}

void retro_init(void)
{
    struct retro_log_callback log;
    unsigned colorMode = RETRO_PIXEL_FORMAT_XRGB8888;
    uint64_t serialization_quirks = RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE;

    screen_pitch = 0;

    //if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
    //    log_cb = log.log;
    //else
    //    log_cb = NULL;

    //if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
    //    perf_get_cpu_features_cb = perf_cb.get_cpu_features;
    //else
    //    perf_get_cpu_features_cb = NULL;

    //environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &colorMode);
    //environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);

    //environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &serialization_quirks);
    initializing = true;

    /* hacky stuff for Glide64 */
    polygonOffsetUnits = -3.0f;
    polygonOffsetFactor = -3.0f;

#ifndef NO_LIBCO
    main_thread = co_active();
    game_thread = co_create(65536 * sizeof(void*) * 16, EmuThreadFunction);
#endif

}

void retro_deinit(void)
{
    mupen_main_stop();
    mupen_main_exit();

#ifndef NO_LIBCO
    co_delete(game_thread);
#endif

    deinit_audio_libretro();

    //if (perf_cb.perf_log)
    //    perf_cb.perf_log();

    vulkan_inited = false;
    gl_inited = false;
}

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
extern void glide_set_filtering(unsigned value);
#endif
//extern void angrylion_set_vi(unsigned value);
//extern void angrylion_set_filtering(unsigned value);
//extern void angrylion_set_dithering(unsigned value);
//extern void  angrylion_set_threads(unsigned value);
//extern void  angrylion_set_overscan(unsigned value);
//extern void  angrylion_set_vi_dedither(unsigned value);
//extern void  angrylion_set_vi_blur(unsigned value);
//extern void angrylion_set_synclevel(unsigned value);

extern void ChangeSize();

static void gfx_set_filtering(void)
{
    if (log_cb)
        log_cb(RETRO_LOG_DEBUG, "set filtering mode...\n");
    switch (gfx_plugin)
    {
    case GFX_GLIDE64:
#ifdef HAVE_GLIDE64
        glide_set_filtering(retro_filtering);
#endif
        break;
    case GFX_ANGRYLION:
#ifdef HAVE_THR_AL
        angrylion_set_filtering(retro_filtering);
#endif
        break;
    case GFX_RICE:
#ifdef HAVE_RICE
        /* TODO/FIXME */
#endif
        break;
    case GFX_PARALLEL:
#ifdef HAVE_PARALLEL
        /* Stub */
#endif
        break;
    case GFX_GLN64:
#if defined(HAVE_GLN64) || defined(HAVE_GLIDEN64)
        /* Stub */
#endif
        break;
    }
}

unsigned setting_get_dithering(void)
{
    return retro_dithering;
}

static void gfx_set_dithering(void)
{
    if (log_cb)
        log_cb(RETRO_LOG_DEBUG, "set dithering mode...\n");

    switch (gfx_plugin)
    {
    case GFX_GLIDE64:
#ifdef HAVE_GLIDE64
        /* Stub */
#endif
        break;
    case GFX_ANGRYLION:
#ifdef HAVE_THR_AL
        angrylion_set_vi_dedither(!retro_dithering);
        angrylion_set_dithering(retro_dithering);
#endif
        break;
    case GFX_RICE:
#ifdef HAVE_RICE
        /* Stub */
#endif
        break;
    case GFX_PARALLEL:
        break;
    case GFX_GLN64:
#if defined(HAVE_GLN64) || defined(HAVE_GLIDEN64)
        /* Stub */
#endif
        break;
    }
}

void update_variables(bool startup)
{
    struct retro_variable var;

    send_allist_to_hle_rsp = false;

    screen_width = 640;
    screen_height = 480;

    if (startup)
    {
        core_settings_autoselect_gfx_plugin();
    }


#ifdef HAVE_THR_AL
    var.key = "parallel-n64-angrylion-vioverlay";
    var.value = NULL;

    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);

    if (var.value)
    {
        if (!strcmp(var.value, "Filtered"))
        {
            angrylion_set_vi(0);
            angrylion_set_vi_dedither(1);
            angrylion_set_vi_blur(1);
        }
        else if (!strcmp(var.value, "AA+Blur"))
        {
            angrylion_set_vi(0);
            angrylion_set_vi_dedither(0);
            angrylion_set_vi_blur(1);
        }
        else if (!strcmp(var.value, "AA+Dedither"))
        {
            angrylion_set_vi(0);
            angrylion_set_vi_dedither(1);
            angrylion_set_vi_blur(0);
        }
        else if (!strcmp(var.value, "AA only"))
        {
            angrylion_set_vi(0);
            angrylion_set_vi_dedither(0);
            angrylion_set_vi_blur(0);
        }
        else if (!strcmp(var.value, "Unfiltered"))
        {
            angrylion_set_vi(1);
            angrylion_set_vi_dedither(1);
            angrylion_set_vi_blur(1);
        }
        else if (!strcmp(var.value, "Depth"))
        {
            angrylion_set_vi(2);
            angrylion_set_vi_dedither(1);
            angrylion_set_vi_blur(1);
        }
        else if (!strcmp(var.value, "Coverage"))
        {
            angrylion_set_vi(3);
            angrylion_set_vi_dedither(1);
            angrylion_set_vi_blur(1);
        }
    }
    else
    {
        angrylion_set_vi(0);
        angrylion_set_vi_dedither(1);
        angrylion_set_vi_blur(1);
    }

    var.key = "parallel-n64-angrylion-sync";
    var.value = NULL;

    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);

    if (var.value)
    {
        if (!strcmp(var.value, "High"))
            angrylion_set_synclevel(2);
        else if (!strcmp(var.value, "Medium"))
            angrylion_set_synclevel(1);
        else if (!strcmp(var.value, "Low"))
            angrylion_set_synclevel(0);
    }
    else
        angrylion_set_synclevel(0);

    var.key = "parallel-n64-angrylion-multithread";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "all threads"))
            angrylion_set_threads(0);
        else
            angrylion_set_threads(atoi(var.value));
    }
    else
        angrylion_set_threads(0);

    var.key = "parallel-n64-angrylion-overscan";
    var.value = NULL;

    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);

    if (var.value)
    {
        if (!strcmp(var.value, "enabled"))
            angrylion_set_overscan(1);
        else if (!strcmp(var.value, "disabled"))
            angrylion_set_overscan(0);
    }
    else
        angrylion_set_overscan(0);
#endif


    CFG_HLE_GFX = 0;

#ifdef HAVE_THR_AL
    if (gfx_plugin != GFX_ANGRYLION)
        CFG_HLE_GFX = 1;
#endif

    CFG_HLE_AUD = 0; /* There is no HLE audio code in libretro audio plugin. */

    var.key = "parallel-n64-filtering";
    var.value = NULL;


#ifdef DISABLE_3POINT
    retro_filtering = 3;
#else
    retro_filtering = 1;
#endif
    gfx_set_filtering();

    retro_dithering = 1;
    gfx_set_dithering();


    //astick_deadzone = (int)(atoi(var.value) * 0.01f * 0x8000);

    //astick_sensitivity = atoi(var.value);

    var.key = "parallel-n64-gfxplugin-accuracy";
    var.value = NULL;

    {
        /*if (var.value && !strcmp(var.value, "veryhigh"))
            gfx_plugin_accuracy = 3;
        else if (var.value && !strcmp(var.value, "high"))*/
            gfx_plugin_accuracy = 2;
        /*else if (var.value && !strcmp(var.value, "medium"))
            gfx_plugin_accuracy = 1;
        else if (var.value && !strcmp(var.value, "low"))
            gfx_plugin_accuracy = 0;*/
    }

    var.key = "parallel-n64-virefresh";
    var.value = NULL;


    var.key = "parallel-n64-bufferswap";
    var.value = NULL;

    BUFFERSWAP = false;

    var.key = "parallel-n64-framerate";
    var.value = NULL;

            frame_dupe = false;

    var.key = "parallel-n64-alt-map";
    var.value = NULL;

    alternate_mapping = false;

    int p1_pak = PLUGIN_MEMPAK;
    //p1_pak = PLUGIN_RAW;
    //p1_pak = PLUGIN_MEMPAK;

    if (controller[0].control)
        controller[0].control->Plugin = p1_pak;
    else
        pad_pak_types[0] = p1_pak;
    if (controller[1].control)
        controller[1].control->Plugin = p1_pak;
    else
        pad_pak_types[1] = p1_pak;
    if (controller[2].control)
        controller[2].control->Plugin = p1_pak;
    else
        pad_pak_types[2] = p1_pak;
    if (controller[3].control)
        controller[3].control->Plugin = p1_pak;
    else
        pad_pak_types[3] = p1_pak;

}

char* loadFile(char* filename, int* size)
{
    FILE* f = fopen(filename, "rb");
    fseek(f, 0, SEEK_END);
    int fsize = ftell(f);
    fseek(f, 0, SEEK_SET);  /* same as rewind(f); */

    char* filecontent = (char*)malloc(fsize);
    int readsize = fread(filecontent, 1, fsize, f);
    fclose(f);

    *size = readsize;

    //filecontent[readsize] = '\0';

    return filecontent;
}

static void format_saved_memory(void)
{
    format_sram(saved_memory.sram);
    format_eeprom(saved_memory.eeprom, sizeof(saved_memory.eeprom));

    //EXAMPLE OF LOADING SAVE
    //int size = 0;
    //char* eep = loadFile("diddy.eep",&size);
    //memcpy(saved_memory.eeprom, eep, size);


    format_flashram(saved_memory.flashram);
    format_mempak(saved_memory.mempack[0]);
    format_mempak(saved_memory.mempack[1]);
    format_mempak(saved_memory.mempack[2]);
    format_mempak(saved_memory.mempack[3]);
    format_disk(saved_memory.disk);
}

bool retro_load_game_new(uint8_t* romdata, int size)
{
    format_saved_memory();

    update_variables(true);
    initial_boot = false;

    init_audio_libretro(audio_buffer_size);

#ifdef HAVE_THR_AL
    if (gfx_plugin != GFX_ANGRYLION)
#endif
    {
        //if (gfx_plugin == GFX_PARALLEL)
        //{
        //    retro_init_vulkan();
        //    vulkan_inited = true;
        //}
        //else
        {
            retro_init_gl();
            gl_inited = true;
        }
    }

    if (gl_inited)
    {
        switch (gfx_plugin)
        {
        case GFX_PARALLEL:
            gfx_plugin = GFX_GLIDE64;
            break;
        default:
            break;
        }

        switch (rsp_plugin)
        {
        case RSP_PARALLEL:
            rsp_plugin = RSP_HLE;
        default:
            break;
        }
    }

    if (is_cartridge_rom(romdata))
    {
        cart_data = malloc(size);
        cart_size = size;
        memcpy(cart_data, romdata, size);
    }
    else
    {
        disk_data = malloc(size);
        disk_size = size;
        memcpy(disk_data, romdata, size);
    }

    stop = false;
    /* Finish ROM load before doing anything funny,
     * so we can return failure if needed. */
#ifdef NO_LIBCO
    emu_step_load_data();
#else
    co_switch(game_thread);
#endif

    if (stop)
        return false;

    first_context_reset = true;

    return true;
}


#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
static void glsm_exit(void)
{
#ifndef HAVE_SHARED_CONTEXT
    if (stop)
        return;
#ifdef HAVE_THR_AL
    if (gfx_plugin == GFX_ANGRYLION)
        return;
#endif
#ifdef HAVE_PARALLEL
    if (gfx_plugin == GFX_PARALLEL)
        return;
#endif
    glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
#endif
}

static void glsm_enter(void)
{
#ifndef HAVE_SHARED_CONTEXT
    if (stop)
        return;
#ifdef HAVE_THR_AL
    if (gfx_plugin == GFX_ANGRYLION)
        return;
#endif
#ifdef HAVE_PARALLEL
    if (gfx_plugin == GFX_PARALLEL)
        return;
#endif
    glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
#endif
}
#endif

void retro_run(void)
{
    static bool updated = false;

    FAKE_SDL_TICKS += 16;
    pushed_frame = false;

    //do
    {
        switch (gfx_plugin)
        {
        case GFX_GLIDE64:
        case GFX_GLN64:
        case GFX_RICE:
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
            glsm_enter();
#endif
            break;
        case GFX_PARALLEL:
#if defined(HAVE_PARALLEL)
            parallel_begin_frame();
#endif
            break;
        case GFX_ANGRYLION:
            break;
        }

        if (first_time)
        {
            first_time = 0;
            emu_step_initialize();
            /* Additional check for vioverlay not set at start */
            update_variables(false);
            gfx_set_filtering();
#ifdef NO_LIBCO
            EmuThreadInit();
#endif
        }

#ifdef NO_LIBCO
        EmuThreadStep();
#else
        co_switch(game_thread);
#endif

        switch (gfx_plugin)
        {
        case GFX_GLIDE64:
        case GFX_GLN64:
        case GFX_RICE:
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
            glsm_exit();
#endif
            break;
        case GFX_PARALLEL:
        case GFX_ANGRYLION:
            break;
        }
    } 
    //while (emu_step_render());
}

unsigned char savestate_buffer[16788288 + 1024];

int savestates_load_m64p(const unsigned char *data, size_t size);
int savestates_save_m64p(unsigned char *data, size_t size);

// void write_save_state_file()
// {
//     FILE* f = fopen("savestate.sav", "wb");
//     fwrite(&savestate_buffer, sizeof(unsigned char), sizeof(savestate_buffer), f);
//     fclose(f);
// }

#ifdef __EMSCRIPTEN__
#include <zlib.h>
#else
#include "../../zlib/zlib.h"
#endif


bool neil_serialize()
{
    printf("save state\n");
    if (savestates_save_m64p(&savestate_buffer, sizeof(savestate_buffer)))
    {
        gzFile* fi = (gzFile*)gzopen("savestate.gz", "wb");
        gzwrite(fi, &savestate_buffer, sizeof(savestate_buffer));
        gzclose(fi);

        //write_save_state_file();
#ifdef __EMSCRIPTEN__
        EM_ASM(
               myApp.SaveStateEvent();
        );
#endif
        return true;
    }



    return false;
}

bool neil_unserialize()
{
    printf("load state\n");
    gzFile* fi = (gzFile*)gzopen("savestate.gz", "rb");
    gzread(fi, &savestate_buffer, sizeof(savestate_buffer));
    gzclose(fi);

    if (savestates_load_m64p(&savestate_buffer, sizeof(savestate_buffer)))
        return true;

    return false;
}


