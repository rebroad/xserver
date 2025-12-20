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

#define XR_MANAGER_OUTPUT_NAME "XR-Manager"
#define XR_AR_MODE_PROPERTY "AR_MODE"
#define XR_CREATE_OUTPUT_PROPERTY "CREATE_XR_OUTPUT"
#define XR_DELETE_OUTPUT_PROPERTY "DELETE_XR_OUTPUT"
#define XR_WIDTH_PROPERTY "XR_WIDTH"
#define XR_HEIGHT_PROPERTY "XR_HEIGHT"
#define XR_REFRESH_PROPERTY "XR_REFRESH"

/* Structure to track a dynamically created virtual output */
typedef struct _xr_virtual_output_rec {
    xf86OutputPtr output;          /* xf86OutputPtr for this virtual output */
    RROutputPtr randr_output;      /* RandR output */
    char *name;                    /* Output name (e.g., "XR-0", "XR-1") */
    int width;                     /* Current width */
    int height;                    /* Current height */
    int refresh;                   /* Current refresh rate */
    struct _xr_virtual_output_rec *next;  /* Linked list */
} xr_virtual_output_rec, *xr_virtual_output_ptr;

/* Get property atoms - use macros to avoid function call overhead */
#define XR_AR_MODE_ATOM() MakeAtom(XR_AR_MODE_PROPERTY, strlen(XR_AR_MODE_PROPERTY), TRUE)
#define XR_CREATE_OUTPUT_ATOM() MakeAtom(XR_CREATE_OUTPUT_PROPERTY, strlen(XR_CREATE_OUTPUT_PROPERTY), TRUE)
#define XR_DELETE_OUTPUT_ATOM() MakeAtom(XR_DELETE_OUTPUT_PROPERTY, strlen(XR_DELETE_OUTPUT_PROPERTY), TRUE)
#define XR_WIDTH_ATOM() MakeAtom(XR_WIDTH_PROPERTY, strlen(XR_WIDTH_PROPERTY), TRUE)
#define XR_HEIGHT_ATOM() MakeAtom(XR_HEIGHT_PROPERTY, strlen(XR_HEIGHT_PROPERTY), TRUE)
#define XR_REFRESH_ATOM() MakeAtom(XR_REFRESH_PROPERTY, strlen(XR_REFRESH_PROPERTY), TRUE)

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

/* Create or ensure AR_MODE property exists on an output */
static Bool
drmmode_xr_virtual_ensure_ar_mode_property(ScrnInfoPtr pScrn, RROutputPtr randr_output)
{
    Atom name = XR_AR_MODE_ATOM();
    INT32 value = 0;
    int err;

    if (name == BAD_RESOURCE) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "Failed to create AR_MODE atom\n");
        return FALSE;
    }

    /* Check if property already exists */
    if (RRQueryOutputProperty(randr_output, name)) {
        return TRUE;
    }

    /* Property doesn't exist, create it */
    err = RRConfigureOutputProperty(randr_output, name, FALSE, FALSE, TRUE, 1, &value);
    if (err != 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "Failed to configure AR_MODE property: %d\n", err);
        return FALSE;
    }

    err = RRChangeOutputProperty(randr_output, name, XA_INTEGER, 32, PropModeReplace, 1,
                                   &value, FALSE, FALSE);
    if (err != 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "Failed to set AR_MODE property: %d\n", err);
        return FALSE;
    }

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

/* Initialize the virtual output function table (called once) */
static void
drmmode_xr_virtual_output_funcs_init(void)
{
    /* Copy from the regular output funcs, but override create_resources */
    drmmode_xr_virtual_output_funcs = drmmode_output_funcs;
    drmmode_xr_virtual_output_funcs.create_resources = drmmode_xr_virtual_create_resources;
}

/**
 * Set modes for a virtual XR output by converting DisplayModePtr to RRModePtr
 */
