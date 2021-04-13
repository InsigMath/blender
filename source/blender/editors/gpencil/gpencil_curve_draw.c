/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2017, Blender Foundation
 * This is a new part of Blender
 * Operators for creating new Grease Pencil primitives (boxes, circles, ...)
 */

/** \file
 * \ingroup edgpencil
 */
#include <stdio.h>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string.h"

#include "BLT_translation.h"

#include "DNA_brush_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_brush.h"
#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_curve.h"
#include "BKE_gpencil_geom.h"
#include "BKE_main.h"
#include "BKE_paint.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_types.h"

#include "ED_gpencil.h"
#include "ED_screen.h"
#include "ED_space_api.h"
#include "ED_view3d.h"

#include "GPU_immediate.h"
#include "GPU_matrix.h"
#include "GPU_shader.h"
#include "GPU_state.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "gpencil_intern.h"

/* ------------------------------------------------------------------------- */
/* Structs & enums */

typedef enum eGPDcurve_draw_state {
  IN_MOVE = 0,
  IN_SET_VECTOR = 1,
  IN_DRAG_ALIGNED_HANDLE = 2,
  IN_DRAG_FREE_HANDLE = 3,
  IN_SET_THICKNESS = 4,
} eGPDcurve_draw_state;

typedef struct tGPDcurve_draw {
  Scene *scene;
  ARegion *region;
  Object *ob;
  bGPdata *gpd;
  bGPDlayer *gpl;
  bGPDframe *gpf;
  bGPDstroke *gps;
  bGPDcurve *gpc;
  int cframe;

  Brush *brush;

  GP_SpaceConversion gsc;

  /* imval of current event */
  int imval[2];
  /* imval of previous event */
  int imval_prev[2];
  /* imval when mouse was last pressed */
  int imval_start[2];
  /* imval when mouse was last released */
  int imval_end[2];
  bool is_mouse_down;

  bool is_cyclic;
  float prev_pressure;

  /* Curve resolution */
  int resolution;

  /* Callback for viewport drawing. */
  void *draw_handle;

  eGPDcurve_draw_state state;
} tGPDcurve_draw;

enum {
  CD_MODAL_CANCEL = 1,
  CD_MODAL_CONFIRM,
  CD_MODAL_FREE_HANDLE_ON,
  CD_MODAL_FREE_HANDLE_OFF,
  CD_MODAL_CYCLIC_TOGGLE,
  CD_MODAL_DELETE_LAST,
  CD_MODAL_SET_THICKNESS,
};

/* Forward declaration */
static void gpencil_curve_draw_init(bContext *C, wmOperator *op, const wmEvent *event);
static void gpencil_curve_draw_update(bContext *C, tGPDcurve_draw *tcd);
static void gpencil_curve_draw_confirm(bContext *C, wmOperator *op, tGPDcurve_draw *tcd);
static void gpencil_curve_draw_exit(bContext *C, wmOperator *op);

/* ------------------------------------------------------------------------- */
/* Helper functions */

static void debug_print_state(tGPDcurve_draw *tcd)
{
  const char *state_str[] = {"MOVE", "VECTOR", "ALIGN", "FREE", "THICK", "ALPHA"};
  printf("State: %s\tMouse x=%d\ty=%d\tpressed:%s\n",
         state_str[tcd->state],
         tcd->imval[0],
         tcd->imval[1],
         (tcd->is_mouse_down) ? "TRUE" : "FALSE");
}

