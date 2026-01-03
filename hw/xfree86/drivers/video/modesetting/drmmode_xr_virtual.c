/*
 * Copyright © 2024
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/Xatom.h>
#include <xf86drm.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>  /* for strcasestr */
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include "xf86.h"
#include "xf86str.h"
#include "xf86Crtc.h"
#include "xf86RandR12.h"
#include "randrstr.h"
#include "randr/randrstr_priv.h"
#include "xf86Modes.h"
#include "xf86DDC.h"
#include "drmmode_display.h"
#include "driver.h"
#ifdef GLAMOR_HAS_GBM
#define GLAMOR_FOR_XORG 1
#include "glamor.h"
#include <gbm.h>
#endif

/* Forward declarations - these functions/objects are in drmmode_display.c */
extern void drmmode_output_create_resources(xf86OutputPtr output);
extern const xf86OutputFuncsRec drmmode_output_funcs;
extern const xf86CrtcFuncsRec drmmode_crtc_funcs;

/* Functions we need are declared in drmmode_display.h */

#define XR_MANAGER_OUTPUT_NAME "XR-Manager"
#define CREATE_XR_OUTPUT_PROPERTY "CREATE_XR_OUTPUT"
#define DELETE_XR_OUTPUT_PROPERTY "DELETE_XR_OUTPUT"
#define XR_WIDTH_PROPERTY "XR_WIDTH" // TODO - used?
#define XR_HEIGHT_PROPERTY "XR_HEIGHT" // TODO - used?
#define XR_REFRESH_PROPERTY "XR_REFRESH" // TODO - used?
#define XR_FB_ID_PROPERTY "FRAMEBUFFER_ID"
#define XR_MODES_PROPERTY "XR_MODES"
#define XR_VIRTUAL_OUTPUT_PROPERTY "VIRTUAL_OUTPUT"

/* Structure to store a display mode */
typedef struct _xr_mode_rec {
    int width;
    int height;
    int refresh;
    struct _xr_mode_rec *next;
} xr_mode_rec, *xr_mode_ptr;

/* Structure to track a dynamically created virtual output */
typedef struct _xr_virtual_output_rec {
    xf86OutputPtr output;          /* xf86OutputPtr for this virtual output */
    xf86CrtcPtr crtc;              /* Virtual CRTC assigned to this output */
    RROutputPtr randr_output;      /* RandR output */
    char *name;                    /* Output name (e.g., "XR-0", "XR-1") */
    int width;                     /* Current width */
    int height;                    /* Current height */
    int refresh;                   /* Current refresh rate */
    xr_mode_ptr modes;             /* List of supported modes */
    drmmode_bo framebuffer_bo;     /* Off-screen DRM buffer object for rendering */
    uint32_t framebuffer_id;       /* DRM framebuffer ID (for capture by renderer) */
    PixmapPtr pixmap;              /* X11 pixmap backed by framebuffer (for compositor) */
    struct _xr_virtual_output_rec *next;  /* Linked list */
} xr_virtual_output_rec, *xr_virtual_output_ptr;

/* Forward declarations for static functions - defined after typedefs */
static xf86CrtcPtr drmmode_xr_create_virtual_crtc(ScrnInfoPtr pScrn, drmmode_ptr drmmode);
static Bool drmmode_xr_create_offscreen_framebuffer(ScrnInfoPtr pScrn, drmmode_ptr drmmode,
                                                     xr_virtual_output_ptr vout,
                                                     int width, int height);
static void drmmode_xr_destroy_offscreen_framebuffer(ScrnInfoPtr pScrn, drmmode_ptr drmmode,
                                                      xr_virtual_output_ptr vout);
static void drmmode_xr_free_modes(xr_mode_ptr modes);

/* Get property atoms - use macros to avoid function call overhead */
#define CREATE_XR_OUTPUT_ATOM() MakeAtom(CREATE_XR_OUTPUT_PROPERTY, strlen(CREATE_XR_OUTPUT_PROPERTY), TRUE)
#define DELETE_XR_OUTPUT_ATOM() MakeAtom(DELETE_XR_OUTPUT_PROPERTY, strlen(DELETE_XR_OUTPUT_PROPERTY), TRUE)
#define XR_WIDTH_ATOM() MakeAtom(XR_WIDTH_PROPERTY, strlen(XR_WIDTH_PROPERTY), TRUE)
#define XR_HEIGHT_ATOM() MakeAtom(XR_HEIGHT_PROPERTY, strlen(XR_HEIGHT_PROPERTY), TRUE)
#define XR_REFRESH_ATOM() MakeAtom(XR_REFRESH_PROPERTY, strlen(XR_REFRESH_PROPERTY), TRUE)
#define XR_FB_ID_ATOM() MakeAtom(XR_FB_ID_PROPERTY, strlen(XR_FB_ID_PROPERTY), TRUE)
#define XR_MODES_ATOM() MakeAtom(XR_MODES_PROPERTY, strlen(XR_MODES_PROPERTY), TRUE)

/* Find a virtual output by name */
static xr_virtual_output_ptr
drmmode_xr_find_virtual_output(modesettingPtr ms, const char *name)
{
    xr_virtual_output_ptr vout = ms->xr_virtual_outputs;
    while (vout) {
        if (vout->name && strcmp(vout->name, name) == 0)
            return vout;
        vout = vout->next;
    }
    return NULL;
}

/* Find a virtual output by CRTC */
static xr_virtual_output_ptr
drmmode_xr_find_virtual_output_by_crtc(modesettingPtr ms, xf86CrtcPtr crtc)
{
    xr_virtual_output_ptr vout = ms->xr_virtual_outputs;
    while (vout) {
        if (vout->crtc == crtc)
            return vout;
        vout = vout->next;
    }
    return NULL;
}

/* Create or ensure FRAMEBUFFER_ID property exists on a virtual output */
static Bool
drmmode_xr_virtual_ensure_fb_id_property(ScrnInfoPtr pScrn, RROutputPtr randr_output, uint32_t fb_id)
{
    Atom name = XR_FB_ID_ATOM();
    INT32 dummy = 0;
    int err;

    if (name == BAD_RESOURCE) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "Failed to create FRAMEBUFFER_ID atom\n");
        return FALSE;
    }

    /* Check if property already exists */
    if (!RRQueryOutputProperty(randr_output, name)) {
        /* Property doesn't exist, create it */
        err = RRConfigureOutputProperty(randr_output, name, FALSE, FALSE, FALSE, 1, &dummy);
        if (err != 0) {
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "Failed to configure FRAMEBUFFER_ID property: %d\n", err);
            return FALSE;
        }
    }

    /* Set/update the property value */
    err = RRChangeOutputProperty(randr_output, name, XA_INTEGER, 32, PropModeReplace, 1,
                                   (unsigned char *)&fb_id, FALSE, FALSE);
    if (err != 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "Failed to set FRAMEBUFFER_ID property: %d\n", err);
        return FALSE;
    }

    RRPostPendingProperties(randr_output);
    return TRUE;
}

/* No-op create_resources for virtual output (doesn't have a real DRM connector) */
static void
drmmode_xr_virtual_create_resources(xf86OutputPtr output)
{
    /* Virtual output doesn't need DRM-specific properties */
}

/* Custom function table for virtual XR output - initialized at runtime */
static xf86OutputFuncsRec drmmode_xr_virtual_output_funcs;

/**
 * Custom detect function for virtual XR outputs
 * Always returns Connected since virtual outputs are always "available"
 */
static xf86OutputStatus
drmmode_xr_virtual_output_detect(xf86OutputPtr output)
{
    return XF86OutputStatusConnected;
}

/**
 * Custom destroy function for virtual XR outputs
 * Only frees the driver_private structure, not DRM resources (which don't exist)
 */
