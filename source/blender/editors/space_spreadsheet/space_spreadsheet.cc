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
 */

#include <cstring>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "BKE_geometry_set.hh"
#include "BKE_screen.h"

#include "ED_screen.h"
#include "ED_space_api.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "UI_interface.h"
#include "UI_resources.h"
#include "UI_view2d.h"

#include "DEG_depsgraph_query.h"

#include "RNA_access.h"

#include "WM_types.h"

#include "GPU_immediate.h"

#include "spreadsheet_intern.hh"

using blender::float3;
using blender::IndexRange;
using blender::MutableSpan;
using blender::Set;
using blender::Span;
using blender::StringRef;
using blender::StringRefNull;
using blender::Vector;
using blender::bke::ReadAttribute;
using blender::bke::ReadAttributePtr;
using blender::fn::CPPType;
using blender::fn::GMutableSpan;
using blender::fn::GSpan;

static SpaceLink *spreadsheet_create(const ScrArea *UNUSED(area), const Scene *UNUSED(scene))
{
  SpaceSpreadsheet *spreadsheet_space = (SpaceSpreadsheet *)MEM_callocN(sizeof(SpaceSpreadsheet),
                                                                        "spreadsheet space");
  spreadsheet_space->spacetype = SPACE_SPREADSHEET;

  {
    /* header */
    ARegion *region = (ARegion *)MEM_callocN(sizeof(ARegion), "spreadsheet header");
    BLI_addtail(&spreadsheet_space->regionbase, region);
    region->regiontype = RGN_TYPE_HEADER;
    region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;
  }

  {
    /* main window */
    ARegion *region = (ARegion *)MEM_callocN(sizeof(ARegion), "spreadsheet main region");
    BLI_addtail(&spreadsheet_space->regionbase, region);
    region->regiontype = RGN_TYPE_WINDOW;
  }

  return (SpaceLink *)spreadsheet_space;
}

static void spreadsheet_free(SpaceLink *UNUSED(sl))
{
}

static void spreadsheet_init(wmWindowManager *UNUSED(wm), ScrArea *UNUSED(area))
{
}

static SpaceLink *spreadsheet_duplicate(SpaceLink *sl)
{
  return (SpaceLink *)MEM_dupallocN(sl);
}

static void spreadsheet_keymap(wmKeyConfig *UNUSED(keyconf))
{
}

static void spreadsheet_main_region_init(wmWindowManager *UNUSED(wm), ARegion *region)
{
  region->v2d.scroll = V2D_SCROLL_RIGHT | V2D_SCROLL_BOTTOM;
  region->v2d.align = V2D_ALIGN_NO_NEG_X | V2D_ALIGN_NO_POS_Y;
  region->v2d.keepzoom = V2D_LOCKZOOM_X | V2D_LOCKZOOM_Y | V2D_LIMITZOOM | V2D_KEEPASPECT;
  region->v2d.keeptot = V2D_KEEPTOT_STRICT;
  region->v2d.minzoom = region->v2d.maxzoom = 1.0f;

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_LIST, region->winx, region->winy);
}

class ColumnHeaderDrawer {
 public:
  virtual ~ColumnHeaderDrawer() = default;
  virtual void draw_header(uiBlock *block, const rcti &rect) const = 0;
};

class CellDrawer {
 public:
  virtual ~CellDrawer() = default;
  virtual void draw_cell(uiBlock *block, const rcti &rect, const int index) const = 0;
};

struct SpreadsheetColumnLayout {
  int width;
  const ColumnHeaderDrawer *header_drawer = nullptr;
  const CellDrawer *cell_drawer = nullptr;
};

struct SpreadsheetLayout {
  int index_column_width;
  int title_row_height;
  int row_height;
  Vector<SpreadsheetColumnLayout> columns;
};

class TextColumnHeaderDrawer final : public ColumnHeaderDrawer {
 private:
  std::string text_;

 public:
  TextColumnHeaderDrawer(std::string text) : text_(std::move(text))
  {
  }

  void draw_header(uiBlock *block, const rcti &rect) const final
  {
    uiBut *but = uiDefIconTextBut(block,
                                  UI_BTYPE_LABEL,
                                  0,
                                  ICON_NONE,
                                  text_.c_str(),
                                  rect.xmin,
                                  rect.ymin,
                                  BLI_rcti_size_x(&rect),
                                  BLI_rcti_size_y(&rect),
                                  nullptr,
                                  0,
                                  0,
                                  0,
                                  0,
                                  nullptr);
    UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
    UI_but_drawflag_disable(but, UI_BUT_TEXT_RIGHT);
  }
};

class ConstantTextCellDrawer final : public CellDrawer {
 private:
  std::string text_;