static void gpencil_project_mval_to_v3(
    Scene *scene, ARegion *region, Object *ob, const int mval_i[2], float r_out[3])
{
  ToolSettings *ts = scene->toolsettings;
  float mval_f[2], mval_prj[2], rvec[3], dvec[3], zfac;
  copy_v2fl_v2i(mval_f, mval_i);

  ED_gpencil_drawing_reference_get(scene, ob, ts->gpencil_v3d_align, rvec);
  zfac = ED_view3d_calc_zfac(region->regiondata, rvec, NULL);

  if (ED_view3d_project_float_global(region, rvec, mval_prj, V3D_PROJ_TEST_NOP) ==
      V3D_PROJ_RET_OK) {
    sub_v2_v2v2(mval_f, mval_prj, mval_f);
    ED_view3d_win_to_delta(region, mval_f, dvec, zfac);
    sub_v3_v3v3(r_out, rvec, dvec);
  }
  else {
    zero_v3(r_out);
  }
}

/* Helper: Add a new curve point at the end (duplicating the previous last) */
static void gpencil_push_curve_point(bContext *C, tGPDcurve_draw *tcd)
{
  bGPDcurve *gpc = tcd->gpc;
  int old_num_points = gpc->tot_curve_points;
  int new_num_points = old_num_points + 1;
  gpc->tot_curve_points = new_num_points;

  gpc->curve_points = MEM_recallocN(gpc->curve_points, sizeof(bGPDcurve_point) * new_num_points);

  bGPDcurve_point *old_last = &gpc->curve_points[gpc->tot_curve_points - 2];
  bGPDcurve_point *new_last = &gpc->curve_points[gpc->tot_curve_points - 1];
  memcpy(new_last, old_last, sizeof(bGPDcurve_point));

  new_last->bezt.h1 = new_last->bezt.h2 = HD_VECT;

  BKE_gpencil_stroke_update_geometry_from_editcurve(
      tcd->gps, tcd->gpd->curve_edit_resolution, false, GP_GEO_UPDATE_DEFAULT);
}

/* Helper: Remove the last curve point */
static void gpencil_pop_curve_point(bContext *C, tGPDcurve_draw *tcd)
{
  bGPdata *gpd = tcd->gpd;
  bGPDstroke *gps = tcd->gps;
  bGPDcurve *gpc = tcd->gpc;
  const int old_num_points = gpc->tot_curve_points;
  const int new_num_points = old_num_points - 1;
  if (G.debug & G_DEBUG) {
    printf("old: %d, new: %d\n", old_num_points, new_num_points);
  }

  /* Create new stroke and curve */
  bGPDstroke *new_stroke = BKE_gpencil_stroke_duplicate(tcd->gps, false, false);
  new_stroke->points = NULL;

  bGPDcurve *new_curve = BKE_gpencil_stroke_editcurve_new(new_num_points);
  new_curve->flag = gpc->flag;
  memcpy(new_curve->curve_points, gpc->curve_points, sizeof(bGPDcurve_point) * new_num_points);
  new_stroke->editcurve = new_curve;

  BKE_gpencil_stroke_update_geometry_from_editcurve(
      new_stroke, gpd->curve_edit_resolution, false, GP_GEO_UPDATE_DEFAULT);

  /* Remove and free old stroke and curve */
  BLI_remlink(&tcd->gpf->strokes, gps);
  BKE_gpencil_free_stroke(gps);

  tcd->gps = new_stroke;
  tcd->gpc = new_curve;

  BLI_addtail(&tcd->gpf->strokes, new_stroke);
  BKE_gpencil_stroke_geometry_update(gpd, new_stroke, GP_GEO_UPDATE_DEFAULT);

  DEG_id_tag_update(&gpd->id, ID_RECALC_COPY_ON_WRITE);
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
}

static void gpencil_set_handle_type_last_point(tGPDcurve_draw *tcd, eBezTriple_Handle type)
{
  bGPDcurve *gpc = tcd->gpc;
  bGPDcurve_point *cpt = &gpc->curve_points[gpc->tot_curve_points - 1];
  cpt->bezt.h1 = cpt->bezt.h2 = type;
}

