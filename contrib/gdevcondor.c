/* Copyright (C) 2001-2021 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  1305 Grant Avenue - Suite 200, Novato,
   CA 94945, U.S.A., +1(415)492-9861, for further information.
*/

/* Condor DeviceN process color model device.
 *
 * Copyright (c) 2021 Soft Horizons, Inc. All Rights Reserved.
 * https://johndesrosiers.com
 * 
 * You probably don't have one and I can't even say what a Condor is,
 * but you may consider this an interesting example. Features:
 * - DeviceN with a constant number of components <= 7. Leverages gdevdevn.
 * - Chunky byte output.
 * - Multithreaded rendering renders directly to seekable OutputFile.
 * - Has NextOutputFile param which allows the PS program to queue up
 *   an OutputFile without blocking the interpreter thread to wait for
 *   rendering to end.
 * - Detects and marks (by ORing 0x80) pixels that are actually written,
 *   as opposed to page-clearing. This is very useful for
 *   making overlay pixmaps for a separate process that will overlay
 *   this output on top of a background, knocking out only pixels
 *   actually written, e.g. text overlay. Note that legit white pixels
 *   or "white" pixels in halftones are marked, will knock out background.
 *
 */

#include "math_.h"
#include "string_.h"
#include "memory_.h"
#include "gxgetbit.h"
#include "gdevprn.h"
#include "gdevdevn.h"
#include "gdevdevnprn.h"
#include "gsparam.h"
#include "gxsync.h"
#include "gxiodev.h"
#include "gsfname.h"
#include "gssprintf.h"


#define MAX_SPOTS 3 /* Max # spot colors (max 3) */
#define COMPONENT_COUNT (4 + MAX_SPOTS)

/*
 * A structure definition for a Condor printer type device, based on DeviceN Printer
 */
typedef struct gx_condor_prn_device_s {
    gx_devn_prn_device_common;

    /* NOTE that these won't be copied to clist devices since setup_device_and_mem_for_thread()
     * calls devn_copy_params directly, and devn doesn't know about Condor-specific fields.
     */
    int graphic_type_tag_valid;
    char next_fname[prn_fname_sizeof];	/* NextOutputFile */
    char report_fname[gp_file_name_sizeof]; /* ReportFile */
} gx_condor_prn_device;


/* Get parameters just for Condor-specific params. gx_condor_prn_get_params does the rest. */
static int
condor_get_params(gx_device* pdev, gs_param_list* plist)
{
    gx_condor_prn_device* ppdev = (gx_condor_prn_device*)pdev;
    int code = 0;
    gs_param_string ofns;
    gs_param_string logfns;

    ofns.data = (const byte*)ppdev->next_fname,
        ofns.size = strlen(ppdev->next_fname),
        ofns.persistent = false;
    code = param_write_string(plist, "NextOutputFile", &ofns);
    if (code < 0)
        return code;

    logfns.data = (const byte*)ppdev->report_fname,
        logfns.size = strlen(ppdev->report_fname),
        logfns.persistent = false;
    code = param_write_string(plist, "ReportFile", &logfns);
    if (code < 0)
        return code;

    return code;
}

static int
compare_equivalent_cmyk_color_params(const equivalent_cmyk_color_params* pequiv_colors1, const equivalent_cmyk_color_params* pequiv_colors2)
{
    int i;
    if (pequiv_colors1->all_color_info_valid != pequiv_colors2->all_color_info_valid)
        return(1);
    for (i = 0; i < GX_DEVICE_MAX_SEPARATIONS; i++) {
        if (pequiv_colors1->color[i].color_info_valid != pequiv_colors2->color[i].color_info_valid)
            return(1);
        if (pequiv_colors1->color[i].c != pequiv_colors2->color[i].c)
            return(1);
        if (pequiv_colors1->color[i].m != pequiv_colors2->color[i].m)
            return(1);
        if (pequiv_colors1->color[i].y != pequiv_colors2->color[i].y)
            return(1);
        if (pequiv_colors1->color[i].k != pequiv_colors2->color[i].k)
            return(1);
    }
    return(0);
}

static bool separations_equal(const gs_separations* p1, const gs_separations* p2)
{
    int k;

    if (p1->num_separations != p2->num_separations)
        return false;
    for (k = 0; k < p1->num_separations; k++) {
        if (p1->names[k].size != p2->names[k].size)
            return false;
        else if (p1->names[k].size > 0) {
            if (memcmp(p1->names[k].data, p2->names[k].data, p1->names[k].size) != 0)
                return false;
        }
    }
    return true;
}

static bool devn_params_equal(const gs_devn_params* p1, const gs_devn_params* p2)
{
    if (p1->bitspercomponent != p2->bitspercomponent)
        return false;
    if (p1->max_separations != p2->max_separations)
        return false;
    if (p1->num_separation_order_names != p2->num_separation_order_names)
        return false;
    if (p1->num_std_colorant_names != p2->num_std_colorant_names)
        return false;
    if (p1->page_spot_colors != p2->page_spot_colors)
        return false;
    if (!separations_equal(&p1->pdf14_separations, &p2->pdf14_separations))
        return false;
    if (!separations_equal(&p1->separations, &p2->separations))
        return false;
    if (memcmp(p1->separation_order_map, p2->separation_order_map, sizeof(gs_separation_map)) != 0)
        return false;
    if (p1->std_colorant_names != p2->std_colorant_names)
        return false;
    return true;
}

