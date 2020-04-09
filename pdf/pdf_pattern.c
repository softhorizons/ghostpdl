/* Copyright (C) 2001-2018 Artifex Software, Inc.
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

/* Pattern operations for the PDF interpreter */

#include "pdf_int.h"
#include "pdf_colour.h"
#include "pdf_pattern.h"
#include "pdf_stack.h"
#include "pdf_array.h"
#include "pdf_gstate.h"
#include "gsicc_manage.h"
#include "gsicc_profilecache.h"
#include "gsicc_create.h"
#include "gsptype2.h"

#include "pdf_file.h"
#include "pdf_dict.h"
#include "pdf_loop_detect.h"
#include "pdf_func.h"
#include "pdf_shading.h"
#include "pdf_check.h"
#include "gscsepr.h"
#include "stream.h"
#include "strmio.h"
#include "gscdevn.h"

typedef struct {
    pdf_context *ctx;
    pdf_dict *page_dict;
    pdf_dict *pat_dict;
    gs_shading_t *shading;
} pdf_pattern_context_t;

/* See pdf_draw.ps, FixPatternBox
 * A BBox where width or height (or both) is 0 should still paint one pixel
 * See the ISO 32000-2:2017 spec, section 8.7.4.3, p228 'BBox' and 8.7.3.1
 */
static void
pdfi_pattern_fix_bbox(pdf_context *ctx, gs_rect *rect)
{
    if (rect->p.x - rect->q.x == 0)
        rect->q.x += .00000001;
    if (rect->p.y - rect->q.y == 0)
        rect->q.y += .00000001;
}

/* Get rect from array, normalize and adjust it */
static int
pdfi_pattern_get_rect(pdf_context *ctx, pdf_array *array, gs_rect *rect)
{
    int code;

    code = pdfi_array_to_gs_rect(ctx, array, rect);
    if (code != 0)
        return code;

    pdfi_normalize_rect(ctx, rect);
    pdfi_pattern_fix_bbox(ctx, rect);

    return code;
}

#if 0 /* Not currently using, not sure if needed (and it didn't work anyway) */
/* NULL Pattern */
static int
pdfi_setpattern_null(pdf_context *ctx, gs_client_color *cc)
{
    int code = 0;
    gs_client_pattern templat;
    gs_matrix mat;
    gs_rect rect;

    gs_pattern1_init(&templat);

    /* Init identity matrix */
    pdfi_array_to_gs_matrix(ctx, NULL, &mat);
    pdfi_pattern_get_rect(ctx, NULL, &rect);
    templat.BBox = rect;
    templat.PaintProc = NULL;
    templat.PaintType = 1;
    templat.TilingType = 3;
    templat.XStep = 1;
    templat.YStep = 1;

    code = gs_makepattern(cc, &templat, &mat, ctx->pgs, ctx->memory);

    return code;
}
#endif

/*
 * Pattern lifetime management turns out to be more complex than we would ideally like. Although
 * Patterns are reference counted, and contain a client_data pointer, they don't have a gs_notify
 * setup. This means that there's no simple way for us to be informed when a Pattern is released.
 * We could patch up the Pattern finalize() method, replacing it with one of our own which calls
 * the original finalize() but that seems like a really nasty hack.
 * For the time being we put code in pdfi_grestore() to check for Pattern colour spaces being
 * restored away, but we also need to check for Pattern spaces being replaced in the current
 * graphics state. We define 'pdfi' variants of several graphics library colour management
 * functions to 'wrap' these with code to check for replacement of Patterns.
 * This comment is duplicated in pdf_color.c
 */
int pdfi_pattern_cleanup(const gs_client_color *pcc)
{
    gs_pattern1_instance_t *pinst = (gs_pattern1_instance_t *)pcc->pattern;
    gs_pattern1_template_t *templat;
    pdf_pattern_context_t *context;

    if (pinst == NULL)
        return 0;
    templat = &pinst->templat;
    if (templat == NULL)
        return 0;
    context = (pdf_pattern_context_t *)templat->client_data;

    if (context != NULL) {
        pdfi_countdown(context->page_dict);
        pdfi_countdown(context->pat_dict);
        if (context->shading)
            pdfi_shading_free(context->ctx, context->shading);
        gs_free_object(context->ctx->memory, context, "Free pattern context");
        templat->client_data = NULL;
    }
    return 0;
}