static void gpencil_set_alpha_last_segment(tGPDcurve_draw *tcd, float alpha)
{
  bGPDstroke *gps = tcd->gps;
  bGPDcurve *gpc = tcd->gpc;

  if (gpc->tot_curve_points < 2) {
    return;
  }

  bGPDcurve_point *old_last = &gpc->curve_points[gpc->tot_curve_points - 2];
  for (uint32_t i = old_last->point_index; i < gps->totpoints; i++) {
    bGPDspoint *pt = &gps->points[i];
    pt->strength = alpha;
  }
}

static void gpencil_curve_draw_ui_callback(const struct bContext *UNUSED(C),
                                           struct ARegion *UNUSED(region),
                                           void *customdata)
{
  const tGPDcurve_draw *tcd = customdata;
  GPU_depth_test(GPU_DEPTH_NONE);

  GPU_matrix_push_projection();
  GPU_polygon_offset(1.0f, 1.0f);

  GPU_matrix_push();
  GPU_matrix_mul(tcd->ob->obmat);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);

  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Draw overlays. */
  if (ELEM(tcd->state, IN_DRAG_ALIGNED_HANDLE, IN_DRAG_FREE_HANDLE)) {
    bGPDcurve *gpc = tcd->gpc;
    bGPDcurve_point *cpt_last = &gpc->curve_points[gpc->tot_curve_points - 1];
    BezTriple *bezt = &cpt_last->bezt;

    float viewport[4];
    GPU_viewport_size_get_f(viewport);
    immUniform2fv("viewportSize", &viewport[2]);

    float color[4] = {0, 0, 0, 1.0f};
    UI_GetThemeColorType3fv(TH_GP_VERTEX_SELECT, SPACE_VIEW3D, color);

    /* TODO: Use the GPU_SHADER_3D_POLYLINE_* shader instead. GPU_line_smooth will be deprecated.
     */
    GPU_line_smooth(true);
    GPU_blend(GPU_BLEND_ALPHA);

    // immUniform1f("lineWidth", U.pixelsize * 2.0f);
    // immUniform1i("lineSmooth", 1);

    immUniformColor4fv(color);

    /* Handle lines. */
    immBegin(GPU_PRIM_LINES, 4);
    immVertex3fv(pos, bezt->vec[0]);
    immVertex3fv(pos, bezt->vec[1]);
    immVertex3fv(pos, bezt->vec[1]);
    immVertex3fv(pos, bezt->vec[2]);
    immEnd();

    // immUniform1f("size", U.pixelsize * UI_GetThemeValuef(TH_GP_VERTEX_SIZE) * 2.0f);

    // immUniformColor4fv(color);

    /* Handle points. */
    immBegin(GPU_PRIM_POINTS, 3);
    immVertex3fv(pos, bezt->vec[0]);
    immVertex3fv(pos, bezt->vec[1]);
    immVertex3fv(pos, bezt->vec[2]);
    immEnd();

    GPU_line_smooth(false);
    GPU_blend(GPU_BLEND_NONE);
  }

  immUnbindProgram();

  GPU_matrix_pop();
  GPU_matrix_pop_projection();

  /* Reset default */
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
}

/* ------------------------------------------------------------------------- */
/* Main drawing functions */

