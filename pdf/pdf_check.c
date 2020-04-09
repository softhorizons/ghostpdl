/* Copyright (C) 2001-2019 Artifex Software, Inc.
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

/* Checks for transparency and spots for the PDF interpreter */

#include "pdf_int.h"
#include "pdf_stack.h"
#include "pdf_page.h"
#include "pdf_file.h"
#include "pdf_dict.h"
#include "pdf_array.h"
#include "pdf_loop_detect.h"
#include "pdf_colour.h"
#include "pdf_trans.h"
#include "pdf_gstate.h"
#include "pdf_misc.h"
#include "pdf_check.h"

/* This routine is slightly misnamed, as it also checks ColorSpaces for spot colours.
 * This is done at the page level, so we maintain a dictionary of the spot colours
 * encountered so far, which we consult before adding any new ones.
 */
static int pdfi_check_Resources_for_transparency(pdf_context *ctx, pdf_dict *Resources_dict, pdf_dict *page_dict, bool *transparent, pdf_dict *spot_dict);

/* For performance and resource reasons we do not want to install the transparency blending
 * compositor unless we need it. Similarly, if a device handles spot colours it can minimise
 * memory usage if it knows ahead of time how many spot colours there will be.
 *
 * The PDF interpreter written in PostScript performed these as two separate tasks, on opening
 * a PDF file it would count spot colour usage and then for each page it would chek if the page
 * used any transparency. The code below is used to check for both transparency and spot colours.
 * If the int pointer to num_spots is NULL then we aren't interested in spot colours (not supported
 * by the device), if the int pointer to transparent is NULL then we aren't interested in transparency
 * (-dNOTRANSPARENCY is set).
 *
 * Currenlty the code is run when we open the PDF file, the existence of transparency on any given
 * page is recorded in a bit array for later use. If it turns out that checking every page when we
 * open the file is a performance burden then we could do it on a per-page basis instead. NB if
 * the device supports spot colours then we do need to scan the file before starting any page.
 *
 * The technique is fairly straight-forward, we start with each page, and open its Resources
 * dictionary, we then check by type each possible resource. Some resources (eg Pattern, XObject)
 * can themselves contain Resources, in which case we recursively check that dictionary. Note; not
 * all Resource types need to be checked for both transparency and spot colours, some types can
 * only contain one or the other.
 *
 * Routines with the name pdfi_check_xxx_dict are intended to check a Resource dictionary entry, this
 * will be a dictioanry of names and values, where the values are objects of the given Resource type.
 *
 */

/*
 * Check the Resources dictionary ColorSpace entry. pdfi_check_ColorSpace_for_spots is defined
 * in pdf_colour.c
 */
