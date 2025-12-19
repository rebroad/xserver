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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/Xatom.h>
#include <xf86drm.h>
#include "xf86.h"
#include "xf86str.h"
#include "xf86Crtc.h"
#include "xf86RandR12.h"
#include "randrstr.h"
#include "xf86Modes.h"
#include "xf86DDC.h"
#include "drmmode_display.h"
#include "driver.h"

/* Forward declarations - these functions/objects are in drmmode_display.c */
extern void drmmode_output_create_resources(xf86OutputPtr output);
extern const xf86OutputFuncsRec drmmode_output_funcs;

#define XR_VIRTUAL_OUTPUT_NAME "XR-0"
#define XR_AR_MODE_PROPERTY "AR_MODE"

/* No-op create_resources for virtual output (doesn't have a real DRM connector) */
static void
drmmode_xr_virtual_create_resources(xf86OutputPtr output)
{
    /* Virtual output doesn't need DRM-specific properties */
    /* Modes and properties are set up in drmmode_xr_virtual_output_post_screen_init */
}

/* Custom function table for virtual XR output - initialized at runtime */
static xf86OutputFuncsRec drmmode_xr_virtual_output_funcs;

/* Initialize the virtual output function table (called once) */
static void
drmmode_xr_virtual_output_funcs_init(void)
{
    /* Copy from the regular output funcs, but override create_resources */
    drmmode_xr_virtual_output_funcs = drmmode_output_funcs;
    drmmode_xr_virtual_output_funcs.create_resources = drmmode_xr_virtual_create_resources;
}

/**
 * Create a virtual XR connector output.
 * This creates a synthetic RandR output that appears as "XR-0" in xrandr.
 * It's not backed by a real DRM connector, but by an off-screen buffer.
 */
Bool
drmmode_xr_virtual_output_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86OutputPtr output;
    drmmode_output_private_ptr drmmode_output;
    DisplayModePtr mode;
    static Bool funcs_initialized = FALSE;

    /* Check if already initialized */
    if (ms->xr_virtual_output)
        return TRUE;

    /* Initialize function table on first call */
    if (!funcs_initialized) {
        drmmode_xr_virtual_output_funcs_init();
        funcs_initialized = TRUE;
    }

    /* Create the output using a custom function table that has a no-op create_resources */
    output = xf86OutputCreate(pScrn, &drmmode_xr_virtual_output_funcs, XR_VIRTUAL_OUTPUT_NAME);
    if (!output) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to create virtual XR output\n");
        return FALSE;
    }

    /* Allocate private data */
    drmmode_output = calloc(1, sizeof(drmmode_output_private_rec));
    if (!drmmode_output) {
        xf86OutputDestroy(output);
        return FALSE;
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
    output->non_desktop = FALSE; /* Appears as normal desktop output */

    /* Create a minimal default mode so the output appears in xrandr */
    /* Modes will be created dynamically when Breezy enables the connector */
    mode = xf86ModesAdd(NULL, xf86CVTMode(1920, 1080, 60, FALSE, FALSE));
    if (mode) {
        mode->name = XNFstrdup("1920x1080");
        mode->type = M_T_DEFAULT;
        output->probed_modes = xf86ModesAdd(output->probed_modes, mode);
    }

    /* Don't create RandR output here - the screen doesn't exist yet.
     * We'll create it in drmmode_xr_virtual_output_post_screen_init() */
    output->randr_output = NULL;

    /* Store reference */
    ms->xr_virtual_output = output;
    ms->xr_virtual_enabled = FALSE; /* Disabled by default */
    ms->xr_ar_mode = FALSE;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Virtual XR connector (XR-0) created (RandR output will be created after screen init)\n");

    return TRUE;
}

/**
 * Set modes for the virtual XR output by converting DisplayModePtr to RRModePtr
 */
static void
drmmode_xr_virtual_set_modes(xf86OutputPtr output)
{
    if (!output->randr_output || !output->probed_modes)
        return;

    DisplayModePtr mode;
    RRModePtr *rrmodes = NULL;
    int nmode = 0;
    int npreferred = 0;

    /* Count modes */
    for (mode = output->probed_modes; mode; mode = mode->next)
        nmode++;

    if (nmode == 0)
        return;

    rrmodes = calloc(nmode, sizeof(RRModePtr));
    if (!rrmodes)
        return;

    nmode = 0;
    for (mode = output->probed_modes; mode; mode = mode->next) {
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
            rrmodes[nmode++] = rrmode;
            if (mode->type & M_T_PREFERRED)
                npreferred++;
        }
    }

    if (nmode > 0) {
        RROutputSetModes(output->randr_output, rrmodes, nmode, npreferred);
    }

    free(rrmodes);
}

/**
 * Create RandR output for virtual XR connector after screen is initialized.
 * This must be called from ScreenInit or later, when the screen exists.
 */