static void gpencil_curve_draw_update_header(bContext *C,
                                             wmOperator *op,
                                             const tGPDcurve_draw *tcd)
{
  char header[UI_MAX_DRAW_STR];
  char buf[UI_MAX_DRAW_STR];

  char *p = buf;
  int available_len = sizeof(buf);

#define WM_MODALKEY(_id) \
  WM_modalkeymap_operator_items_to_string_buf( \
      op->type, (_id), true, UI_MAX_SHORTCUT_STR, &available_len, &p)

  switch (tcd->state) {
    case IN_MOVE:
    case IN_SET_VECTOR:
      BLI_snprintf(header,
                   sizeof(header),
                   TIP_("%s: confirm, %s: cancel, "
                        "%s: toggle cyclic (%s), "
                        "%s: delete last, %s: set thickness"),
                   WM_MODALKEY(CD_MODAL_CONFIRM),
                   WM_MODALKEY(CD_MODAL_CANCEL),
                   WM_MODALKEY(CD_MODAL_CYCLIC_TOGGLE),
                   WM_bool_as_string(tcd->is_cyclic),
                   WM_MODALKEY(CD_MODAL_DELETE_LAST),
                   WM_MODALKEY(CD_MODAL_SET_THICKNESS));
      break;
    case IN_DRAG_FREE_HANDLE:
    case IN_DRAG_ALIGNED_HANDLE:
      BLI_snprintf(header,
                   sizeof(header),
                   TIP_("%s: confirm, %s: cancel, "
                        "%s: toggle cyclic (%s), "
                        "%s: free handle (%s), "
                        "%s: delete last, %s: set thickness"),
                   WM_MODALKEY(CD_MODAL_CONFIRM),
                   WM_MODALKEY(CD_MODAL_CANCEL),
                   WM_MODALKEY(CD_MODAL_CYCLIC_TOGGLE),
                   WM_bool_as_string(tcd->is_cyclic),
                   WM_MODALKEY(CD_MODAL_FREE_HANDLE_ON),
                   WM_bool_as_string(tcd->state == IN_DRAG_FREE_HANDLE),
                   WM_MODALKEY(CD_MODAL_DELETE_LAST),
                   WM_MODALKEY(CD_MODAL_SET_THICKNESS));
      break;
    case IN_SET_THICKNESS:
      BLI_snprintf(header,
                   sizeof(header),
                   TIP_("%s: confirm, %s: cancel, "
                        "%s: toggle cyclic (%s), "
                        "%s: delete last"),
                   WM_MODALKEY(CD_MODAL_CONFIRM),
                   WM_MODALKEY(CD_MODAL_CANCEL),
                   WM_MODALKEY(CD_MODAL_CYCLIC_TOGGLE),
                   WM_bool_as_string(tcd->is_cyclic),
                   WM_MODALKEY(CD_MODAL_DELETE_LAST));
      break;
  }

  ED_workspace_status_text(C, header);

#undef WM_MODALKEY
}

/* ------------------------------------------------------------------------- */
/* Main drawing functions */