static int pdfi_check_ColorSpace_dict(pdf_context *ctx, pdf_dict *cspace_dict,
                                      pdf_dict *page_dict, pdf_dict *spot_dict)
{
    int code, i, index;
    pdf_obj *Key = NULL, *Value = NULL;

    if (pdfi_dict_entries(cspace_dict) > 0) {
        code = pdfi_loop_detector_mark(ctx); /* Mark the start of the ColorSpace dictionary loop */
        if (code < 0)
            return code;

        code = pdfi_dict_first(ctx, cspace_dict, &Key, &Value, (void *)&index);
        if (code < 0)
            goto error1;

        i = 1;
        do {
            code = pdfi_check_ColorSpace_for_spots(ctx, Value, cspace_dict, page_dict, spot_dict);
            if (code < 0)
                goto error2;

            pdfi_countdown(Key);
            Key = NULL;
            pdfi_countdown(Value);
            Value = NULL;

            (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the Shading dictionary loop */

            code = pdfi_loop_detector_mark(ctx); /* Mark the new start of the Shading dictionary loop */
            if (code < 0)
                goto error1;

            do {
                if (i++ >= pdfi_dict_entries(cspace_dict)) {
                    code = 0;
                    goto transparency_exit;
                }

                code = pdfi_dict_next(ctx, cspace_dict, &Key, &Value, (void *)&index);
                if (code == 0 && Value->type == PDF_ARRAY)
                    break;
                pdfi_countdown(Key);
                Key = NULL;
                pdfi_countdown(Value);
                Value = NULL;
            } while(1);
        }while (1);
    }
    return 0;

transparency_exit:
error2:
    pdfi_countdown(Key);
    pdfi_countdown(Value);

error1:
    (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the current resource loop */
    return code;
}

/*
 * Process an individual Shading dictionary to see if it contains a ColorSpace with a spot colour
 */
static int pdfi_check_Shading(pdf_context *ctx, pdf_dict *shading,
                              pdf_dict *page_dict, pdf_dict *spot_dict)
{
    int code;
    pdf_obj *o = NULL;

    code = pdfi_dict_knownget(ctx, shading, "ColorSpace", (pdf_obj **)&o);
    if (code > 0) {
        code = pdfi_check_ColorSpace_for_spots(ctx, o, shading, page_dict, spot_dict);
        pdfi_countdown(o);
        return code;
    }
    return 0;
}

/*
 * Check the Resources dictionary Shading entry.
 */
static int pdfi_check_Shading_dict(pdf_context *ctx, pdf_dict *shading_dict,
                                   pdf_dict *page_dict, pdf_dict *spot_dict)
{
    int code, i, index;
    pdf_obj *Key = NULL, *Value = NULL;

    if (pdfi_dict_entries(shading_dict) > 0) {
        code = pdfi_loop_detector_mark(ctx); /* Mark the start of the Shading dictionary loop */
        if (code < 0)
            return code;

        code = pdfi_dict_first(ctx, shading_dict, &Key, &Value, (void *)&index);
        if (code < 0 || Value->type != PDF_DICT)
            goto error1;

        i = 1;
        do {
            code = pdfi_check_Shading(ctx, (pdf_dict *)Value, page_dict, spot_dict);
            if (code < 0)
                goto error2;

            pdfi_countdown(Key);
            Key = NULL;
            pdfi_countdown(Value);
            Value = NULL;

            (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the Shading dictionary loop */

            code = pdfi_loop_detector_mark(ctx); /* Mark the new start of the Shading dictionary loop */
            if (code < 0)
                goto error1;

            do {
                if (i++ >= pdfi_dict_entries(shading_dict)) {
                    code = 0;
                    goto transparency_exit;
                }

                code = pdfi_dict_next(ctx, shading_dict, &Key, &Value, (void *)&index);
                if (code == 0 && Value->type == PDF_DICT)
                    break;
                pdfi_countdown(Key);
                Key = NULL;
                pdfi_countdown(Value);
                Value = NULL;
            } while(1);
        }while (1);
    }
    return 0;

transparency_exit:
error2:
    pdfi_countdown(Key);
    pdfi_countdown(Value);

error1:
    (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the current resource loop */
    return code;
}

/*
 * This routine checks an XObject to see if it contains any spot
 * colour definitions, or transparency usage.
 */
static int pdfi_check_XObject(pdf_context *ctx, pdf_dict *xobject, pdf_dict *page_dict,
                              bool *transparent, pdf_dict *spot_dict)
{
    int code = 0;
    pdf_name *n = NULL;
    bool known = false;
    double f;

    code = pdfi_dict_get_type(ctx, xobject, "Subtype", PDF_NAME, (pdf_obj **)&n);
    if (code >= 0) {
        if (pdfi_name_is((const pdf_name *)n, "Image")) {
            pdf_obj *CS = NULL;

            pdfi_countdown(n);
            n = NULL;
            code = pdfi_dict_known(xobject, "SMask", &known);
            if (code >= 0) {
                if (known == true) {
                    *transparent = true;
                    if (spot_dict == NULL)
                        goto transparency_exit;
                }
                code = pdfi_dict_knownget_number(ctx, xobject, "SMaskInData", &f);
                if (code > 0) {
                    code = 0;
                    if (f != 0.0)
                        *transparent = true;
                    if (spot_dict == NULL)
                        goto transparency_exit;
                }
                /* Check the image dictionary for a ColorSpace entry, if we are checking spot names */
                if (spot_dict) {
                    code = pdfi_dict_knownget(ctx, xobject, "ColorSpace", (pdf_obj **)&CS);
                    if (code > 0) {
                        /* We don't care if there's an error here, it'll be picked up if we use the ColorSpace later */
                        (void)pdfi_check_ColorSpace_for_spots(ctx, CS, xobject, page_dict, spot_dict);
                        pdfi_countdown(CS);
                    }
                }
            }
        } else {
            if (pdfi_name_is((const pdf_name *)n, "Form")) {
                pdf_dict *group_dict = NULL, *resource_dict = NULL;
                pdf_obj *CS = NULL;

                pdfi_countdown(n);
                code = pdfi_dict_knownget_type(ctx, xobject, "Group", PDF_DICT, (pdf_obj **)&group_dict);
                if (code > 0) {
                    *transparent = true;
                    if (spot_dict == NULL) {
                        pdfi_countdown(group_dict);
                        goto transparency_exit;
                    }

                    /* Start a new loop detector group to avoid this being detected in the Resources check below */
                    code = pdfi_loop_detector_mark(ctx); /* Mark the start of the XObject dictionary loop */
                    if (code > 0) {
                        code = pdfi_dict_knownget(ctx, group_dict, "CS", &CS);
                        if (code > 0)
                            /* We don't care if there's an error here, it'll be picked up if we use the ColorSpace later */
                            (void)pdfi_check_ColorSpace_for_spots(ctx, CS, group_dict, page_dict, spot_dict);
                    }
                    pdfi_countdown(group_dict);
                    pdfi_countdown(CS);
                    (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the XObject dictionary loop */
                }

                code = pdfi_dict_knownget_type(ctx, xobject, "Resources", PDF_DICT, (pdf_obj **)&resource_dict);
                if (code > 0) {
                    code = pdfi_check_Resources_for_transparency(ctx, resource_dict, page_dict,
                                                                 transparent, spot_dict);
                    pdfi_countdown(resource_dict);
                    if (code < 0)
                        goto transparency_exit;
                }
            } else
                pdfi_countdown(n);
        }
    }

    return 0;

transparency_exit:
    return code;
}

/*
 * Check the Resources dictionary XObject entry.
 */
static int pdfi_check_XObject_dict(pdf_context *ctx, pdf_dict *xobject_dict, pdf_dict *page_dict,
                                   bool *transparent, pdf_dict *spot_dict)
{
    int code, i, index;
    pdf_obj *Key = NULL, *Value = NULL; //, *o = NULL;

    if (pdfi_dict_entries(xobject_dict) > 0) {
        code = pdfi_loop_detector_mark(ctx); /* Mark the start of the XObject dictionary loop */
        if (code < 0)
            return code;

        code = pdfi_dict_first(ctx, xobject_dict, &Key, &Value, (void *)&index);
        if (code < 0 || Value->type != PDF_DICT)
            goto error1;

        i = 1;
        do {
            code = pdfi_check_XObject(ctx, (pdf_dict *)Value, page_dict, transparent, spot_dict);
            if (code < 0)
                goto error2;

            (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the XObject dictionary loop */

            code = pdfi_loop_detector_mark(ctx); /* Mark the new start of the XObject dictionary loop */
            if (code < 0)
                goto error1;

            pdfi_countdown(Key);
            Key = NULL;
            pdfi_countdown(Value);
            Value = NULL;

            do {
                if (i++ >= pdfi_dict_entries(xobject_dict)) {
                    code = 0;
                    goto transparency_exit;
                }

                code = pdfi_dict_next(ctx, xobject_dict, &Key, &Value, (void *)&index);
                if (code == 0 && Value->type == PDF_DICT)
                    break;
                pdfi_countdown(Key);
                Key = NULL;
                pdfi_countdown(Value);
                Value = NULL;
            } while(1);
        }while(1);
    }
    return 0;

transparency_exit:
error2:
    pdfi_countdown(Key);
    pdfi_countdown(Value);

error1:
    (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the current resource loop */
    return code;
}

/*
 * This routine checks an ExtGState dictionary to see if it contains any spot
 * colour definitions, or transparency usage.
 */
static int pdfi_check_ExtGState(pdf_context *ctx, pdf_dict *extgstate_dict, pdf_dict *page_dict,
                                bool *transparent, pdf_dict *spot_dict)
{
    int code;
    pdf_obj *o = NULL;
    double f;

    if (pdfi_dict_entries(extgstate_dict) > 0) {
        /* Check SMask first, because if we test spot colours first we can exit as
         * soon as we detect transparency.
         */
        code = pdfi_dict_knownget(ctx, extgstate_dict, "SMask", &o);
        if (code > 0) {
            if (o->type == PDF_NAME) {
                if (!pdfi_name_is((pdf_name *)o, "None")) {
                    pdfi_countdown(o);
                    *transparent = true;
                    return 0;
                }
            } else {
                if (o->type == PDF_DICT) {
                    pdf_obj *G = NULL;

                    *transparent = true;

                    if (spot_dict != NULL) {
                        /* Check if the SMask has a /G (Group) */
                        code = pdfi_dict_knownget(ctx, (pdf_dict *)o, "G", &G);
                        if (code > 0) {
                            code = pdfi_check_XObject(ctx, (pdf_dict *)G, page_dict,
                                                      transparent, page_dict);
                            pdfi_countdown(G);
                        }
                    }
                    pdfi_countdown(o);
                    return code;
                }
            }
        }
        pdfi_countdown(o);
        o = NULL;

        code = pdfi_dict_knownget_type(ctx, extgstate_dict, "BM", PDF_NAME, &o);
        if (code > 0) {
            if (!pdfi_name_is((pdf_name *)o, "Normal")) {
                if (!pdfi_name_is((pdf_name *)o, "Compatible")) {
                    pdfi_countdown(o);
                    *transparent = true;
                    return 0;
                }
            }
        }
        pdfi_countdown(o);
        o = NULL;

        code = pdfi_dict_knownget_number(ctx, extgstate_dict, "CA", &f);
        if (code > 0) {
            if (f != 1.0) {
                *transparent = true;
                return 0;
            }
        }

        code = pdfi_dict_knownget_number(ctx, extgstate_dict, "ca", &f);
        if (code > 0) {
            if (f != 1.0) {
                *transparent = true;
                return 0;
            }
        }
    }
    return 0;
}

/*
 * Check the Resources dictionary ExtGState entry.
 */
static int pdfi_check_ExtGState_dict(pdf_context *ctx, pdf_dict *extgstate_dict, pdf_dict *page_dict,
                                     bool *transparent, pdf_dict *spot_dict)
{
    int code, i, index;
    pdf_obj *Key = NULL, *Value = NULL;

    if (pdfi_dict_entries(extgstate_dict) > 0) {
        code = pdfi_loop_detector_mark(ctx); /* Mark the start of the ColorSpace dictionary loop */
        if (code < 0)
            return code;

        code = pdfi_dict_first(ctx, extgstate_dict, &Key, &Value, (void *)&index);
        if (code < 0)
            goto error1;

        i = 1;
        do {

            (void)pdfi_check_ExtGState(ctx, (pdf_dict *)Value, page_dict, transparent, spot_dict);
            if (*transparent == true && spot_dict == NULL)
                goto transparency_exit;

            pdfi_countdown(Key);
            Key = NULL;
            pdfi_countdown(Value);
            Value = NULL;

            (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the ExtGState dictionary loop */

            code = pdfi_loop_detector_mark(ctx); /* Mark the new start of the ExtGState dictionary loop */
            if (code < 0)
                goto error1;

            do {
                if (i++ >= pdfi_dict_entries(extgstate_dict)) {
                    code = 0;
                    goto transparency_exit;
                }

                code = pdfi_dict_next(ctx, extgstate_dict, &Key, &Value, (void *)&index);
                if (code == 0 && Value->type == PDF_DICT)
                    break;
                pdfi_countdown(Key);
                Key = NULL;
                pdfi_countdown(Value);
                Value = NULL;
            } while(1);
        }while (1);
    }
    return 0;

transparency_exit:
    pdfi_countdown(Key);
    pdfi_countdown(Value);

error1:
    (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the current resource loop */
    return code;
}

/*
 * This routine checks a Pattern dictionary to see if it contains any spot
 * colour definitions, or transparency usage.
 */
int pdfi_check_Pattern(pdf_context *ctx, pdf_dict *pattern, pdf_dict *page_dict,
                       bool *transparent, pdf_dict *spot_dict)
{
    int code = 0;
    pdf_obj *o = NULL;

    if (spot_dict != NULL) {
        code = pdfi_dict_knownget_type(ctx, pattern, "Shading", PDF_DICT, &o);
        if (code > 0)
            (void)pdfi_check_Shading(ctx, (pdf_dict *)o, page_dict, spot_dict);
        pdfi_countdown(o);
        o = NULL;
    }

    code = pdfi_dict_knownget_type(ctx, pattern, "Resources", PDF_DICT, &o);
    if (code > 0)
        (void)pdfi_check_Resources_for_transparency(ctx, (pdf_dict *)o, page_dict,
                                                    transparent, spot_dict);
    pdfi_countdown(o);
    o = NULL;
    if (*transparent == true && spot_dict == NULL)
        goto transparency_exit;

    code = pdfi_dict_knownget_type(ctx, pattern, "ExtGState", PDF_DICT, &o);
    if (code > 0)
        (void)pdfi_check_ExtGState(ctx, (pdf_dict *)o, page_dict, transparent, spot_dict);
    pdfi_countdown(o);
    o = NULL;

transparency_exit:
    return 0;
}

/*
 * Check the Resources dictionary Pattern entry.
 */
static int pdfi_check_Pattern_dict(pdf_context *ctx, pdf_dict *pattern_dict, pdf_dict *page_dict,
                                   bool *transparent, pdf_dict *spot_dict)
{
    int code, i, index;
    pdf_obj *Key = NULL, *Value = NULL;

    if (pdfi_dict_entries(pattern_dict) > 0) {
        code = pdfi_loop_detector_mark(ctx); /* Mark the start of the Pattern dictionary loop */
        if (code < 0)
            return code;

        code = pdfi_dict_first(ctx, pattern_dict, &Key, &Value, (void *)&index);
        if (code < 0)
            goto error1;

        if (Value->type != PDF_DICT)
            goto transparency_exit;

        i = 1;
        do {
            code = pdfi_check_Pattern(ctx, (pdf_dict *)Value, page_dict, transparent, spot_dict);
            if (code < 0)
                goto transparency_exit;

            pdfi_countdown(Key);
            Key = NULL;
            pdfi_countdown(Value);
            Value = NULL;
            (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the Shading dictionary loop */

            code = pdfi_loop_detector_mark(ctx); /* Mark the new start of the Shading dictionary loop */
            if (code < 0)
                goto error1;

            do {
                if (i++ >= pdfi_dict_entries(pattern_dict)) {
                    code = 0;
                    goto transparency_exit;
                }

                code = pdfi_dict_next(ctx, pattern_dict, &Key, &Value, (void *)&index);
                if (code == 0 && Value->type == PDF_DICT)
                    break;
                pdfi_countdown(Key);
                Key = NULL;
                pdfi_countdown(Value);
                Value = NULL;
            } while(1);
        }while (1);
    }
    return 0;

transparency_exit:
    pdfi_countdown(Key);
    pdfi_countdown(Value);

error1:
    (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the current resource loop */
    return code;
}

/*
 * This routine checks a Font dictionary to see if it contains any spot
 * colour definitions, or transparency usage.
 */
static int pdfi_check_Font(pdf_context *ctx, pdf_dict *font, pdf_dict *page_dict,
                           bool *transparent, pdf_dict *spot_dict)
{
    int code = 0;
    pdf_obj *o = NULL;

    if (font->type != PDF_DICT)
        return_error(gs_error_typecheck);

    code = pdfi_dict_knownget_type(ctx, font, "Subtype", PDF_NAME, &o);
    if (code > 0) {
        if (pdfi_name_is((pdf_name *)o, "Type3")) {
            pdfi_countdown(o);
            o = NULL;

            code = pdfi_dict_knownget_type(ctx, font, "Resources", PDF_DICT, &o);
            if (code > 0)
                (void)pdfi_check_Resources_for_transparency(ctx, (pdf_dict *)o, page_dict,
                                                            transparent, spot_dict);
        }
    }
    pdfi_countdown(o);
    o = NULL;

    return 0;
}

/*
 * Check the Resources dictionary Font entry.
 */
static int pdfi_check_Font_dict(pdf_context *ctx, pdf_dict *font_dict, pdf_dict *page_dict,
                                bool *transparent, pdf_dict *spot_dict)
{
    int code, i, index;
    pdf_obj *Key = NULL, *Value = NULL;

    if (pdfi_dict_entries(font_dict) > 0) {
        code = pdfi_loop_detector_mark(ctx); /* Mark the start of the Font dictionary loop */
        if (code < 0)
            return code;

        code = pdfi_dict_first(ctx, font_dict, &Key, &Value, (void *)&index);
        if (code < 0)
            goto error1;

        i = 1;
        do {
            code = pdfi_check_Font(ctx, (pdf_dict *)Value, page_dict, transparent, spot_dict);

            pdfi_countdown(Key);
            Key = NULL;
            pdfi_countdown(Value);
            Value = NULL;

            (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the Font dictionary loop */

            code = pdfi_loop_detector_mark(ctx); /* Mark the new start of the Font dictionary loop */
            if (code < 0)
                goto error1;

            do {
                if (i++ >= pdfi_dict_entries(font_dict)) {
                    code = 0;
                    goto transparency_exit;
                }

                code = pdfi_dict_next(ctx, font_dict, &Key, &Value, (void *)&index);
                if (code == 0 && Value->type == PDF_DICT)
                    break;
                pdfi_countdown(Key);
                Key = NULL;
                pdfi_countdown(Value);
                Value = NULL;
            } while(1);
        }while (1);
    }
    return 0;

transparency_exit:
    pdfi_countdown(Key);
    pdfi_countdown(Value);

error1:
    (void)pdfi_loop_detector_cleartomark(ctx); /* Clear to the mark for the current resource loop */
    return code;
}

static int pdfi_check_Resources_for_transparency(pdf_context *ctx, pdf_dict *Resources_dict,
                                                 pdf_dict *page_dict,
                                                 bool *transparent, pdf_dict *spot_dict)
{
    int code;
    pdf_obj *d = NULL;

    /* First up, check any colour spaces, for new spot colours.
     * We only do this if asked because its expensive. spot_dict being NULL
     * means we aren't interested in spot colours (not a DeviceN or Separation device)
     */
    if (spot_dict != NULL) {
        code = pdfi_dict_knownget_type(ctx, Resources_dict, "ColorSpace", PDF_DICT, &d);
        if (code > 0)
            (void)pdfi_check_ColorSpace_dict(ctx, (pdf_dict *)d, page_dict, spot_dict);

        pdfi_countdown(d);
        d = NULL;

        code = pdfi_dict_knownget_type(ctx, Resources_dict, "Shading", PDF_DICT, &d);
        if (code > 0)
            (void)pdfi_check_Shading_dict(ctx, (pdf_dict *)d, page_dict, spot_dict);
        pdfi_countdown(d);
        d = NULL;
    }

    code = pdfi_dict_knownget_type(ctx, Resources_dict, "XObject", PDF_DICT, &d);
    if (code > 0)
        (void)pdfi_check_XObject_dict(ctx, (pdf_dict *)d, page_dict, transparent, spot_dict);
    pdfi_countdown(d);
    d = NULL;

    code = pdfi_dict_knownget_type(ctx, Resources_dict, "Pattern", PDF_DICT, &d);
    if (code > 0)
        (void)pdfi_check_Pattern_dict(ctx, (pdf_dict *)d, page_dict, transparent, spot_dict);
    pdfi_countdown(d);
    d = NULL;

    code = pdfi_dict_knownget_type(ctx, Resources_dict, "Font", PDF_DICT, &d);
    if (code > 0)
        (void)pdfi_check_Font_dict(ctx, (pdf_dict *)d, page_dict, transparent, spot_dict);
    /* From this point onwards, if we detect transparency (or have already detected it) we
     * can exit, we have already counted up any spot colours.
     */
    pdfi_countdown(d);
    d = NULL;

    code = pdfi_dict_knownget_type(ctx, Resources_dict, "ExtGState", PDF_DICT, &d);
    if (code > 0)
        (void)pdfi_check_ExtGState_dict(ctx, (pdf_dict *)d, page_dict, transparent, spot_dict);
    pdfi_countdown(d);
    d = NULL;

    return 0;
}

static int pdfi_check_annot_for_transparency(pdf_context *ctx, pdf_dict *annot, pdf_dict *page_dict,
                                             bool *transparent, pdf_dict *spot_dict)
{
    int code;
    pdf_name *n;
    pdf_dict *ap = NULL, *N = NULL, *Resources = NULL;
    double f;

    /* Check #1 Does the (Normal) Appearnce stream use any Resources which include transparency.
     * We check this first, because this also checks for spot colour spaces. Once we've done that we
     * can exit the checks as soon as we detect transparency.
     */
    code = pdfi_dict_knownget_type(ctx, annot, "AP", PDF_DICT, (pdf_obj **)&ap);
    if (code > 0)
    {
        code = pdfi_dict_knownget_type(ctx, ap, "N", PDF_DICT, (pdf_obj **)&N);
        if (code > 0) {
            code = pdfi_dict_knownget_type(ctx, N, "Resources", PDF_DICT, (pdf_obj **)&Resources);
            if (code > 0)
                code = pdfi_check_Resources_for_transparency(ctx, (pdf_dict *)Resources, page_dict,
                                                             transparent, spot_dict);
        }
    }
    pdfi_countdown(ap);
    pdfi_countdown(N);
    pdfi_countdown(Resources);
    if (code < 0)
        return code;
    /* We've checked the Resources, and nothing else in an annotation can define spot colours, so
     * if we detected transparency in the Resources we need not check further.
     */
    if (*transparent == true)
        return 0;

    code = pdfi_dict_get_type(ctx, annot, "Subtype", PDF_NAME, (pdf_obj **)&n);
    if (code < 0) {
        if (ctx->pdfstoponerror)
            return code;
    } else {
        /* Check #2, Highlight annotations are always preformed with transparency */
        if (pdfi_name_is((const pdf_name *)n, "Highlight")) {
            pdfi_countdown(n);
            *transparent = true;
            return 0;
        }
        pdfi_countdown(n);
        n = NULL;

        /* Check #3 Blend Mode (BM) not being 'Normal' or 'Compatible' */
        code = pdfi_dict_knownget_type(ctx, annot, "BM", PDF_NAME, (pdf_obj **)&n);
        if (code > 0) {
            if (!pdfi_name_is((const pdf_name *)n, "Normal")) {
                if (!pdfi_name_is((const pdf_name *)n, "Compatible")) {
                    pdfi_countdown(n);
                    *transparent = true;
                    return 0;
                }
            }
            code = 0;
        }
        pdfi_countdown(n);
        if (code < 0)
            return code;

        /* Check #4 stroke constant alpha (CA) is not 1 (100% opaque) */
        code = pdfi_dict_knownget_number(ctx, annot, "CA", &f);
        if (code > 0) {
            if (f != 1.0) {
                *transparent = true;
                return 0;
            }
        }
        if (code < 0)
            return code;

        /* Check #5 non-stroke constant alpha (ca) is not 1 (100% opaque) */
        code = pdfi_dict_knownget_number(ctx, annot, "ca", &f);
        if (code > 0) {
            if (f != 1.0) {
                *transparent = true;
                return 0;
            }
        }
        if (code < 0)
            return code;
    }

    return 0;
}

static int pdfi_check_Annots_for_transparency(pdf_context *ctx, pdf_array *annots_array,
                                              pdf_dict *page_dict, bool *transparent, pdf_dict *spot_dict)
{
    int i, code = 0;
    pdf_dict *annot = NULL;

    for (i=0; i < pdfi_array_size(annots_array); i++) {
        code = pdfi_array_get_type(ctx, annots_array, (uint64_t)i, PDF_DICT, (pdf_obj **)&annot);
        if (code >= 0) {
            code = pdfi_check_annot_for_transparency(ctx, annot, page_dict, transparent, spot_dict);
            if (code < 0 && ctx->pdfstoponerror)
                goto exit;

            /* If we've found transparency, and don't need to continue checkign for spot colours
             * just exit as fast as possible.
             */
            if (*transparent == true && spot_dict == NULL)
                goto exit;

            pdfi_countdown(annot);
            annot = NULL;
        }
        if (code < 0 && ctx->pdfstoponerror)
            goto exit;
        code = 0;
    }
exit:
    pdfi_countdown(annot);
    return code;
}

/* Check for transparency and spots on page.
 *
 * Sets ctx->spot_capable_device
 * Builds a dictionary of the unique spot names in spot_dict
 * Set 'transparent' to true if there is transparency on the page
 *
 * From the original PDF interpreter written in PostScript:
 * Note: we deliberately don't check to see whether a Group is defined,
 * because Adobe Illustrator 10 (and possibly other applications) define
 * a page-level group whether transparency is actually used or not.
 * Ignoring the presence of Group is justified because, in the absence
 * of any other transparency features, they have no effect.
 */
static int pdfi_check_page_inner(pdf_context *ctx, pdf_dict *page_dict,
                                 bool *transparent, pdf_dict *spot_dict)
{
    int code;
    pdf_dict *Resources = NULL;
    pdf_array *Annots = NULL;
    pdf_dict *Group = NULL;
    pdf_obj *CS = NULL;
    int intval;

    *transparent = false;

    /* See if the device supports spots (if not, don't check for them) */
    ctx->spot_capable_device = false;
    gs_c_param_list_read(&ctx->pdfi_param_list);
    code = param_read_int((gs_param_list *)&ctx->pdfi_param_list, "PageSpotColors", &intval);
    if (code < 0)
        return code;
    if (code == 0)
        ctx->spot_capable_device = true;

    /* Disable spot-checking if not spot capable device */
    if (!ctx->spot_capable_device)
        spot_dict = NULL;

    /* Check if the page dictionary has a page Group entry (for spots).
     * Page group should mean the page has transparency but we ignore it for the purposes
     * of transparency detection. See above.
     */
    if (spot_dict) {
        code = pdfi_dict_knownget_type(ctx, page_dict, "Group", PDF_DICT, (pdf_obj **)&Group);
        if (code > 0) {
            /* If Group has a ColorSpace (CS), then check it for spot colours */
            code = pdfi_dict_knownget(ctx, Group, "CS", &CS);
            if (code > 0)
                code = pdfi_check_ColorSpace_for_spots(ctx, CS, Group, page_dict, spot_dict);
            if (code < 0 && ctx->pdfstoponerror)
                goto exit;
        }
    }

    /* Now check any Resources dictionary in the Page dictionary */
    code = pdfi_dict_knownget_type(ctx, page_dict, "Resources", PDF_DICT, (pdf_obj **)&Resources);
    if (code > 0)
        code = pdfi_check_Resources_for_transparency(ctx, Resources, page_dict,
                                                     transparent, spot_dict);
    if (code < 0 && ctx->pdfstoponerror)
        goto exit;

    /* If we are drawing Annotations, check to see if the page uses any Annots */
    if (ctx->showannots) {
        code = pdfi_dict_knownget_type(ctx, page_dict, "Annots", PDF_ARRAY, (pdf_obj **)&Annots);
        if (code > 0)
            code = pdfi_check_Annots_for_transparency(ctx, Annots, page_dict,
                                                      transparent, spot_dict);
        if (code < 0 && ctx->pdfstoponerror)
            goto exit;
    }

    code = 0;
 exit:
    pdfi_countdown(Resources);
    pdfi_countdown(Annots);
    pdfi_countdown(CS);
    pdfi_countdown(Group);
    return code;
}

/* Checks page for transparency, and sets up device for spots, if applicable
 * Sets ctx->page_has_transparency and ctx->page_num_spots
 * do_setup -- indicates whether to actually set up the device with the spot count.
 */
int pdfi_check_page(pdf_context *ctx, pdf_dict *page_dict, bool do_setup)
{
    int code;
    bool uses_transparency = false;
    pdf_dict *spot_dict = NULL;
    int spots = 0;

    ctx->page_num_spots = 0;
    ctx->page_has_transparency = false;

    code = pdfi_alloc_object(ctx, PDF_DICT, 32, (pdf_obj **)&spot_dict);
    if (code < 0)
        goto exit;
    pdfi_countup(spot_dict);

    /* Check for spots and transparency in this page */
    code = pdfi_check_page_inner(ctx, page_dict, &uses_transparency, spot_dict);
    if (code < 0)
        goto exit;

    /* Count the spots */
    spots = pdfi_dict_entries(spot_dict);

    /* If there are spot colours (and by inference, the device renders spot plates) then
     * send the number of Spots to the device, so it can setup correctly.
     */
    if (spots > 0 && do_setup) {
        gs_c_param_list_write(&ctx->pdfi_param_list, ctx->memory);
        param_write_int((gs_param_list *)&ctx->pdfi_param_list, "PageSpotColors", &spots);
        gs_c_param_list_read(&ctx->pdfi_param_list);
        code = gs_putdeviceparams(ctx->pgs->device, (gs_param_list *)&ctx->pdfi_param_list);
        if (code > 0) {
            /* The device was closed, we need to reopen it */
            code = gs_setdevice_no_erase(ctx->pgs, ctx->pgs->device);
            if (code < 0) {
                if (uses_transparency)
                    (void)gs_abort_pdf14trans_device(ctx->pgs);
                goto exit;
            }
            gs_erasepage(ctx->pgs);
        }
    }

    /* Set our values in the context, for caller */
    ctx->page_has_transparency = uses_transparency;
    ctx->page_num_spots = spots;

 exit:
    pdfi_countdown(spot_dict);
    return code;
}