/* See px_paint_pattern() */
static int
pdfi_pattern_paint_stream(pdf_context *ctx, const gs_client_color *pcc)
{
    gs_pattern1_instance_t *pinst = (gs_pattern1_instance_t *)pcc->pattern;
    const gs_pattern1_template_t *templat = &pinst->templat;
    pdf_pattern_context_t *context = (pdf_pattern_context_t *)templat->client_data;
    pdf_dict *page_dict = context->page_dict;
    pdf_dict *pat_dict = context->pat_dict;
    int code = 0;

    /* Interpret inner stream */
    code = pdfi_run_context(ctx, pat_dict, page_dict, true, "PATTERN");

    return code;
}

/* See px_paint_pattern() */
static int
pdfi_pattern_paint(const gs_client_color *pcc, gs_gstate *pgs)
{
    gs_pattern1_instance_t *pinst = (gs_pattern1_instance_t *)pcc->pattern;
    const gs_pattern1_template_t *templat = &pinst->templat;
    pdf_pattern_context_t *context = (pdf_pattern_context_t *)templat->client_data;
    pdf_context *ctx = context->ctx;
    int code = 0;

    dbgmprintf(ctx->memory, "BEGIN PATTERN PaintProc\n");
    code = pdfi_gsave(ctx); /* TODO: This might be redundant? */
    if (code < 0)
        return code;
    code = gs_setgstate(ctx->pgs, pgs);
    if (code < 0)
        goto exit;

    /* TODO: This hack here is to emulate some stuff that happens in the PS code.
     * Basically gx_pattern_load() gets called twice in PS code path, which causes this
     * flag to end up being set, which changes some transparency stuff that might matter, to happen.
     * By forcing this flag here, it makes the trace more closely follow what the PS code
     * does.  It could turn out to be a meaningless side-effect, or it might be important.
     * (sometime in the future, try taking this out and see what happens :)
     */
    if (pinst->templat.uses_transparency) {
        dbgmprintf(ctx->memory, "pdfi_pattern_paint forcing trans_flags.xtate_change = TRUE\n");
        ctx->pgs->trans_flags.xstate_change = true;
    }
    code = pdfi_op_q(ctx);
    if (code < 0)
        goto exit;

    code = pdfi_pattern_paint_stream(ctx, pcc);
    pdfi_op_Q(ctx);
    if (code < 0) {
        dbgmprintf1(ctx->memory, "ERROR: pdfi_pattern_paint: code %d when rendering pattern\n", code);
        goto exit;
    }

 exit:
    pdfi_grestore(ctx);
    dbgmprintf(ctx->memory, "END PATTERN PaintProc\n");
    return code;
}