static void gpencil_curve_draw_init(bContext *C, wmOperator *op, const wmEvent *event)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = CTX_data_gpencil_data(C);

  ToolSettings *ts = scene->toolsettings;
  Paint *paint = &ts->gp_paint->paint;
  int cfra = CFRA;

  /* Allocate temp curve draw data. */
  tGPDcurve_draw *tcd = MEM_callocN(sizeof(tGPDcurve_draw), __func__);
  tcd->scene = scene;
  tcd->region = region;
  tcd->gpd = gpd;
  tcd->ob = ob;
  /* Fixed resolution. */
  tcd->resolution = 32;

  /* Initialize mouse state */
  copy_v2_v2_int(tcd->imval, event->mval);
  copy_v2_v2_int(tcd->imval_prev, event->mval);
  tcd->is_mouse_down = (event->val == KM_PRESS);
  tcd->state = IN_SET_VECTOR;

  if ((paint->brush == NULL) || (paint->brush->gpencil_settings == NULL)) {
    BKE_brush_gpencil_paint_presets(bmain, ts, true);
  }

  Brush *brush = BKE_paint_toolslots_brush_get(paint, 0);
  BKE_brush_tool_set(brush, paint, 0);
  BKE_paint_brush_set(paint, brush);
  BrushGpencilSettings *brush_settings = brush->gpencil_settings;
  tcd->brush = brush;

  /* Get active layer or create a new one. */
  bGPDlayer *gpl = CTX_data_active_gpencil_layer(C);
  if (gpl == NULL) {
    gpl = BKE_gpencil_layer_addnew(tcd->gpd, DATA_("Curve"), true);
  }
  tcd->gpl = gpl;

  /* Recalculate layer transform matrix to avoid problems if props are animated. */
  loc_eul_size_to_mat4(
      tcd->gpl->layer_mat, tcd->gpl->location, tcd->gpl->rotation, tcd->gpl->scale);
  invert_m4_m4(tcd->gpl->layer_invmat, tcd->gpl->layer_mat);

  /* Get current frame or create new one. */
  short add_frame_mode;
  if (ts->gpencil_flags & GP_TOOL_FLAG_RETAIN_LAST) {
    add_frame_mode = GP_GETFRAME_ADD_COPY;
  }
  else {
    add_frame_mode = GP_GETFRAME_ADD_NEW;
  }

  tcd->cframe = cfra;
  bool need_tag = tcd->gpl->actframe == NULL;
  bGPDframe *gpf = BKE_gpencil_layer_frame_get(tcd->gpl, tcd->cframe, add_frame_mode);
  if (need_tag) {
    DEG_id_tag_update(&tcd->gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  }
  tcd->gpf = gpf;

  /* Create stroke. */
  int mat_idx = BKE_gpencil_object_material_get_index_from_brush(ob, brush);
  bGPDstroke *gps = BKE_gpencil_stroke_new(mat_idx, 1, brush->size);
  gps->thickness = brush->size;
  gps->hardeness = brush_settings->hardeness;
  copy_v2_v2(gps->aspect_ratio, brush_settings->aspect_ratio);

  float first_pt[3];
  gpencil_project_mval_to_v3(scene, region, ob, tcd->imval, first_pt);
  gps->points[0].pressure = 1.0f;
  gps->points[0].strength = 1.0f;
  copy_v3_v3(&gps->points[0].x, first_pt);

  BLI_addtail(&gpf->strokes, gps);
  tcd->gps = gps;

  /* Create editcurve. */
  bGPDcurve *gpc = BKE_gpencil_stroke_editcurve_new(1);
  bGPDcurve_point *cpt = &gpc->curve_points[0];
  copy_v3_v3(cpt->bezt.vec[0], first_pt);
  copy_v3_v3(cpt->bezt.vec[1], first_pt);
  copy_v3_v3(cpt->bezt.vec[2], first_pt);
  cpt->pressure = 1.0f;
  cpt->strength = 1.0f;

  gps->editcurve = gpc;
  tcd->gpc = gpc;

  /* Calc geometry data. */
  BKE_gpencil_stroke_geometry_update(tcd->gpd, gps, GP_GEO_UPDATE_DEFAULT);

  /* Initialize space conversion. */
  gpencil_point_conversion_init(C, &tcd->gsc);

  tcd->draw_handle = ED_region_draw_cb_activate(
      tcd->region->type, gpencil_curve_draw_ui_callback, tcd, REGION_DRAW_POST_VIEW);

  gpencil_curve_draw_update(C, tcd);
  op->customdata = tcd;
}