static void
drmmode_xr_virtual_output_destroy(xf86OutputPtr output)
{
    ScrnInfoPtr pScrn = output->scrn;
    modesettingPtr ms = modesettingPTR(pScrn);
    xr_virtual_output_ptr vout = NULL;
    xr_virtual_output_ptr prev = NULL;

    /* Find and free the virtual output record */
    vout = ms->xr_virtual_outputs;
    while (vout) {
        if (vout->output == output) {
            /* Remove from list */
            if (prev) {
                prev->next = vout->next;
            } else {
                ms->xr_virtual_outputs = vout->next;
            }
            drmmode_xr_free_modes(vout->modes);
            if (vout->name)
                free(vout->name);
            free(vout);
            break;
        }
        prev = vout;
        vout = vout->next;
    }

    drmmode_output_private_ptr drmmode_output = output->driver_private;
    if (drmmode_output) {
        /* Virtual outputs don't have DRM resources to free, just free the structure */
        free(drmmode_output);
    }
    output->driver_private = NULL;
}

/* Initialize the virtual output function table (called once) */
static void
drmmode_xr_virtual_output_funcs_init(void)
{
    /* Copy from the regular output funcs, but override create_resources, destroy, and detect */
    drmmode_xr_virtual_output_funcs = drmmode_output_funcs;
    drmmode_xr_virtual_output_funcs.create_resources = drmmode_xr_virtual_create_resources;
    drmmode_xr_virtual_output_funcs.destroy = drmmode_xr_virtual_output_destroy;
    drmmode_xr_virtual_output_funcs.detect = drmmode_xr_virtual_output_detect;
}

/**
 * Free mode list
 */
static void
drmmode_xr_free_modes(xr_mode_ptr modes)
{
    while (modes) {
        xr_mode_ptr next = modes->next;
        free(modes);
        modes = next;
    }
}

/**
 * Set modes for a virtual XR output by converting DisplayModePtr to RRModePtr
 * If vout->modes is set, uses those modes; otherwise creates common modes
 */
static void
drmmode_xr_virtual_set_modes(xf86OutputPtr output, int width, int height, int refresh)
{
    if (!output->randr_output)
        return;

    ScrnInfoPtr pScrn = output->scrn;
    modesettingPtr ms = modesettingPTR(pScrn);
    xr_virtual_output_ptr vout = NULL;

    /* Find the virtual output record */
    vout = ms->xr_virtual_outputs;
    while (vout) {
        if (vout->output == output)
            break;
        vout = vout->next;
    }

    DisplayModePtr mode;
    RRModePtr *rrmodes = NULL;
    int nmode = 0;
    char mode_name[64];

    /* If we have custom modes from TV receiver, use those */
    if (vout && vout->modes) {
        xr_mode_ptr mode_ptr = vout->modes;
        while (mode_ptr) {
            /* Create mode */
            mode = xf86CVTMode(mode_ptr->width, mode_ptr->height, mode_ptr->refresh, FALSE, FALSE);
            if (!mode) {
                mode_ptr = mode_ptr->next;
                continue;
            }

            snprintf(mode_name, sizeof(mode_name), "%dx%d@%dHz",
                     mode_ptr->width, mode_ptr->height, mode_ptr->refresh);
            mode->name = XNFstrdup(mode_name);
            mode->type = M_T_USERPREF;
            if (mode_ptr->width == width && mode_ptr->height == height) {
                mode->type |= M_T_PREFERRED;
            }

            /* Convert to RRMode */
            {
                xRRModeInfo modeInfo;
                RRModePtr rrmode;

                modeInfo.nameLength = strlen(mode->name);
                modeInfo.width = mode->HDisplay;
                modeInfo.dotClock = mode->Clock * 1000;
                modeInfo.hSyncStart = mode->HSyncStart;
                modeInfo.hSyncEnd = mode->HSyncEnd;
                modeInfo.hTotal = mode->HTotal;
                modeInfo.hSkew = mode->HSkew;
                modeInfo.height = mode->VDisplay;
                modeInfo.vSyncStart = mode->VSyncStart;
                modeInfo.vSyncEnd = mode->VSyncEnd;
                modeInfo.vTotal = mode->VTotal;
                modeInfo.modeFlags = mode->Flags;

                rrmode = RRModeGet(&modeInfo, mode->name);
                if (rrmode) {
                    RRModePtr *new_rrmodes = realloc(rrmodes, (nmode + 1) * sizeof(RRModePtr));
                    if (new_rrmodes) {
                        rrmodes = new_rrmodes;
                        rrmodes[nmode] = rrmode;
                        nmode++;
                    }
                }
            }

            xf86DeleteMode(&output->probed_modes, mode);
            mode_ptr = mode_ptr->next;
        }
    } else {
        /* Fallback: Create multiple common modes for virtual outputs */
        /* This allows users to change resolution via standard RandR APIs */
        int common_widths[] = {1920, 2560, 3840, 0};
        int common_heights[] = {1080, 1440, 2160, 0};
        int i, j;

        for (i = 0; common_widths[i] != 0; i++) {
            for (j = 0; common_heights[j] != 0; j++) {
                int w = common_widths[i];
                int h = common_heights[j];

                /* Create mode */
                mode = xf86CVTMode(w, h, refresh, FALSE, FALSE);
                if (!mode)
                    continue;

                snprintf(mode_name, sizeof(mode_name), "%dx%d", w, h);
                mode->name = XNFstrdup(mode_name);
                mode->type = M_T_USERPREF;
                if (w == width && h == height) {
                    mode->type |= M_T_PREFERRED;
                }

                /* Convert to RRMode */
                {
                    xRRModeInfo modeInfo;
                    RRModePtr rrmode;

                    modeInfo.nameLength = strlen(mode->name);
                    modeInfo.width = mode->HDisplay;
                    modeInfo.dotClock = mode->Clock * 1000;
                    modeInfo.hSyncStart = mode->HSyncStart;
                    modeInfo.hSyncEnd = mode->HSyncEnd;
                    modeInfo.hTotal = mode->HTotal;
                    modeInfo.hSkew = mode->HSkew;
                    modeInfo.height = mode->VDisplay;
                    modeInfo.vSyncStart = mode->VSyncStart;
                    modeInfo.vSyncEnd = mode->VSyncEnd;
                    modeInfo.vTotal = mode->VTotal;
                    modeInfo.modeFlags = mode->Flags;

                    rrmode = RRModeGet(&modeInfo, mode->name);
                    if (rrmode) {
                        RRModePtr *new_rrmodes = realloc(rrmodes, (nmode + 1) * sizeof(RRModePtr));
                        if (new_rrmodes) {
                            rrmodes = new_rrmodes;
                            rrmodes[nmode] = rrmode;
                            nmode++;
                        }
                    }
                }

                xf86DeleteMode(&output->probed_modes, mode);
            }
        }
    }

    if (nmode > 0) {
        RROutputSetModes(output->randr_output, rrmodes, nmode, 1);
    }

    free(rrmodes);
}

/**
 * Create a new virtual XR output dynamically
 */
/* Forward declaration */
static Bool drmmode_xr_virtual_set_property(xf86OutputPtr output, Atom property,
                                             RRPropertyValuePtr value);