/* Validate an OutputFile or LogFile name by checking any %-formats. */
static int
validate_output_file(const gs_param_string* ofs, gs_memory_t* memory)
{
    gs_parsed_file_name_t parsed;
    const char* fmt;

    return gx_parse_output_file_name(&parsed, &fmt, (const char*)ofs->data,
        ofs->size, memory) >= 0;
}

static int
condor_devn_printer_put_params(gx_device* pdev, gs_param_list* plist,
    gs_devn_params* pdevn_params, equivalent_cmyk_color_params* pequiv_colors)
{
    /* Here we'd like to just do this:
      *   code = devn_printer_put_params(pdev, plist, pdevn_params, pequiv_colors);
      *   pdev->color_info.depth = 8; //always do this since we ignore attempts to reset
      * but that will conflict with our setting color_info.depth = 8 since that routine calls
      * devn_put_params() which recomputes depth to whatever, then immediately calls
      * gdev_prn_put_params() which will cause an error because BitsPerPixel (derived from depth)
      * is considered read-only.
      *
      * So, we copy devn_printer_put_params() and hack it to reset depth = 8
      * immediately after devn_put_params()
      */
    int code;

    /* Save current data in case we have a problem */
    gx_device_color_info save_info = pdev->color_info;
    gs_devn_params saved_devn_params = *pdevn_params;
    equivalent_cmyk_color_params saved_equiv_colors;

    if (pequiv_colors != NULL)
        saved_equiv_colors = *pequiv_colors;

    /* Use utility routine to handle parameters */
    code = devn_put_params(pdev, plist, pdevn_params, pequiv_colors);

    /* HACK FOR CONDOR RIGHT HERE */
    pdev->color_info.depth = 8; /* always 8, but devn_put_params recomputes it */

    /* Check for default printer parameters */
    if (code >= 0)
        code = gdev_prn_put_params(pdev, plist);

    /* If we have an error then restore original data. */
    if (code < 0) {
        pdev->color_info = save_info;
        *pdevn_params = saved_devn_params;
        if (pequiv_colors != NULL)
            *pequiv_colors = saved_equiv_colors;
        return code;
    }

    /* If anything changed, then close the device, etc. */
    if (!gx_color_info_equal(&pdev->color_info, &save_info) ||
        !devn_params_equal(pdevn_params, &saved_devn_params) ||
        (pequiv_colors != NULL &&
            compare_equivalent_cmyk_color_params(pequiv_colors, &saved_equiv_colors))) {
        gs_closedevice(pdev);
        /* Reset the sparable and linear shift, masks, bits. */
        set_linear_color_bits_mask_shift(pdev);
    }
    /*
     * Also check for parameters which are being passed from the PDF 1.4
     * compositior clist write device.  This device needs to pass info
     * to the PDF 1.4 compositor clist reader device.  However this device
     * is not crated until the clist is being read.  Thus we have to buffer
     * this info in the output device.   (This is only needed for devices
     * which support spot colors.)
     */
    code = pdf14_put_devn_params(pdev, pdevn_params, plist);
    return code;
}


/*
 * Utility routine for handling DeviceN related parameters in a
 * standard raster printer type device.
 */
static int
condor_printer_put_params(gx_device* pdev, gs_param_list* plist,
    gs_devn_params* pdevn_params, equivalent_cmyk_color_params* pequiv_colors)
{
    int code;
    int ecode = 0;
    gx_condor_prn_device* ppdev = (gx_condor_prn_device*)pdev;
    gs_param_string ofns;
    gs_param_string logfns;
    const char* param_name;

    switch (code = param_read_string(plist, (param_name = "NextOutputFile"), &ofns)) {
    case 0:
        if (sizeof(ppdev->next_fname) <= ofns.size)
            code = gs_error_limitcheck;
        else if (pdev->LockSafetyParams &&
            bytes_compare(ofns.data, ofns.size,
                (const byte*)ppdev->next_fname, strlen(ppdev->next_fname))) {
            code = gs_error_invalidaccess;
        }
        else
            code = validate_output_file(&ofns, pdev->memory);
        if (code >= 0)
            break;
        /* fall through */
    default:
        ecode = code;
        param_signal_error(plist, param_name, ecode);
        /* fall through */
    case 1:
        ofns.data = 0;
        break;
    }

    switch (code = param_read_string(plist, (param_name = "ReportFile"), &logfns)) {
    case 0:
        if (sizeof(ppdev->report_fname) <= logfns.size)
            code = gs_error_limitcheck;
        else if (pdev->LockSafetyParams &&
            bytes_compare(logfns.data, logfns.size,
                (const byte*)ppdev->report_fname, strlen(ppdev->report_fname))) {
            code = gs_error_invalidaccess;
        }
        else
            code = validate_output_file(&logfns, pdev->memory);
        if (code >= 0)
            break;
        /* fall through */
    default:
        ecode = code;
        param_signal_error(plist, param_name, ecode);
        /* fall through */
    case 1:
        logfns.data = 0;
        break;
    }

    /* Here we'd like to just do this:
     *   code = devn_printer_put_params(pdev, plist, pdevn_params, pequiv_colors);
     *   pdev->color_info.depth = 8; //always do this since we ignore attempts to reset
     * but that will conflict with our setting color_info.depth = 8 since that routine calls
     * devn_put_params() which recomputes depth to whatever, then immediately calls
     * gdev_prn_put_params() which will cause an error because BitsPerPixel (derived from depth)
     * is considered read-only.
     * 
     * So, we copy devn_printer_put_params and hack it to reset depth = 8 
     * immediately after devn_put_params()
     */
    code = condor_devn_printer_put_params(pdev, plist, pdevn_params, pequiv_colors);
    if (code < 0)
        ecode = code;

    if (ecode < 0)
        return ecode;

    /* If we make it here successfully, set pre-validated params */
    if (ofns.data != 0) {
        memcpy(ppdev->next_fname, ofns.data, ofns.size);
        ppdev->next_fname[ofns.size] = 0;
    }
    if (logfns.data != 0) {
        memcpy(ppdev->report_fname, logfns.data, logfns.size);
        ppdev->report_fname[logfns.size] = 0;
    }

    return ecode;
}