static void gpencil_curve_draw_update(bContext *C, tGPDcurve_draw *tcd)
{
  bGPdata *gpd = tcd->gpd;
  bGPDstroke *gps = tcd->gps;
  bGPDcurve *gpc = tcd->gpc;
  int tot_points = gpc->tot_curve_points;
  bGPDcurve_point *cpt = &gpc->curve_points[tot_points - 1];
  BezTriple *bezt = &cpt->bezt;

  float co[3];
  switch (tcd->state) {
    case IN_MOVE: {
      gpencil_project_mval_to_v3(tcd->scene, tcd->region, tcd->ob, tcd->imval, co);
      copy_v3_v3(bezt->vec[0], co);
      copy_v3_v3(bezt->vec[1], co);
      copy_v3_v3(bezt->vec[2], co);

      BKE_gpencil_editcurve_recalculate_handles(gps);
      BKE_gpencil_stroke_update_geometry_from_editcurve(
          gps, tcd->resolution, false, GP_GEO_UPDATE_DEFAULT);
      gpencil_set_alpha_last_segment(tcd, 0.1f);
      break;
    }
    case IN_DRAG_ALIGNED_HANDLE: {
      float vec[3];
      gpencil_project_mval_to_v3(tcd->scene, tcd->region, tcd->ob, tcd->imval, co);
      sub_v3_v3v3(vec, bezt->vec[1], co);
      add_v3_v3(vec, bezt->vec[1]);
      copy_v3_v3(bezt->vec[0], vec);
      copy_v3_v3(bezt->vec[2], co);

      BKE_gpencil_stroke_update_geometry_from_editcurve(
          gps, tcd->resolution, false, GP_GEO_UPDATE_DEFAULT);
      break;
    }
    case IN_DRAG_FREE_HANDLE: {
      gpencil_project_mval_to_v3(tcd->scene, tcd->region, tcd->ob, tcd->imval, co);
      copy_v3_v3(bezt->vec[2], co);

      BKE_gpencil_stroke_update_geometry_from_editcurve(
          gps, tcd->resolution, false, GP_GEO_UPDATE_DEFAULT);
      break;
    }
    case IN_SET_THICKNESS: {
      int move[2];
      sub_v2_v2v2_int(move, tcd->imval, tcd->imval_start);
      int dir = move[0] > 0.0f ? 1 : -1;
      int dist = len_manhattan_v2_int(move);
      /* TODO: calculate correct radius. */
      float dr = dir * ((float)dist / 10.0f);
      cpt->pressure = tcd->prev_pressure + dr;
      CLAMP_MIN(cpt->pressure, 0.0f);

      BKE_gpencil_stroke_update_geometry_from_editcurve(
          gps, tcd->resolution, false, GP_GEO_UPDATE_DEFAULT);
      break;
    }
    default:
      break;
  }

  BKE_gpencil_stroke_geometry_update(gpd, gps, GP_GEO_UPDATE_DEFAULT);

  DEG_id_tag_update(&gpd->id, ID_RECALC_COPY_ON_WRITE);
  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
}

static void gpencil_curve_draw_confirm(bContext *C, wmOperator *op, tGPDcurve_draw *tcd)
{
  if (G.debug & G_DEBUG) {
    printf("Confirm curve draw\n");
  }
  bGPDcurve *gpc = tcd->gpc;
  int tot_points = gpc->tot_curve_points;
  bGPDcurve_point *cpt = &gpc->curve_points[tot_points - 1];
  cpt->flag &= ~GP_CURVE_POINT_SELECT;
  BEZT_DESEL_ALL(&cpt->bezt);

  BKE_gpencil_editcurve_recalculate_handles(tcd->gps);
}

static void gpencil_curve_draw_exit(bContext *C, wmOperator *op)
{
  if (G.debug & G_DEBUG) {
    printf("Exit curve draw\n");
  }

  wmWindow *win = CTX_wm_window(C);
  tGPDcurve_draw *tcd = op->customdata;

  ED_workspace_status_text(C, NULL);
  WM_cursor_modal_restore(win);

  ED_region_draw_cb_exit(tcd->region->type, tcd->draw_handle);

  bGPdata *gpd = tcd->gpd;

  MEM_SAFE_FREE(tcd);

  DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_COPY_ON_WRITE);
  WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, NULL);

  op->customdata = NULL;
}

/* ------------------------------------------------------------------------- */
/* Operator callbacks */