static xr_virtual_output_ptr
drmmode_xr_create_virtual_output(ScrnInfoPtr pScrn, drmmode_ptr drmmode,
                                 const char *name, int width, int height, int refresh)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86OutputPtr output;
    drmmode_output_private_ptr drmmode_output;
    xr_virtual_output_ptr vout;
    ScreenPtr pScreen = xf86ScrnToScreen(pScrn);
    static Bool funcs_initialized = FALSE;

    /* Check if output with this name already exists */
    if (drmmode_xr_find_virtual_output(ms, name)) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "Virtual XR output '%s' already exists\n", name);
        return NULL;
    }

    /* Initialize function table on first call */
    if (!funcs_initialized) {
        drmmode_xr_virtual_output_funcs_init();
        funcs_initialized = TRUE;
    }

    /* Create the xf86Output with property handler for resize */
    {
        static xf86OutputFuncsRec virtual_output_funcs;
        virtual_output_funcs = drmmode_xr_virtual_output_funcs;
        virtual_output_funcs.set_property = drmmode_xr_virtual_set_property;
        output = xf86OutputCreate(pScrn, &virtual_output_funcs, name);
    }
    if (!output) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to create virtual XR output '%s'\n", name);
        return NULL;
    }

    /* Allocate private data */
    drmmode_output = calloc(1, sizeof(drmmode_output_private_rec));
    if (!drmmode_output) {
        xf86OutputDestroy(output);
        return NULL;
    }

    drmmode_output->drmmode = drmmode;
    drmmode_output->output_id = 0; /* Virtual connector, no DRM ID */
    drmmode_output->mode_output = NULL; /* No real DRM connector */
    drmmode_output->mode_encoders = NULL;
    output->driver_private = drmmode_output;

    /* Set output properties */
    output->mm_width = 0;  /* Virtual, no physical size */
    output->mm_height = 0;
    output->subpixel_order = SubPixelUnknown;
    output->interlaceAllowed = TRUE;
    output->doubleScanAllowed = TRUE;
    output->non_desktop = FALSE;
    output->status = XF86OutputStatusConnected; /* Mark as connected so xf86RandR12SetInfo12 preserves RR_Connected */

    /* Create RandR output */
    output->randr_output = RROutputCreate(pScreen, name, strlen(name), output);
    if (!output->randr_output) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to create RandR output for '%s'\n", name);
        free(drmmode_output);
        xf86OutputDestroy(output);
        return NULL;
    }

    /* Virtual outputs are always connected */
    RROutputSetConnection(output->randr_output, RR_Connected);

    /* Set modes */
    drmmode_xr_virtual_set_modes(output, width, height, refresh);

    /* Create resize properties */
    {
        Atom width_atom = XR_WIDTH_ATOM();
        Atom height_atom = XR_HEIGHT_ATOM();
        Atom refresh_atom = XR_REFRESH_ATOM();
        INT32 width_val = width;
        INT32 height_val = height;
        INT32 refresh_val = refresh;

        if (width_atom != BAD_RESOURCE) {
            RRConfigureOutputProperty(output->randr_output, width_atom, FALSE, FALSE, FALSE, 1, &width_val);
            RRChangeOutputProperty(output->randr_output, width_atom, XA_INTEGER, 32,
                                   PropModeReplace, 1, &width_val, FALSE, FALSE);
        }
        if (height_atom != BAD_RESOURCE) {
            RRConfigureOutputProperty(output->randr_output, height_atom, FALSE, FALSE, FALSE, 1, &height_val);
            RRChangeOutputProperty(output->randr_output, height_atom, XA_INTEGER, 32,
                                   PropModeReplace, 1, &height_val, FALSE, FALSE);
        }
        if (refresh_atom != BAD_RESOURCE) {
            RRConfigureOutputProperty(output->randr_output, refresh_atom, FALSE, FALSE, FALSE, 1, &refresh_val);
            RRChangeOutputProperty(output->randr_output, refresh_atom, XA_INTEGER, 32,
                                   PropModeReplace, 1, &refresh_val, FALSE, FALSE);
        }
    }

    /* Create XR_MODES property for setting custom modes */
    {
        Atom modes_atom = XR_MODES_ATOM();
        if (modes_atom != BAD_RESOURCE) {
            INT32 dummy = 0;
            RRConfigureOutputProperty(output->randr_output, modes_atom, FALSE, FALSE, FALSE, 1, &dummy);
            /* Initial empty value */
            char empty_str[] = "";
            RRChangeOutputProperty(output->randr_output, modes_atom, XA_STRING, 8,
                                   PropModeReplace, 1, (unsigned char *)empty_str, FALSE, FALSE);
        }
    }

    RRPostPendingProperties(output->randr_output);
    RROutputChanged(output->randr_output, TRUE);
    RRTellChanged(pScreen);

    /* Create and assign a virtual CRTC for this output */
    xf86CrtcPtr crtc = drmmode_xr_create_virtual_crtc(pScrn, drmmode);
    if (!crtc) {
        RROutputDestroy(output->randr_output);
        free(drmmode_output);
        xf86OutputDestroy(output);
        return NULL;
    }

    /* Create RandR CRTC for this virtual CRTC if screen is initialized */
    if (pScreen && pScreen->root) {
        RRCrtcPtr randr_crtc = RRCrtcCreate(pScreen, crtc);
        if (randr_crtc) {
            crtc->randr_crtc = randr_crtc;
            /* Link output to CRTC */
            if (!RROutputSetCrtcs(output->randr_output, &randr_crtc, 1)) {
                xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                           "Failed to set CRTCs for virtual output '%s'\n", name);
            }
            /* Set output properties for CRTC assignment */
            output->possible_crtcs = 1 << (randr_crtc->id);
            output->possible_clones = 0;

            /* Mark this output as virtual by setting VIRTUAL_OUTPUT property */
            {
                Atom virtual_atom = MakeAtom(XR_VIRTUAL_OUTPUT_PROPERTY,
                                             strlen(XR_VIRTUAL_OUTPUT_PROPERTY), TRUE);
                INT32 dummy = 0;
                if (virtual_atom != BAD_RESOURCE) {
                    if (!RRQueryOutputProperty(output->randr_output, virtual_atom)) {
                        /* Property doesn't exist, create it */
                        if (RRConfigureOutputProperty(output->randr_output, virtual_atom,
                                                      FALSE, FALSE, FALSE, 1, &dummy) == 0) {
                            /* Set the property value to 1 (true) */
                            INT32 value = 1;
                            RRChangeOutputProperty(output->randr_output, virtual_atom,
                                                   XA_INTEGER, 32, PropModeReplace, 1,
                                                   (unsigned char *)&value, FALSE, FALSE);
                            RRPostPendingProperties(output->randr_output);
                        }
                    }
                }
            }

            /* Enable the output automatically by setting a mode on the CRTC */
            if (output->randr_output->numModes > 0) {
                /* Find the preferred mode (the one matching width x height) */
                RRModePtr preferred_mode = NULL;
                for (int i = 0; i < output->randr_output->numModes; i++) {
                    RRModePtr mode = output->randr_output->modes[i];
                    if (mode && mode->mode.width == width && mode->mode.height == height) {
                        preferred_mode = mode;
                        break;
                    }
                }
                /* If no exact match, use the first mode */
                if (!preferred_mode) {
                    preferred_mode = output->randr_output->modes[0];
                }

                if (preferred_mode) {
                    /* Enable the CRTC with the preferred mode */
                    if (RRCrtcNotify(randr_crtc, preferred_mode, 0, 0, RR_Rotate_0, NULL,
                                     1, &output->randr_output)) {
                        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                                   "Virtual XR output '%s' enabled automatically with mode %dx%d\n",
                                   name, width, height);
                    } else {
                        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                                   "Failed to enable virtual XR output '%s' automatically\n", name);
                    }
                }
            }
        } else {
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                       "Failed to create RandR CRTC for virtual output '%s'\n", name);
        }
    }

    /* Assign CRTC to output (for XF86 layer) */
    output->crtc = crtc;
    drmmode_output->current_crtc = crtc;

    /* Allocate and initialize virtual output record */
    vout = calloc(1, sizeof(xr_virtual_output_rec));
    if (!vout) {
        xf86CrtcDestroy(crtc);
        RROutputDestroy(output->randr_output);
        free(drmmode_output);
        xf86OutputDestroy(output);
        return NULL;
    }

    vout->output = output;
    vout->crtc = crtc;  /* Store CRTC reference for easy lookup */
    vout->randr_output = output->randr_output;
    vout->name = strdup(name);
    vout->width = width;
    vout->height = height;
    vout->refresh = refresh;
    vout->modes = NULL;  /* Will be set via XR_MODES property if provided */
    
    /* Initialize framebuffer fields */
    memset(&vout->framebuffer_bo, 0, sizeof(vout->framebuffer_bo));
    vout->framebuffer_id = 0;
    vout->pixmap = NULL;

    /* Create off-screen framebuffer for this virtual output */
    if (!drmmode_xr_create_offscreen_framebuffer(pScrn, drmmode, vout,
                                                 width, height)) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "Failed to create off-screen framebuffer for '%s', continuing anyway\n",
                   name);
        /* Continue anyway - framebuffer creation failure is not fatal */
    }

    /* Add to list */
    vout->next = ms->xr_virtual_outputs;
    ms->xr_virtual_outputs = vout;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Created virtual XR output '%s' (%dx%d@%dHz) with virtual CRTC and off-screen framebuffer\n",
               name, width, height, refresh);

    return vout;
}