/* Define the device parameters. */
#ifndef X_DPI
#  define X_DPI 600
#endif
#ifndef Y_DPI
#  define Y_DPI 600
#endif

/* The device descriptor */
static dev_proc_open_device(condor_spotcmyk_prn_open);
static dev_proc_print_page(condor_spotcmyk_print_page);
static dev_proc_get_params(gx_condor_prn_get_params);
static dev_proc_put_params(gx_condor_prn_put_params);
static dev_proc_encode_color(gx_condor_prn_encode_color);
static dev_proc_decode_color(gx_condor_prn_decode_color);
static dev_proc_output_page(gx_condor_prn_output_page);


/* GC procedures taken from gdevdevn. We should really find a way
 * to copy pointers to devn's procedures instead, maybe once GS 9.55
 * ships; drivers will get initialize_device_procs() we can use.
 */
static
ENUM_PTRS_WITH(gx_condor_prn_device_enum_ptrs, gx_condor_prn_device* pdev)
{
    if (index < pdev->devn_params.separations.num_separations)
        ENUM_RETURN(pdev->devn_params.separations.names[index].data);
    ENUM_PREFIX(st_device_printer,
        pdev->devn_params.separations.num_separations);
}

ENUM_PTRS_END
static RELOC_PTRS_WITH(gx_condor_prn_device_reloc_ptrs, gx_condor_prn_device* pdev)
{
    RELOC_PREFIX(st_device_printer);
    {
        int i;

        for (i = 0; i < pdev->devn_params.separations.num_separations; ++i) {
            RELOC_PTR(gx_condor_prn_device, devn_params.separations.names[i].data);
        }
    }
}
RELOC_PTRS_END

static void
gx_condor_prn_device_finalize(const gs_memory_t* cmem, void* vpdev)
{
    devn_free_params((gx_device*)vpdev);
    gx_device_finalize(cmem, vpdev);
}

/* Even though gx_condor_prn_device_finalize is the same as gx_device_finalize, */
/* we need to implement it separately because st_composite_final */
/* declares all 3 procedures as private. */
static void
static_gx_condor_prn_device_finalize(const gs_memory_t* cmem, void* vpdev)
{
    gx_condor_prn_device_finalize(cmem, vpdev);
}

gs_public_st_composite_final(st_gx_condor_prn_device, gx_condor_prn_device,
    "gx_condor_prn_device", gx_condor_prn_device_enum_ptrs, gx_condor_prn_device_reloc_ptrs,
    static_gx_condor_prn_device_finalize);

/*
 * Macro definition for Condor procedures, based on DeviceN
 */
#define device_procs()\
{       condor_spotcmyk_prn_open,\
        gx_default_get_initial_matrix,\
        NULL,                           /* sync_output */\
        gx_condor_prn_output_page,      /* output_page */\
        gdev_prn_close,                 /* close */\
        NULL,                           /* map_rgb_color - not used */\
        NULL,                           /* map_color_rgb - not used */\
        NULL,                           /* fill_rectangle */\
        NULL,                           /* tile_rectangle */\
        NULL,                           /* copy_mono */\
        NULL,                           /* copy_color */\
        NULL,                           /* draw_line */\
        NULL,                           /* get_bits */\
        gx_condor_prn_get_params,         /* get_params */\
        gx_condor_prn_put_params,         /* put_params */\
        NULL,                           /* map_cmyk_color - not used */\
        NULL,                           /* get_xfont_procs */\
        NULL,                           /* get_xfont_device */\
        NULL,                           /* map_rgb_alpha_color */\
        gx_page_device_get_page_device, /* get_page_device */\
        NULL,                           /* get_alpha_bits */\
        NULL,                           /* copy_alpha */\
        NULL,                           /* get_band */\
        NULL,                           /* copy_rop */\
        NULL,                           /* fill_path */\
        NULL,                           /* stroke_path */\
        NULL,                           /* fill_mask */\
        NULL,                           /* fill_trapezoid */\
        NULL,                           /* fill_parallelogram */\
        NULL,                           /* fill_triangle */\
        NULL,                           /* draw_thin_line */\
        NULL,                           /* begin_image */\
        NULL,                           /* image_data */\
        NULL,                           /* end_image */\
        NULL,                           /* strip_tile_rectangle */\
        NULL,                           /* strip_copy_rop */\
        NULL,                           /* get_clipping_box */\
        NULL,                           /* begin_typed_image */\
        NULL,                           /* get_bits_rectangle */\
        NULL,                           /* map_color_rgb_alpha */\
        NULL,                           /* create_compositor */\
        NULL,                           /* get_hardware_params */\
        NULL,                           /* text_begin */\
        NULL,                           /* finish_copydevice */\
        NULL,                           /* begin_transparency_group */\
        NULL,                           /* end_transparency_group */\
        NULL,                           /* begin_transparency_mask */\
        NULL,                           /* end_transparency_mask */\
        NULL,                           /* discard_transparency_layer */\
        gx_devn_prn_get_color_mapping_procs,/* get_color_mapping_procs */\
        gx_devn_prn_get_color_comp_index,/* get_color_comp_index */\
        gx_condor_prn_encode_color,       /* encode_color */\
        gx_condor_prn_decode_color,       /* decode_color */\
        NULL,                           /* pattern_manage */\
        NULL,                           /* fill_rectangle_hl_color */\
        NULL,                           /* include_color_space */\
        NULL,                           /* fill_linear_color_scanline */\
        NULL,                           /* fill_linear_color_trapezoid */\
        NULL,                           /* fill_linear_color_triangle */\
        gx_devn_prn_update_spot_equivalent_colors,/* update_spot_equivalent_colors */\
        gx_devn_prn_ret_devn_params     /* ret_devn_params */\
}