 public:
  ConstantTextCellDrawer(std::string text) : text_(std::move(text))
  {
  }

  void draw_cell(uiBlock *block, const rcti &rect, const int UNUSED(index)) const final
  {
    uiDefIconTextBut(block,
                     UI_BTYPE_LABEL,
                     0,
                     ICON_NONE,
                     text_.c_str(),
                     rect.xmin,
                     rect.ymin,
                     BLI_rcti_size_x(&rect),
                     BLI_rcti_size_y(&rect),
                     nullptr,
                     0,
                     0,
                     0,
                     0,
                     nullptr);
  }
};

static void draw_index_column_background(const uint pos,
                                         const ARegion *region,
                                         const SpreadsheetLayout &spreadsheet_layout)
{
  immUniformThemeColorShade(TH_BACK, 11);
  immRecti(pos,
           0,
           region->winy - spreadsheet_layout.title_row_height,
           spreadsheet_layout.index_column_width,
           0);
}

static void draw_alternating_row_overlay(const uint pos,
                                         const int scroll_offset_y,
                                         const ARegion *region,
                                         const SpreadsheetLayout &spreadsheet_layout)
{
  immUniformThemeColor(TH_ROW_ALTERNATE);
  GPU_blend(GPU_BLEND_ALPHA);
  const int row_pair_height = spreadsheet_layout.row_height * 2;
  const int row_top_y = region->winy - spreadsheet_layout.title_row_height -
                        scroll_offset_y % row_pair_height;
  for (const int i : IndexRange(region->winy / row_pair_height + 1)) {
    int x_left = 0;
    int x_right = region->winx;
    int y_top = row_top_y - i * row_pair_height;
    int y_bottom = y_top - spreadsheet_layout.row_height;
    y_top = std::min(y_top, region->winy - spreadsheet_layout.title_row_height);
    y_bottom = std::min(y_bottom, region->winy - spreadsheet_layout.title_row_height);
    immRecti(pos, x_left, y_top, x_right, y_bottom);
  }
  GPU_blend(GPU_BLEND_NONE);
}

static void draw_title_row_background(const uint pos,
                                      const ARegion *region,
                                      const SpreadsheetLayout &spreadsheet_layout)
{
  immUniformThemeColorShade(TH_BACK, 11);
  immRecti(pos, 0, region->winy, region->winx, region->winy - spreadsheet_layout.title_row_height);
}

static void draw_separator_lines(const uint pos,
                                 const int scroll_offset_x,
                                 const ARegion *region,
                                 const SpreadsheetLayout &spreadsheet_layout)
{
  immUniformThemeColorShade(TH_BACK, -11);

  immBeginAtMost(GPU_PRIM_LINES, spreadsheet_layout.columns.size() * 2 + 4);

  /* Index column line. */
  immVertex2i(pos, spreadsheet_layout.index_column_width, region->winy);
  immVertex2i(pos, spreadsheet_layout.index_column_width, 0);

  /* Title row line. */
  immVertex2i(pos, 0, region->winy - spreadsheet_layout.title_row_height);
  immVertex2i(pos, region->winx, region->winy - spreadsheet_layout.title_row_height);

  /* Column separator lines. */
  int line_x = spreadsheet_layout.index_column_width - scroll_offset_x;
  for (const int i : spreadsheet_layout.columns.index_range()) {
    const SpreadsheetColumnLayout &column = spreadsheet_layout.columns[i];
    line_x += column.width;
    if (line_x >= spreadsheet_layout.index_column_width) {
      immVertex2i(pos, line_x, region->winy);
      immVertex2i(pos, line_x, 0);
    }
  }
  immEnd();
}

static void get_visible_rows(const SpreadsheetLayout &spreadsheet_layout,
                             const ARegion *region,
                             const int scroll_offset_y,
                             int *r_first_row,
                             int *r_max_visible_rows)
{
  *r_first_row = -scroll_offset_y / spreadsheet_layout.row_height;
  *r_max_visible_rows = region->winy / spreadsheet_layout.row_height + 1;
}

