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
 */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "BLI_math.h"

#include "DNA_defaults.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_context.h"
#include "BKE_deform.h"
#include "BKE_gpencil_geom.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_lib_query.h"
#include "BKE_modifier.h"
#include "BKE_screen.h"

#include "DEG_depsgraph.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "MOD_gpencil_modifiertypes.h"
#include "MOD_gpencil_ui_common.h"
#include "MOD_gpencil_util.h"

static void initData(GpencilModifierData *md)
{
  OffsetGpencilModifierData *gpmd = (OffsetGpencilModifierData *)md;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(gpmd, modifier));

  MEMCPY_STRUCT_AFTER(gpmd, DNA_struct_default_get(OffsetGpencilModifierData), modifier);
}

static void copyData(const GpencilModifierData *md, GpencilModifierData *target)
{
  BKE_gpencil_modifier_copydata_generic(md, target);
}

/* change stroke offsetness */
static void deformPolyline(GpencilModifierData *md,
                           Depsgraph *UNUSED(depsgraph),
                           Object *ob,
                           bGPDlayer *gpl,
                           bGPDframe *UNUSED(gpf),
                           bGPDstroke *gps)
{
  OffsetGpencilModifierData *mmd = (OffsetGpencilModifierData *)md;
  const int def_nr = BKE_object_defgroup_name_index(ob, mmd->vgname);

  float mat[4][4];
  float loc[3], rot[3], scale[3];

  if (!is_stroke_affected_by_modifier(ob,
                                      mmd->layername,
                                      mmd->material,
                                      mmd->pass_index,
                                      mmd->layer_pass,
                                      1,
                                      gpl,
                                      gps,
                                      mmd->flag & GP_OFFSET_INVERT_LAYER,
                                      mmd->flag & GP_OFFSET_INVERT_PASS,
                                      mmd->flag & GP_OFFSET_INVERT_LAYERPASS,
                                      mmd->flag & GP_OFFSET_INVERT_MATERIAL)) {
    return;
  }
  bGPdata *gpd = ob->data;

  for (int i = 0; i < gps->totpoints; i++) {
    bGPDspoint *pt = &gps->points[i];
    MDeformVert *dvert = gps->dvert != NULL ? &gps->dvert[i] : NULL;

    /* Verify vertex group. */
    const float weight = get_modifier_point_weight(
        dvert, (mmd->flag & GP_OFFSET_INVERT_VGROUP) != 0, def_nr);
    if (weight < 0.0f) {
      continue;
    }
    /* Calculate matrix. */
    mul_v3_v3fl(loc, mmd->loc, weight);
    mul_v3_v3fl(rot, mmd->rot, weight);
    mul_v3_v3fl(scale, mmd->scale, weight);
    add_v3_fl(scale, 1.0);
    loc_eul_size_to_mat4(mat, loc, rot, scale);

    /* Apply scale to thickness. */
    float unit_scale = (scale[0] + scale[1] + scale[2]) / 3.0f;
    pt->pressure *= unit_scale;

    mul_m4_v3(mat, &pt->x);
  }
  /* Calc geometry data. */
  BKE_gpencil_stroke_geometry_update(gpd, gps);
}