#define gx_condor_prn_device_body(procs, dname, ncomp, pol, depth, mg, mc, cn)\
    std_device_full_body_type_extended(gx_condor_prn_device, &procs, dname,\
          &st_gx_condor_prn_device,\
          (int)((long)(DEFAULT_WIDTH_10THS) * (X_DPI) / 10),\
          (int)((long)(DEFAULT_HEIGHT_10THS) * (Y_DPI) / 10),\
          X_DPI, Y_DPI,\
          4 + MAX_SPOTS,       /* MaxComponents */\
          ncomp,                /* NumComp */\
          pol,                  /* Polarity */\
          depth, 0,             /* Depth, GrayIndex */\
          mg, mc,               /* MaxGray, MaxColor */\
          mg + 1, mc + 1,       /* DitherGray, DitherColor */\
          GX_CINFO_SEP_LIN,     /* Linear & Separable */\
          cn,                   /* Process color model name */\
          0, 0,                 /* offsets */\
          0, 0, 0, 0            /* margins */\
        ),\
        prn_device_body_rest_(condor_spotcmyk_print_page)

/*
 * Device with CMYK and spot color support
 */
static const gx_device_procs spot_cmyk_procs = device_procs();

const gx_condor_prn_device gs_condor_device =
{
    gx_condor_prn_device_body(spot_cmyk_procs, "condor", COMPONENT_COUNT, GX_CINFO_POLARITY_SUBTRACTIVE, 8, 1, 1, "DeviceCMYK"),
    /* DeviceN device specific parameters */
    { 1,                        /* Bits per color - must match ncomp, depth, etc. above */
      DeviceCMYKComponents,     /* Names of color model colorants */
      4,                        /* Number colorants for CMYK */
      COMPONENT_COUNT,          /* MaxSeparations */
      -1,                       /* PageSpotColors has not been specified */
      {0},                      /* SeparationNames */
      0,                        /* SeparationOrder names */
      {0, 1, 2, 3, 4, 5, 6, 7 } /* Initial component SeparationOrder */
    },
    {0},                        /* equiv cmyk colors */
    0,                          /* graphic_type_tag_valid */
    "",                         /* next_fname */
    ""                          /* report_fname*/
};

#if 0
void
test_extraction_speed(gs_memory_t* memory)
{
    unsigned swidth = 7200;
    unsigned dwidth = 900;
    unsigned height = 3600;

    byte* dest = (byte *)gs_malloc(memory, dwidth * height, 1, "tester");
    byte* src = (byte*)gs_malloc(memory, swidth * height, 1, "tester src");
    byte lookup[256];

    unsigned scnt = swidth * height / 8;
    byte accum = 0;
    for (unsigned cnt = 0; cnt < scnt; ++cnt)
    {
        for (unsigned bit = 0; bit < 8; ++bit)
            accum = 2 * accum + (*src++ & 0x4 ? 1 : 0);
        *dest++ = accum;
        accum = 0;
    }
    accum = 0;
}
#endif

static int
noclose(FILE* f)
{
    return 0;
}