Bool
drmmode_xr_virtual_output_post_screen_init(ScrnInfoPtr pScrn)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86OutputPtr output = ms->xr_virtual_output;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "drmmode_xr_virtual_output_post_screen_init: called\n");

    if (!output) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "Virtual XR output not found in post_screen_init\n");
        return FALSE;
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "drmmode_xr_virtual_output_post_screen_init: output found, randr_output=%p\n",
               output->randr_output);

    /* If RandR output already exists, skip creating it again.
     * This can happen if xf86RandR12CreateObjects12 already created it. */
    if (output->randr_output) {
        /* Make sure modes are set */
        drmmode_xr_virtual_set_modes(output);
        return TRUE;
    }

    /* Now we can safely create the RandR output - screen exists */
    output->randr_output = RROutputCreate(xf86ScrnToScreen(pScrn),
                                          XR_VIRTUAL_OUTPUT_NAME,
                                          strlen(XR_VIRTUAL_OUTPUT_NAME),
                                          output);
    if (!output->randr_output) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to create RandR output for virtual XR\n");
        return FALSE;
    }

    /* Create AR_MODE property */
    {
        Atom name = MakeAtom(XR_AR_MODE_PROPERTY, strlen(XR_AR_MODE_PROPERTY), TRUE);
        INT32 value = 0; /* AR mode disabled by default */

        if (name != BAD_RESOURCE) {
            int err = RRConfigureOutputProperty(output->randr_output, name,
                                                FALSE, FALSE, TRUE,
                                                1, &value);
            if (err != 0) {
                xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                           "Failed to configure AR_MODE property: %d\n", err);
            } else {
                err = RRChangeOutputProperty(output->randr_output, name,
                                             XA_INTEGER, 32, PropModeReplace, 1,
                                             &value, FALSE, FALSE);
                if (err != 0) {
                    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                               "Failed to set AR_MODE property: %d\n", err);
                }
            }
        }
    }

    /* Set modes for the output */
    drmmode_xr_virtual_set_modes(output);

    /* Note: We do NOT call drmmode_output_create_resources() for the virtual output
     * because it expects a real DRM connector (mode_output), which we don't have.
     * The virtual output doesn't need DRM-specific properties. */
    RRPostPendingProperties(output->randr_output);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Virtual XR connector (XR-0) RandR output created\n");

    return TRUE;
}

/**
 * Clean up virtual XR connector
 */
void
drmmode_xr_virtual_output_fini(ScrnInfoPtr pScrn)
{
    modesettingPtr ms = modesettingPTR(pScrn);

    if (ms->xr_virtual_output) {
        xf86OutputPtr output = ms->xr_virtual_output;
        drmmode_output_private_ptr drmmode_output = output->driver_private;

        if (drmmode_output) {
            free(drmmode_output);
            output->driver_private = NULL;
        }

        xf86OutputDestroy(output);
        ms->xr_virtual_output = NULL;
    }

    ms->xr_virtual_enabled = FALSE;
    ms->xr_ar_mode = FALSE;
}

/**
 * Check if AR mode is enabled for the virtual XR connector
 */
Bool
drmmode_xr_get_ar_mode(ScrnInfoPtr pScrn)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86OutputPtr output = ms->xr_virtual_output;

    if (!output || !output->randr_output)
        return FALSE;

    /* Read AR_MODE property from RandR output */
    Atom prop = MakeAtom(XR_AR_MODE_PROPERTY, strlen(XR_AR_MODE_PROPERTY), TRUE);
    if (prop == BAD_RESOURCE)
        return FALSE;

    /* TODO: Actually read the property value */
    /* For now, return cached value */
    return ms->xr_ar_mode;
}

/**
 * Add a mode to the virtual XR connector
 * Called by Breezy when enabling the connector with specific dimensions
 */
Bool
drmmode_xr_add_mode(ScrnInfoPtr pScrn, int width, int height, int refresh)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86OutputPtr output = ms->xr_virtual_output;
    DisplayModePtr mode;
    char mode_name[32];

    if (!output)
        return FALSE;

    /* Check if mode already exists */
    for (mode = output->probed_modes; mode; mode = mode->next) {
        if (mode->HDisplay == width && mode->VDisplay == height &&
            (int)(mode->VRefresh + 0.5) == refresh) {
            return TRUE; /* Mode already exists */
        }
    }

    /* Create new mode */
    mode = xf86CVTMode(width, height, refresh, FALSE, FALSE);
    if (!mode)
        return FALSE;

    snprintf(mode_name, sizeof(mode_name), "%dx%d", width, height);
    mode->name = XNFstrdup(mode_name);
    mode->type = M_T_USERPREF;

    /* Add to output's mode list */
    output->probed_modes = xf86ModesAdd(output->probed_modes, mode);

    /* Force output to refresh its modes via get_modes callback */
    if (output->randr_output && output->funcs && output->funcs->get_modes) {
        DisplayModePtr all_modes = output->funcs->get_modes(output);
        (void)all_modes; /* Modes will be registered with RandR by the get_modes implementation */
        RROutputChanged(output->randr_output, TRUE);
    }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Added mode %dx%d@%dHz to virtual XR connector\n",
               width, height, refresh);

    return TRUE;
}

/**
 * Set AR mode for the virtual XR connector
 */
Bool
drmmode_xr_set_ar_mode(ScrnInfoPtr pScrn, Bool enabled)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86OutputPtr output = ms->xr_virtual_output;

    if (!output || !output->randr_output)
        return FALSE;

    Atom prop = MakeAtom(XR_AR_MODE_PROPERTY, strlen(XR_AR_MODE_PROPERTY), TRUE);
    if (prop == BAD_RESOURCE)
        return FALSE;

    INT32 value = enabled ? 1 : 0;
    int err = RRChangeOutputProperty(output->randr_output, prop,
                                     XA_INTEGER, 32, PropModeReplace, 1,
                                     &value, FALSE, FALSE);
    if (err != 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "Failed to set AR_MODE property: %d\n", err);
        return FALSE;
    }

    ms->xr_ar_mode = enabled;

    /* TODO: Implement actual AR mode logic:
     * - When enabled: hide physical XR connector, show virtual XR connector
     * - When disabled: show physical XR connector, hide virtual XR connector
     */

    return TRUE;
}