static void draw_row_indices(const int scroll_offset_y,
                             const Span<int64_t> row_indices,
                             const bContext *C,
                             ARegion *region,
                             const SpreadsheetLayout &spreadsheet_layout)
{
  GPU_scissor_test(true);
  GPU_scissor(0,
              0,
              spreadsheet_layout.index_column_width,
              region->winy - spreadsheet_layout.title_row_height);

  uiBlock *indices_block = UI_block_begin(C, region, __func__, UI_EMBOSS_NONE);
  int first_row, max_visible_rows;
  get_visible_rows(spreadsheet_layout, region, scroll_offset_y, &first_row, &max_visible_rows);
  for (const int i : IndexRange(first_row, max_visible_rows)) {
    if (i >= row_indices.size()) {
      break;
    }
    const int index = row_indices[i];
    const std::string index_str = std::to_string(index);
    const int x = 0;
    const int y = region->winy - spreadsheet_layout.title_row_height -
                  (i + 1) * spreadsheet_layout.row_height - scroll_offset_y;
    const int width = spreadsheet_layout.index_column_width;
    const int height = spreadsheet_layout.row_height;
    uiBut *but = uiDefIconTextBut(indices_block,
                                  UI_BTYPE_LABEL,
                                  0,
                                  ICON_NONE,
                                  index_str.c_str(),
                                  x,
                                  y,
                                  width,
                                  height,
                                  nullptr,
                                  0,
                                  0,
                                  0,
                                  0,
                                  nullptr);
    UI_but_drawflag_enable(but, UI_BUT_TEXT_RIGHT);
    UI_but_drawflag_disable(but, UI_BUT_TEXT_LEFT);
  }

  UI_block_end(C, indices_block);
  UI_block_draw(C, indices_block);

  GPU_scissor_test(false);
}

static void draw_column_headers(const bContext *C,
                                ARegion *region,
                                const SpreadsheetLayout &spreadsheet_layout,
                                const int scroll_offset_x)
{
  GPU_scissor_test(true);
  GPU_scissor(spreadsheet_layout.index_column_width + 1,
              region->winy - spreadsheet_layout.title_row_height,
              region->winx - spreadsheet_layout.index_column_width,
              spreadsheet_layout.title_row_height);

  uiBlock *column_headers_block = UI_block_begin(C, region, __func__, UI_EMBOSS_NONE);

  int left_x = spreadsheet_layout.index_column_width - scroll_offset_x;
  for (const int i : spreadsheet_layout.columns.index_range()) {
    const SpreadsheetColumnLayout &column_layout = spreadsheet_layout.columns[i];
    const int right_x = left_x + column_layout.width;

    rcti rect;
    BLI_rcti_init(&rect,
                  left_x,
                  right_x,
                  region->winy - spreadsheet_layout.index_column_width,
                  region->winy);
    if (column_layout.header_drawer != nullptr) {
      column_layout.header_drawer->draw_header(column_headers_block, rect);
    }

    left_x = right_x;
  }

  UI_block_end(C, column_headers_block);
  UI_block_draw(C, column_headers_block);

  GPU_scissor_test(false);
}

static void draw_cell_contents(const bContext *C,
                               ARegion *region,
                               const SpreadsheetLayout &spreadsheet_layout,
                               const Span<int64_t> row_indices,
                               const int scroll_offset_x,
                               const int scroll_offset_y)
{
  GPU_scissor_test(true);
  GPU_scissor(spreadsheet_layout.index_column_width + 1,
              0,
              region->winx - spreadsheet_layout.index_column_width,
              region->winy - spreadsheet_layout.title_row_height);

  uiBlock *cells_block = UI_block_begin(C, region, __func__, UI_EMBOSS_NONE);

  int first_row, max_visible_rows;
  get_visible_rows(spreadsheet_layout, region, scroll_offset_y, &first_row, &max_visible_rows);

  int left_x = spreadsheet_layout.index_column_width - scroll_offset_x;
  for (const int column_index : spreadsheet_layout.columns.index_range()) {
    const SpreadsheetColumnLayout &column_layout = spreadsheet_layout.columns[column_index];
    const int right_x = left_x + column_layout.width;

    for (const int i : IndexRange(first_row, max_visible_rows)) {
      if (i >= row_indices.size()) {
        break;
      }
      const int bottom_y = region->winy - spreadsheet_layout.title_row_height -
                           (i + 1) * spreadsheet_layout.row_height - scroll_offset_y;
      const int top_y = bottom_y + spreadsheet_layout.row_height;
      rcti rect;
      BLI_rcti_init(&rect, left_x, right_x, bottom_y, top_y);

      const int index = row_indices[i];

      if (column_layout.cell_drawer != nullptr) {
        column_layout.cell_drawer->draw_cell(cells_block, rect, index);
      }
    }

    left_x = right_x;
  }

  UI_block_end(C, cells_block);
  UI_block_draw(C, cells_block);

  GPU_scissor_test(false);
}