/**
 * Delete a virtual XR output
 */
static Bool
drmmode_xr_delete_virtual_output(ScrnInfoPtr pScrn, const char *name)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xr_virtual_output_ptr vout, prev = NULL;

    /* Find the output */
    vout = ms->xr_virtual_outputs;
    while (vout) {
        if (vout->name && strcmp(vout->name, name) == 0)
            break;
        prev = vout;
        vout = vout->next;
    }

    if (!vout) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "Virtual XR output '%s' not found\n", name);
        return FALSE;
    }

    /* Remove from list */
    if (prev)
        prev->next = vout->next;
    else
        ms->xr_virtual_outputs = vout->next;

    /* Get drmmode from output */
    drmmode_ptr drmmode = NULL;
    if (vout->output && vout->output->driver_private) {
        drmmode_output_private_ptr drmmode_output = vout->output->driver_private;
        drmmode = drmmode_output->drmmode;
    }
    
    if (drmmode) {
        /* Destroy off-screen framebuffer first */
        drmmode_xr_destroy_offscreen_framebuffer(pScrn, drmmode, vout);
    }

    /* Clean up CRTC if assigned */
    if (vout->crtc) {
        xf86CrtcPtr crtc = vout->crtc;
        /* Destroy RandR CRTC if it exists */
        if (crtc->randr_crtc) {
            RRCrtcDestroy(crtc->randr_crtc);
            crtc->randr_crtc = NULL;
        }
        /* Destroy XF86 CRTC */
        xf86CrtcDestroy(crtc);
        vout->crtc = NULL;
        if (vout->output) {
            vout->output->crtc = NULL;
        }
    }

    /* Destroy RandR output first (before xf86Output) */
    if (vout->randr_output) {
        /* Mark as disconnected before destroying */
        RROutputSetConnection(vout->randr_output, RR_Disconnected);
        RROutputChanged(vout->randr_output, TRUE);
        RRTellChanged(xf86ScrnToScreen(pScrn));
        /* Clear the pointer in xf86Output before destroying */
        if (vout->output)
            vout->output->randr_output = NULL;
        RROutputDestroy(vout->randr_output);
        vout->randr_output = NULL;
    }

    /* Destroy xf86Output - the destroy function will handle freeing driver_private */
    if (vout->output) {
        xf86OutputDestroy(vout->output);
        vout->output = NULL;
    }

    /* Free name, modes, and record */
    if (vout->name)
        free(vout->name);
    drmmode_xr_free_modes(vout->modes);
    free(vout);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Deleted virtual XR output '%s'\n", name);

    return TRUE;
}

/**
 * Resize a virtual XR output
 */
static Bool
drmmode_xr_resize_virtual_output(ScrnInfoPtr pScrn, xr_virtual_output_ptr vout,
                                  int width, int height, int refresh)
{
    if (!vout || !vout->output || !vout->randr_output)
        return FALSE;

    /* Update dimensions */
    vout->width = width;
    vout->height = height;
    vout->refresh = refresh;

    /* Update modes */
    drmmode_xr_virtual_set_modes(vout->output, width, height, refresh);

    /* Update properties */
    {
        Atom width_atom = XR_WIDTH_ATOM();
        Atom height_atom = XR_HEIGHT_ATOM();
        Atom refresh_atom = XR_REFRESH_ATOM();
        INT32 width_val = width;
        INT32 height_val = height;
        INT32 refresh_val = refresh;

        if (width_atom != BAD_RESOURCE) {
            RRChangeOutputProperty(vout->randr_output, width_atom, XA_INTEGER, 32,
                                   PropModeReplace, 1, &width_val, FALSE, FALSE);
        }
        if (height_atom != BAD_RESOURCE) {
            RRChangeOutputProperty(vout->randr_output, height_atom, XA_INTEGER, 32,
                                   PropModeReplace, 1, &height_val, FALSE, FALSE);
        }
        if (refresh_atom != BAD_RESOURCE) {
            RRChangeOutputProperty(vout->randr_output, refresh_atom, XA_INTEGER, 32,
                                   PropModeReplace, 1, &refresh_val, FALSE, FALSE);
        }
    }

    RROutputChanged(vout->randr_output, TRUE);
    RRTellChanged(xf86ScrnToScreen(pScrn));

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Resized virtual XR output '%s' to %dx%d@%dHz\n",
               vout->name, width, height, refresh);

    return TRUE;
}

/**
 * Property handler for XR-Manager output (handles CREATE/DELETE commands)
 */
static Bool
drmmode_xr_manager_set_property(xf86OutputPtr output, Atom property,
                                 RRPropertyValuePtr value)
{
    ScrnInfoPtr pScrn = output->scrn;
    modesettingPtr ms = modesettingPTR(pScrn);
    drmmode_ptr drmmode = &ms->drmmode;
    const char *prop_name;
    char *command = NULL;
    char *name = NULL, *end;
    int width = 1920, height = 1080, refresh = 60;

    prop_name = NameForAtom(property);
    if (!prop_name)
        return FALSE;

    /* Only handle our custom properties */
    if (strcmp(prop_name, CREATE_XR_OUTPUT_PROPERTY) != 0 &&
        strcmp(prop_name, DELETE_XR_OUTPUT_PROPERTY) != 0) {
        return FALSE; /* Let default handler deal with it */
    }

    /* Extract command string from property value */
    if (value->type != XA_STRING || value->format != 8) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "XR property value must be STRING format\n");
        return FALSE;
    }

    command = strndup((char *)value->data, value->size);
    if (!command)
        return FALSE;

    if (strcmp(prop_name, CREATE_XR_OUTPUT_PROPERTY) == 0) {
        /* Format: "NAME:WIDTH:HEIGHT:REFRESH" or "NAME:WIDTH:HEIGHT" (refresh defaults to 60) */
        /* Name can be arbitrary (e.g., "XR-0", "REMOTE-0", "STREAM-0", etc.) */
        name = command;
        end = strchr(name, ':');
        if (end) {
            *end++ = '\0';
            width = (int)strtol(end, &end, 10);
            if (*end == ':') {
                end++;
                height = (int)strtol(end, &end, 10);
                if (*end == ':') {
                    end++;
                    refresh = (int)strtol(end, NULL, 10);
                }
            }
        }

        if (!name || !name[0]) {
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                       "CREATE_XR_OUTPUT: invalid format, expected 'NAME:WIDTH:HEIGHT[:REFRESH]'\n");
            free(command);
            return FALSE;
        }

        if (drmmode_xr_create_virtual_output(pScrn, drmmode, name, width, height, refresh)) {
            xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                       "Successfully created virtual output '%s'\n", name);
            free(command);
            return TRUE;
        } else {
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                       "Failed to create virtual output '%s'\n", name);
            free(command);
            return FALSE;
        }
    } else if (strcmp(prop_name, DELETE_XR_OUTPUT_PROPERTY) == 0) {
        /* Format: "NAME" (virtual display name is arbitrary) */
        if (drmmode_xr_delete_virtual_output(pScrn, command)) {
            xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                       "Successfully deleted virtual output '%s'\n", command);
            free(command);
            return TRUE;
        } else {
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                       "Failed to delete virtual output '%s'\n", command);
            free(command);
            return FALSE;
        }
    }

    free(command);
    return FALSE;
}