/* See px_high_level_pattern(), pattern_paint_prepare() */
static int
pdfi_pattern_paint_high_level(const gs_client_color *pcc, gs_gstate *pgs_ignore)
{
    gs_pattern1_instance_t *pinst = (gs_pattern1_instance_t *)pcc->pattern;
    const gs_pattern1_template_t *templat = &pinst->templat;
    pdf_pattern_context_t *context = (pdf_pattern_context_t *)templat->client_data;
    pdf_context *ctx = context->ctx;
    gs_gstate *pgs = ctx->pgs;
    gs_matrix m;
    gs_rect bbox;
    gs_fixed_rect clip_box;
    int code;
    gx_device_color *pdc = gs_currentdevicecolor_inline(pgs);
    pattern_accum_param_s param;

    code = gx_pattern_cache_add_dummy_entry(pgs, pinst, pgs->device->color_info.depth);
    if (code < 0)
        return code;

    code = pdfi_gsave(ctx);
    if (code < 0)
        return code;
    code = gs_setgstate(ctx->pgs, pinst->saved);
    if (code < 0)
        goto errorExit;

    dev_proc(pgs->device, get_initial_matrix)(pgs->device, &m);
    gs_setmatrix(pgs, &m);
    code = gs_bbox_transform(&templat->BBox, &ctm_only(pgs), &bbox);
    if (code < 0)
        goto errorExit;
    clip_box.p.x = float2fixed(bbox.p.x);
    clip_box.p.y = float2fixed(bbox.p.y);
    clip_box.q.x = float2fixed(bbox.q.x);
    clip_box.q.y = float2fixed(bbox.q.y);
    code = gx_clip_to_rectangle(pgs, &clip_box);
    if (code < 0)
        goto errorExit;

    param.pinst = (void *)pinst;
    param.interpreter_memory = ctx->memory;
    param.graphics_state = (void *)pgs;
    param.pinst_id = pinst->id;

    code = (*dev_proc(pgs->device, dev_spec_op))
        ((gx_device *)pgs->device, gxdso_pattern_start_accum, &param, sizeof(pattern_accum_param_s));

    if (code < 0)
        goto errorExit;

    dbgmprintf(ctx->memory, "PATTERN: BEGIN high level pattern stream\n");
    code = pdfi_pattern_paint_stream(ctx, &pdc->ccolor);
    dbgmprintf(ctx->memory, "PATTERN: END high level pattern stream\n");
    if (code < 0)
        goto errorExit;

    code = dev_proc(pgs->device, dev_spec_op)
        (pgs->device, gxdso_pattern_finish_accum, &param, sizeof(pattern_accum_param_s));
    if (code < 0)
        goto errorExit;

    code = pdfi_grestore(ctx);
    if (code < 0)
        return code;
    return gs_error_handled;

 errorExit:
    pdfi_grestore(ctx);
    return code;
}

/* Called from gx_pattern_load(), see px_remap_pattern()  */
static int
pdfi_pattern_paintproc(const gs_client_color *pcc, gs_gstate *pgs)
{
    const gs_client_pattern *pinst = gs_getpattern(pcc);
    int code = 0;

    /* pgs->device is the newly created pattern accumulator, but we want to test the device
     * that is 'behind' that, the actual output device, so we use the one from
     * the saved graphics state.
     */
    if (pgs->have_pattern_streams) {
        code = dev_proc(pcc->pattern->saved->device, dev_spec_op)(pcc->pattern->saved->device,
                                gxdso_pattern_can_accum, (void *)pinst, pinst->uid.id);
    }

    if (code == 1) {
        return pdfi_pattern_paint_high_level(pcc, pgs);
    } else {
        return pdfi_pattern_paint(pcc, pgs);
    }
}



/* Setup the correct gstate for a pattern */
static int
pdfi_pattern_gset(pdf_context *ctx)
{
    int code;
    float strokealpha, fillalpha;

    strokealpha = gs_getstrokeconstantalpha(ctx->pgs);
    fillalpha = gs_getfillconstantalpha(ctx->pgs);

    /* This will preserve the ->level and a couple other things */
    dbgmprintf2(ctx->memory, "PATTERN setting DefaultQState, old device=%s, new device=%s\n",
                ctx->pgs->device->dname, ctx->DefaultQState->device->dname);
    code = gs_setgstate(ctx->pgs, pdfi_get_DefaultQState(ctx));

    if (code < 0)
        goto exit;
    code = gs_setstrokeconstantalpha(ctx->pgs, strokealpha);
    code = gs_setfillconstantalpha(ctx->pgs, fillalpha);

 exit:
    return code;
}

/* Setup the pattern gstate and other context */
static int
pdfi_pattern_setup(pdf_context *ctx, gs_pattern_template_t *templat, pdf_dict *page_dict, pdf_dict *pat_dict)
{
    int code = 0;
    pdf_pattern_context_t *context = NULL;

    code = pdfi_pattern_gset(ctx);
    if (code < 0)
        goto errorExit;

    context = (pdf_pattern_context_t *) gs_alloc_bytes(ctx->memory, sizeof(*context),
                                                       "pdfi_pattern_setup(context)");
    if (!context) {
        code = gs_note_error(gs_error_VMerror);
        goto errorExit;
    }
    context->ctx = ctx;
    context->page_dict = page_dict;
    context->pat_dict = pat_dict;
    context->shading = NULL;
    pdfi_countup(page_dict);
    pdfi_countup(pat_dict);
    templat->client_data = context;

    return 0;
 errorExit:
    gs_free_object(ctx->memory, context, "pdfi_pattern_setup(context)");
    return code;
}