/* Open the output file for a device. Lifted from gx_device_open_output() */
static int
open_report_file(const gx_device* dev, char* fname,
    bool binary, bool positionable, gp_file** pfile)
{
    gs_parsed_file_name_t parsed;
    const char* fmt;
    char* pfname = (char*)gs_alloc_bytes(dev->memory, gp_file_name_sizeof, "gdevcondor_open_report_file(fname)");
    int code;

    if (pfname == NULL) {
        code = gs_note_error(gs_error_VMerror);
        goto done;
    }

    if (strlen(fname) == 0) {
        code = gs_note_error(gs_error_undefinedfilename);
        emprintf1(dev->memory, "Device '%s' requires an output file but no file was specified.\n", dev->dname);
        goto done;
    }
    code = gx_parse_output_file_name(&parsed, &fmt, fname, strlen(fname), dev->memory);
    if (code < 0) {
        goto done;
    }

    if (parsed.iodev && !strcmp(parsed.iodev->dname, "%stdout%")) {
        if (parsed.fname) {
            code = gs_note_error(gs_error_undefinedfilename);
            goto done;
        }
        *pfile = gp_file_FILE_alloc(dev->memory);
        if (*pfile == NULL) {
            code = gs_note_error(gs_error_VMerror);
            goto done;
        }
        gp_file_FILE_set(*pfile, dev->memory->gs_lib_ctx->core->fstdout, noclose);
        /* Force stdout to binary. */
        code = gp_setmode_binary_impl(dev->memory->gs_lib_ctx->core->fstdout, true);
        goto done;
    }
    else if (parsed.iodev && !strcmp(parsed.iodev->dname, "%pipe%")) {
        positionable = false;
    }
    if (fmt) {						/* filename includes "%nnd" */
        long count1 = dev->PageCount + 1;

        while (*fmt != 'l' && *fmt != '%')
            --fmt;
        if (*fmt == 'l')
            gs_sprintf(pfname, parsed.fname, count1);
        else
            gs_sprintf(pfname, parsed.fname, (int)count1);
    }
    else if (parsed.len && strchr(parsed.fname, '%'))	/* filename with "%%" but no "%nnd" */
        gs_sprintf(pfname, parsed.fname);
    else
        pfname[0] = 0; /* 0 to use "fname", not "pfname" */
    if (pfname[0]) {
        parsed.fname = pfname;
        parsed.len = strlen(parsed.fname);
    }
    if (parsed.iodev &&
        (positionable || parsed.iodev != iodev_default(dev->memory))) {
        char fmode[4];

        if (!parsed.fname) {
            code = gs_note_error(gs_error_undefinedfilename);
            goto done;
        }
        strcpy(fmode, binary ? "ab" : "a");
        if (positionable)
            strcat(fmode, "+");
        code = parsed.iodev->procs.gp_fopen(parsed.iodev, parsed.fname, fmode,
            pfile, NULL, 0, dev->memory);
        if (code)
            emprintf1(dev->memory,
                "**** Could not open the file %s .\n",
                parsed.fname);
    }
    else {
        if (pfname[0] || fname[0]) { /* no null name allowed since never log to printer */
            *pfile = gp_open_printer(dev->memory, (pfname[0] ? pfname : fname), binary);
        }
        if (!(*pfile)) {
            emprintf1(dev->memory, "**** Could not open the file '%s'.\n", (pfname[0] ? pfname : fname));
            code = gs_note_error(gs_error_invalidfileaccess);
        }
    }

done:
    if (pfname != NULL)
        gs_free_object(dev->memory, pfname, "gdevcondor_open_report_file(fname)");

    return(code);
}

/* Close the report file for a device. */
static int
close_report_file(const gx_device* dev, const char* fname,
    gp_file* file)
{
    gs_parsed_file_name_t parsed;
    const char* fmt;
    int code = gx_parse_output_file_name(&parsed, &fmt, fname, strlen(fname),
        dev->memory);

    if (code < 0)
        return code;
    if (parsed.iodev) {
        if (!strcmp(parsed.iodev->dname, "%stdout%"))
            return 0;
        /* NOTE: fname is unsubstituted if the name has any %nnd formats. */
        if (parsed.iodev != iodev_default(dev->memory))
            return parsed.iodev->procs.fclose(parsed.iodev, file);
    }
    gp_close_printer(file, (parsed.fname ? parsed.fname : fname));

    return 0;
}

static const char*
error_string(int errn) {
    static const char* messages[] = {
        "OK",                   /*gs_error_ok = 0*/
        "Unknown Error",        /*gs_error_unknownerror = -1 */
        "Dictonary full",       /*gs_error_dictfull = -2, */
        "Dict stack overflow",  /*gs_error_dictstackoverflow = -3,*/
        "Dict stack underflow", /*gs_error_dictstackunderflow = -4,*/
        "Exec stack overflow",  /*gs_error_execstackoverflow = -5*/
        "Interrupt",            /*gs_error_interrupt = -6*/
        "Invalid access",       /*gs_error_invalidaccess = -7*/
        "Invalid exit",         /*gs_error_invalidexit = -8*/
        "Invalid file access",  /*gs_error_invalidfileaccess = -9*/
        "Invalid font",         /*gs_error_invalidfont = -10*/
        "Invalid restore",      /*gs_error_invalidrestore = -11*/
        "IO error",             /*gs_error_ioerror = -12*/
        "Limit check",          /*gs_error_limitcheck = -13*/
        "No current point",     /*gs_error_nocurrentpoint = -14*/
        "Range check",          /*gs_error_rangecheck = -15*/
        "Stack overflow",       /*gs_error_stackoverflow = -16*/
        "Stack underflow",      /*gs_error_stackunderflow = -17*/
        "Syntax error",         /*gs_error_syntaxerror = -18*/
        "Timeout",              /*gs_error_timeout = -19*/
        "Type check",           /*gs_error_typecheck = -20*/
        "Undefined",            /*gs_error_undefined = -21*/
        "Undefined file name",  /*gs_error_undefinedfilename = -22*/
        "Undefined result",     /*gs_error_undefinedresult = -23*/
        "Unmatched mark",       /*gs_error_unmatchedmark = -24*/
        "VM error",             /*gs_error_VMerror = -25*/
        "Configuration error",  /*gs_error_configurationerror = -26*/
        "Undefined resource",   /*gs_error_undefinedresource = -27*/
        "Unregistered",         /*gs_error_unregistered = -28*/
        "Invalid context"       /*gs_error_invalidcontext = -29*/
    };
    int idx = -errn;
    return (idx >= 0 && idx < sizeof(messages) / sizeof(messages[0]))
        ? messages[idx] : "unknown error";
}