static void deformBezier(GpencilModifierData *md,
                         Depsgraph *UNUSED(depsgraph),
                         Object *ob,
                         bGPDlayer *gpl,
                         bGPDframe *UNUSED(gpf),
                         bGPDstroke *gps)
{
  OffsetGpencilModifierData *mmd = (OffsetGpencilModifierData *)md;
  const int def_nr = BKE_object_defgroup_name_index(ob, mmd->vgname);

  float mat[4][4];
  float loc[3], rot[3], scale[3];

  if (!is_stroke_affected_by_modifier(ob,
                                      mmd->layername,
                                      mmd->material,
                                      mmd->pass_index,
                                      mmd->layer_pass,
                                      1,
                                      gpl,
                                      gps,
                                      mmd->flag & GP_OFFSET_INVERT_LAYER,
                                      mmd->flag & GP_OFFSET_INVERT_PASS,
                                      mmd->flag & GP_OFFSET_INVERT_LAYERPASS,
                                      mmd->flag & GP_OFFSET_INVERT_MATERIAL)) {
    return;
  }
  bGPdata *gpd = ob->data;
  bGPDcurve *gpc = gps->editcurve;

  for (int i = 0; i < gpc->tot_curve_points; i++) {
    bGPDcurve_point *pt = &gpc->curve_points[i];
    BezTriple *bezt = &pt->bezt;
    MDeformVert *dvert = (gpc->dvert != NULL) ? &gpc->dvert[i] : NULL;

    /* Verify vertex group. */
    const float weight = get_modifier_point_weight(
        dvert, (mmd->flag & GP_OFFSET_INVERT_VGROUP) != 0, def_nr);
    if (weight < 0.0f) {
      continue;
    }
    /* Calculate matrix. */
    mul_v3_v3fl(loc, mmd->loc, weight);
    mul_v3_v3fl(rot, mmd->rot, weight);
    mul_v3_v3fl(scale, mmd->scale, weight);
    add_v3_fl(scale, 1.0);
    loc_eul_size_to_mat4(mat, loc, rot, scale);

    /* Apply scale to thickness. */
    float unit_scale = (scale[0] + scale[1] + scale[2]) / 3.0f;
    pt->pressure *= unit_scale;

    for (int j = 0; j < 3; j++) {
      mul_m4_v3(mat, bezt->vec[j]);
    }
  }
  gps->flag |= GP_STROKE_NEEDS_CURVE_UPDATE; /* Calc geometry data. */

  BKE_gpencil_stroke_geometry_update(gpd, gps);
}

static void bakeModifier(struct Main *UNUSED(bmain),
                         Depsgraph *depsgraph,
                         GpencilModifierData *md,
                         Object *ob)
{
  bGPdata *gpd = ob->data;

  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        deformPolyline(md, depsgraph, ob, gpl, gpf, gps);
      }
    }
  }
}

static void foreachIDLink(GpencilModifierData *md, Object *ob, IDWalkFunc walk, void *userData)
{
  OffsetGpencilModifierData *mmd = (OffsetGpencilModifierData *)md;

  walk(userData, ob, (ID **)&mmd->material, IDWALK_CB_USER);
}

static void panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  uiLayout *layout = panel->layout;

  PointerRNA *ptr = gpencil_modifier_panel_get_property_pointers(panel, NULL);

  uiLayoutSetPropSep(layout, true);

  uiItemR(layout, ptr, "location", 0, NULL, ICON_NONE);
  uiItemR(layout, ptr, "rotation", 0, NULL, ICON_NONE);
  uiItemR(layout, ptr, "scale", 0, NULL, ICON_NONE);

  gpencil_modifier_panel_end(layout, ptr);
}

static void mask_panel_draw(const bContext *UNUSED(C), Panel *panel)
{
  gpencil_modifier_masking_panel_draw(panel, true, true);
}

static void panelRegister(ARegionType *region_type)
{
  PanelType *panel_type = gpencil_modifier_panel_register(
      region_type, eGpencilModifierType_Offset, panel_draw);
  gpencil_modifier_subpanel_register(
      region_type, "mask", "Influence", NULL, mask_panel_draw, panel_type);
}

GpencilModifierTypeInfo modifierType_Gpencil_Offset = {
    /* name */ "Offset",
    /* structName */ "OffsetGpencilModifierData",
    /* structSize */ sizeof(OffsetGpencilModifierData),
    /* type */ eGpencilModifierTypeType_Gpencil,
    /* flags */ eGpencilModifierTypeFlag_SupportsEditmode,

    /* copyData */ copyData,

    /* deformPolyline */ deformPolyline,
    /* deformBezier */ deformBezier,
    /* generateStrokes */ NULL,
    /* bakeModifier */ bakeModifier,
    /* remapTime */ NULL,

    /* initData */ initData,
    /* freeData */ NULL,
    /* isDisabled */ NULL,
    /* updateDepsgraph */ NULL,
    /* dependsOnTime */ NULL,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ NULL,
    /* panelRegister */ panelRegister,
};
