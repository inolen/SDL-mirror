/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2020 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_KMSDRM

/* SDL internals */
#include "../SDL_sysvideo.h"
#include "SDL_syswm.h"
#include "SDL_log.h"
#include "SDL_hints.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_keyboard_c.h"

#ifdef SDL_INPUT_LINUXEV
#include "../../core/linux/SDL_evdev.h"
#endif

/* KMS/DRM declarations */
#include "SDL_kmsdrmvideo.h"
#include "SDL_kmsdrmevents.h"
#include "SDL_kmsdrmopengles.h"
#include "SDL_kmsdrmmouse.h"
#include "SDL_kmsdrmdyn.h"
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>

#define KMSDRM_DRI_PATH "/dev/dri/"

static int
check_modestting(int devindex)
{
    SDL_bool available = SDL_FALSE;
    char device[512];
    int drm_fd;

    SDL_snprintf(device, sizeof (device), "%scard%d", KMSDRM_DRI_PATH, devindex);

    drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (drm_fd >= 0) {
        if (SDL_KMSDRM_LoadSymbols()) {
            drmModeRes *resources = KMSDRM_drmModeGetResources(drm_fd);
            if (resources) {
                SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "%scard%d connector, encoder and CRTC counts are: %d %d %d",
                             KMSDRM_DRI_PATH, devindex,
                             resources->count_connectors, resources->count_encoders, resources->count_crtcs);

                if (resources->count_connectors > 0 && resources->count_encoders > 0 && resources->count_crtcs > 0) {
                    available = SDL_TRUE;
                }
                KMSDRM_drmModeFreeResources(resources);
            }
            SDL_KMSDRM_UnloadSymbols();
        }
        close(drm_fd);
    }

    return available;
}

static int get_dricount(void)
{
    int devcount = 0;
    struct dirent *res;
    struct stat sb;
    DIR *folder;

    if (!(stat(KMSDRM_DRI_PATH, &sb) == 0
                && S_ISDIR(sb.st_mode))) {
        printf("The path %s cannot be opened or is not available\n",
               KMSDRM_DRI_PATH);
        return 0;
    }

    if (access(KMSDRM_DRI_PATH, F_OK) == -1) {
        printf("The path %s cannot be opened\n",
               KMSDRM_DRI_PATH);
        return 0;
    }

    folder = opendir(KMSDRM_DRI_PATH);
    if (folder) {
        while ((res = readdir(folder))) {
            int len = SDL_strlen(res->d_name);
            if (len > 4 && SDL_strncmp(res->d_name, "card", 4) == 0) {
                devcount++;
            }
        }
        closedir(folder);
    }

    return devcount;
}

static int
get_driindex(void)
{
    const int devcount = get_dricount();
    int i;

    for (i = 0; i < devcount; i++) {
        if (check_modestting(i)) {
            return i;
        }
    }

    return -ENOENT;
}

static int
KMSDRM_Available(void)
{
    int ret = -ENOENT;

    ret = get_driindex();
    if (ret >= 0)
        return 1;

    return ret;
}

static void
KMSDRM_DeleteDevice(SDL_VideoDevice * device)
{
    if (device->driverdata) {
        SDL_free(device->driverdata);
        device->driverdata = NULL;
    }

    SDL_free(device);

    SDL_KMSDRM_UnloadSymbols();
}