/* Open the condor device */
int
condor_spotcmyk_prn_open(gx_device* pdev)
{
    gx_condor_prn_device* pcondor = (gx_condor_prn_device*)pdev;

    /* For the planar device we need to set up the bit depth of each plane.
       For other devices this is handled in check_device_separable where
       we compute the bit shift for the components etc. */
    int k;
    for (k = 0; k < GS_CLIENT_COLOR_MAX_COMPONENTS; k++) {
        pdev->color_info.comp_bits[k] = 1;
    }

    /* Note that we set this in open, but that copied devices will default back to false.
     * That means that the direct renderers and clist writers will see this true, but 
     * but the copies that the clist renderer makes for playback (w/o calling this open rtn)
     * will have this flag false since they don't call open(). That's perfect since
     * graphic_type_tag isn't valid during clist render phase, and that's exactly what we
     * want to detect.
     */
    pcondor->graphic_type_tag_valid = 1;

    /* return spotcmyk_prn_open(pdev); I can't get it to link, so copied the code below: */
    {
        int code = gdev_prn_open(pdev);

        while (pdev->child)
            pdev = pdev->child;

        set_linear_color_bits_mask_shift(pdev);
        pdev->color_info.separable_and_linear = GX_CINFO_SEP_LIN;
        return code;
    }
}

/* Get parameters. */
int
gx_condor_prn_get_params(gx_device* dev, gs_param_list* plist)
{
    int code = gx_devn_prn_get_params(dev, plist);
    if (code < 0)
        return code;

    return condor_get_params(dev, plist);
}

/* Set parameters. */
int
gx_condor_prn_put_params(gx_device* dev, gs_param_list* plist)
{
    gx_devn_prn_device* pdev = (gx_devn_prn_device*)dev;

    return condor_printer_put_params(dev, plist, &pdev->devn_params,
        &pdev->equiv_cmyk_colors);
}

/*
 * Encode a list of colorant values into a gx_color_index_value.
 */
gx_color_index
gx_condor_prn_encode_color(gx_device* dev, const gx_color_value colors[])
{
    gx_devn_prn_device* pdev = (gx_devn_prn_device*)dev;
    gx_condor_prn_device* pcondor = (gx_condor_prn_device*)dev;
    int bpc = pdev->devn_params.bitspercomponent;
    int isfillpage;
    gx_color_index color = 0;
    uchar ncomp = pdev->color_info.num_components;
    int i;
    COLROUND_VARS;

    if (ncomp > COMPONENT_COUNT) ncomp = COMPONENT_COUNT;
    COLROUND_SETUP(bpc);
    for (i = ncomp-1; i >= 0; i--) {
        color <<= bpc;
        color |= COLROUND_ROUND(colors[i]);
    } //i.e. Spot=0x10, C=0x08, M=0x08, etc

    /* OR 0x80 onto each pixel if it's an opaque mark, but not a background fillpage.
     *
     * dev->graphic_type_tag == GS_UNTOUCHED_TAG when doing fillpage, has other values thereafter.
     * HOWEVER, tag is only valid while writing the clist or rendering directly,
     * not when playing back a clist (i.e. clist doesn't pass thru tags). Turns out
     * we can workaround because the clist does pass thru pure (undithered) device colors
     * like the white that's used to clear the page. Those will contain the opaque value we compute.
     * 
     * This means that this func is called for fillpage when we're writing the clist (when
     * graphic_type_tag_valid is set), so we'll write the correct device color (not marked as opaque)
     * into the clist. When reading back the clist we then use the stored device color 
     * without calling this function, so invalid graphic_type_tag doesn't matter.
     *
     * We've explained why this func will never be called *for fillpage* during clist playback
     * (i.e. when !graphics_type_tag_valid) & can assume that remaining calls in that state are
     * for proper opaque pixels. Note that while the clist playback uses recorded device
     * colors for pure colors, it still does call this routine for halftoned
     * colors since this device is SEPARABLE, meaning that that the playback will generate
     * its own halftone tiles by ORing component-by-component the gx_color_index values we 
     * produce. We want to cause those pixels to be marked as opaque.
     * 
     * Needless to say this is a nasty hack, but seems likely to be resistant to GS changes.
     */
    isfillpage = pcondor->graphic_type_tag_valid ?
        (dev->graphics_type_tag & ~GS_DEVICE_ENCODES_TAGS) == GS_UNTOUCHED_TAG : 0;
    return color | (isfillpage ? 0 : 0x80);
}

/*
 * Decode a gx_color_index value back to a list of colorant values.
 */
int
gx_condor_prn_decode_color(gx_device* dev, gx_color_index color, gx_color_value* out)
{
    int bpc = ((gx_devn_prn_device*)dev)->devn_params.bitspercomponent;
    int mask = (1 << bpc) - 1;
    uchar ncomp = dev->color_info.num_components;
    int i = 0;
    COLDUP_VARS;

    color = color & ~0x80;
    COLDUP_SETUP(bpc);
    for (i = ncomp - 1; i >= 0; i--) {
        out[ncomp - i - 1] = COLDUP_DUP(color & mask);
        color >>= bpc;
    }
    return 0;
}