static void
drmmode_xr_virtual_set_modes(xf86OutputPtr output, int width, int height, int refresh)
{
    if (!output->randr_output)
        return;

    DisplayModePtr mode;
    RRModePtr *rrmodes = NULL;
    int nmode = 0;
    char mode_name[32];

    /* Create a single mode with the specified dimensions */
    mode = xf86CVTMode(width, height, refresh, FALSE, FALSE);
    if (!mode)
        return;

    snprintf(mode_name, sizeof(mode_name), "%dx%d", width, height);
    mode->name = XNFstrdup(mode_name);
    mode->type = M_T_USERPREF | M_T_PREFERRED;

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
            rrmodes = calloc(1, sizeof(RRModePtr));
            if (rrmodes) {
                rrmodes[0] = rrmode;
                nmode = 1;
            }
        }
    }

    if (nmode > 0) {
        RROutputSetModes(output->randr_output, rrmodes, nmode, 1);
    }

    free(rrmodes);
    xf86DeleteMode(&output->probed_modes, mode);
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

    /* Create RandR output */
    output->randr_output = RROutputCreate(pScreen, name, strlen(name), output);
    if (!output->randr_output) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to create RandR output for '%s'\n", name);
        free(drmmode_output);
        xf86OutputDestroy(output);
        return NULL;
    }

    /* Set connection to disconnected initially (will be connected when enabled) */
    RROutputSetConnection(output->randr_output, RR_Disconnected);

    /* Set modes */
    drmmode_xr_virtual_set_modes(output, width, height, refresh);

    /* Create AR_MODE property */
    drmmode_xr_virtual_ensure_ar_mode_property(pScrn, output->randr_output);

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

    RRPostPendingProperties(output->randr_output);
    RROutputChanged(output->randr_output, TRUE);
    RRTellChanged(pScreen);

    /* Allocate and initialize virtual output record */
    vout = calloc(1, sizeof(xr_virtual_output_rec));
    if (!vout) {
        RROutputDestroy(output->randr_output);
        free(drmmode_output);
        xf86OutputDestroy(output);
        return NULL;
    }

    vout->output = output;
    vout->randr_output = output->randr_output;
    vout->name = strdup(name);
    vout->width = width;
    vout->height = height;
    vout->refresh = refresh;

    /* Add to list */
    vout->next = ms->xr_virtual_outputs;
    ms->xr_virtual_outputs = vout;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "Created virtual XR output '%s' (%dx%d@%dHz)\n",
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

    /* Destroy RandR output */
    if (vout->randr_output) {
        RROutputDestroy(vout->randr_output);
        RRTellChanged(xf86ScrnToScreen(pScrn));
    }

    /* Destroy xf86Output */
    if (vout->output) {
        drmmode_output_private_ptr drmmode_output = vout->output->driver_private;
        if (drmmode_output)
            free(drmmode_output);
        xf86OutputDestroy(vout->output);
    }

    /* Free name and record */
    if (vout->name)
        free(vout->name);
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
    if (strcmp(prop_name, XR_CREATE_OUTPUT_PROPERTY) != 0 &&
        strcmp(prop_name, XR_DELETE_OUTPUT_PROPERTY) != 0) {
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

    if (strcmp(prop_name, XR_CREATE_OUTPUT_PROPERTY) == 0) {
        /* Format: "XR-0:1920:1080:60" or "XR-0:1920:1080" (refresh defaults to 60) */
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
    } else if (strcmp(prop_name, XR_DELETE_OUTPUT_PROPERTY) == 0) {
        /* Format: "XR-0" */
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

    if (strcmp(prop_name, XR_WIDTH_PROPERTY) == 0) {
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
    output->non_desktop = FALSE;

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

    if (!output) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
                   "XR-Manager output not found in post_screen_init\n");
        return FALSE;
    }

    /* If RandR output already exists, we're done */
    if (output->randr_output) {
        return TRUE;
    }

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

    /* Create CREATE_XR_OUTPUT and DELETE_XR_OUTPUT properties */
    create_atom = XR_CREATE_OUTPUT_ATOM();
    delete_atom = XR_DELETE_OUTPUT_ATOM();

    if (create_atom != BAD_RESOURCE) {
        /* Configure as mutable STRING property (pending=FALSE, range=FALSE, immutable=FALSE) */
        char empty_str[] = "";
        RRConfigureOutputProperty(output->randr_output, create_atom,
                                 FALSE, FALSE, FALSE,
                                 1, (unsigned char *)empty_str);
        /* Set initial empty value as STRING */
        RRChangeOutputProperty(output->randr_output, create_atom,
                              XA_STRING, 8, PropModeReplace, 1,
                              (unsigned char *)empty_str, FALSE, FALSE);
    }
    if (delete_atom != BAD_RESOURCE) {
        /* Configure as mutable STRING property */
        char empty_str[] = "";
        RRConfigureOutputProperty(output->randr_output, delete_atom,
                                 FALSE, FALSE, FALSE,
                                 1, (unsigned char *)empty_str);
        /* Set initial empty value as STRING */
        RRChangeOutputProperty(output->randr_output, delete_atom,
                              XA_STRING, 8, PropModeReplace, 1,
                              (unsigned char *)empty_str, FALSE, FALSE);
    }

    RRPostPendingProperties(output->randr_output);

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
    ms->xr_ar_mode = FALSE;
}

/**
 * Check if AR mode is enabled for a virtual XR connector
 */
Bool
drmmode_xr_get_ar_mode(ScrnInfoPtr pScrn)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    return ms->xr_ar_mode;
}

/**
 * Set AR mode for virtual XR connectors
 */
Bool
drmmode_xr_set_ar_mode(ScrnInfoPtr pScrn, Bool enabled)
{
    modesettingPtr ms = modesettingPTR(pScrn);
    ms->xr_ar_mode = enabled;

    /* TODO: Implement actual AR mode logic:
     * - When enabled: hide physical XR connector, show virtual XR connector
     * - When disabled: show physical XR connector, hide virtual XR connector
     */

    return TRUE;
}