static SDL_VideoDevice *
KMSDRM_CreateDevice(int devindex)
{
    SDL_VideoDevice *device;
    SDL_VideoData *viddata;

    if (!devindex || (devindex > 99)) {
        devindex = get_driindex();
    }

    if (devindex < 0) {
        SDL_SetError("devindex (%d) must be between 0 and 99.\n", devindex);
        return NULL;
    }

    if (!SDL_KMSDRM_LoadSymbols()) {
        return NULL;
    }

    device = (SDL_VideoDevice *) SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }

    viddata = (SDL_VideoData *) SDL_calloc(1, sizeof(SDL_VideoData));
    if (!viddata) {
        SDL_OutOfMemory();
        goto cleanup;
    }
    viddata->devindex = devindex;
    viddata->drm_fd = -1;

    device->driverdata = viddata;

    /* Setup all functions which we can handle */
    device->VideoInit = KMSDRM_VideoInit;
    device->VideoQuit = KMSDRM_VideoQuit;
    device->GetDisplayModes = KMSDRM_GetDisplayModes;
    device->SetDisplayMode = KMSDRM_SetDisplayMode;
    device->CreateSDLWindow = KMSDRM_CreateWindow;
    device->CreateSDLWindowFrom = KMSDRM_CreateWindowFrom;
    device->SetWindowTitle = KMSDRM_SetWindowTitle;
    device->SetWindowIcon = KMSDRM_SetWindowIcon;
    device->SetWindowPosition = KMSDRM_SetWindowPosition;
    device->SetWindowSize = KMSDRM_SetWindowSize;
    device->ShowWindow = KMSDRM_ShowWindow;
    device->HideWindow = KMSDRM_HideWindow;
    device->RaiseWindow = KMSDRM_RaiseWindow;
    device->MaximizeWindow = KMSDRM_MaximizeWindow;
    device->MinimizeWindow = KMSDRM_MinimizeWindow;
    device->RestoreWindow = KMSDRM_RestoreWindow;
    device->SetWindowGrab = KMSDRM_SetWindowGrab;
    device->DestroyWindow = KMSDRM_DestroyWindow;
    device->GetWindowWMInfo = KMSDRM_GetWindowWMInfo;
#if SDL_VIDEO_OPENGL_EGL
    device->GL_LoadLibrary = KMSDRM_GLES_LoadLibrary;
    device->GL_GetProcAddress = KMSDRM_GLES_GetProcAddress;
    device->GL_UnloadLibrary = KMSDRM_GLES_UnloadLibrary;
    device->GL_CreateContext = KMSDRM_GLES_CreateContext;
    device->GL_MakeCurrent = KMSDRM_GLES_MakeCurrent;
    device->GL_SetSwapInterval = KMSDRM_GLES_SetSwapInterval;
    device->GL_GetSwapInterval = KMSDRM_GLES_GetSwapInterval;
    device->GL_SwapWindow = KMSDRM_GLES_SwapWindow;
    device->GL_DeleteContext = KMSDRM_GLES_DeleteContext;
#endif
    device->PumpEvents = KMSDRM_PumpEvents;
    device->free = KMSDRM_DeleteDevice;

    return device;

cleanup:
    if (device)
        SDL_free(device);
    if (viddata)
        SDL_free(viddata);
    return NULL;
}

VideoBootStrap KMSDRM_bootstrap = {
    "KMSDRM",
    "KMS/DRM Video Driver",
    KMSDRM_Available,
    KMSDRM_CreateDevice
};


static void
KMSDRM_FBDestroyCallback(struct gbm_bo *bo, void *data)
{
    KMSDRM_FBInfo *fb_info = (KMSDRM_FBInfo *)data;

    if (fb_info && fb_info->drm_fd >= 0 && fb_info->fb_id != 0) {
        KMSDRM_drmModeRmFB(fb_info->drm_fd, fb_info->fb_id);
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Delete DRM FB %u", fb_info->fb_id);
    }

    SDL_free(fb_info);
}