static void draw_spreadsheet(const bContext *C,
                             const SpreadsheetLayout &spreadsheet_layout,
                             ARegion *region,
                             Span<int64_t> row_indices)
{
  UI_ThemeClearColor(TH_BACK);

  View2D *v2d = &region->v2d;
  const int scroll_offset_y = v2d->cur.ymax;
  const int scroll_offset_x = v2d->cur.xmin;

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_I32, 2, GPU_FETCH_INT_TO_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_2D_UNIFORM_COLOR);

  draw_index_column_background(pos, region, spreadsheet_layout);
  draw_alternating_row_overlay(pos, scroll_offset_y, region, spreadsheet_layout);
  draw_title_row_background(pos, region, spreadsheet_layout);
  draw_separator_lines(pos, scroll_offset_x, region, spreadsheet_layout);

  immUnbindProgram();

  draw_row_indices(scroll_offset_y, row_indices, C, region, spreadsheet_layout);
  draw_column_headers(C, region, spreadsheet_layout, scroll_offset_x);
  draw_cell_contents(C, region, spreadsheet_layout, row_indices, scroll_offset_x, scroll_offset_y);

  rcti scroller_mask;
  BLI_rcti_init(&scroller_mask,
                spreadsheet_layout.index_column_width,
                region->winx,
                0,
                region->winy - spreadsheet_layout.title_row_height);
  UI_view2d_scrollers_draw(v2d, &scroller_mask);
}

static void update_view2d_tot_rect(const SpreadsheetLayout &spreadsheet_layout,
                                   ARegion *region,
                                   const int row_amount)
{
  int column_width_sum = 0;
  for (const SpreadsheetColumnLayout &column_layout : spreadsheet_layout.columns) {
    column_width_sum += column_layout.width;
  }
  UI_view2d_totRect_set(&region->v2d,
                        column_width_sum + spreadsheet_layout.index_column_width,
                        row_amount * spreadsheet_layout.row_height +
                            spreadsheet_layout.title_row_height);
}

static void spreadsheet_main_region_draw(const bContext *C, ARegion *region)
{
  TextColumnHeaderDrawer my_header_drawer{"Hello"};
  ConstantTextCellDrawer my_cell_drawer{"test"};

  SpreadsheetLayout spreadsheet_layout;
  spreadsheet_layout.index_column_width = 2 * UI_UNIT_X;
  spreadsheet_layout.row_height = UI_UNIT_Y;
  spreadsheet_layout.title_row_height = 1.5 * UI_UNIT_Y;
  spreadsheet_layout.columns.append({100, &my_header_drawer, &my_cell_drawer});
  spreadsheet_layout.columns.append({200, &my_header_drawer, &my_cell_drawer});
  spreadsheet_layout.columns.append({100, &my_header_drawer, &my_cell_drawer});
  spreadsheet_layout.columns.append({200, &my_header_drawer, &my_cell_drawer});
  spreadsheet_layout.columns.append({80, &my_header_drawer, &my_cell_drawer});

  const int row_amount = 101;
  draw_spreadsheet(C, spreadsheet_layout, region, IndexRange(row_amount).as_span());
  update_view2d_tot_rect(spreadsheet_layout, region, row_amount);
}

static void spreadsheet_main_region_listener(const wmRegionListenerParams *params)
{
  /* TODO: Do more precise check. */
  ED_region_tag_redraw(params->region);
}

static void spreadsheet_header_region_init(wmWindowManager *UNUSED(wm), ARegion *region)
{
  ED_region_header_init(region);
}

static void spreadsheet_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

static void spreadsheet_header_region_free(ARegion *UNUSED(region))
{
}

void ED_spacetype_spreadsheet(void)
{
  SpaceType *st = (SpaceType *)MEM_callocN(sizeof(SpaceType), "spacetype spreadsheet");
  ARegionType *art;

  st->spaceid = SPACE_SPREADSHEET;
  strncpy(st->name, "Spreadsheet", BKE_ST_MAXNAME);

  st->create = spreadsheet_create;
  st->free = spreadsheet_free;
  st->init = spreadsheet_init;
  st->duplicate = spreadsheet_duplicate;
  st->operatortypes = spreadsheet_operatortypes;
  st->keymap = spreadsheet_keymap;

  /* regions: main window */
  art = (ARegionType *)MEM_callocN(sizeof(ARegionType), "spacetype spreadsheet region");
  art->regionid = RGN_TYPE_WINDOW;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;

  art->init = spreadsheet_main_region_init;
  art->draw = spreadsheet_main_region_draw;
  art->listener = spreadsheet_main_region_listener;
  BLI_addhead(&st->regiontypes, art);

  /* regions: header */
  art = (ARegionType *)MEM_callocN(sizeof(ARegionType), "spacetype spreadsheet header region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = 0;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;

  art->init = spreadsheet_header_region_init;
  art->draw = spreadsheet_header_region_draw;
  art->free = spreadsheet_header_region_free;
  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(st);
}