int
gx_condor_prn_output_page(gx_device* pdev, int num_copies, int flush)
{
    /* Set up next OutputFile in anticipation of a possible open of output file. 
     * Can only do this now since we didn't want to force a close by setting
     * OutputFile directly since that could drain pipeline.
     * 
     * Note this may only change actual output file if ReopenPerPage is true.
     */
    gx_condor_prn_device* pcondor = (gx_condor_prn_device*)pdev;
    if (strlen(pcondor->next_fname) > 0)
        strcpy(pcondor->fname, pcondor->next_fname);

    return(gdev_prn_bg_output_page_seekable(pdev, num_copies, flush));
}


/* Next we have the code and structures required to support process_page
 * operation.
 *
 * First, we have a structure full of information. A pointer to this is
 * passed to every process_page function:
 */
typedef struct condor_process_arg_s {
    int dev_raster;
    int component_count;
    gx_monitor_t* file_monitor;
    gp_file* file;
    int must_accumulate_usage;
    volatile int result_code;
    volatile unsigned char result_usage;
} condor_process_arg_t;

/* Next we have a structure that describes each 'buffer' of data. There will
 * be one of these created for each background rendering thread. The
 * process_fn fills in the details, and the output_fn then outputs based on
 * it. */
typedef struct condor_process_buffer_s {
    int w;
    int h;
    gs_get_bits_params_t params;
} condor_process_buffer_t;

/* This function is called once per rendering thread to set up the buffer
 * that will be used in future calls. */
static int
condor_init_buffer(void* arg_, gx_device* dev_, gs_memory_t* memory, int w, int h, void** bufferp)
{
    condor_process_buffer_t* buffer;
    (void)arg_;
    (void)dev_;

    *bufferp = NULL;
    buffer = (condor_process_buffer_t*)gs_alloc_bytes(memory, sizeof(*buffer), "condor_init_buffer");
    if (buffer == NULL)
        return_error(gs_error_VMerror);
    memset(buffer, 0, sizeof(*buffer));
    buffer->w = w;
    buffer->h = h;

    *bufferp = buffer;
    return 0;
}

/* This function is called once per rendering thread after rendering
 * completes to free the buffer allocated in the init function above. */
static void
condor_free_buffer(void* arg_, gx_device* dev_, gs_memory_t* memory, void* buffer_)
{
    condor_process_buffer_t* buffer = (condor_process_buffer_t*)buffer_;
    (void)arg_;
    (void)dev_;
 
    if (buffer) {
        gs_free_object(memory, buffer, "condor_init_buffer");
    }
}

/* This is the function that does the bulk of the processing for the device.
 * This will be called back from the process_page call after each 'band'
 * has been drawn. Must be thread-safe; rendering order is arbitrary. */
#if ARCH_SIZEOF_PTR == 8
# define ULLONG long long unsigned /* assume long long pointers ==> 64 bit data regs */
#else
# define ULLONG long unsigned
#endif
static int
condor_process(void* arg_, gx_device* dev_, gx_device* bdev,
    const gs_int_rect* rect, void* buffer_)
{
    condor_process_arg_t* arg = (condor_process_arg_t*)arg_;
    condor_process_buffer_t* buffer = (condor_process_buffer_t*)buffer_;
    int code = 0;
    int w = rect->q.x - rect->p.x;
    int h = rect->q.y - rect->p.y;
    int stride = arg->dev_raster;
    gp_file* file = arg->file;
    gs_int_rect my_rect;
    long int ofs;
    (void)dev_;

    if (arg->result_code < 0)
        goto done;

    /* Render. We call 'get_bits_rectangle' to retrieve pointers to data
     * for the supplied rectangle.
     *
     * Note that 'rect' as supplied to this function gives the position on the
     * page, where 'my_rect' is the equivalent rectangle in the current band.
     */
    buffer->params.options = 
        GB_COLORS_NATIVE | GB_ALPHA_NONE | GB_PACKING_CHUNKY | GB_RETURN_POINTER |
        GB_ALIGN_ANY | GB_OFFSET_0 | GB_RASTER_ANY;
    my_rect.p.x = 0;
    my_rect.p.y = 0;
    my_rect.q.x = w;
    my_rect.q.y = h;
    code = dev_proc(bdev, get_bits_rectangle)(bdev, &my_rect, &buffer->params, NULL);
    if (code < 0)
        goto done;

    /* Force stride to be actual width, not GS' 32-bit padded */
    if (w & 3 != 0) {
        const unsigned char* src = buffer->params.data[0] + stride; /* skip 1st line since effectively done */
        unsigned char* dest = buffer->params.data[0] + w;
        int cnt;
        for (cnt = h - 1; cnt > 0; cnt--) {
            memmove(dest, src, w); /* memmove() tolerates overlap */
            src += stride;
            dest += w;
        }
        stride = w;
    }

    /* Write out buffer */
    gx_monitor_enter(arg->file_monitor);
    ofs = rect->p.y * stride;
    if (gp_fseek(file, ofs, 0) != 0 ||
        gp_fwrite(buffer->params.data[0], 1, stride * h, file) != stride * h)
        code = gs_error_ioerror;
    gx_monitor_leave(arg->file_monitor);
    if (code < 0)
        goto done;

    /* accumulate color usage */
    if (arg->must_accumulate_usage) {
        ULLONG accum = 0; /* accumulate word oriented for speed */
        ULLONG amask = sizeof(ULLONG) - 1;
        int word_cnt;
        int i;
        unsigned char usage;
        unsigned char* raster_ptr = buffer->params.data[0];

        int raster_cnt = stride * h;

        /* leading non-LL aligned bytes */
        while (raster_cnt != 0 && ((ULLONG)raster_ptr & amask) != 0) {
            accum |= *raster_ptr++;
            --raster_cnt;
        }

        /* LL-aligned */
        word_cnt = raster_cnt / sizeof(ULLONG);
        raster_cnt -= word_cnt * sizeof(ULLONG);
        while (word_cnt-- != 0) {
            accum |= *(ULLONG*)raster_ptr;
            raster_ptr += sizeof(ULLONG);
        }

        /* trailing non-LL aligned */
        while (raster_cnt-- != 0) {
           accum |= *raster_ptr++;
        }

        /* de-parallelize usage from ULLONG to unsigned char*/
        usage = 0;
        for (i = 0; i < sizeof(ULLONG); ++i) {
            usage |= (unsigned char)(accum & 0x7f);
            accum >>= 8;
        }
        arg->result_usage |= usage;
    }
done:
    if (arg->result_code >= 0 && code != 0)
        arg->result_code = code;
    return code;
}