KMSDRM_FBInfo *
KMSDRM_FBFromBO(_THIS, struct gbm_bo *bo)
{
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);

    /* Check for an existing framebuffer */
    KMSDRM_FBInfo *fb_info = (KMSDRM_FBInfo *)KMSDRM_gbm_bo_get_user_data(bo);

    if (fb_info) {
        return fb_info;
    }

    /* Create a structure that contains enough info to remove the framebuffer
       when the backing buffer is destroyed */
    fb_info = (KMSDRM_FBInfo *)SDL_calloc(1, sizeof(KMSDRM_FBInfo));

    if (!fb_info) {
        SDL_OutOfMemory();
        return NULL;
    }

    fb_info->drm_fd = viddata->drm_fd;

    /* Create framebuffer object for the buffer */
    unsigned w = KMSDRM_gbm_bo_get_width(bo);
    unsigned h = KMSDRM_gbm_bo_get_height(bo);
    Uint32 stride = KMSDRM_gbm_bo_get_stride(bo);
    Uint32 handle = KMSDRM_gbm_bo_get_handle(bo).u32;
    int ret = KMSDRM_drmModeAddFB(viddata->drm_fd, w, h, 24, 32, stride, handle,
                                  &fb_info->fb_id);
    if (ret) {
      SDL_free(fb_info);
      return NULL;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "New DRM FB (%u): %ux%u, stride %u from BO %p",
                 fb_info->fb_id, w, h, stride, (void *)bo);

    /* Associate our DRM framebuffer with this buffer object */
    KMSDRM_gbm_bo_set_user_data(bo, fb_info, KMSDRM_FBDestroyCallback);

    return fb_info;
}

static void
KMSDRM_FlipHandler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    *((SDL_bool *) data) = SDL_FALSE;
}

SDL_bool
KMSDRM_WaitPageFlip(_THIS, SDL_WindowData *windata, int timeout) {
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);

    drmEventContext ev = {0};
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = KMSDRM_FlipHandler;

    struct pollfd pfd = {0};
    pfd.fd = viddata->drm_fd;
    pfd.events = POLLIN;

    while (windata->waiting_for_flip) {
        pfd.revents = 0;

        if (poll(&pfd, 1, timeout) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "DRM poll error");
            return SDL_FALSE;
        }

        if (pfd.revents & (POLLHUP | POLLERR)) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "DRM poll hup or error");
            return SDL_FALSE;
        }

        if (pfd.revents & POLLIN) {
            /* Page flip? If so, drmHandleEvent will unset windata->waiting_for_flip */
            KMSDRM_drmHandleEvent(viddata->drm_fd, &ev);
        } else {
            /* Timed out and page flip didn't happen */
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Dropping frame while waiting_for_flip");
            return SDL_FALSE;
        }
    }

    return SDL_TRUE;
}