/* Type 1 (tiled) Pattern */
static int
pdfi_setpattern_type1(pdf_context *ctx, pdf_dict *stream_dict, pdf_dict *page_dict,
                      pdf_dict *pdict, gs_client_color *cc)
{
    int code = 0;
    gs_client_pattern templat;
    gs_matrix mat;
    gs_rect rect;
    int64_t PaintType, TilingType;
    pdf_array *BBox = NULL;
    double XStep, YStep;
    pdf_dict *Resources = NULL;
    pdf_array *Matrix = NULL;
    bool transparency = false;

    dbgmprintf(ctx->memory, "PATTERN: Type 1 pattern\n");

    gs_pattern1_init(&templat);

    /* Required */
    code = pdfi_dict_get_int(ctx, pdict, "PaintType", &PaintType);
    if (code < 0)
        goto exit;
    code = pdfi_dict_get_int(ctx, pdict, "TilingType", &TilingType);
    if (code < 0)
        goto exit;
    code = pdfi_dict_get_type(ctx, pdict, "BBox", PDF_ARRAY, (pdf_obj **)&BBox);
    if (code < 0)
        goto exit;
    code = pdfi_pattern_get_rect(ctx, BBox, &rect);
    if (code < 0)
        goto exit;
    code = pdfi_dict_get_number(ctx, pdict, "XStep", &XStep);
    if (code < 0)
        goto exit;
    code = pdfi_dict_get_number(ctx, pdict, "YStep", &YStep);
    if (code < 0)
        goto exit;

    /* The spec says Resources are required, but in fact this doesn't seem to be true.
     * (tests_private/pdf/sumatra/infinite_pattern_recursion.pdf)
     */
    code = pdfi_dict_get_type(ctx, pdict, "Resources", PDF_DICT, (pdf_obj **)&Resources);
    if (code < 0) {
        dbgmprintf(ctx->memory, "PATTERN: Missing Resources in Pattern dict\n");
        ctx->pdf_warnings |= W_PDF_BADPATTERN;
        code = 0;
    }

    /* (optional Matrix) */
    code = pdfi_dict_knownget_type(ctx, pdict, "Matrix", PDF_ARRAY, (pdf_obj **)&Matrix);
    if (code < 0)
        goto exit;
    code = pdfi_array_to_gs_matrix(ctx, Matrix, &mat);
    if (code < 0)
        goto exit;

    if (PaintType != 1 && PaintType != 2) {
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }
    if (TilingType != 1 && TilingType != 2 && TilingType != 3) {
        code = gs_note_error(gs_error_rangecheck);
        goto exit;
    }

    /* See if pattern uses transparency */
    if (ctx->page_has_transparency) {
        code = pdfi_check_Pattern(ctx, pdict, page_dict, &transparency, NULL);
        if (code < 0)
            goto exit;
    }

    /* TODO: Resources?  Maybe I should check that they are all valid before proceeding, or something? */

    templat.BBox = rect;
    /* (see zPaintProc or px_remap_pattern) */
    templat.PaintProc = pdfi_pattern_paintproc;
    templat.PaintType = PaintType;
    templat.TilingType = TilingType;
    templat.XStep = XStep;
    templat.YStep = YStep;
    templat.uses_transparency = transparency;
    //templat.uses_transparency = false; /* disable */

    code = pdfi_gsave(ctx);
    if (code < 0)
        goto exit;

    code = pdfi_pattern_setup(ctx, (gs_pattern_template_t *)&templat, page_dict, pdict);
    if (code < 0) {
        (void) pdfi_grestore(ctx);
        goto exit;
    }

    code = gs_make_pattern(cc, (const gs_pattern_template_t *)&templat, &mat, ctx->pgs, ctx->memory);
    if (code < 0) {
        (void) pdfi_grestore(ctx);
        goto exit;
    }

    code = pdfi_grestore(ctx);
    if (code < 0)
        goto exit;
 exit:
    pdfi_countdown(Resources);
    pdfi_countdown(Matrix);
    pdfi_countdown(BBox);
    return code;
}