/**
 * Property handler for individual XR outputs (handles resize commands)
 */
static Bool
drmmode_xr_virtual_set_property(xf86OutputPtr output, Atom property,
                                 RRPropertyValuePtr value)
{
    ScrnInfoPtr pScrn = output->scrn;
    modesettingPtr ms = modesettingPTR(pScrn);
    xr_virtual_output_ptr vout;
    const char *prop_name;
    int new_width, new_height, new_refresh;

    prop_name = NameForAtom(property);
    if (!prop_name)
        return FALSE;

    /* Find the virtual output record */
    vout = ms->xr_virtual_outputs;
    while (vout) {
        if (vout->output == output)
            break;
        vout = vout->next;
    }

    if (!vout)
        return FALSE; /* Not a virtual output, let default handler deal with it */

    /* Handle resize properties */
    if (value->type != XA_INTEGER || value->format != 32 || value->size != 1) {
        return FALSE;
    }

    new_width = vout->width;
    new_height = vout->height;
    new_refresh = vout->refresh;

    if (strcmp(prop_name, XR_MODES_PROPERTY) == 0) {
        /* Handle XR_MODES property - format: "WIDTH:HEIGHT:REFRESH|WIDTH:HEIGHT:REFRESH|..." */
        if (value->type != XA_STRING || value->format != 8) {
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                       "XR_MODES property must be STRING format\n");
            return FALSE;
        }

        char *modes_str = strndup((char *)value->data, value->size);
        if (!modes_str)
            return FALSE;

        /* Free existing modes */
        drmmode_xr_free_modes(vout->modes);
        vout->modes = NULL;

        /* Parse modes string */
        char *saveptr = NULL;
        char *token = strtok_r(modes_str, "|", &saveptr);
        while (token) {
            int w = 0, h = 0, r = 60;
            if (sscanf(token, "%d:%d:%d", &w, &h, &r) >= 2) {
                if (w >= 64 && w <= 16384 && h >= 64 && h <= 16384 && r >= 1 && r <= 1000) {
                    xr_mode_ptr mode = calloc(1, sizeof(xr_mode_rec));
                    if (mode) {
                        mode->width = w;
                        mode->height = h;
                        mode->refresh = r;
                        mode->next = vout->modes;
                        vout->modes = mode;
                    }
                }
            }
            token = strtok_r(NULL, "|", &saveptr);
        }
        free(modes_str);

        /* Re-set modes with the new mode list */
        drmmode_xr_virtual_set_modes(vout->output, vout->width, vout->height, vout->refresh);
        return TRUE;
    } else if (strcmp(prop_name, XR_WIDTH_PROPERTY) == 0) {
        new_width = *(INT32 *)value->data;
    } else if (strcmp(prop_name, XR_HEIGHT_PROPERTY) == 0) {
        new_height = *(INT32 *)value->data;
    } else if (strcmp(prop_name, XR_REFRESH_PROPERTY) == 0) {
        new_refresh = *(INT32 *)value->data;
    } else {
        return FALSE; /* Not a resize property */
    }

    /* Validate dimensions */
    if (new_width < 64 || new_width > 16384 ||
        new_height < 64 || new_height > 16384 ||
        new_refresh < 1 || new_refresh > 1000) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "Invalid dimensions/refresh for XR output '%s': %dx%d@%dHz\n",
                   vout->name, new_width, new_height, new_refresh);
        return FALSE;
    }

    /* Apply resize */
    return drmmode_xr_resize_virtual_output(pScrn, vout, new_width, new_height, new_refresh);
}

/**
 * Create the XR-Manager control output (always disconnected, used for control)
 */
Bool
drmmode_xr_virtual_output_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86OutputPtr output;
    drmmode_output_private_ptr drmmode_output;
    static Bool funcs_initialized = FALSE;

    /* Check if already initialized */
    if (ms->xr_manager_output)
        return TRUE;

    /* Initialize function table on first call */
    if (!funcs_initialized) {
        drmmode_xr_virtual_output_funcs_init();
        funcs_initialized = TRUE;
    }

    /* Create custom function table for manager with property handler */
    {
        static xf86OutputFuncsRec manager_funcs;
        manager_funcs = drmmode_xr_virtual_output_funcs;
        manager_funcs.set_property = drmmode_xr_manager_set_property;
        output = xf86OutputCreate(pScrn, &manager_funcs, XR_MANAGER_OUTPUT_NAME);
    }

    if (!output) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to create XR-Manager output\n");
        return FALSE;
    }

    /* Allocate private data */
    drmmode_output = calloc(1, sizeof(drmmode_output_private_rec));
    if (!drmmode_output) {
        xf86OutputDestroy(output);
        return FALSE;
    }

    drmmode_output->drmmode = drmmode;
    drmmode_output->output_id = 0;
    drmmode_output->mode_output = NULL;
    drmmode_output->mode_encoders = NULL;
    output->driver_private = drmmode_output;

    /* Set output properties */
    output->mm_width = 0;
    output->mm_height = 0;
    output->subpixel_order = SubPixelUnknown;
    output->interlaceAllowed = TRUE;
    output->doubleScanAllowed = TRUE;
    output->non_desktop = TRUE; /* XR-Manager is not a real display, hide from Display Settings */
    output->status = XF86OutputStatusDisconnected; /* Keep disconnected so xf86RandR12SetInfo12 preserves RR_Disconnected */

    /* Don't create RandR output here - the screen doesn't exist yet */
    output->randr_output = NULL;

    /* Store reference */
    ms->xr_manager_output = output;
    ms->xr_virtual_outputs = NULL; /* List of dynamically created outputs */

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "XR-Manager output created (RandR output will be created after screen init)\n");

    return TRUE;
}

/**
 * Create RandR output for XR-Manager after screen is initialized
 */