/*****************************************************************************/
/* SDL Video and Display initialization/handling functions                   */
/* _this is a SDL_VideoDevice *                                              */
/*****************************************************************************/
int
KMSDRM_VideoInit(_THIS)
{
    int i, j;
    SDL_bool found;
    int ret = 0;
    char *devname;
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);
    drmModeRes *resources = NULL;
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    SDL_DisplayMode current_mode;
    SDL_VideoDisplay display;

    /* Allocate display internal data */
    SDL_DisplayData *dispdata = (SDL_DisplayData *) SDL_calloc(1, sizeof(SDL_DisplayData));
    if (!dispdata) {
        return SDL_OutOfMemory();
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "KMSDRM_VideoInit()");

    /* Open /dev/dri/cardNN */
    devname = (char *) SDL_calloc(1, 16);
    if (!devname) {
        ret = SDL_OutOfMemory();
        goto cleanup;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Opening device /dev/dri/card%d", viddata->devindex);
    SDL_snprintf(devname, 16, "/dev/dri/card%d", viddata->devindex);
    viddata->drm_fd = open(devname, O_RDWR | O_CLOEXEC);
    SDL_free(devname);

    if (viddata->drm_fd < 0) {
        ret = SDL_SetError("Could not open /dev/dri/card%d.", viddata->devindex);
        goto cleanup;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Opened DRM FD (%d)", viddata->drm_fd);

    viddata->gbm = KMSDRM_gbm_create_device(viddata->drm_fd);
    if (!viddata->gbm) {
        ret = SDL_SetError("Couldn't create gbm device.");
        goto cleanup;
    }

    /* Find the first available connector with modes */
    resources = KMSDRM_drmModeGetResources(viddata->drm_fd);
    if (!resources) {
        ret = SDL_SetError("drmModeGetResources(%d) failed", viddata->drm_fd);
        goto cleanup;
    }

    for (i = 0; i < resources->count_connectors; i++) {
        connector = KMSDRM_drmModeGetConnector(viddata->drm_fd, resources->connectors[i]);
        if (!connector)
            continue;

        if (connector->connection == DRM_MODE_CONNECTED &&
            connector->count_modes > 0) {
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Found connector %d with %d modes.",
                         connector->connector_id, connector->count_modes);
            dispdata->conn_id = connector->connector_id;
            break;
        }

        KMSDRM_drmModeFreeConnector(connector);
        connector = NULL;
    }

    if (i == resources->count_connectors) {
        ret = SDL_SetError("No currently active connector found.");
        goto cleanup;
    }

    found = SDL_FALSE;

    for (i = 0; i < resources->count_encoders; i++) {
        encoder = KMSDRM_drmModeGetEncoder(viddata->drm_fd, resources->encoders[i]);

        if (!encoder)
            continue;

        if (encoder->encoder_id == connector->encoder_id) {
            found = SDL_TRUE;
        } else {
            for (j = 0; j < connector->count_encoders; j++) {
                if (connector->encoders[j] == encoder->encoder_id) {
                    found = SDL_TRUE;
                    break;
                }
            }
        }

        if (found == SDL_TRUE) {
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Found encoder %d.", encoder->encoder_id);
            break;
        }

        KMSDRM_drmModeFreeEncoder(encoder);
        encoder = NULL;
    }

    if (i == resources->count_encoders) {
        ret = SDL_SetError("No connected encoder found.");
        goto cleanup;
    }

    dispdata->saved_crtc = KMSDRM_drmModeGetCrtc(viddata->drm_fd, encoder->crtc_id);

    if (!dispdata->saved_crtc) {
        for (i = 0; i < resources->count_crtcs; i++) {
            if (encoder->possible_crtcs & (1 << i)) {
                encoder->crtc_id = resources->crtcs[i];
                SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Set encoder's CRTC to %d.", encoder->crtc_id);
                dispdata->saved_crtc = KMSDRM_drmModeGetCrtc(viddata->drm_fd, encoder->crtc_id);
                break;
            }
        }
    }

    if (!dispdata->saved_crtc) {
        ret = SDL_SetError("No CRTC found.");
        goto cleanup;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Saved crtc_id %u, fb_id %u, (%u,%u), %ux%u",
                 dispdata->saved_crtc->crtc_id, dispdata->saved_crtc->buffer_id, dispdata->saved_crtc->x,
                 dispdata->saved_crtc->y, dispdata->saved_crtc->width, dispdata->saved_crtc->height);
    dispdata->crtc_id = encoder->crtc_id;
    dispdata->mode = dispdata->saved_crtc->mode;

    // select default mode if this one is not valid
    if (dispdata->saved_crtc->mode_valid == 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO,
            "Current mode is invalid, selecting connector's mode #0.");
        dispdata->mode = connector->modes[0];
    }

    SDL_zero(current_mode);

    current_mode.w = dispdata->mode.hdisplay;
    current_mode.h = dispdata->mode.vdisplay;
    current_mode.refresh_rate = dispdata->mode.vrefresh;

    /* FIXME ?
    drmModeFB *fb = drmModeGetFB(viddata->drm_fd, dispdata->saved_crtc->buffer_id);
    current_mode.format = drmToSDLPixelFormat(fb->bpp, fb->depth);
    drmModeFreeFB(fb);
    */
    current_mode.format = SDL_PIXELFORMAT_ARGB8888;

    current_mode.driverdata = NULL;

    SDL_zero(display);
    display.desktop_mode = current_mode;
    display.current_mode = current_mode;

    display.driverdata = dispdata;
    /* SDL_VideoQuit will later SDL_free(display.driverdata) */
    SDL_AddVideoDisplay(&display);