/* This function is called back from process_page for each band (in order)
 * after the process_fn has completed. All we need to do is to output
 * the contents of the buffer. */
static int
condor_output(void* arg_, gx_device* dev_, void* buffer_)
{
    (void)arg_;
    (void)dev_;
    (void)buffer_;
    /*
    condor_process_arg_t* arg = (condor_process_arg_t*)arg_;
    struct gx_prn_device_condor_s* dev = (struct gx_prn_device_condor_s *)dev_;
    condor_process_buffer_t* buffer = (condor_process_buffer_t*)buffer_;
    unsigned char i;
    int w = buffer->w;
    int h = buffer->h;
    int raster = arg->dev_raster;
    */

    return 0;
}

static int
condor_report(condor_process_arg_t* arg, gx_condor_prn_device* ppdev)
{
    int code = 0;
    gp_file* report_file = NULL;
    gx_device* dev = (gx_device*)ppdev;

    if (arg->must_accumulate_usage
        && (code = open_report_file(dev, ppdev->report_fname, 1, 1, &report_file)) == 0) {
        /* file was opened for write or append, so just write */
        char buffer[4000] = "";
        char* buf = buffer;
        char* bufend = buffer + sizeof(buffer);
        gs_separations* seps = &ppdev->devn_params.separations;
        int i;

        /* output tab-separated:
         *  outfile name
         *  status code (0 good, -ve gs code)
         *  error message
         *  pix width
         *  Pix height
         *  spot name 1
         *  spot name 2
         *  spot name 3
         *  actual usage bitmap 0x40:spot3 0x20:spot2 0x10:spot1 0x8:k 0x4:y 0x2:m 0x1:c
         */
        int errcode = arg->result_code != 0 ? arg->result_code : code;
        const char* errstr = error_string(errcode);
        gs_snprintf(buf, bufend - buf, "%s\t%d\t%s\t%d\t%d\t",
            ppdev->fname,
            errcode,
            errstr,
            ppdev->width,
            ppdev->height
        );
        gp_fputs(buf, report_file);

        /* should honor separation_order_map; punt bc condor never changes
           SeparationOrder */
        for (i = 0;
            i < ppdev->devn_params.max_separations -
             ppdev->devn_params.num_std_colorant_names;
            ++i) {
            if (i < seps->num_separations) {
                devn_separation_name* sep = &seps->names[i]; //@@@check sequence
                    gp_fwrite(sep->data, sep->size, 1, report_file);
            }
            gp_fputs("\t", report_file);
        }

        gs_snprintf(buf, bufend - buf, "%d", (int)arg->result_usage);
        gp_fputs(buf, report_file);

        gp_fputs("\r\n", report_file);
        code = close_report_file(dev, ppdev->report_fname, report_file);
    }
    return code;
}

/*
 * This is a print page routine for a DeviceN device.  This routine
 */
static int
condor_spotcmyk_print_page(gx_device_printer* pdev, gp_file* prn_stream)
{
    int code = 0;
    gx_device* dev = (gx_device*)pdev;
    gx_condor_prn_device* ppdev = (gx_condor_prn_device*)pdev;
    gx_process_page_options_t options;
    condor_process_arg_t arg;
    arg.dev_raster = gx_device_raster(dev, 1);
    arg.component_count = COMPONENT_COUNT;
    arg.file = prn_stream;
    arg.file_monitor
        = gx_monitor_label(gx_monitor_alloc(pdev->memory->stable_memory), "CondorFile");
    if (arg.file_monitor == NULL)
        return_error(gs_error_configurationerror);
    arg.must_accumulate_usage = ppdev->report_fname[0] ? 1 : 0;

    arg.result_code = 0;
    arg.result_usage = 0;
 
    /* Kick off the actual hard work */
    options.init_buffer_fn = condor_init_buffer;
    options.free_buffer_fn = condor_free_buffer;
    options.process_fn = condor_process;
    options.output_fn = condor_output;
    options.arg = &arg;
    options.options = 0;
    code = dev_proc(pdev, process_page)(dev, &options);
    
    gx_monitor_free(arg.file_monitor);

    (void)condor_report(&arg, ppdev);

    return code;
}