Bool
drmmode_xr_virtual_output_post_screen_init(ScrnInfoPtr pScrn)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86OutputPtr output = ms->xr_manager_output;
    ScreenPtr pScreen = xf86ScrnToScreen(pScrn);
    Atom create_atom, delete_atom;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "drmmode_xr_virtual_output_post_screen_init called\n");

    if (!output) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "XR-Manager output not found in post_screen_init\n");
        return FALSE;
    }

    /* If RandR output already exists, ensure properties are registered */
    if (output->randr_output) {
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "XR-Manager RandR output already exists, ensuring properties are registered\n");
        /* Re-apply non-desktop and disconnected status (xf86RandR12SetInfo12 may have reset them) */
        RROutputSetConnection(output->randr_output, RR_Disconnected);
        RROutputSetNonDesktop(output->randr_output, TRUE);
        /* Fall through to register properties */
    } else {

        /* Create RandR output for XR-Manager */
        output->randr_output = RROutputCreate(pScreen, XR_MANAGER_OUTPUT_NAME,
                                              strlen(XR_MANAGER_OUTPUT_NAME), output);
        if (!output->randr_output) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Failed to create RandR output for XR-Manager\n");
            return FALSE;
        }

        /* Always mark as disconnected (it's a control interface, not a real display) */
        RROutputSetConnection(output->randr_output, RR_Disconnected);
        /* Mark as non-desktop so it doesn't show in Display Settings */
        RROutputSetNonDesktop(output->randr_output, TRUE);
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "XR-Manager RandR output created (non-desktop, disconnected)\n");
    }

    /* Create CREATE_XR_OUTPUT and DELETE_XR_OUTPUT properties */
    create_atom = CREATE_XR_OUTPUT_ATOM();
    delete_atom = DELETE_XR_OUTPUT_ATOM();

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Creating properties: create_atom=%lu, delete_atom=%lu\n",
               (unsigned long)create_atom, (unsigned long)delete_atom);

    if (create_atom != BAD_RESOURCE) {
        /* Check if property already exists */
        RRPropertyPtr prop = RRQueryOutputProperty(output->randr_output, create_atom);
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "CREATE_XR_OUTPUT property exists: %s\n",
                   prop ? "yes" : "no");
        if (!prop) {
            /* Configure property metadata (RRConfigureOutputProperty expects INT32 for config) */
            INT32 dummy = 0;
            int err = RRConfigureOutputProperty(output->randr_output, create_atom,
                                             FALSE, FALSE, FALSE, /* pending, range, immutable */
                                             1, &dummy);
            if (err != 0) {
                xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                           "Failed to configure CREATE_XR_OUTPUT property: %d\n", err);
            } else {
                /* Set initial empty value as STRING (this sets the actual type) */
                char empty_str[] = "";
                err = RRChangeOutputProperty(output->randr_output, create_atom,
                                          XA_STRING, 8, PropModeReplace, 1,
                                          (unsigned char *)empty_str, FALSE, FALSE);
                if (err != 0) {
                    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                               "Failed to set CREATE_XR_OUTPUT property value: %d\n", err);
                } else {
                    RRPostPendingProperties(output->randr_output);
                    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                               "CREATE_XR_OUTPUT property registered\n");
                }
            }
        }
    }
    if (delete_atom != BAD_RESOURCE) {
        /* Check if property already exists */
        RRPropertyPtr prop = RRQueryOutputProperty(output->randr_output, delete_atom);
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "DELETE_XR_OUTPUT property exists: %s\n",
                   prop ? "yes" : "no");
        if (!prop) {
            /* Configure property metadata (RRConfigureOutputProperty expects INT32 for config) */
            INT32 dummy = 0;
            int err = RRConfigureOutputProperty(output->randr_output, delete_atom,
                                             FALSE, FALSE, FALSE, /* pending, range, immutable */
                                             1, &dummy);
            if (err != 0) {
                xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                           "Failed to configure DELETE_XR_OUTPUT property: %d\n", err);
            } else {
                /* Set initial empty value as STRING (this sets the actual type) */
                char empty_str[] = "";
                err = RRChangeOutputProperty(output->randr_output, delete_atom,
                                          XA_STRING, 8, PropModeReplace, 1,
                                          (unsigned char *)empty_str, FALSE, FALSE);
                if (err != 0) {
                    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                               "Failed to set DELETE_XR_OUTPUT property value: %d\n", err);
                } else {
                    RRPostPendingProperties(output->randr_output);
                    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                               "DELETE_XR_OUTPUT property registered\n");
                }
            }
        }
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "XR-Manager RandR output created (use CREATE_XR_OUTPUT/DELETE_XR_OUTPUT properties)\n");

    return TRUE;
}

/**
 * Clean up virtual XR outputs
 */
void
drmmode_xr_virtual_output_fini(ScrnInfoPtr pScrn)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xr_virtual_output_ptr vout, next;

    /* Delete all virtual outputs */
    vout = ms->xr_virtual_outputs;
    while (vout) {
        next = vout->next;
        if (vout->randr_output)
            RROutputDestroy(vout->randr_output);
        if (vout->output) {
            drmmode_output_private_ptr drmmode_output = vout->output->driver_private;
            if (drmmode_output)
                free(drmmode_output);
            xf86OutputDestroy(vout->output);
        }
        if (vout->name)
            free(vout->name);
        drmmode_xr_free_modes(vout->modes);
        free(vout);
        vout = next;
    }
    ms->xr_virtual_outputs = NULL;

    /* Clean up XR-Manager */
    if (ms->xr_manager_output) {
        xf86OutputPtr output = ms->xr_manager_output;
        drmmode_output_private_ptr drmmode_output = output->driver_private;

        if (output->randr_output)
            RROutputDestroy(output->randr_output);

        if (drmmode_output) {
            free(drmmode_output);
            output->driver_private = NULL;
        }

        xf86OutputDestroy(output);
        ms->xr_manager_output = NULL;
    }

    ms->xr_virtual_enabled = FALSE;
}


/* ============================================================
 * Off-screen Framebuffer Implementation
 * ============================================================ */

/**
 * Create an off-screen framebuffer for a virtual XR output
 * 
 * "Off-screen" means this framebuffer is not connected to any physical display.
 * The compositor renders to it as if it were a display, but instead of being
 * sent to hardware, our 3D renderer captures it and applies transformations.
 * 
 * This function:
 * 1. Creates a DRM buffer object (BO) in GPU or system memory
 * 2. Imports it as a DRM framebuffer (gets an FB ID)
 * 3. Creates an X11 pixmap backed by the BO so the compositor can render
 * 
 * The framebuffer ID can later be used by the renderer to capture frames.
 */
static Bool
drmmode_xr_create_offscreen_framebuffer(ScrnInfoPtr pScrn, drmmode_ptr drmmode,
                                        xr_virtual_output_ptr vout,
                                        int width, int height)
{
    ScreenPtr pScreen = xf86ScrnToScreen(pScrn);
    modesettingPtr ms = modesettingPTR(pScrn);
    PixmapPtr pixmap;
    msPixmapPrivPtr ppriv;
    void *pixmap_ptr = NULL;
    int ret;
    int pitch;
    
    /* Initialize framebuffer BO structure */
    memset(&vout->framebuffer_bo, 0, sizeof(vout->framebuffer_bo));
    vout->framebuffer_id = 0;
    vout->pixmap = NULL;

    /* Create DRM buffer object (BO) - this allocates memory for the framebuffer */
    /* We replicate the logic from drmmode_create_bo() to support both GBM and dumb buffers */
    vout->framebuffer_bo.width = width;
    vout->framebuffer_bo.height = height;
    
#ifdef GLAMOR_HAS_GBM
    /* Try GBM first if available (GPU-optimized, better performance) */
    if (drmmode->glamor && drmmode->gbm) {
        uint32_t format;
        
        /* Select GBM format based on screen depth (same logic as drmmode_create_bo) */
        switch (pScrn->depth) {
        case 15:
            format = GBM_FORMAT_ARGB1555;
            break;
        case 16:
            format = GBM_FORMAT_RGB565;
            break;
        case 30:
            format = GBM_FORMAT_ARGB2101010;
            break;
        default:
            format = GBM_FORMAT_ARGB8888;
            break;
        }
        
        /* Create GBM buffer object with scanout and rendering flags */
        vout->framebuffer_bo.gbm = gbm_bo_create(drmmode->gbm, width, height, format,
                                                GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        vout->framebuffer_bo.used_modifiers = FALSE;
        
        if (vout->framebuffer_bo.gbm) {
            /* GBM BO created successfully - we'll use this instead of dumb buffer */
            xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                       "Created GBM buffer object for off-screen framebuffer '%s' (GPU-optimized)\n",
                       vout->name);
            goto bo_created;
        }
        
        /* GBM creation failed, fall through to dumb buffer */
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "Failed to create GBM BO for '%s', falling back to dumb buffer (CPU-accessible, less efficient)\n",
                   vout->name);
    } else {
        /* GBM not available - log fallback to dumb buffer */
        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "GBM not available for '%s', using dumb buffer (CPU-accessible)\n",
                   vout->name);
    }
#else
    /* GBM support not compiled in - log that we're using dumb buffer */
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "GBM support not compiled in for '%s', using dumb buffer (CPU-accessible)\n",
               vout->name);
#endif

    /* Fallback to dumb buffer (works on all systems, CPU-accessible) */
    vout->framebuffer_bo.dumb = dumb_bo_create(drmmode->fd, width, height,
                                               drmmode->kbpp);
    if (!vout->framebuffer_bo.dumb) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to create off-screen framebuffer BO for '%s'\n",
                   vout->name);
        return FALSE;
    }