#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Init();
#endif

    KMSDRM_InitMouse(_this);

    return ret;

cleanup:
    if (encoder)
        KMSDRM_drmModeFreeEncoder(encoder);
    if (connector)
        KMSDRM_drmModeFreeConnector(connector);
    if (resources)
        KMSDRM_drmModeFreeResources(resources);

    if (ret != 0) {
        /* Error (complete) cleanup */
        if (dispdata->saved_crtc) {
            KMSDRM_drmModeFreeCrtc(dispdata->saved_crtc);
            dispdata->saved_crtc = NULL;
        }
        if (viddata->gbm) {
            KMSDRM_gbm_device_destroy(viddata->gbm);
            viddata->gbm = NULL;
        }
        if (viddata->drm_fd >= 0) {
            close(viddata->drm_fd);
            viddata->drm_fd = -1;
        }
        SDL_free(dispdata);
    }
    return ret;
}

void
KMSDRM_VideoQuit(_THIS)
{
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayDriverData(0);

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "KMSDRM_VideoQuit()");

    if (_this->gl_config.driver_loaded) {
        SDL_GL_UnloadLibrary();
    }

    if (dispdata->saved_crtc) {
        if (viddata->drm_fd >= 0 && dispdata->conn_id > 0) {
            /* Restore saved CRTC settings */
            drmModeCrtc *crtc = dispdata->saved_crtc;
            if(KMSDRM_drmModeSetCrtc(viddata->drm_fd, crtc->crtc_id, crtc->buffer_id,
                                     crtc->x, crtc->y, &dispdata->conn_id, 1,
                                     &crtc->mode) != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not restore original CRTC mode");
            }
        }
        KMSDRM_drmModeFreeCrtc(dispdata->saved_crtc);
        dispdata->saved_crtc = NULL;
    }
    if (viddata->gbm) {
        KMSDRM_gbm_device_destroy(viddata->gbm);
        viddata->gbm = NULL;
    }
    if (viddata->drm_fd >= 0) {
        close(viddata->drm_fd);
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Closed DRM FD %d", viddata->drm_fd);
        viddata->drm_fd = -1;
    }
#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Quit();
#endif
}

void
KMSDRM_GetDisplayModes(_THIS, SDL_VideoDisplay * display)
{
    /* Only one display mode available, the current one */
    SDL_AddDisplayMode(display, &display->current_mode);
}

int
KMSDRM_SetDisplayMode(_THIS, SDL_VideoDisplay * display, SDL_DisplayMode * mode)
{
    return 0;
}

int
KMSDRM_CreateWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *windata;
    SDL_VideoDisplay *display;
    SDL_VideoData *viddata = ((SDL_VideoData *)_this->driverdata);
    Uint32 surface_fmt, surface_flags;

    /* Allocate window internal data */
    windata = (SDL_WindowData *) SDL_calloc(1, sizeof(SDL_WindowData));
    if (!windata) {
        SDL_OutOfMemory();
        goto error;
    }

    display = SDL_GetDisplayForWindow(window);

    /* Windows have one size for now */
    window->w = display->desktop_mode.w;
    window->h = display->desktop_mode.h;

    /* Maybe you didn't ask for a fullscreen OpenGL window, but that's what you get */
    window->flags |= (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);

    surface_fmt = GBM_FORMAT_XRGB8888;
    surface_flags = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;

    if (!KMSDRM_gbm_device_is_format_supported(viddata->gbm, surface_fmt, surface_flags)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "GBM surface format not supported. Trying anyway.");
    }
    windata->gs = KMSDRM_gbm_surface_create(viddata->gbm, window->w, window->h, surface_fmt, surface_flags);