static int gpencil_curve_draw_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (G.debug & G_DEBUG) {
    printf("Invoke curve draw\n");
  }
  wmWindow *win = CTX_wm_window(C);

  /* Set cursor to dot. */
  WM_cursor_modal_set(win, WM_CURSOR_DOT);

  gpencil_curve_draw_init(C, op, event);
  // tGPDcurve_draw *tcd = op->customdata;

  /* Add modal handler. */
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static int gpencil_curve_draw_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  tGPDcurve_draw *tcd = op->customdata;
  wmWindow *win = CTX_wm_window(C);
  float drag_threshold = (float)WM_event_drag_threshold(event);

  copy_v2_v2_int(tcd->imval, event->mval);

  /* Modal keymap event. */
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case CD_MODAL_CONFIRM: {
        /* Delete the 'preview' point. */
        if (tcd->state == IN_MOVE) {
          gpencil_pop_curve_point(C, tcd);
        }
        /* Create curve */
        gpencil_curve_draw_confirm(C, op, tcd);
        gpencil_curve_draw_exit(C, op);
        return OPERATOR_FINISHED;
      }
      case CD_MODAL_CANCEL: {
        /* Delete the stroke. */
        BLI_remlink(&tcd->gpf->strokes, tcd->gps);
        BKE_gpencil_free_stroke(tcd->gps);
        gpencil_curve_draw_exit(C, op);
        return OPERATOR_CANCELLED;
      }
      case CD_MODAL_FREE_HANDLE_ON: {
        if (tcd->state == IN_DRAG_ALIGNED_HANDLE) {
          tcd->state = IN_DRAG_FREE_HANDLE;
          gpencil_set_handle_type_last_point(tcd, HD_FREE);
          gpencil_curve_draw_update(C, tcd);
        }
        break;
      }
      case CD_MODAL_FREE_HANDLE_OFF: {
        if (tcd->state == IN_DRAG_FREE_HANDLE) {
          tcd->state = IN_DRAG_ALIGNED_HANDLE;
          gpencil_set_handle_type_last_point(tcd, HD_ALIGN);
          gpencil_curve_draw_update(C, tcd);
        }
        break;
      }
      case CD_MODAL_CYCLIC_TOGGLE: {
        if (tcd->is_cyclic) {
          tcd->gps->flag &= ~GP_STROKE_CYCLIC;
        }
        else {
          tcd->gps->flag |= GP_STROKE_CYCLIC;
        }
        tcd->is_cyclic = !tcd->is_cyclic;
        gpencil_curve_draw_update(C, tcd);
        break;
      }
      case CD_MODAL_DELETE_LAST: {
        if (tcd->state == IN_MOVE) {
          gpencil_pop_curve_point(C, tcd);
        }
        else if (ELEM(tcd->state, IN_DRAG_ALIGNED_HANDLE, IN_DRAG_FREE_HANDLE)) {
          tcd->state = IN_MOVE;
        }
        gpencil_curve_draw_update(C, tcd);
        break;
      }
      case CD_MODAL_SET_THICKNESS: {
        if (tcd->state != IN_SET_THICKNESS) {
          tcd->state = IN_SET_THICKNESS;
          WM_cursor_modal_set(win, WM_CURSOR_EW_SCROLL);

          bGPDcurve_point *cpt_last = &tcd->gpc->curve_points[tcd->gpc->tot_curve_points - 1];
          tcd->prev_pressure = cpt_last->pressure;
          copy_v2_v2_int(tcd->imval_start, tcd->imval);

          gpencil_curve_draw_update(C, tcd);
        }
        break;
      }
    }
  }
  /* Event not in keymap. */
  else {
    switch (event->type) {
      case LEFTMOUSE: {
        if (event->val == KM_PRESS) {
          copy_v2_v2_int(tcd->imval_start, tcd->imval);
          tcd->is_mouse_down = true;
          /* Set state to vector. */
          if (tcd->state == IN_MOVE) {
            tcd->state = IN_SET_VECTOR;
          }
          /* Reset state to move. */
          else if (tcd->state == IN_SET_THICKNESS) {
            tcd->state = IN_MOVE;
            WM_cursor_modal_set(win, WM_CURSOR_DOT);
          }
        }
        else if (event->val == KM_RELEASE) {
          copy_v2_v2_int(tcd->imval_end, tcd->imval);
          tcd->is_mouse_down = false;
          /* Reset state to move. */
          if (ELEM(tcd->state, IN_SET_VECTOR, IN_DRAG_ALIGNED_HANDLE, IN_DRAG_FREE_HANDLE)) {
            tcd->state = IN_MOVE;
            gpencil_push_curve_point(C, tcd);
          }

          gpencil_curve_draw_update(C, tcd);
        }
        break;
      }
      case MOUSEMOVE: {
        if (tcd->state == IN_SET_VECTOR &&
            len_v2v2_int(tcd->imval, tcd->imval_start) > drag_threshold) {
          tcd->state = IN_DRAG_ALIGNED_HANDLE;
          gpencil_set_handle_type_last_point(tcd, HD_ALIGN);
        }
        gpencil_curve_draw_update(C, tcd);
        break;
      }
      default: {
        copy_v2_v2_int(tcd->imval_prev, tcd->imval);
        return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
      }
    }
  }

  gpencil_curve_draw_update_header(C, op, tcd);

  if (G.debug & G_DEBUG) {
    debug_print_state(tcd);
  }
  copy_v2_v2_int(tcd->imval_prev, tcd->imval);
  return OPERATOR_RUNNING_MODAL;
}