bo_created:

    /* Import BO as DRM framebuffer (get FB ID for capture) */
    ret = drmmode_bo_import(drmmode, &vout->framebuffer_bo,
                            &vout->framebuffer_id);
    if (ret != 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to import framebuffer BO for '%s': %s\n",
                   vout->name, strerror(-ret));
        drmmode_bo_destroy(drmmode, &vout->framebuffer_bo);
        return FALSE;
    }

    /* Get pitch for pixmap creation */
    pitch = drmmode_bo_get_pitch(&vout->framebuffer_bo);

    /* Map the BO to CPU-accessible memory (or prepare for GPU access) */
#ifdef GLAMOR_HAS_GBM
    if (vout->framebuffer_bo.gbm) {
        /* GBM BO - cannot be CPU-mapped directly, but glamor can use it */
        /* For GBM BOs, we don't set pixmap_ptr - glamor will handle it via EGL */
        pixmap_ptr = NULL;
        
        /* Note: GBM BOs are GPU-optimized and may not be CPU-mappable.
         * The compositor will render via OpenGL/EGL, and the renderer will
         * capture via DMA-BUF export (which we'll implement in item #6). */
    } else
#endif
    {
        /* Dumb buffer - map it to CPU-accessible memory */
        ret = dumb_bo_map(drmmode->fd, vout->framebuffer_bo.dumb);
        if (ret != 0) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Failed to map framebuffer BO for '%s': %s\n",
                       vout->name, strerror(-ret));
            drmModeRmFB(drmmode->fd, vout->framebuffer_id);
            drmmode_bo_destroy(drmmode, &vout->framebuffer_bo);
            return FALSE;
        }
        pixmap_ptr = vout->framebuffer_bo.dumb->ptr;
    }

    /* Create X11 pixmap backed by the framebuffer BO */
#ifdef GLAMOR_HAS_GBM
    if (vout->framebuffer_bo.gbm && ms->glamor.egl_create_textured_pixmap_from_gbm_bo) {
        /* For GBM BOs, use glamor's EGL texture creation (GPU-optimized path) */
        pixmap = (*pScreen->CreatePixmap)(pScreen, width, height, pScrn->depth, 0);
        if (!pixmap) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Failed to create pixmap for GBM off-screen framebuffer '%s'\n",
                       vout->name);
            drmModeRmFB(drmmode->fd, vout->framebuffer_id);
            drmmode_bo_destroy(drmmode, &vout->framebuffer_bo);
            return FALSE;
        }
        
        /* Create EGL texture from GBM BO */
        if (!ms->glamor.egl_create_textured_pixmap_from_gbm_bo(pixmap, vout->framebuffer_bo.gbm, FALSE)) {
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                       "Failed to create EGL texture from GBM BO for '%s', falling back to CPU path\n",
                       vout->name);
            (*pScreen->DestroyPixmap)(pixmap);
            /* Fall through to dumb buffer path */
        } else {
            /* Successfully created EGL texture from GBM BO */
            xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                       "Created EGL texture from GBM BO for '%s' (GPU-optimized pixmap)\n",
                       vout->name);
            
            /* Set up pixmap private data to track the BO */
            ppriv = msGetPixmapPriv(drmmode, pixmap);
            if (ppriv) {
                ppriv->fb_id = vout->framebuffer_id;
                /* For GBM BOs, backing_bo is NULL - glamor handles it via EGL */
                ppriv->backing_bo = NULL;
            }
            vout->pixmap = pixmap;
            goto pixmap_created;
        }
    }
    
    /* If we reach here, either:
     * 1. GBM BO creation failed earlier (already logged)
     * 2. GBM is not available (already logged)
     * 3. EGL texture creation from GBM BO failed (just logged)
     * In all cases, fall through to dumb buffer path */
    if (vout->framebuffer_bo.gbm) {
        /* We have a GBM BO but couldn't create EGL texture - this is unusual */
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "Using CPU-mappable fallback path for GBM BO '%s' (less efficient)\n",
                   vout->name);
    }
#endif
    {
        /* Dumb buffer path - use CPU-accessible memory */
        pixmap = (*pScreen->CreatePixmap)(pScreen, 0, 0, pScrn->depth, 0);
        if (!pixmap) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Failed to create pixmap for off-screen framebuffer '%s'\n",
                       vout->name);
            if (pixmap_ptr && vout->framebuffer_bo.dumb) {
                munmap(vout->framebuffer_bo.dumb->ptr, vout->framebuffer_bo.dumb->size);
                vout->framebuffer_bo.dumb->ptr = NULL;
            }
            drmModeRmFB(drmmode->fd, vout->framebuffer_id);
            drmmode_bo_destroy(drmmode, &vout->framebuffer_bo);
            return FALSE;
        }

        /* Modify pixmap header to use our framebuffer memory */
        if (!(*pScreen->ModifyPixmapHeader)(pixmap, width, height,
                                            pScrn->depth, pScrn->bitsPerPixel,
                                            pitch, pixmap_ptr)) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "Failed to modify pixmap header for off-screen framebuffer '%s'\n",
                       vout->name);
            (*pScreen->DestroyPixmap)(pixmap);
            if (pixmap_ptr && vout->framebuffer_bo.dumb) {
                munmap(vout->framebuffer_bo.dumb->ptr, vout->framebuffer_bo.dumb->size);
                vout->framebuffer_bo.dumb->ptr = NULL;
            }
            drmModeRmFB(drmmode->fd, vout->framebuffer_id);
            drmmode_bo_destroy(drmmode, &vout->framebuffer_bo);
            return FALSE;
        }

        /* Set up pixmap private data to track the BO */
        ppriv = msGetPixmapPriv(drmmode, pixmap);
        if (ppriv) {
            ppriv->fb_id = vout->framebuffer_id;
            ppriv->backing_bo = vout->framebuffer_bo.dumb;
        }
    }

pixmap_created:
    vout->pixmap = pixmap;

    /* Set FRAMEBUFFER_ID property on RandR output for renderer access */
    if (vout->randr_output) {
        drmmode_xr_virtual_ensure_fb_id_property(pScrn, vout->randr_output, vout->framebuffer_id);
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Created off-screen framebuffer for '%s': %dx%d, FB ID %u\n",
               vout->name, width, height, vout->framebuffer_id);

    return TRUE;
}

/**
 * Destroy an off-screen framebuffer for a virtual XR output
 */
static void
drmmode_xr_destroy_offscreen_framebuffer(ScrnInfoPtr pScrn, drmmode_ptr drmmode,
                                         xr_virtual_output_ptr vout)
{
    ScreenPtr pScreen = xf86ScrnToScreen(pScrn);

    if (!vout)
        return;

    /* Destroy pixmap if it exists */
    if (vout->pixmap && pScreen) {
        (*pScreen->DestroyPixmap)(vout->pixmap);
        vout->pixmap = NULL;
    }

    /* Remove DRM framebuffer */
    if (vout->framebuffer_id != 0) {
        drmModeRmFB(drmmode->fd, vout->framebuffer_id);
        vout->framebuffer_id = 0;
    }

    /* Destroy BO (this will also unmap if needed) */
    if (vout->framebuffer_bo.dumb || vout->framebuffer_bo.gbm) {
        drmmode_bo_destroy(drmmode, &vout->framebuffer_bo);
    }

    memset(&vout->framebuffer_bo, 0, sizeof(vout->framebuffer_bo));
}

/* ============================================================
 * Virtual CRTC Implementation
 * ============================================================ */

/* Virtual CRTC function callbacks - handle software-based CRTCs */
static void
drmmode_xr_virtual_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
    /* Virtual CRTCs don't need DPMS - just track state */
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_crtc->dpms_mode = mode;
}