/* Type 2 (shading) Pattern */
static int
pdfi_setpattern_type2(pdf_context *ctx, pdf_dict *stream_dict, pdf_dict *page_dict,
                      pdf_dict *pdict, gs_client_color *cc)
{
    int code = 0;
    pdf_dict *Shading = NULL;
    pdf_dict *ExtGState = NULL;
    pdf_array *Matrix = NULL;
    gs_matrix mat;
    gs_shading_t *shading;
    gs_pattern2_template_t templat;
    pdf_pattern_context_t *context = NULL;

    /* See zbuildshadingpattern() */

    dbgmprintf(ctx->memory, "PATTERN: Type 2 pattern\n");

    /* (optional Matrix) */
    code = pdfi_dict_knownget_type(ctx, pdict, "Matrix", PDF_ARRAY, (pdf_obj **)&Matrix);
    if (code < 0)
        goto exit;
    code = pdfi_array_to_gs_matrix(ctx, Matrix, &mat);
    if (code < 0)
        goto exit;

    /* Required Shading, can be stream or dict (but a stream is also a dict..) */
    code = pdfi_dict_knownget_type(ctx, pdict, "Shading", PDF_DICT, (pdf_obj **)&Shading);
    if (code < 0)
        goto exit;
    if (code == 0) {
        dbgmprintf(ctx->memory, "ERROR: Shading not found in Pattern Type 2\n");
        code = gs_note_error(gs_error_syntaxerror);
        goto exit;
    }

    /* Optional ExtGState */
    code = pdfi_dict_knownget_type(ctx, pdict, "ExtGState", PDF_DICT, (pdf_obj **)&ExtGState);
    if (code < 0)
        goto exit;
    if (code != 0) {
        /* Not implemented, ignore for now */
        dbgmprintf(ctx->memory, "WARNING: Pattern ExtGState not supported, skipping\n");
    }

    code = pdfi_gsave(ctx);
    if (code < 0)
        goto exit;

    gs_pattern2_init(&templat);

    code = pdfi_pattern_setup(ctx, (gs_pattern_template_t *)&templat, NULL, NULL);
    if (code < 0) {
        (void) pdfi_grestore(ctx);
        goto exit;
    }

    code = pdfi_shading_build(ctx, stream_dict, page_dict, Shading, &shading);
    if (code != 0) {
        (void) pdfi_grestore(ctx);
        dbgmprintf(ctx->memory, "ERROR: can't build shading structure\n");
        goto exit;
    }

    /* We need to call pdfi_pattern_setup (in order to create the ColorSpace) before
     * we call pdfi_shading_build. So we can't pass the shading dictionary to pdfi_pattern_setup
     * in the same way as the type 1 pattern passes its persistent data, so we need to do it
     * ourselves here.
     */
    context = (pdf_pattern_context_t *)templat.client_data;
    context->shading = shading;

    templat.Shading = shading;
    code = gs_make_pattern(cc, (const gs_pattern_template_t *)&templat, &mat, ctx->pgs, ctx->memory);
    if (code < 0) {
        (void) pdfi_grestore(ctx);
        goto exit;
    }

    code = pdfi_grestore(ctx);
    if (code < 0)
        goto exit;

 exit:
    pdfi_countdown(Shading);
    pdfi_countdown(Matrix);
    pdfi_countdown(ExtGState);
    return code;
}