#if SDL_VIDEO_OPENGL_EGL
    if (!_this->egl_data) {
        if (SDL_GL_LoadLibrary(NULL) < 0) {
            goto error;
        }
    }
    SDL_EGL_SetRequiredVisualId(_this, surface_fmt);
    windata->egl_surface = SDL_EGL_CreateSurface(_this, (NativeWindowType) windata->gs);

    if (windata->egl_surface == EGL_NO_SURFACE) {
        SDL_SetError("Could not create EGL window surface");
        goto error;
    }
#endif /* SDL_VIDEO_OPENGL_EGL */

    /* In case we want low-latency, double-buffer video, we take note here */
    windata->double_buffer = SDL_FALSE;
    if (SDL_GetHintBoolean(SDL_HINT_VIDEO_DOUBLE_BUFFER, SDL_FALSE)) {
        windata->double_buffer = SDL_TRUE;
    }

    /* Setup driver data for this window */
    window->driverdata = windata;

    /* One window, it always has focus */
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);

    /* Window has been successfully created */
    return 0;

error:
    if (windata) {
#if SDL_VIDEO_OPENGL_EGL
        if (windata->egl_surface != EGL_NO_SURFACE)
            SDL_EGL_DestroySurface(_this, windata->egl_surface);
#endif /* SDL_VIDEO_OPENGL_EGL */
        if (windata->gs)
            KMSDRM_gbm_surface_destroy(windata->gs);
        SDL_free(windata);
    }
    return -1;
}

void
KMSDRM_DestroyWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *windata = (SDL_WindowData *) window->driverdata;
    if(windata) {
        /* Wait for any pending page flips and unlock buffer */
        KMSDRM_WaitPageFlip(_this, windata, -1);
        if (windata->curr_bo) {
            KMSDRM_gbm_surface_release_buffer(windata->gs, windata->curr_bo);
            windata->curr_bo = NULL;
        }
        if (windata->next_bo) {
            KMSDRM_gbm_surface_release_buffer(windata->gs, windata->next_bo);
            windata->next_bo = NULL;
        }
#if SDL_VIDEO_OPENGL_EGL
        SDL_EGL_MakeCurrent(_this, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (windata->egl_surface != EGL_NO_SURFACE) {
            SDL_EGL_DestroySurface(_this, windata->egl_surface);
        }
#endif /* SDL_VIDEO_OPENGL_EGL */
        if (windata->gs) {
            KMSDRM_gbm_surface_destroy(windata->gs);
            windata->gs = NULL;
        }
        SDL_free(windata);
        window->driverdata = NULL;
    }
}

int
KMSDRM_CreateWindowFrom(_THIS, SDL_Window * window, const void *data)
{
    return -1;
}

void
KMSDRM_SetWindowTitle(_THIS, SDL_Window * window)
{
}
void
KMSDRM_SetWindowIcon(_THIS, SDL_Window * window, SDL_Surface * icon)
{
}
void
KMSDRM_SetWindowPosition(_THIS, SDL_Window * window)
{
}
void
KMSDRM_SetWindowSize(_THIS, SDL_Window * window)
{
}
void
KMSDRM_ShowWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_HideWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_RaiseWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_MaximizeWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_MinimizeWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_RestoreWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_SetWindowGrab(_THIS, SDL_Window * window, SDL_bool grabbed)
{

}

/*****************************************************************************/
/* SDL Window Manager function                                               */
/*****************************************************************************/
SDL_bool
KMSDRM_GetWindowWMInfo(_THIS, SDL_Window * window, struct SDL_SysWMinfo *info)
{
    if (info->version.major <= SDL_MAJOR_VERSION) {
        return SDL_TRUE;
    } else {
        SDL_SetError("application not compiled with SDL %d.%d\n",
                     SDL_MAJOR_VERSION, SDL_MINOR_VERSION);
        return SDL_FALSE;
    }

    /* Failed to get window manager information */
    return SDL_FALSE;
}

#endif /* SDL_VIDEO_DRIVER_KMSDRM */

/* vi: set ts=4 sw=4 expandtab: */