static Bool
drmmode_xr_virtual_crtc_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
                                       Rotation rotation, int x, int y)
{
    ScrnInfoPtr pScrn = crtc->scrn;
    modesettingPtr ms = modesettingPTR(pScrn);
    drmmode_ptr drmmode = &ms->drmmode;
    xr_virtual_output_ptr vout;
    int new_width, new_height;

    /* Virtual CRTCs just update internal state - no hardware programming */
    if (mode) {
        new_width = mode->HDisplay;
        new_height = mode->VDisplay;

        /* Find the virtual output associated with this CRTC */
        vout = drmmode_xr_find_virtual_output_by_crtc(ms, crtc);
        
        if (vout) {
            /* Check if framebuffer needs to be resized */
            if (vout->width != new_width || vout->height != new_height) {
                xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                           "Resizing virtual XR output '%s' framebuffer from %dx%d to %dx%d\n",
                           vout->name, vout->width, vout->height, new_width, new_height);

                /* Destroy old framebuffer */
                drmmode_xr_destroy_offscreen_framebuffer(pScrn, drmmode, vout);

                /* Create new framebuffer at new resolution */
                if (!drmmode_xr_create_offscreen_framebuffer(pScrn, drmmode, vout,
                                                             new_width, new_height)) {
                    xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                               "Failed to resize framebuffer for '%s' to %dx%d\n",
                               vout->name, new_width, new_height);
                    /* Continue anyway - mode change will still update CRTC state */
                } else {
                    /* Update FRAMEBUFFER_ID property with new framebuffer */
                    if (vout->randr_output) {
                        drmmode_xr_virtual_ensure_fb_id_property(pScrn, vout->randr_output, vout->framebuffer_id);
                    }
                    /* Update virtual output dimensions */
                    vout->width = new_width;
                    vout->height = new_height;
                }
            }
        }

        /* Update CRTC state */
        crtc->mode = *mode;
        crtc->x = x;
        crtc->y = y;
        crtc->rotation = rotation;
    }

    return TRUE;
}

static void
drmmode_xr_virtual_crtc_set_cursor_colors(xf86CrtcPtr crtc, int bg, int fg)
{
    /* Virtual CRTCs don't support hardware cursors */
}

static void
drmmode_xr_virtual_crtc_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
    /* Virtual CRTCs don't support hardware cursors */
}

static Bool
drmmode_xr_virtual_crtc_show_cursor(xf86CrtcPtr crtc)
{
    /* Virtual CRTCs don't support hardware cursors */
    return TRUE;
}

static void
drmmode_xr_virtual_crtc_hide_cursor(xf86CrtcPtr crtc)
{
    /* Virtual CRTCs don't support hardware cursors */
}

static Bool
drmmode_xr_virtual_crtc_load_cursor_argb(xf86CrtcPtr crtc, CARD32 *image)
{
    /* Virtual CRTCs don't support hardware cursors */
    return TRUE;
}

static void
drmmode_xr_virtual_crtc_gamma_set(xf86CrtcPtr crtc,
                                  uint16_t *red, uint16_t *green, uint16_t *blue,
                                  int size)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    int i;
    
    /* Store gamma values but don't program hardware */
    for (i = 0; i < size && i < 256; i++) {
        drmmode_crtc->lut_r[i] = red[i];
        drmmode_crtc->lut_g[i] = green[i];
        drmmode_crtc->lut_b[i] = blue[i];
    }
}

static void
drmmode_xr_virtual_crtc_destroy(xf86CrtcPtr crtc)
{
    /* Virtual CRTCs don't have hardware resources to free */
    /* The driver_private will be freed by xf86CrtcDestroy */
}

static Bool
drmmode_xr_virtual_crtc_set_scanout_pixmap(xf86CrtcPtr crtc, PixmapPtr ppix)
{
    /* For virtual CRTCs, we'll track the scanout pixmap but not program hardware */
    /* This will be used later to create DRM framebuffers for capture */
    (void)crtc;  /* unused */
    (void)ppix;  /* unused */
    
    /* Store reference to scanout pixmap for framebuffer export */
    
    return TRUE;
}

static void *
drmmode_xr_virtual_crtc_shadow_allocate(xf86CrtcPtr crtc, int width, int height)
{
    /* Virtual CRTCs use software framebuffers, shadow allocation not needed */
    return NULL;
}

static PixmapPtr
drmmode_xr_virtual_crtc_shadow_create(xf86CrtcPtr crtc, void *data, int width, int height)
{
    /* Virtual CRTCs use software framebuffers, shadow not needed */
    return NULL;
}

static void
drmmode_xr_virtual_crtc_shadow_destroy(xf86CrtcPtr crtc, PixmapPtr pPixmap, void *data)
{
    /* Virtual CRTCs use software framebuffers, shadow not needed */
}

/* Function table for virtual CRTCs */
static const xf86CrtcFuncsRec drmmode_xr_virtual_crtc_funcs = {
    .dpms = drmmode_xr_virtual_crtc_dpms,
    .set_mode_major = drmmode_xr_virtual_crtc_set_mode_major,
    .set_cursor_colors = drmmode_xr_virtual_crtc_set_cursor_colors,
    .set_cursor_position = drmmode_xr_virtual_crtc_set_cursor_position,
    .show_cursor_check = drmmode_xr_virtual_crtc_show_cursor,
    .hide_cursor = drmmode_xr_virtual_crtc_hide_cursor,
    .load_cursor_argb_check = drmmode_xr_virtual_crtc_load_cursor_argb,
    .gamma_set = drmmode_xr_virtual_crtc_gamma_set,
    .destroy = drmmode_xr_virtual_crtc_destroy,
    .set_scanout_pixmap = drmmode_xr_virtual_crtc_set_scanout_pixmap,
    .shadow_allocate = drmmode_xr_virtual_crtc_shadow_allocate,
    .shadow_create = drmmode_xr_virtual_crtc_shadow_create,
    .shadow_destroy = drmmode_xr_virtual_crtc_shadow_destroy,
};

/**
 * Create a virtual CRTC for virtual XR outputs
 * Virtual CRTCs are software-based and don't have real DRM CRTCs
 */
static xf86CrtcPtr
drmmode_xr_create_virtual_crtc(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
    xf86CrtcPtr crtc;
    drmmode_crtc_private_ptr drmmode_crtc;
    (void)drmmode;  /* unused for now */

    /* Create the CRTC using virtual function table */
    crtc = xf86CrtcCreate(pScrn, &drmmode_xr_virtual_crtc_funcs);
    if (!crtc) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to create virtual XR CRTC\n");
        return NULL;
    }

    /* Allocate private data structure */
    drmmode_crtc = calloc(1, sizeof(drmmode_crtc_private_rec));
    if (!drmmode_crtc) {
        xf86CrtcDestroy(crtc);
        return NULL;
    }

    crtc->driver_private = drmmode_crtc;

    /* Initialize virtual CRTC state */
    drmmode_crtc->drmmode = drmmode;
    drmmode_crtc->mode_crtc = NULL;  /* No real DRM CRTC for virtual CRTCs */
    drmmode_crtc->vblank_pipe = 0;   /* Virtual CRTCs don't have vblank pipes */
    drmmode_crtc->dpms_mode = DPMSModeOn;
    drmmode_crtc->cursor_up = FALSE;
    drmmode_crtc->next_msc = UINT64_MAX;
    drmmode_crtc->need_modeset = FALSE;
    drmmode_crtc->enable_flipping = FALSE;
    drmmode_crtc->flipping_active = FALSE;
    drmmode_crtc->vrr_enabled = FALSE;
    drmmode_crtc->use_gamma_lut = FALSE;

    /* Initialize lists */
    xorg_list_init(&drmmode_crtc->mode_list);
    xorg_list_init(&drmmode_crtc->tearfree.dri_flip_list);

    /* Virtual CRTCs don't have DRM properties - initialize empty prop arrays */
    memset(drmmode_crtc->props, 0, sizeof(drmmode_crtc->props));
    memset(drmmode_crtc->props_plane, 0, sizeof(drmmode_crtc->props_plane));

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Created virtual XR CRTC\n");

    return crtc;
}