int
pdfi_pattern_set(pdf_context *ctx, pdf_dict *stream_dict,
                pdf_dict *page_dict, pdf_name *pname,
                gs_client_color *cc)
{
    pdf_dict *pdict = NULL;
    int code;
    int64_t patternType;

    memset(cc, 0, sizeof(*cc));
    code = pdfi_find_resource(ctx, (unsigned char *)"Pattern", pname, stream_dict,
                              page_dict, (pdf_obj **)&pdict);
    if (code < 0) {
        dbgmprintf(ctx->memory, "WARNING: Pattern object not found in resources\n");
        goto exit;
    }

    if (pdict->type != PDF_DICT) {
        /* NOTE: Bug696410.pdf gets a bogus pattern while trying to process pattern.
         * Seems like a corrupted file, but this prevents crash
         */
        dbgmprintf(ctx->memory, "ERROR: Pattern found in resources is not a dict\n");
        code = gs_note_error(gs_error_typecheck);
        goto exit;
    }
    dbgmprintf1(ctx->memory, "PATTERN: pdfi_setpattern: found pattern object %lu\n", pdict->object_num);

    code = pdfi_dict_get_int(ctx, pdict, "PatternType", &patternType);
    if (code < 0)
        goto exit;
    if (patternType == 1) {
        code = pdfi_setpattern_type1(ctx, stream_dict, page_dict, pdict, cc);
        if (code < 0)
            goto exit;
    } else if (patternType == 2) {
        code = pdfi_setpattern_type2(ctx, stream_dict, page_dict, pdict, cc);
        if (code < 0)
            goto exit;
    } else {
        code = gs_note_error(gs_error_syntaxerror);
        goto exit;
    }

 exit:
    pdfi_countdown(pdict);
    return code;
}

/* Create a Pattern colorspace.
 * If ppcs is NULL, then we will set the colorspace
 * If ppcs not NULL, point the new colorspace to it
 *
 * If color_array is NULL, then this is a simple "Pattern" colorspace, e.g. "/Pattern cs".
 * If it is an array, then first element is "Pattern" and second element should be the base colorspace.
 * e.g. "/CS1 cs" where /CS1 is a ColorSpace Resource containing "[/Pattern /DeviceRGB]"
 *
 */
int
pdfi_pattern_create(pdf_context *ctx, pdf_array *color_array,
                    pdf_dict *stream_dict, pdf_dict *page_dict, gs_color_space **ppcs)
{
    gs_color_space *pcs = NULL;
    gs_color_space *base_space;
    pdf_obj *base_obj = NULL;
    int code = 0;

    /* TODO: should set to "the initial color is a pattern object that causes nothing to be painted."
     * (see page 288 of PDF 1.7)
     * Need to make a "nullpattern" (see pdf_ops.c, /nullpattern)
     */
    /* NOTE: See zcolor.c/setpatternspace */
    dbgmprintf(ctx->memory, "PATTERN: pdfi_create_Pattern\n");
    //    return 0;

    pcs = gs_cspace_alloc(ctx->memory, &gs_color_space_type_Pattern);
    if (pcs == NULL) {
        return_error(gs_error_VMerror);
    }
    if (color_array == NULL || pdfi_array_size(color_array) == 1) {
        pcs->base_space = NULL;
        pcs->params.pattern.has_base_space = false;
    } else {
        dbgmprintf(ctx->memory, "PATTERN: with base space! pdfi_create_Pattern\n");
        code = pdfi_array_get(ctx, color_array, 1, &base_obj);
        if (code < 0)
            goto exit;
        code = pdfi_create_colorspace(ctx, base_obj, stream_dict, page_dict, &base_space, false);
        if (code < 0)
            goto exit;
        pcs->base_space = base_space;
        pcs->params.pattern.has_base_space = true;
    }
    if (ppcs != NULL) {
        *ppcs = pcs;
    } else {
        code = gs_setcolorspace(ctx->pgs, pcs);
#if 0
        /* An attempt to init a "Null" pattern, causes crashes on cluster */
        {
        gs_client_color cc;
        memset(&cc, 0, sizeof(cc));
        code = pdfi_setpattern_null(ctx, &cc);
        code = gs_setcolor(ctx->pgs, &cc);
        }
#endif
    }


 exit:
    /* release reference from construction */
    rc_decrement_only_cs(pcs, "create_Pattern");
    pdfi_countdown(base_obj);
    return code;
}