static void gpencil_curve_draw_cancel(bContext *C, wmOperator *op)
{
  if (G.debug & G_DEBUG) {
    printf("Cancel curve draw\n");
  }
  gpencil_curve_draw_exit(C, op);
}

static bool gpencil_curve_draw_poll(bContext *C)
{
  if (G.debug & G_DEBUG) {
    printf("Poll curve draw\n");
  }
  ScrArea *area = CTX_wm_area(C);
  if (area && area->spacetype != SPACE_VIEW3D) {
    return false;
  }

  bGPdata *gpd = CTX_data_gpencil_data(C);
  if (gpd == NULL) {
    return false;
  }

  if ((gpd->flag & GP_DATA_STROKE_PAINTMODE) == 0) {
    return false;
  }

  bGPDlayer *gpl = BKE_gpencil_layer_active_get(gpd);
  if ((gpl) && (gpl->flag & (GP_LAYER_LOCKED | GP_LAYER_HIDE))) {
    return false;
  }

  return true;
}

wmKeyMap *gpencil_curve_draw_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {CD_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {CD_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
      {CD_MODAL_FREE_HANDLE_ON, "FREE_HANDLE_ON", 0, "Free Handle On", ""},
      {CD_MODAL_FREE_HANDLE_OFF, "FREE_HANDLE_OFF", 0, "Free Handle Off", ""},
      {CD_MODAL_CYCLIC_TOGGLE, "CYCLIC_TOGGLE", 0, "Toggle Stroke Cyclic", ""},
      {CD_MODAL_DELETE_LAST, "DELETE_LAST", 0, "Delete the Last Confirmed Point", ""},
      {CD_MODAL_SET_THICKNESS, "SET_THICKNESS", 0, "Set the Thickness", ""},
      {0, NULL, 0, NULL, NULL},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Curve Draw Tool Modal Map");

  /* this function is called for each spacetype, only needs to add map once */
  if (keymap && keymap->modal_items) {
    return NULL;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Curve Draw Tool Modal Map", modal_items);

  WM_modalkeymap_assign(keymap, "GPENCIL_OT_draw_curve");

  return keymap;
}

void GPENCIL_OT_draw_curve(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Grease Pencil Draw Curve";
  ot->idname = "GPENCIL_OT_draw_curve";
  ot->description = "Draw a bézier stroke in the active grease pencil object";

  /* api callbacks */
  ot->invoke = gpencil_curve_draw_invoke;
  ot->modal = gpencil_curve_draw_modal;
  ot->cancel = gpencil_curve_draw_cancel;
  ot->poll = gpencil_curve_draw_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}
