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

/** \file
 * \ingroup bli
 */

/*

TODO:

Convergence improvements:
1. DONE: Limit number of edges processed per run.
2. DONE: Scale split steps by ratio of long to short edges to
   prevent runaway tesselation.
3. DONE: Detect and dissolve three and four valence vertices that are surrounded by
   all tris.
4. DONE: Use different (coarser) brush spacing for applying dyntopo

Drawing improvements:
4. PARTIAL DONE: Build and cache vertex index buffers, to reduce GPU bandwidth

Topology rake:
5. DONE: Enable new curvature topology rake code and add to UI.
6. DONE: Add code to cache curvature data per vertex in a CD layer.

*/

#include "MEM_guardedalloc.h"

#include "BLI_array.h"
#include "BLI_buffer.h"
#include "BLI_ghash.h"
#include "BLI_heap_simple.h"
#include "BLI_math.h"
#include "BLI_memarena.h"
#include "BLI_rand.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"
#include "PIL_time.h"
#include "atomic_ops.h"

#include "BKE_DerivedMesh.h"
#include "BKE_ccg.h"
#include "BKE_pbvh.h"

#include "GPU_buffers.h"

#include "bmesh.h"
#include "pbvh_intern.h"

#define DYNTOPO_MAX_ITER 4096

#define DYNTOPO_USE_HEAP

#ifndef DYNTOPO_USE_HEAP
/* don't add edges into the queue multiple times */
#  define USE_EDGEQUEUE_TAG
#endif

/* Avoid skinny faces */
#define USE_EDGEQUEUE_EVEN_SUBDIV

/* How much longer we need to be to consider for subdividing
 * (avoids subdividing faces which are only *slightly* skinny) */
#define EVEN_EDGELEN_THRESHOLD 1.2f
/* How much the limit increases per recursion
 * (avoids performing subdivisions too far away). */
#define EVEN_GENERATION_SCALE 1.1f

// recursion depth to start applying front face test
#define DEPTH_START_LIMIT 5

//#define FANCY_EDGE_WEIGHTS
#define SKINNY_EDGE_FIX

// slightly relax geometry by this factor along surface tangents
// to improve convergence of remesher
#define DYNTOPO_SAFE_SMOOTH_FAC 0.05f

#ifdef USE_EDGEQUEUE_EVEN_SUBDIV
#  include "BKE_global.h"
#endif

#ifndef DEBUG
#  define DEBUG_DEFINED
#  define DEBUG
#endif

#ifdef WIN32
#  include "crtdbg.h"
#endif

static void check_heap()
{
#ifdef WIN32
  if (!_CrtCheckMemory()) {
    printf("Memory corruption!");
    _CrtDbgBreak();
  }
#  ifdef DEBUG_DEFINED
#    undef DEBUG_DEFINED
#    undef DEBUG
#  endif
#endif
}
/* Support for only operating on front-faces */
#define USE_EDGEQUEUE_FRONTFACE

/**
 * Ensure we don't have dirty tags for the edge queue, and that they are left cleared.
 * (slow, even for debug mode, so leave disabled for now).
 */
#if defined(USE_EDGEQUEUE_TAG) && 0
#  if !defined(NDEBUG)
#    define USE_EDGEQUEUE_TAG_VERIFY
#  endif
#endif

// #define USE_VERIFY

#define DYNTOPO_MASK(cd_mask_offset, v) BM_ELEM_CD_GET_FLOAT(v, cd_mask_offset)

#ifdef USE_VERIFY
static void pbvh_bmesh_verify(PBVH *pbvh);
#endif

/* -------------------------------------------------------------------- */
/** \name BMesh Utility API
 *
 * Use some local functions which assume triangles.
 * \{ */

/**
 * Typically using BM_LOOPS_OF_VERT and BM_FACES_OF_VERT iterators are fine,
 * however this is an area where performance matters so do it in-line.
 *
 * Take care since 'break' won't works as expected within these macros!
 */

#define BM_LOOPS_OF_VERT_ITER_BEGIN(l_iter_radial_, v_) \
  { \
    struct { \
      BMVert *v; \
      BMEdge *e_iter, *e_first; \
      BMLoop *l_iter_radial; \
    } _iter; \
    _iter.v = v_; \
    if (_iter.v->e) { \
      _iter.e_iter = _iter.e_first = _iter.v->e; \
      do { \
        if (_iter.e_iter->l) { \
          _iter.l_iter_radial = _iter.e_iter->l; \
          do { \
            if (_iter.l_iter_radial->v == _iter.v) { \
              l_iter_radial_ = _iter.l_iter_radial;

#define BM_LOOPS_OF_VERT_ITER_END \
  } \
  } \
  while ((_iter.l_iter_radial = _iter.l_iter_radial->radial_next) != _iter.e_iter->l) \
    ; \
  } \
  } \
  while ((_iter.e_iter = BM_DISK_EDGE_NEXT(_iter.e_iter, _iter.v)) != _iter.e_first) \
    ; \
  } \
  } \
  ((void)0)

#define BM_FACES_OF_VERT_ITER_BEGIN(f_iter_, v_) \
  { \
    BMLoop *l_iter_radial_; \
    BM_LOOPS_OF_VERT_ITER_BEGIN (l_iter_radial_, v_) { \
      f_iter_ = l_iter_radial_->f;

#define BM_FACES_OF_VERT_ITER_END \
  } \
  BM_LOOPS_OF_VERT_ITER_END; \
  } \
  ((void)0)

BLI_INLINE void surface_smooth_v_safe(BMVert *v)
{
  float co[3];
  float tan[3];
  float tot = 0.0;

  zero_v3(co);

  // this is a manual edge walk

  BMEdge *e = v->e;
  if (!e) {
    return;
  }

  do {
    BMVert *v2 = e->v1 == v ? e->v2 : e->v1;

    sub_v3_v3v3(tan, v2->co, v->co);
    float d = dot_v3v3(tan, v->no);

    madd_v3_v3fl(tan, v->no, -d * 0.99f);
    add_v3_v3(co, tan);
    tot += 1.0f;
    e = v == e->v1 ? e->v1_disk_link.next : e->v2_disk_link.next;
  } while (e != v->e);

  if (tot == 0.0f) {
    return;
  }

  mul_v3_fl(co, 1.0f / tot);
  float x = v->co[0], y = v->co[1], z = v->co[2];

  // conflicts here should be pretty rare.
  atomic_cas_float(&v->co[0], x, x + co[0] * DYNTOPO_SAFE_SMOOTH_FAC);
  atomic_cas_float(&v->co[1], y, y + co[1] * DYNTOPO_SAFE_SMOOTH_FAC);
  atomic_cas_float(&v->co[2], z, z + co[2] * DYNTOPO_SAFE_SMOOTH_FAC);
}

static void bm_edges_from_tri(BMesh *bm, BMVert *v_tri[3], BMEdge *e_tri[3])
{
  e_tri[0] = BM_edge_create(bm, v_tri[0], v_tri[1], NULL, BM_CREATE_NO_DOUBLE);
  e_tri[1] = BM_edge_create(bm, v_tri[1], v_tri[2], NULL, BM_CREATE_NO_DOUBLE);
  e_tri[2] = BM_edge_create(bm, v_tri[2], v_tri[0], NULL, BM_CREATE_NO_DOUBLE);
}

BLI_INLINE void bm_face_as_array_index_tri(BMFace *f, int r_index[3])
{
  BMLoop *l = BM_FACE_FIRST_LOOP(f);

  BLI_assert(f->len == 3);

  r_index[0] = BM_elem_index_get(l->v);
  l = l->next;
  r_index[1] = BM_elem_index_get(l->v);
  l = l->next;
  r_index[2] = BM_elem_index_get(l->v);
}

/**
 * A version of #BM_face_exists, optimized for triangles
 * when we know the loop and the opposite vertex.
 *
 * Check if any triangle is formed by (l_radial_first->v, l_radial_first->next->v, v_opposite),
 * at either winding (since its a triangle no special checks are needed).
 *
 * <pre>
 * l_radial_first->v & l_radial_first->next->v
 * +---+
 * |  /
 * | /
 * + v_opposite
 * </pre>
 *
 * Its assumed that \a l_radial_first is never forming the target face.
 */
static BMFace *bm_face_exists_tri_from_loop_vert(BMLoop *l_radial_first, BMVert *v_opposite)
{
  BLI_assert(
      !ELEM(v_opposite, l_radial_first->v, l_radial_first->next->v, l_radial_first->prev->v));
  if (l_radial_first->radial_next != l_radial_first) {
    BMLoop *l_radial_iter = l_radial_first->radial_next;
    do {
      BLI_assert(l_radial_iter->f->len == 3);
      if (l_radial_iter->prev->v == v_opposite) {
        return l_radial_iter->f;
      }
    } while ((l_radial_iter = l_radial_iter->radial_next) != l_radial_first);
  }
  return NULL;
}

/**
 * Uses a map of vertices to lookup the final target.
 * References can't point to previous items (would cause infinite loop).
 */
static BMVert *bm_vert_hash_lookup_chain(GHash *deleted_verts, BMVert *v)
{
  while (true) {
    BMVert **v_next_p = (BMVert **)BLI_ghash_lookup_p(deleted_verts, v);
    if (v_next_p == NULL) {
      /* not remapped*/
      return v;
    }
    if (*v_next_p == NULL) {
      /* removed and not remapped */
      return NULL;
    }

    /* remapped */
    v = *v_next_p;
  }
}

/** \} */

/****************************** Building ******************************/

/* Update node data after splitting */
static void pbvh_bmesh_node_finalize(PBVH *pbvh,
                                     const int node_index,
                                     const int cd_vert_node_offset,
                                     const int cd_face_node_offset,
                                     bool add_orco)
{
  PBVHNode *n = &pbvh->nodes[node_index];
  bool has_visible = false;

  /* Create vert hash sets */
  n->bm_unique_verts = BLI_table_gset_new("bm_unique_verts");
  n->bm_other_verts = BLI_table_gset_new("bm_other_verts");

  BB_reset(&n->vb);
  BMFace *f;

  TGSET_ITER (f, n->bm_faces) {
    /* Update ownership of faces */
    BM_ELEM_CD_SET_INT(f, cd_face_node_offset, node_index);

    /* Update vertices */
    BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
    BMLoop *l_iter = l_first;

    do {
      BMVert *v = l_iter->v;
      if (!BLI_table_gset_haskey(n->bm_unique_verts, v)) {
        if (BM_ELEM_CD_GET_INT(v, cd_vert_node_offset) != DYNTOPO_NODE_NONE) {
          BLI_table_gset_add(n->bm_other_verts, v);
        }
        else {
          BLI_table_gset_insert(n->bm_unique_verts, v);
          BM_ELEM_CD_SET_INT(v, cd_vert_node_offset, node_index);
        }
      }
      /* Update node bounding box */
      BB_expand(&n->vb, v->co);
    } while ((l_iter = l_iter->next) != l_first);

    if (!BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      has_visible = true;
    }
  }
  TGSET_ITER_END

  BLI_assert(n->vb.bmin[0] <= n->vb.bmax[0] && n->vb.bmin[1] <= n->vb.bmax[1] &&
             n->vb.bmin[2] <= n->vb.bmax[2]);

  n->orig_vb = n->vb;

  /* Build GPU buffers for new node and update vertex normals */
  BKE_pbvh_node_mark_rebuild_draw(n);

  BKE_pbvh_node_fully_hidden_set(n, !has_visible);
  n->flag |= PBVH_UpdateNormals | PBVH_UpdateTopology | PBVH_UpdateCurvatureDir | PBVH_UpdateTris;

  if (add_orco) {
    BKE_pbvh_bmesh_check_tris(pbvh, n);
  }
}

/* Recursively split the node if it exceeds the leaf_limit */
static void pbvh_bmesh_node_split(
    PBVH *pbvh, const BBC *bbc_array, int node_index, bool add_orco, int depth)
{
  const int cd_vert_node_offset = pbvh->cd_vert_node_offset;
  const int cd_face_node_offset = pbvh->cd_face_node_offset;
  PBVHNode *n = &pbvh->nodes[node_index];

#ifdef PROXY_ADVANCED
  BKE_pbvh_free_proxyarray(pbvh, n);
#endif

  if (depth > 6 || BLI_table_gset_len(n->bm_faces) <= pbvh->leaf_limit) {
    /* Node limit not exceeded */
    pbvh_bmesh_node_finalize(pbvh, node_index, cd_vert_node_offset, cd_face_node_offset, add_orco);
    return;
  }

  /* Calculate bounding box around primitive centroids */
  BB cb;
  BB_reset(&cb);
  BMFace *f;

  TGSET_ITER (f, n->bm_faces) {
    const BBC *bbc = &bbc_array[BM_elem_index_get(f)];

    BB_expand(&cb, bbc->bcentroid);
  }
  TGSET_ITER_END

  /* Find widest axis and its midpoint */
  const int axis = BB_widest_axis(&cb);
  const float mid = (cb.bmax[axis] + cb.bmin[axis]) * 0.5f;

  if (isnan(mid)) {
    printf("NAN ERROR! %s\n", __func__);
  }

  /* Add two new child nodes */
  const int children = pbvh->totnode;
  n->children_offset = children;
  pbvh_grow_nodes(pbvh, pbvh->totnode + 2);

  /* Array reallocated, update current node pointer */
  n = &pbvh->nodes[node_index];

  /* Initialize children */
  PBVHNode *c1 = &pbvh->nodes[children], *c2 = &pbvh->nodes[children + 1];

  c1->flag |= PBVH_Leaf;
  c2->flag |= PBVH_Leaf;
  c1->bm_faces = BLI_table_gset_new_ex("bm_faces", BLI_table_gset_len(n->bm_faces) / 2);
  c2->bm_faces = BLI_table_gset_new_ex("bm_faces", BLI_table_gset_len(n->bm_faces) / 2);

  c1->bm_unique_verts = c2->bm_unique_verts = NULL;
  c1->bm_other_verts = c2->bm_other_verts = NULL;

  /* Partition the parent node's faces between the two children */
  TGSET_ITER (f, n->bm_faces) {
    const BBC *bbc = &bbc_array[BM_elem_index_get(f)];

    if (bbc->bcentroid[axis] < mid) {
      BLI_table_gset_insert(c1->bm_faces, f);
    }
    else {
      BLI_table_gset_insert(c2->bm_faces, f);
    }
  }
  TGSET_ITER_END
#if 0
  /* Enforce at least one primitive in each node */
  TableGSet *empty = NULL, *other;
  if (BLI_table_gset_len(c1->bm_faces) == 0) {
    empty = c1->bm_faces;
    other = c2->bm_faces;
  }
  else if (BLI_table_gset_len(c2->bm_faces) == 0) {
    empty = c2->bm_faces;
    other = c1->bm_faces;
  }

  if (empty) {
    void *key;
    TGSET_ITER (key, other) {
      BLI_table_gset_insert(empty, key);
      BLI_table_gset_remove(other, key, NULL);
      break;
    } TGSET_ITER_END
  }
#endif
  /* Clear this node */

  BMVert *v;

  /* Mark this node's unique verts as unclaimed */
  if (n->bm_unique_verts) {
    TGSET_ITER (v, n->bm_unique_verts) {
      BM_ELEM_CD_SET_INT(v, cd_vert_node_offset, DYNTOPO_NODE_NONE);
    }
    TGSET_ITER_END

    BLI_table_gset_free(n->bm_unique_verts, NULL);
  }

  if (n->bm_faces) {
    /* Unclaim faces */
    TGSET_ITER (f, n->bm_faces) {
      BM_ELEM_CD_SET_INT(f, cd_face_node_offset, DYNTOPO_NODE_NONE);
    }
    TGSET_ITER_END

    BLI_table_gset_free(n->bm_faces, NULL);
  }

  if (n->bm_other_verts) {
    BLI_table_gset_free(n->bm_other_verts, NULL);
  }

  if (n->layer_disp) {
    MEM_freeN(n->layer_disp);
  }

  n->bm_faces = NULL;
  n->bm_unique_verts = NULL;
  n->bm_other_verts = NULL;
  n->layer_disp = NULL;

  if (n->draw_buffers) {
    GPU_pbvh_buffers_free(n->draw_buffers);
    n->draw_buffers = NULL;
  }
  n->flag &= ~PBVH_Leaf;

  /* Recurse */
  pbvh_bmesh_node_split(pbvh, bbc_array, children, add_orco, depth + 1);
  pbvh_bmesh_node_split(pbvh, bbc_array, children + 1, add_orco, depth + 1);

  /* Array maybe reallocated, update current node pointer */
  n = &pbvh->nodes[node_index];

  /* Update bounding box */
  BB_reset(&n->vb);
  BB_expand_with_bb(&n->vb, &pbvh->nodes[n->children_offset].vb);
  BB_expand_with_bb(&n->vb, &pbvh->nodes[n->children_offset + 1].vb);
  n->orig_vb = n->vb;
}

static void pbvh_bmesh_copy_facedata(BMesh *bm, BMFace *dest, BMFace *src)
{
  dest->head.hflag = src->head.hflag;
  dest->mat_nr = src->mat_nr;
  CustomData_bmesh_copy_data(&bm->pdata, &bm->pdata, src->head.data, &dest->head.data);
}

/* Recursively split the node if it exceeds the leaf_limit */
static bool pbvh_bmesh_node_limit_ensure(PBVH *pbvh, int node_index)
{
  TableGSet *bm_faces = pbvh->nodes[node_index].bm_faces;
  const int bm_faces_size = BLI_table_gset_len(bm_faces);

  if (bm_faces_size <= pbvh->leaf_limit) {
    /* Node limit not exceeded */
    return false;
  }

  /* For each BMFace, store the AABB and AABB centroid */
  BBC *bbc_array = MEM_mallocN(sizeof(BBC) * bm_faces_size, "BBC");

  BMFace *f;

  int i;

  /*
  TGSET_ITER_INDEX(f, bm_faces, i)
  {
  }
  TGSET_ITER_INDEX_END
  printf("size: %d %d\n", i + 1, bm_faces_size);
  */

  TGSET_ITER_INDEX(f, bm_faces, i)
  {
    BBC *bbc = &bbc_array[i];

    BB_reset((BB *)bbc);
    BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
    BMLoop *l_iter = l_first;
    do {
      BB_expand((BB *)bbc, l_iter->v->co);
    } while ((l_iter = l_iter->next) != l_first);
    BBC_update_centroid(bbc);

    /* so we can do direct lookups on 'bbc_array' */
    BM_elem_index_set(f, i); /* set_dirty! */
  }
  TGSET_ITER_INDEX_END

  /* Likely this is already dirty. */
  pbvh->bm->elem_index_dirty |= BM_FACE;

  pbvh_bmesh_node_split(pbvh, bbc_array, node_index, false, 0);

  MEM_freeN(bbc_array);

  return true;
}

/**********************************************************************/

#if 0
static int pbvh_bmesh_node_offset_from_elem(PBVH *pbvh, BMElem *ele)
{
  switch (ele->head.htype) {
    case BM_VERT:
      return pbvh->cd_vert_node_offset;
    default:
      BLI_assert(ele->head.htype == BM_FACE);
      return pbvh->cd_face_node_offset;
  }
}

static int pbvh_bmesh_node_index_from_elem(PBVH *pbvh, void *key)
{
  const int cd_node_offset = pbvh_bmesh_node_offset_from_elem(pbvh, key);
  const int node_index = BM_ELEM_CD_GET_INT((BMElem *)key, cd_node_offset);

  BLI_assert(node_index != DYNTOPO_NODE_NONE);
  BLI_assert(node_index < pbvh->totnode);
  (void)pbvh;

  return node_index;
}

static PBVHNode *pbvh_bmesh_node_from_elem(PBVH *pbvh, void *key)
{
  return &pbvh->nodes[pbvh_bmesh_node_index_from_elem(pbvh, key)];
}

/* typecheck */
#  define pbvh_bmesh_node_index_from_elem(pbvh, key) \
    (CHECK_TYPE_ANY(key, BMFace *, BMVert *), pbvh_bmesh_node_index_from_elem(pbvh, key))
#  define pbvh_bmesh_node_from_elem(pbvh, key) \
    (CHECK_TYPE_ANY(key, BMFace *, BMVert *), pbvh_bmesh_node_from_elem(pbvh, key))
#endif

BLI_INLINE int pbvh_bmesh_node_index_from_vert(PBVH *pbvh, const BMVert *key)
{
  const int node_index = BM_ELEM_CD_GET_INT((const BMElem *)key, pbvh->cd_vert_node_offset);
  BLI_assert(node_index != DYNTOPO_NODE_NONE);
  BLI_assert(node_index < pbvh->totnode);
  return node_index;
}

BLI_INLINE int pbvh_bmesh_node_index_from_face(PBVH *pbvh, const BMFace *key)
{
  const int node_index = BM_ELEM_CD_GET_INT((const BMElem *)key, pbvh->cd_face_node_offset);
  BLI_assert(node_index != DYNTOPO_NODE_NONE);
  BLI_assert(node_index < pbvh->totnode);
  return node_index;
}

BLI_INLINE PBVHNode *pbvh_bmesh_node_from_vert(PBVH *pbvh, const BMVert *key)
{
  int ni = pbvh_bmesh_node_index_from_vert(pbvh, key);

  return ni >= 0 ? pbvh->nodes + ni : NULL;
  // return &pbvh->nodes[pbvh_bmesh_node_index_from_vert(pbvh, key)];
}

BLI_INLINE PBVHNode *pbvh_bmesh_node_from_face(PBVH *pbvh, const BMFace *key)
{
  int ni = pbvh_bmesh_node_index_from_face(pbvh, key);

  return ni >= 0 ? pbvh->nodes + ni : NULL;
  // return &pbvh->nodes[pbvh_bmesh_node_index_from_face(pbvh, key)];
}

static BMVert *pbvh_bmesh_vert_create(PBVH *pbvh,
                                      int node_index,
                                      const float co[3],
                                      const float no[3],
                                      BMVert *v_example,
                                      const int cd_vert_mask_offset)
{
  PBVHNode *node = &pbvh->nodes[node_index];

  BLI_assert((pbvh->totnode == 1 || node_index) && node_index <= pbvh->totnode);

  /* avoid initializing customdata because its quite involved */
  BMVert *v = BM_vert_create(pbvh->bm, co, NULL, BM_CREATE_SKIP_CD);
  CustomData_bmesh_set_default(&pbvh->bm->vdata, &v->head.data);

  if (v_example) {
    v->head.hflag = v_example->head.hflag;

    CustomData_bmesh_copy_data(
        &pbvh->bm->vdata, &pbvh->bm->vdata, v_example->head.data, &v->head.data);

    /* This value is logged below */
    copy_v3_v3(v->no, no);

    // keep MDynTopoVert copied from v_example as-is
  }
  else {
    MDynTopoVert *mv = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, v);

    copy_v3_v3(mv->origco, co);
    copy_v3_v3(mv->origno, no);
    mv->origmask = 0.0f;
    mv->flag = 0;

    /* This value is logged below */
    copy_v3_v3(v->no, no);
  }

  BLI_table_gset_insert(node->bm_unique_verts, v);
  BM_ELEM_CD_SET_INT(v, pbvh->cd_vert_node_offset, node_index);

  node->flag |= PBVH_UpdateDrawBuffers | PBVH_UpdateBB | PBVH_UpdateTris;

  /* Log the new vertex */
  BM_log_vert_added(pbvh->bm_log, v, cd_vert_mask_offset);
  v->head.index = pbvh->bm->totvert;  // set provisional index

  return v;
}

/**
 * \note Callers are responsible for checking if the face exists before adding.
 */
static BMFace *pbvh_bmesh_face_create(PBVH *pbvh,
                                      int node_index,
                                      BMVert *v_tri[3],
                                      BMEdge *e_tri[3],
                                      const BMFace *f_example,
                                      bool ensure_verts,
                                      bool log_face)
{
  PBVHNode *node = &pbvh->nodes[node_index];

  /* ensure we never add existing face */
  BLI_assert(!BM_face_exists(v_tri, 3));

  BMFace *f;

  if (!e_tri) {
    f = BM_face_create_verts(pbvh->bm, v_tri, 3, f_example, BM_CREATE_NOP, true);
  }
  else {
    f = BM_face_create(pbvh->bm, v_tri, e_tri, 3, f_example, BM_CREATE_NOP);
  }

  if (f_example) {
    f->head.hflag = f_example->head.hflag;
  }

  BLI_table_gset_insert(node->bm_faces, f);
  BM_ELEM_CD_SET_INT(f, pbvh->cd_face_node_offset, node_index);

  /* mark node for update */
  node->flag |= PBVH_UpdateDrawBuffers | PBVH_UpdateNormals | PBVH_UpdateTris;
  node->flag &= ~PBVH_FullyHidden;

  /* Log the new face */
  if (log_face) {
    BM_log_face_added(pbvh->bm_log, f);
  }

  int cd_vert_node = pbvh->cd_vert_node_offset;

  if (ensure_verts) {
    BMLoop *l = f->l_first;
    do {
      if (BM_ELEM_CD_GET_INT(l->v, cd_vert_node) == DYNTOPO_NODE_NONE) {
        BLI_table_gset_add(node->bm_unique_verts, l->v);
        BM_ELEM_CD_SET_INT(l->v, cd_vert_node, node_index);

        node->flag |= PBVH_UpdateDrawBuffers | PBVH_UpdateBB | PBVH_UpdateTris;
      }
      else {
        BLI_table_gset_add(node->bm_other_verts, l->v);
      }

      l = l->next;
    } while (l != f->l_first);
  }

  return f;
}

BMVert *BKE_pbvh_vert_create_bmesh(
    PBVH *pbvh, float co[3], float no[3], PBVHNode *node, BMVert *v_example)
{
  if (!node) {
    for (int i = 0; i < pbvh->totnode; i++) {
      PBVHNode *node2 = pbvh->nodes + i;

      if (!(node2->flag & PBVH_Leaf)) {
        continue;
      }

      // ensure we have at least some node somewhere picked
      node = node2;

      bool ok = true;

      for (int j = 0; j < 3; j++) {
        if (co[j] < node2->vb.bmin[j] || co[j] >= node2->vb.bmax[j]) {
          continue;
        }
      }

      if (ok) {
        break;
      }
    }
  }

  BMVert *v;

  if (!node) {
    printf("possible pbvh error\n");
    v = BM_vert_create(pbvh->bm, co, v_example, BM_CREATE_NOP);
    BM_ELEM_CD_SET_INT(v, pbvh->cd_vert_node_offset, DYNTOPO_NODE_NONE);

    MDynTopoVert *mv = BM_ELEM_CD_GET_VOID_P(v, pbvh->cd_dyn_vert);

    copy_v3_v3(mv->origco, co);

    return v;
  }

  return pbvh_bmesh_vert_create(
      pbvh, node - pbvh->nodes, co, no, v_example, pbvh->cd_vert_mask_offset);
}

PBVHNode *BKE_pbvh_node_from_face_bmesh(PBVH *pbvh, BMFace *f)
{
  return BM_ELEM_CD_GET_INT(f, pbvh->cd_face_node_offset);
}

BMFace *BKE_pbvh_face_create_bmesh(PBVH *pbvh,
                                   BMVert *v_tri[3],
                                   BMEdge *e_tri[3],
                                   const BMFace *f_example)
{
  int ni = DYNTOPO_NODE_NONE;

  for (int i = 0; i < 3; i++) {
    BMVert *v = v_tri[i];
    BMLoop *l;
    BMIter iter;

    BM_ITER_ELEM (l, &iter, v, BM_LOOPS_OF_VERT) {
      int ni2 = BM_ELEM_CD_GET_INT(l->f, pbvh->cd_face_node_offset);
      if (ni2 != DYNTOPO_NODE_NONE) {
        ni = ni2;
        break;
      }
    }
  }

  if (ni == DYNTOPO_NODE_NONE) {
    BMFace *f;

    // no existing nodes? find one
    for (int i = 0; i < pbvh->totnode; i++) {
      PBVHNode *node = pbvh->nodes + i;

      if (!(node->flag & PBVH_Leaf)) {
        continue;
      }

      for (int j = 0; j < 3; j++) {
        BMVert *v = v_tri[j];

        bool ok = true;

        for (int k = 0; k < 3; k++) {
          if (v->co[k] < node->vb.bmin[k] || v->co[k] >= node->vb.bmax[k]) {
            ok = false;
          }
        }

        if (ok &&
            (ni == DYNTOPO_NODE_NONE || BLI_table_gset_len(node->bm_faces) < pbvh->leaf_limit)) {
          ni = i;
          break;
        }
      }

      if (ni != DYNTOPO_NODE_NONE) {
        break;
      }
    }

    if (ni == DYNTOPO_NODE_NONE) {
      // empty pbvh?
      printf("possibly pbvh error\n");

      if (e_tri) {
        f = BM_face_create_verts(pbvh->bm, v_tri, 3, f_example, BM_CREATE_NOP, true);
      }
      else {
        f = BM_face_create(pbvh->bm, v_tri, e_tri, 3, f_example, BM_CREATE_NOP);
      }

      if (f_example) {
        f->head.hflag = f_example->head.hflag;
      }

      BM_ELEM_CD_SET_INT(f, pbvh->cd_face_node_offset, DYNTOPO_NODE_NONE);

      return f;
    }
  }

  return pbvh_bmesh_face_create(pbvh, ni, v_tri, e_tri, f_example, true, true);
}

/* Return the number of faces in 'node' that use vertex 'v' */
#if 0
static int pbvh_bmesh_node_vert_use_count(PBVH *pbvh, PBVHNode *node, BMVert *v)
{
  BMFace *f;
  int count = 0;

  BM_FACES_OF_VERT_ITER_BEGIN (f, v) {
    PBVHNode *f_node = pbvh_bmesh_node_from_face(pbvh, f);
    if (f_node == node) {
      count++;
    }
  }
  BM_FACES_OF_VERT_ITER_END;

  return count;
}
#endif

#define pbvh_bmesh_node_vert_use_count_is_equal(pbvh, node, v, n) \
  (pbvh_bmesh_node_vert_use_count_at_most(pbvh, node, v, (n) + 1) == n)

static int pbvh_bmesh_node_vert_use_count_at_most(PBVH *pbvh,
                                                  PBVHNode *node,
                                                  BMVert *v,
                                                  const int count_max)
{
  int count = 0;
  BMFace *f;

  BM_FACES_OF_VERT_ITER_BEGIN (f, v) {
    PBVHNode *f_node = pbvh_bmesh_node_from_face(pbvh, f);
    if (f_node == node) {
      count++;
      if (count == count_max) {
        return count;
      }
    }
  }
  BM_FACES_OF_VERT_ITER_END;

  return count;
}

/* Return a node that uses vertex 'v' other than its current owner */
static PBVHNode *pbvh_bmesh_vert_other_node_find(PBVH *pbvh, BMVert *v)
{
  PBVHNode *current_node = pbvh_bmesh_node_from_vert(pbvh, v);
  BMFace *f;

  BM_FACES_OF_VERT_ITER_BEGIN (f, v) {
    PBVHNode *f_node = pbvh_bmesh_node_from_face(pbvh, f);

    if (f_node != current_node) {
      return f_node;
    }
  }
  BM_FACES_OF_VERT_ITER_END;

  return NULL;
}

static void pbvh_bmesh_vert_ownership_transfer(PBVH *pbvh, PBVHNode *new_owner, BMVert *v)
{
  PBVHNode *current_owner = pbvh_bmesh_node_from_vert(pbvh, v);
  /* mark node for update */

  if (current_owner) {
    current_owner->flag |= PBVH_UpdateDrawBuffers | PBVH_UpdateBB;

    BLI_assert(current_owner != new_owner);

    /* Remove current ownership */
    BLI_table_gset_remove(current_owner->bm_unique_verts, v, NULL);
  }

  /* Set new ownership */
  BM_ELEM_CD_SET_INT(v, pbvh->cd_vert_node_offset, new_owner - pbvh->nodes);
  BLI_table_gset_insert(new_owner->bm_unique_verts, v);
  BLI_table_gset_remove(new_owner->bm_other_verts, v, NULL);
  BLI_assert(!BLI_table_gset_haskey(new_owner->bm_other_verts, v));

  /* mark node for update */
  new_owner->flag |= PBVH_UpdateDrawBuffers | PBVH_UpdateBB;
}

static bool pbvh_bmesh_vert_relink(PBVH *pbvh, BMVert *v)
{
  const int cd_vert_node = pbvh->cd_vert_node_offset;
  const int cd_face_node = pbvh->cd_face_node_offset;

  BMFace *f;
  BLI_assert(BM_ELEM_CD_GET_INT(v, cd_vert_node) == DYNTOPO_NODE_NONE);

  bool added = false;

  BM_FACES_OF_VERT_ITER_BEGIN (f, v) {
    const int ni = BM_ELEM_CD_GET_INT(f, cd_face_node);

    if (ni == DYNTOPO_NODE_NONE) {
      continue;
    }

    PBVHNode *node = pbvh->nodes + ni;

    if (BM_ELEM_CD_GET_INT(v, cd_vert_node) == DYNTOPO_NODE_NONE) {
      BLI_table_gset_add(node->bm_unique_verts, v);
      BM_ELEM_CD_SET_INT(v, cd_vert_node, ni);
    }
    else {
      BLI_table_gset_add(node->bm_other_verts, v);
    }
  }
  BM_FACES_OF_VERT_ITER_END;

  return added;
}

static void pbvh_bmesh_vert_remove(PBVH *pbvh, BMVert *v)
{
  /* never match for first time */
  int f_node_index_prev = DYNTOPO_NODE_NONE;

  PBVHNode *v_node = pbvh_bmesh_node_from_vert(pbvh, v);

  if (v_node) {
    BLI_table_gset_remove(v_node->bm_unique_verts, v, NULL);
  }

  BM_ELEM_CD_SET_INT(v, pbvh->cd_vert_node_offset, DYNTOPO_NODE_NONE);

  /* Have to check each neighboring face's node */
  BMFace *f;
  BM_FACES_OF_VERT_ITER_BEGIN (f, v) {
    const int f_node_index = pbvh_bmesh_node_index_from_face(pbvh, f);

    if (f_node_index == DYNTOPO_NODE_NONE) {
      continue;
    }

    /* faces often share the same node,
     * quick check to avoid redundant #BLI_table_gset_remove calls */
    if (f_node_index_prev != f_node_index) {
      f_node_index_prev = f_node_index;

      PBVHNode *f_node = &pbvh->nodes[f_node_index];
      f_node->flag |= PBVH_UpdateDrawBuffers | PBVH_UpdateBB | PBVH_UpdateTris;

      /* Remove current ownership */
      BLI_table_gset_remove(f_node->bm_other_verts, v, NULL);

      BLI_assert(!BLI_table_gset_haskey(f_node->bm_unique_verts, v));
      BLI_assert(!BLI_table_gset_haskey(f_node->bm_other_verts, v));
    }
  }
  BM_FACES_OF_VERT_ITER_END;
}

static void pbvh_bmesh_face_remove(PBVH *pbvh, BMFace *f)
{
  PBVHNode *f_node = pbvh_bmesh_node_from_face(pbvh, f);

  if (!f_node) {
    printf("pbvh corruption\n");
    fflush(stdout);
    return;
  }
  /* Check if any of this face's vertices need to be removed
   * from the node */
  BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
  BMLoop *l_iter = l_first;
  do {
    BMVert *v = l_iter->v;
    if (pbvh_bmesh_node_vert_use_count_is_equal(pbvh, f_node, v, 1)) {
      if (BLI_table_gset_haskey(f_node->bm_unique_verts, v)) {
        /* Find a different node that uses 'v' */
        PBVHNode *new_node;

        new_node = pbvh_bmesh_vert_other_node_find(pbvh, v);
        BLI_assert(new_node || BM_vert_face_count_is_equal(v, 1));

        if (new_node) {
          pbvh_bmesh_vert_ownership_transfer(pbvh, new_node, v);
        }
      }
      else {
        /* Remove from other verts */
        BLI_table_gset_remove(f_node->bm_other_verts, v, NULL);
      }
    }
  } while ((l_iter = l_iter->next) != l_first);

  /* Remove face from node and top level */
  BLI_table_gset_remove(f_node->bm_faces, f, NULL);
  BM_ELEM_CD_SET_INT(f, pbvh->cd_face_node_offset, DYNTOPO_NODE_NONE);

  /* Log removed face */
  BM_log_face_removed(pbvh->bm_log, f);

  /* mark node for update */
  f_node->flag |= PBVH_UpdateDrawBuffers | PBVH_UpdateNormals | PBVH_UpdateTris;
}

void BKE_pbvh_bmesh_face_kill(PBVH *pbvh, BMFace *f)
{
  pbvh_bmesh_face_remove(pbvh, f);
  BM_face_kill(pbvh->bm, f);
}

static void pbvh_bmesh_edge_loops(BLI_Buffer *buf, BMEdge *e)
{
  /* fast-path for most common case where an edge has 2 faces,
   * no need to iterate twice.
   * This assumes that the buffer */
  BMLoop **data = buf->data;
  BLI_assert(buf->alloc_count >= 2);
  if (LIKELY(BM_edge_loop_pair(e, &data[0], &data[1]))) {
    buf->count = 2;
  }
  else {
    BLI_buffer_reinit(buf, BM_edge_face_count(e));
    BM_iter_as_array(NULL, BM_LOOPS_OF_EDGE, e, buf->data, buf->count);
  }
}

/****************************** EdgeQueue *****************************/

struct EdgeQueue;

typedef struct EdgeQueue {
  HeapSimple *heap;

  void **elems;
  int totelems;

  const float *center;
  float center_proj[3]; /* for when we use projected coords. */
  float radius_squared;
  float limit_len_squared;
#ifdef USE_EDGEQUEUE_EVEN_SUBDIV
  float limit_len;
#endif

  bool (*edge_queue_tri_in_range)(const struct EdgeQueue *q, BMFace *f);
  bool (*edge_queue_vert_in_range)(const struct EdgeQueue *q, BMVert *v);

  const float *view_normal;
#ifdef USE_EDGEQUEUE_FRONTFACE
  unsigned int use_view_normal : 1;
#endif
} EdgeQueue;

typedef struct {
  EdgeQueue *q;
  BLI_mempool *pool;
  BMesh *bm;
  int cd_dyn_vert;
  int cd_vert_mask_offset;
  int cd_vert_node_offset;
  int cd_face_node_offset;
  float avg_elen;
  float max_elen;
  float min_elen;
  float totedge;
} EdgeQueueContext;

BLI_INLINE float calc_weighted_edge_split(EdgeQueueContext *eq_ctx, BMVert *v1, BMVert *v2)
{
#ifdef FANCY_EDGE_WEIGHTS
  float l = len_squared_v3v3(v1->co, v2->co);
  float val = (float)BM_vert_edge_count(v1) + (float)BM_vert_edge_count(v2);
  val = MAX2(val * 0.5 - 6.0f, 1.0f);
  val = powf(val, 0.5);
  l *= val;

  return l;
#elif 0  // penalize 4-valence verts
  float l = len_squared_v3v3(v1->co, v2->co);
  if (BM_vert_edge_count(v1) == 4 || BM_vert_edge_count(v2) == 4) {
    l *= 0.25f;
  }

  return l;
#else
  return len_squared_v3v3(v1->co, v2->co);
#endif
}

BLI_INLINE float calc_weighted_edge_collapse(EdgeQueueContext *eq_ctx, BMVert *v1, BMVert *v2)
{
#ifdef FANCY_EDGE_WEIGHTS
  float l = len_squared_v3v3(v1->co, v2->co);
  float val = (float)BM_vert_edge_count(v1) + (float)BM_vert_edge_count(v2);
  val = MAX2(val * 0.5 - 6.0f, 1.0f);
  val = powf(val, 0.5);
  l /= val;

  // if (BM_vert_edge_count(v1) == 4 || BM_vert_edge_count(v2) == 4) {
  //  l *= 0.25f;
  //}

  return l;
#else
  return len_squared_v3v3(v1->co, v2->co);
#endif
}

/* only tag'd edges are in the queue */
#ifdef USE_EDGEQUEUE_TAG
#  define EDGE_QUEUE_TEST(e) (BM_elem_flag_test((CHECK_TYPE_INLINE(e, BMEdge *), e), BM_ELEM_TAG))
#  define EDGE_QUEUE_ENABLE(e) \
    BM_elem_flag_enable((CHECK_TYPE_INLINE(e, BMEdge *), e), BM_ELEM_TAG)
#  define EDGE_QUEUE_DISABLE(e) \
    BM_elem_flag_disable((CHECK_TYPE_INLINE(e, BMEdge *), e), BM_ELEM_TAG)
#endif

#ifdef USE_EDGEQUEUE_TAG_VERIFY
/* simply check no edges are tagged
 * (it's a requirement that edges enter and leave a clean tag state) */
static void pbvh_bmesh_edge_tag_verify(PBVH *pbvh)
{
  for (int n = 0; n < pbvh->totnode; n++) {
    PBVHNode *node = &pbvh->nodes[n];
    if (node->bm_faces) {
      GSetIterator gs_iter;
      GSET_ITER (gs_iter, node->bm_faces) {
        BMFace *f = BLI_gsetIterator_getKey(&gs_iter);
        BMEdge *e_tri[3];
        BMLoop *l_iter;

        BLI_assert(f->len == 3);
        l_iter = BM_FACE_FIRST_LOOP(f);
        e_tri[0] = l_iter->e;
        l_iter = l_iter->next;
        e_tri[1] = l_iter->e;
        l_iter = l_iter->next;
        e_tri[2] = l_iter->e;

        BLI_assert((EDGE_QUEUE_TEST(e_tri[0]) == false) && (EDGE_QUEUE_TEST(e_tri[1]) == false) &&
                   (EDGE_QUEUE_TEST(e_tri[2]) == false));
      }
    }
  }
}
#endif

static bool edge_queue_vert_in_sphere(const EdgeQueue *q, BMVert *v)
{
  /* Check if triangle intersects the sphere */
  return len_squared_v3v3(q->center, v->co) <= q->radius_squared;
}

/* reduce script

on factor;

ax := 0;
ay := 0;

e1x := bx - ax;
e1y := by - ay;
e2x := cx - bx;
e2y := cy - by;
e3x := ax - cx;
e3y := ay - cy;

l1 := (e1x**2 + e1y**2)**0.5;
l2 := (e2x**2 + e2y**2)**0.5;
l3 := (e3x**2 + e3y**2)**0.5;

load_package "avector";

e1 := avec(e1x / l1, e1y / l1, 0.0);
e2 := avec(e2x / l2, e2y / l2, 0.0);
e3 := avec(e3x / l3, e3y / l3, 0.0);

ax := 0;
ay := 0;

d1 := x1*e1[1] - y1*e1[0];
d2 := x1*e2[1] - y1*e2[0];
d3 := x1*e3[1] - y1*e3[0];

d1 := d1**2;
d2 := d2**2;
d3 := d3**2;

on fort;
d1;
d2;
d3;
off fort;

fdis := (sqrt(dis)/nz)**2 + planedis**2;
*/

/*
static inline float dot_v3v3(const float a[3], const float b[3])
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline void sub_v3_v3v3(float r[3], const float a[3], const float b[3])
{
  r[0] = a[0] - b[0];
  r[1] = a[1] - b[1];
  r[2] = a[2] - b[2];
}


static inline void add_v3_v3v3(float r[3], const float a[3], const float b[3])
{
  r[0] = a[0] + b[0];
  r[1] = a[1] + b[1];
  r[2] = a[2] + b[2];
}

static inline void add_v3_v3(float r[3], const float a[3])
{
  r[0] += a[0];
  r[1] += a[1];
  r[2] += a[2];
}

static inline void mul_v3_fl(float r[3], float f)
{
  r[0] *= f;
  r[1] *= f;
  r[2] *= f;
}


static inline float len_squared_v3v3(const float a[3], const float b[3])
{
  float d[3];

  sub_v3_v3v3(d, b, a);
  return dot_v3v3(d, d);
}
*/

static float dist_to_tri_sphere_simple(
    float p[3], float v1[3], float v2[3], float v3[3], float n[3])
{
  float co[3];

  float dis = len_squared_v3v3(p, v1);
  dis = fmin(dis, len_squared_v3v3(p, v2));
  dis = fmin(dis, len_squared_v3v3(p, v3));

  add_v3_v3v3(co, v1, v2);
  mul_v3_fl(co, 0.5f);
  dis = fmin(dis, len_squared_v3v3(p, co));

  add_v3_v3v3(co, v2, v3);
  mul_v3_fl(co, 0.5f);
  dis = fmin(dis, len_squared_v3v3(p, co));

  add_v3_v3v3(co, v3, v1);
  mul_v3_fl(co, 0.5f);
  dis = fmin(dis, len_squared_v3v3(p, co));

  add_v3_v3v3(co, v1, v2);
  add_v3_v3(co, v3);
  mul_v3_fl(co, 1.0f / 3.0f);
  dis = fmin(dis, len_squared_v3v3(p, co));

  return dis;
}

//#include <cmath>

//  int mask = (nx < ny) | ((nx < nz) << 1) | ((ny < nz) << 2) | ((nx < nz) << 3);

/*
let axis1, axis2, axis3;

let tabx = new Array(8);
let taby = new Array(8);
let tabz = new Array(8);

let b1 = 1;//nx > ny;
let b2 = 2;//nx > nz;
let b3 = 4;//ny > nz;

m1 = 1 | 2;
m2 = 4;

tabx[m1] = 1;
taby[m1] = 2;
tabz[m1] = 0;

tabx[m2] = 0;
taby[m2] = 2;
tabz[m2] = 1;

for (let i=0; i<tabx.length; i++) {
  if (tabx[i] === undefined) {
    tabx[i] = 0;
    taby[i] = 1;
    tabz[i] = 2;
  }
}

function format(tab) {
  let s = '';
  for (let i=0; i<tab.length; i++) {
    if (i > 0) {
      s += ', '
    }

    s += tab[i];
  }

  return s;
}

let buf = `
  static int tritablex[${tabx.length}] = {${format(tabx)}};
  static int tritabley[${taby.length}] = {${format(taby)}};
  static int tritablez[${tabz.length}] = {${format(tabz)}};
`;
console.log(buf);
*/

static int tritablex[8] = {0, 0, 0, 1, 0, 0, 0, 0};
static int tritabley[8] = {1, 1, 1, 2, 2, 1, 1, 1};
static int tritablez[8] = {2, 2, 2, 0, 1, 2, 2, 2};

float dist_to_tri_sphere(float p[3], float v1[3], float v2[3], float v3[3], float n[3])
{

  // find projection axis;
  int axis1, axis2, axis3;

  // clang optimizes fabsf better
  double nx = fabsf(n[0]);  // n[0] < 0.0 ? -n[0] : n[0];
  double ny = fabsf(n[1]);  // n[1] < 0.0 ? -n[1] : n[1];
  double nz = fabsf(n[2]);  // n[2] < 0.0 ? -n[2] : n[2];

  const double feps = 0.000001;

#if 0
  if (nx > ny && nx > nz) {
    axis1 = 1;
    axis2 = 2;
    axis3 = 0;
  }
  else if (ny > nx && ny > nz) {
    axis1 = 0;
    axis2 = 2;
    axis3 = 1;
  }
  else {
    axis1 = 0;
    axis2 = 1;
    axis3 = 2;
  }
#else
  int mask = 0;

  //
  // let b1 = 1;  // nx > ny;
  // let b2 = 2;  // nx > nz;
  // let b3 = 4;  // ny > nz;

  mask = mask | (nx > ny);
  mask = mask | ((nx > nz) << 1);
  mask = mask | ((ny > nz) << 2);

  axis1 = tritablex[mask];
  axis2 = tritabley[mask];
  axis3 = tritablez[mask];
#endif

#if 1
  double planedis = (p[0] - v1[0]) * n[0] + (p[1] - v1[1]) * n[1] + (p[2] - v1[2]) * n[2];
  planedis = planedis < 0.0 ? -planedis : planedis;

  double ax = v1[axis1], ay = v1[axis2];
  double bx = v2[axis1] - ax, by = v2[axis2] - ay;
  double cx = v3[axis1] - ax, cy = v3[axis2] - ay;
  double bx2 = bx * bx, by2 = by * by, cx2 = cx * cx, cy2 = cy * cy;

  double x1 = p[axis1] - ax;
  double y1 = p[axis2] - ay;

  bool s1 = x1 * by - y1 * bx < 0.0;
  bool s2 = x1 * (cy - by) - y1 * (cx - bx) < 0.0;
  bool s3 = x1 * -cy - y1 * -cx < 0.0;

  int side = 0;

  mask = s1 | (s2 << 1) | (s3 << 2);
  if (mask == 0.0) {
    return planedis * planedis;
  }

  double d1, d2, d3, div;

  /*
//\  3|
//  \ |
//    b
//    | \
//  1 |   \  2
//    |  0  \
// ___a_______c___
//  5 |   4      \ 6
*/

  double dis = 0.0;
  switch (mask) {
    case 1:
      div = (bx2 + by2);

      if (div > feps) {
        d1 = (bx * y1 - by * x1);
        d1 = (d1 * d1) / div;
      }
      else {
        d1 = x1 * x1 + y1 * y1;
      }

      dis = d1;
      break;
    case 3:
      dis = ((x1 - bx) * (x1 - bx) + (y1 - by) * (y1 - by));
      break;
    case 2:
      div = ((bx - cx) * (bx - cx) + (by - cy) * (by - cy));
      if (div > feps) {
        d2 = ((bx - cx) * y1 - (by - cy) * x1);
        d2 = (d2 * d2) / div;
      }
      else {
        d2 = (x1 - bx) * (x1 - bx) + (y1 - by) * (y1 - by);
      }
      dis = d2;
      break;
    case 6:
      dis = (x1 - cx) * (x1 - cx) + (y1 - cy) * (y1 - cy);
      break;
    case 4:
      div = (cx2 + cy2);

      if (div > feps) {
        d3 = (cx * y1 - cy * x1);
        d3 = (d3 * d3) / div;
      }
      else {
        d3 = (x1 - cx) * (x1 - cx) + (y1 - cy) * (y1 - cy);
      }

      dis = d3;
      break;
    case 5:
      dis = x1 * x1 + y1 * y1;
      break;
  }

  nz = n[axis3] < 0.0 ? -n[axis3] : n[axis3];

  return (float)(dis + nz * nz * planedis * planedis) / (nz * nz);
#else
  return (float)axis1 + (float)axis2 + (float)axis3;
#endif
}

static bool edge_queue_tri_in_sphere(const EdgeQueue *q, BMFace *f)
{
  float c[3];
  float v1[3], v2[3], v3[3], co[3];
  const float mul = 1.0f;

  BMLoop *l = f->l_first;

  /* Check if triangle intersects the sphere */
  float dis = dist_to_tri_sphere_simple((float *)q->center,
                                        (float *)l->v->co,
                                        (float *)l->next->v->co,
                                        (float *)l->prev->v->co,
                                        (float *)f->no);

  // closest_on_tri_to_point_v3(c, co, v1, v2, v3);

  // float dis2 = len_squared_v3v3(q->center, c);
  // float dis3 = sqrtf(dis2);

  return dis <= q->radius_squared;

  /* Get closest point in triangle to sphere center */
#if 0
  /*
  closest_on_tri_to_point_v3 is being slow
  */

  float mindis = 1e17;
  float dis;
  copy_v3_v3(c, q->center);

  for (int i=0; i<3; i++) {
    dis = len_squared_v3v3(v_tri[i]->co, c);
    mindis = MIN2(mindis, dis);

    dis = dist_squared_to_line_segment_v3(c, v_tri[i]->co, v_tri[(i+1)%3]->co);
    mindis = MIN2(mindis, dis);
  }
  return mindis <= q->radius_squared;
#else
  closest_on_tri_to_point_v3(c, q->center, l->v->co, l->next->v->co, l->prev->v->co);

  /* Check if triangle intersects the sphere */
  return len_squared_v3v3(q->center, c) <= q->radius_squared;
#endif
}

static bool edge_queue_tri_in_circle(const EdgeQueue *q, BMFace *f)
{
  BMVert *v_tri[3];
  float c[3];
  float tri_proj[3][3];

  /* Get closest point in triangle to sphere center */
  BM_face_as_array_vert_tri(f, v_tri);

  project_plane_normalized_v3_v3v3(tri_proj[0], v_tri[0]->co, q->view_normal);
  project_plane_normalized_v3_v3v3(tri_proj[1], v_tri[1]->co, q->view_normal);
  project_plane_normalized_v3_v3v3(tri_proj[2], v_tri[2]->co, q->view_normal);

  closest_on_tri_to_point_v3(c, q->center_proj, tri_proj[0], tri_proj[1], tri_proj[2]);

  /* Check if triangle intersects the sphere */
  return len_squared_v3v3(q->center_proj, c) <= q->radius_squared;
}

typedef struct EdgeQueueThreadData {
  PBVH *pbvh;
  PBVHNode *node;
  BMEdge **edges;
  EdgeQueueContext *eq_ctx;
  int totedge;
  int size;
} EdgeQueueThreadData;

void edge_thread_data_insert(EdgeQueueThreadData *tdata, BMEdge *e)
{
  if (tdata->size <= tdata->totedge) {
    tdata->size = (tdata->totedge + 1) << 1;
    if (!tdata->edges) {
      tdata->edges = MEM_mallocN(sizeof(void *) * tdata->size, "edge_thread_data_insert");
    }
    else {
      tdata->edges = MEM_reallocN(tdata->edges, sizeof(void *) * tdata->size);
    }
  }

  e->head.hflag |= BM_ELEM_TAG;

  tdata->edges[tdata->totedge] = e;
  tdata->totedge++;
}

static bool edge_queue_vert_in_circle(const EdgeQueue *q, BMVert *v)
{
  float c[3];

  project_plane_normalized_v3_v3v3(c, v->co, q->view_normal);

  return len_squared_v3v3(q->center_proj, c) <= q->radius_squared;
}

/* Return true if the vertex mask is less than 1.0, false otherwise */
static bool check_mask(EdgeQueueContext *eq_ctx, BMVert *v)
{
  return DYNTOPO_MASK(eq_ctx->cd_dyn_vert, v) < 1.0f;
}

static void edge_queue_insert(EdgeQueueContext *eq_ctx, BMEdge *e, float priority)
{
  void **elems = eq_ctx->q->elems;
  BLI_array_declare(elems);
  BLI_array_len_set(elems, eq_ctx->q->totelems);

  /* Don't let topology update affect fully masked vertices. This used to
   * have a 50% mask cutoff, with the reasoning that you can't do a 50%
   * topology update. But this gives an ugly border in the mesh. The mask
   * should already make the brush move the vertices only 50%, which means
   * that topology updates will also happen less frequent, that should be
   * enough. */
  if (((eq_ctx->cd_vert_mask_offset == -1) ||
       (check_mask(eq_ctx, e->v1) || check_mask(eq_ctx, e->v2))) &&
      !(BM_elem_flag_test_bool(e->v1, BM_ELEM_HIDDEN) ||
        BM_elem_flag_test_bool(e->v2, BM_ELEM_HIDDEN))) {

    float dis = len_v3v3(e->v1->co, e->v2->co);
    eq_ctx->avg_elen += dis;
    eq_ctx->max_elen = MAX2(eq_ctx->max_elen, dis);
    eq_ctx->min_elen = MIN2(eq_ctx->min_elen, dis);
    eq_ctx->totedge += 1.0f;

    BMVert **pair = BLI_mempool_alloc(eq_ctx->pool);
    pair[0] = e->v1;
    pair[1] = e->v2;
#ifdef DYNTOPO_USE_HEAP
    BLI_heapsimple_insert(eq_ctx->q->heap, priority, pair);
#endif

    BLI_array_append(elems, pair);
    eq_ctx->q->elems = elems;
    eq_ctx->q->totelems = BLI_array_len(elems);

#ifdef USE_EDGEQUEUE_TAG
    BLI_assert(EDGE_QUEUE_TEST(e) == false);
    EDGE_QUEUE_ENABLE(e);
#endif
  }
}

static void long_edge_queue_edge_add(EdgeQueueContext *eq_ctx, BMEdge *e)
{
#ifdef USE_EDGEQUEUE_TAG
  if (EDGE_QUEUE_TEST(e) == false)
#endif
  {
    const float len_sq = BM_edge_calc_length_squared(e);
    if (len_sq > eq_ctx->q->limit_len_squared) {
      edge_queue_insert(eq_ctx, e, -len_sq);
    }
  }
}

#ifdef USE_EDGEQUEUE_EVEN_SUBDIV
static void long_edge_queue_edge_add_recursive(EdgeQueueContext *eq_ctx,
                                               BMLoop *l_edge,
                                               BMLoop *l_end,
                                               const float len_sq,
                                               float limit_len,
                                               int depth)
{
  BLI_assert(len_sq > square_f(limit_len));

#  ifdef USE_EDGEQUEUE_FRONTFACE
  if (depth > DEPTH_START_LIMIT && eq_ctx->q->use_view_normal) {
    if (dot_v3v3(l_edge->f->no, eq_ctx->q->view_normal) < 0.0f) {
      return;
    }
  }
#  endif

#  ifdef USE_EDGEQUEUE_TAG
  if (EDGE_QUEUE_TEST(l_edge->e) == false)
#  endif
  {
    edge_queue_insert(eq_ctx, l_edge->e, -len_sq);
  }

  /* temp support previous behavior! */
  if (UNLIKELY(G.debug_value == 1234)) {
    return;
  }

  if ((l_edge->radial_next != l_edge)) {
    const float len_sq_cmp = len_sq * EVEN_EDGELEN_THRESHOLD;

    limit_len *= EVEN_GENERATION_SCALE;
    const float limit_len_sq = square_f(limit_len);

    BMLoop *l_iter = l_edge;
    do {
      BMLoop *l_adjacent[2] = {l_iter->next, l_iter->prev};
      for (int i = 0; i < ARRAY_SIZE(l_adjacent); i++) {
        float len_sq_other = BM_edge_calc_length_squared(l_adjacent[i]->e);
        if (len_sq_other > max_ff(len_sq_cmp, limit_len_sq)) {
          //                  edge_queue_insert(eq_ctx, l_adjacent[i]->e, -len_sq_other);
          long_edge_queue_edge_add_recursive(eq_ctx,
                                             l_adjacent[i]->radial_next,
                                             l_adjacent[i],
                                             len_sq_other,
                                             limit_len,
                                             depth + 1);
        }
      }
    } while ((l_iter = l_iter->radial_next) != l_end);
  }
}
#endif /* USE_EDGEQUEUE_EVEN_SUBDIV */

static void short_edge_queue_edge_add(EdgeQueueContext *eq_ctx, BMEdge *e)
{
#ifdef USE_EDGEQUEUE_TAG
  if (EDGE_QUEUE_TEST(e) == false)
#endif
  {
    const float len_sq = calc_weighted_edge_collapse(eq_ctx, e->v1, e->v2);
    if (len_sq < eq_ctx->q->limit_len_squared) {
      edge_queue_insert(eq_ctx, e, len_sq);
    }
  }
}

static void long_edge_queue_face_add(EdgeQueueContext *eq_ctx, BMFace *f, bool ignore_frontface)
{
#ifdef USE_EDGEQUEUE_FRONTFACE
  if (!ignore_frontface && eq_ctx->q->use_view_normal) {
    if (dot_v3v3(f->no, eq_ctx->q->view_normal) < 0.0f) {
      return;
    }
  }
#endif

  if (eq_ctx->q->edge_queue_tri_in_range(eq_ctx->q, f)) {
    /* Check each edge of the face */
    BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
    BMLoop *l_iter = l_first;
    do {
#ifdef USE_EDGEQUEUE_EVEN_SUBDIV
      const float len_sq = BM_edge_calc_length_squared(l_iter->e);
      if (len_sq > eq_ctx->q->limit_len_squared) {
        long_edge_queue_edge_add_recursive(eq_ctx,
                                           l_iter->radial_next,
                                           l_iter,
                                           len_sq,
                                           eq_ctx->q->limit_len,
                                           DEPTH_START_LIMIT +
                                               1);  // ignore_frontface ? 0 : DEPTH_START_LIMIT+1);
      }
#else
      long_edge_queue_edge_add(eq_ctx, l_iter->e);
#endif
    } while ((l_iter = l_iter->next) != l_first);
  }
}

static void short_edge_queue_face_add(EdgeQueueContext *eq_ctx, BMFace *f)
{
#ifdef USE_EDGEQUEUE_FRONTFACE
  if (eq_ctx->q->use_view_normal) {
    if (dot_v3v3(f->no, eq_ctx->q->view_normal) < 0.0f) {
      return;
    }
  }
#endif

  if (eq_ctx->q->edge_queue_tri_in_range(eq_ctx->q, f)) {
    BMLoop *l_iter;
    BMLoop *l_first;

    /* Check each edge of the face */
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      short_edge_queue_edge_add(eq_ctx, l_iter->e);
    } while ((l_iter = l_iter->next) != l_first);
  }
}

static void short_edge_queue_edge_add_recursive_2(EdgeQueueThreadData *tdata,
                                                  BMLoop *l_edge,
                                                  BMLoop *l_end,
                                                  const float len_sq,
                                                  float limit_len,
                                                  int depth)
{
  BLI_assert(len_sq > square_f(limit_len));

  if (l_edge->e->head.hflag & BM_ELEM_TAG) {
    return;
  }

#ifdef USE_EDGEQUEUE_FRONTFACE
  if (depth > DEPTH_START_LIMIT && tdata->eq_ctx->q->use_view_normal) {
    if (dot_v3v3(l_edge->f->no, tdata->eq_ctx->q->view_normal) < 0.0f) {
      return;
    }
  }
#endif

  edge_thread_data_insert(tdata, l_edge->e);

  /* temp support previous behavior! */
  if (UNLIKELY(G.debug_value == 1234)) {
    return;
  }

  if ((l_edge->radial_next != l_edge)) {
    const float len_sq_cmp = len_sq * EVEN_EDGELEN_THRESHOLD;

    limit_len *= EVEN_GENERATION_SCALE;
    const float limit_len_sq = square_f(limit_len);

    BMLoop *l_iter = l_edge;
    do {
      BMLoop *l_adjacent[2] = {l_iter->next, l_iter->prev};
      for (int i = 0; i < ARRAY_SIZE(l_adjacent); i++) {

        float len_sq_other = calc_weighted_edge_collapse(
            tdata->eq_ctx, l_adjacent[i]->e->v1, l_adjacent[i]->e->v2);

        if (len_sq_other > max_ff(len_sq_cmp, limit_len_sq)) {
          //                  edge_queue_insert(eq_ctx, l_adjacent[i]->e, -len_sq_other);
          short_edge_queue_edge_add_recursive_2(tdata,
                                                l_adjacent[i]->radial_next,
                                                l_adjacent[i],
                                                len_sq_other,
                                                limit_len,
                                                depth + 1);
        }
      }
    } while ((l_iter = l_iter->radial_next) != l_end);
  }
}

static void long_edge_queue_edge_add_recursive_2(EdgeQueueThreadData *tdata,
                                                 BMLoop *l_edge,
                                                 BMLoop *l_end,
                                                 const float len_sq,
                                                 float limit_len,
                                                 int depth)
{
  BLI_assert(len_sq > square_f(limit_len));

  if (l_edge->e->head.hflag & BM_ELEM_TAG) {
    return;
  }

#ifdef USE_EDGEQUEUE_FRONTFACE
  if (depth > DEPTH_START_LIMIT && tdata->eq_ctx->q->use_view_normal) {
    if (dot_v3v3(l_edge->f->no, tdata->eq_ctx->q->view_normal) < 0.0f) {
      return;
    }
  }
#endif

  edge_thread_data_insert(tdata, l_edge->e);

  /* temp support previous behavior! */
  if (UNLIKELY(G.debug_value == 1234)) {
    return;
  }

  if ((l_edge->radial_next != l_edge)) {
    const float len_sq_cmp = len_sq * EVEN_EDGELEN_THRESHOLD;

    limit_len *= EVEN_GENERATION_SCALE;
    const float limit_len_sq = square_f(limit_len);

    BMLoop *l_iter = l_edge;
    do {
      BMLoop *l_adjacent[2] = {l_iter->next, l_iter->prev};
      for (int i = 0; i < ARRAY_SIZE(l_adjacent); i++) {

        float len_sq_other = calc_weighted_edge_split(
            tdata->eq_ctx, l_adjacent[i]->e->v1, l_adjacent[i]->e->v2);

        if (len_sq_other > max_ff(len_sq_cmp, limit_len_sq)) {
          //                  edge_queue_insert(eq_ctx, l_adjacent[i]->e, -len_sq_other);
          long_edge_queue_edge_add_recursive_2(tdata,
                                               l_adjacent[i]->radial_next,
                                               l_adjacent[i],
                                               len_sq_other,
                                               limit_len,
                                               depth + 1);
        }
      }
    } while ((l_iter = l_iter->radial_next) != l_end);
  }
}

void long_edge_queue_task_cb(void *__restrict userdata,
                             const int n,
                             const TaskParallelTLS *__restrict tls)
{
  EdgeQueueThreadData *tdata = ((EdgeQueueThreadData *)userdata) + n;
  PBVHNode *node = tdata->node;
  EdgeQueueContext *eq_ctx = tdata->eq_ctx;

  BMFace *f;

  TGSET_ITER (f, node->bm_faces) {
    BMLoop *l = f->l_first;

    do {
      l->e->head.hflag &= ~BM_ELEM_TAG;
      l = l->next;
    } while (l != f->l_first);
  }
  TGSET_ITER_END

  TGSET_ITER (f, node->bm_faces) {
#ifdef USE_EDGEQUEUE_FRONTFACE
    if (eq_ctx->q->use_view_normal) {
      if (dot_v3v3(f->no, eq_ctx->q->view_normal) < 0.0f) {
        continue;
      }
    }
#endif

    if (eq_ctx->q->edge_queue_tri_in_range(eq_ctx->q, f)) {
      /* Check each edge of the face */
      BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
      BMLoop *l_iter = l_first;
      do {
        // try to improve convergence by applying a small amount of smoothing to topology,
        // but tangentially to surface.
        surface_smooth_v_safe(l_iter->v);

#ifdef USE_EDGEQUEUE_EVEN_SUBDIV
        const float len_sq = BM_edge_calc_length_squared(l_iter->e);
        if (len_sq > eq_ctx->q->limit_len_squared) {
          long_edge_queue_edge_add_recursive_2(
              tdata, l_iter->radial_next, l_iter, len_sq, eq_ctx->q->limit_len, 0);
        }
#else
        const float len_sq = BM_edge_calc_length_squared(l_iter->e);
        if (len_sq > eq_ctx->q->limit_len_squared) {
          edge_thread_data_insert(tdata, l_iter->e);
        }
#endif
      } while ((l_iter = l_iter->next) != l_first);
    }
  }
  TGSET_ITER_END
}

void short_edge_queue_task_cb(void *__restrict userdata,
                              const int n,
                              const TaskParallelTLS *__restrict tls)
{
  EdgeQueueThreadData *tdata = ((EdgeQueueThreadData *)userdata) + n;
  PBVHNode *node = tdata->node;
  EdgeQueueContext *eq_ctx = tdata->eq_ctx;

  BMFace *f;

  TGSET_ITER (f, node->bm_faces) {
    BMLoop *l = f->l_first;

    do {
      l->e->head.hflag &= ~BM_ELEM_TAG;
      l = l->next;
    } while (l != f->l_first);
  }
  TGSET_ITER_END

  TGSET_ITER (f, node->bm_faces) {
#ifdef USE_EDGEQUEUE_FRONTFACE
    if (eq_ctx->q->use_view_normal) {
      if (dot_v3v3(f->no, eq_ctx->q->view_normal) < 0.0f) {
        continue;
      }
    }
#endif

    if (eq_ctx->q->edge_queue_tri_in_range(eq_ctx->q, f)) {
      /* Check each edge of the face */
      BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
      BMLoop *l_iter = l_first;
      do {
#ifdef USE_EDGEQUEUE_EVEN_SUBDIV
        const float len_sq = calc_weighted_edge_collapse(eq_ctx, l_iter->e->v1, l_iter->e->v2);
        if (len_sq < eq_ctx->q->limit_len_squared) {
          short_edge_queue_edge_add_recursive_2(
              tdata, l_iter->radial_next, l_iter, len_sq, eq_ctx->q->limit_len, 0);
        }
#else
        const float len_sq = calc_weighted_edge_split(eq_ctx, l_iter->e->v1, l_iter->e->v2);
        if (len_sq > eq_ctx->q->limit_len_squared) {
          edge_thread_data_insert(tdata, l_iter->e);
        }
#endif
      } while ((l_iter = l_iter->next) != l_first);
    }
  }
  TGSET_ITER_END
}

/* Create a priority queue containing vertex pairs connected by a long
 * edge as defined by PBVH.bm_max_edge_len.
 *
 * Only nodes marked for topology update are checked, and in those
 * nodes only edges used by a face intersecting the (center, radius)
 * sphere are checked.
 *
 * The highest priority (lowest number) is given to the longest edge.
 */
static void long_edge_queue_create(EdgeQueueContext *eq_ctx,
                                   PBVH *pbvh,
                                   const float center[3],
                                   const float view_normal[3],
                                   float radius,
                                   const bool use_frontface,
                                   const bool use_projected)
{
  eq_ctx->q->heap = BLI_heapsimple_new();
  eq_ctx->q->elems = NULL;
  eq_ctx->q->totelems = 0;
  eq_ctx->q->center = center;
  eq_ctx->q->radius_squared = radius * radius;
  eq_ctx->q->limit_len_squared = pbvh->bm_max_edge_len * pbvh->bm_max_edge_len;
#ifdef USE_EDGEQUEUE_EVEN_SUBDIV
  eq_ctx->q->limit_len = pbvh->bm_max_edge_len;
#endif

  eq_ctx->q->view_normal = view_normal;

#ifdef USE_EDGEQUEUE_FRONTFACE
  eq_ctx->q->use_view_normal = use_frontface;
#else
  UNUSED_VARS(use_frontface);
#endif

  if (use_projected) {
    eq_ctx->q->edge_queue_tri_in_range = edge_queue_tri_in_circle;
    eq_ctx->q->edge_queue_vert_in_range = edge_queue_vert_in_circle;
    project_plane_normalized_v3_v3v3(eq_ctx->q->center_proj, center, view_normal);
  }
  else {
    eq_ctx->q->edge_queue_tri_in_range = edge_queue_tri_in_sphere;
    eq_ctx->q->edge_queue_vert_in_range = edge_queue_vert_in_sphere;
  }

#ifdef USE_EDGEQUEUE_TAG_VERIFY
  pbvh_bmesh_edge_tag_verify(pbvh);
#endif

  EdgeQueueThreadData *tdata = NULL;
  BLI_array_declare(tdata);

  for (int n = 0; n < pbvh->totnode; n++) {
    PBVHNode *node = &pbvh->nodes[n];

    /* Check leaf nodes marked for topology update */
    if ((node->flag & PBVH_Leaf) && (node->flag & PBVH_UpdateTopology) &&
        !(node->flag & PBVH_FullyHidden)) {
      EdgeQueueThreadData td;

      memset(&td, 0, sizeof(td));

      td.pbvh = pbvh;
      td.node = node;
      td.eq_ctx = eq_ctx;

      BLI_array_append(tdata, td);
      /* Check each face */
      /*
      BMFace *f;
      TGSET_ITER (f, node->bm_faces) {
        long_edge_queue_face_add(eq_ctx, f);
      }
      TGSET_ITER_END
      */
    }
  }

  int count = BLI_array_len(tdata);

  TaskParallelSettings settings;

  BLI_parallel_range_settings_defaults(&settings);
  BLI_task_parallel_range(0, count, tdata, long_edge_queue_task_cb, &settings);

  for (int i = 0; i < count; i++) {
    EdgeQueueThreadData *td = tdata + i;

    BMEdge **edges = td->edges;
    for (int j = 0; j < td->totedge; j++) {
      BMEdge *e = edges[j];
      e->head.hflag &= ~BM_ELEM_TAG;
      edge_queue_insert(eq_ctx, e, -calc_weighted_edge_split(eq_ctx, e->v1, e->v2));
    }

    if (td->edges) {
      MEM_freeN(td->edges);
    }
  }
  BLI_array_free(tdata);
}

/* Create a priority queue containing vertex pairs connected by a
 * short edge as defined by PBVH.bm_min_edge_len.
 *
 * Only nodes marked for topology update are checked, and in those
 * nodes only edges used by a face intersecting the (center, radius)
 * sphere are checked.
 *
 * The highest priority (lowest number) is given to the shortest edge.
 */
static void short_edge_queue_create(EdgeQueueContext *eq_ctx,
                                    PBVH *pbvh,
                                    const float center[3],
                                    const float view_normal[3],
                                    float radius,
                                    const bool use_frontface,
                                    const bool use_projected)
{
  eq_ctx->q->heap = BLI_heapsimple_new();
  eq_ctx->q->elems = NULL;
  eq_ctx->q->totelems = 0;
  eq_ctx->q->center = center;
  eq_ctx->q->radius_squared = radius * radius;
  eq_ctx->q->limit_len_squared = pbvh->bm_min_edge_len * pbvh->bm_min_edge_len;
#ifdef USE_EDGEQUEUE_EVEN_SUBDIV
  eq_ctx->q->limit_len = pbvh->bm_min_edge_len;
#endif

  eq_ctx->q->view_normal = view_normal;

#ifdef USE_EDGEQUEUE_FRONTFACE
  eq_ctx->q->use_view_normal = use_frontface;
#else
  UNUSED_VARS(use_frontface);
#endif

  if (use_projected) {
    eq_ctx->q->edge_queue_tri_in_range = edge_queue_tri_in_circle;
    eq_ctx->q->edge_queue_vert_in_range = edge_queue_vert_in_circle;
    project_plane_normalized_v3_v3v3(eq_ctx->q->center_proj, center, view_normal);
  }
  else {
    eq_ctx->q->edge_queue_tri_in_range = edge_queue_tri_in_sphere;
    eq_ctx->q->edge_queue_vert_in_range = edge_queue_vert_in_sphere;
  }

  EdgeQueueThreadData *tdata = NULL;
  BLI_array_declare(tdata);

  for (int n = 0; n < pbvh->totnode; n++) {
    PBVHNode *node = &pbvh->nodes[n];
    EdgeQueueThreadData td;

    if ((node->flag & PBVH_Leaf) && (node->flag & PBVH_UpdateTopology) &&
        !(node->flag & PBVH_FullyHidden)) {
      memset(&td, 0, sizeof(td));
      td.pbvh = pbvh;
      td.node = node;
      td.eq_ctx = eq_ctx;

      BLI_array_append(tdata, td);
    }

#if 0
    /* Check leaf nodes marked for topology update */
      BMFace *f;

      /* Check each face */
      TGSET_ITER (f, node->bm_faces) {
        short_edge_queue_face_add(eq_ctx, f);
      }
      TGSET_ITER_END
    }
#endif
  }

  int count = BLI_array_len(tdata);

  TaskParallelSettings settings;

  BLI_parallel_range_settings_defaults(&settings);
  BLI_task_parallel_range(0, count, tdata, short_edge_queue_task_cb, &settings);

  for (int i = 0; i < count; i++) {
    EdgeQueueThreadData *td = tdata + i;

    BMEdge **edges = td->edges;
    for (int j = 0; j < td->totedge; j++) {
      BMEdge *e = edges[j];
      e->head.hflag &= ~BM_ELEM_TAG;
      edge_queue_insert(eq_ctx, e, calc_weighted_edge_collapse(eq_ctx, e->v1, e->v2));
    }

    if (td->edges) {
      MEM_freeN(td->edges);
    }
  }

  BLI_array_free(tdata);
}

/*************************** Topology update **************************/

static void pbvh_bmesh_split_edge(EdgeQueueContext *eq_ctx,
                                  PBVH *pbvh,
                                  BMEdge *e,
                                  BLI_Buffer *edge_loops)
{
  BMesh *bm = pbvh->bm;

  float co_mid[3], no_mid[3];
  MDynTopoVert *mv1 = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, e->v1);
  MDynTopoVert *mv2 = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, e->v2);

  bool boundary = (mv1->flag & DYNVERT_BOUNDARY) && (mv2->flag & DYNVERT_BOUNDARY);

  /* Get all faces adjacent to the edge */
  pbvh_bmesh_edge_loops(edge_loops, e);

  /* Create a new vertex in current node at the edge's midpoint */
  mid_v3_v3v3(co_mid, e->v1->co, e->v2->co);
  mid_v3_v3v3(no_mid, e->v1->no, e->v2->no);
  normalize_v3(no_mid);

  int node_index = BM_ELEM_CD_GET_INT(e->v1, eq_ctx->cd_vert_node_offset);
  BMVert *v_new = pbvh_bmesh_vert_create(
      pbvh, node_index, co_mid, no_mid, NULL, eq_ctx->cd_vert_mask_offset);
  // transfer edge flags

  BMEdge *e1 = BM_edge_create(pbvh->bm, e->v1, v_new, e, BM_CREATE_NOP);
  BMEdge *e2 = BM_edge_create(pbvh->bm, v_new, e->v2, e, BM_CREATE_NOP);

  int eflag = e->head.hflag & ~BM_ELEM_HIDDEN;
  int vflag = (e->v1->head.hflag | e->v2->head.hflag) & ~BM_ELEM_HIDDEN;

  e1->head.hflag = e2->head.hflag = eflag;
  v_new->head.hflag = vflag;

  /*TODO: is it worth interpolating edge customdata?*/

  void *vsrcs[2] = {e->v1->head.data, e->v2->head.data};
  float vws[2] = {0.5f, 0.5f};
  CustomData_bmesh_interp(
      &pbvh->bm->vdata, (const void **)vsrcs, (float *)vws, NULL, 2, v_new->head.data);

  if (boundary) {
    MDynTopoVert *mv_new = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, v_new);
    mv_new->flag |= DYNVERT_BOUNDARY;
  }

  /* update paint mask */
  if (eq_ctx->cd_dyn_vert != -1) {
    float mask_v1 = DYNTOPO_MASK(eq_ctx->cd_dyn_vert, e->v1);
    float mask_v2 = DYNTOPO_MASK(eq_ctx->cd_dyn_vert, e->v2);

    float mask_v_new = 0.5f * (mask_v1 + mask_v2);

    BM_ELEM_CD_SET_FLOAT(v_new, eq_ctx->cd_vert_mask_offset, mask_v_new);
  }

  /* For each face, add two new triangles and delete the original */
  for (int i = 0; i < edge_loops->count; i++) {
    BMLoop *l_adj = BLI_buffer_at(edge_loops, BMLoop *, i);
    BMFace *f_adj = l_adj->f;
    BMFace *f_new;
    BMVert *v_opp, *v1, *v2;
    BMVert *v_tri[3];
    BMEdge *e_tri[3];

    BLI_assert(f_adj->len == 3);
    int ni = BM_ELEM_CD_GET_INT(f_adj, eq_ctx->cd_face_node_offset);

    /* Find the vertex not in the edge */
    v_opp = l_adj->prev->v;

    /* Get e->v1 and e->v2 in the order they appear in the
     * existing face so that the new faces' winding orders
     * match */
    v1 = l_adj->v;
    v2 = l_adj->next->v;

    if (ni != node_index && i == 0) {
      pbvh_bmesh_vert_ownership_transfer(pbvh, &pbvh->nodes[ni], v_new);
    }

    /**
     * The 2 new faces created and assigned to ``f_new`` have their
     * verts & edges shuffled around.
     *
     * - faces wind anticlockwise in this example.
     * - original edge is ``(v1, v2)``
     * - original face is ``(v1, v2, v3)``
     *
     * <pre>
     *         + v3(v_opp)
     *        /|\
     *       / | \
     *      /  |  \
     *   e4/   |   \ e3
     *    /    |e5  \
     *   /     |     \
     *  /  e1  |  e2  \
     * +-------+-------+
     * v1      v4(v_new) v2
     *  (first) (second)
     * </pre>
     *
     * - f_new (first):  ``v_tri=(v1, v4, v3), e_tri=(e1, e5, e4)``
     * - f_new (second): ``v_tri=(v4, v2, v3), e_tri=(e2, e3, e5)``
     */

    /* Create two new faces */
    v_tri[0] = v1;
    v_tri[1] = v_new;
    v_tri[2] = v_opp;
    bm_edges_from_tri(pbvh->bm, v_tri, e_tri);
    f_new = pbvh_bmesh_face_create(pbvh, ni, v_tri, e_tri, f_adj, false, true);
    long_edge_queue_face_add(eq_ctx, f_new, true);

    pbvh_bmesh_copy_facedata(bm, f_new, f_adj);

    // customdata interpolation
    BMLoop *lfirst = f_adj->l_first;
    while (lfirst->v != v1) {
      lfirst = lfirst->next;

      // paranoia check
      if (lfirst == f_adj->l_first) {
        break;
      }
    }

    BMLoop *l1 = lfirst;
    BMLoop *l2 = lfirst->next;
    BMLoop *l3 = lfirst->next->next;

    void *lsrcs[2] = {l1->head.data, l2->head.data};
    float lws[2] = {0.5f, 0.5f};

    CustomData_bmesh_interp(
        &pbvh->bm->ldata, (const void **)lsrcs, lws, lws, 2, f_new->l_first->next->head.data);

    lsrcs[0] = l1->head.data;
    lws[0] = 1.0f;

    CustomData_bmesh_interp(
        &pbvh->bm->ldata, (const void **)lsrcs, lws, lws, 1, f_new->l_first->head.data);

    lsrcs[0] = l3->head.data;
    lws[0] = 1.0f;

    CustomData_bmesh_interp(
        &pbvh->bm->ldata, (const void **)lsrcs, lws, lws, 1, f_new->l_first->prev->head.data);

    v_tri[0] = v_new;
    v_tri[1] = v2;
    /* v_tri[2] = v_opp; */ /* unchanged */
    e_tri[0] = BM_edge_create(pbvh->bm, v_tri[0], v_tri[1], NULL, BM_CREATE_NO_DOUBLE);
    e_tri[2] = e_tri[1]; /* switched */
    e_tri[1] = BM_edge_create(pbvh->bm, v_tri[1], v_tri[2], NULL, BM_CREATE_NO_DOUBLE);

    f_new = pbvh_bmesh_face_create(pbvh, ni, v_tri, e_tri, f_adj, false, true);
    long_edge_queue_face_add(eq_ctx, f_new, true);

    pbvh_bmesh_copy_facedata(bm, f_new, f_adj);

    // customdata interpolation
    lsrcs[0] = lfirst->head.data;
    lsrcs[1] = lfirst->next->head.data;
    lws[0] = lws[1] = 0.5f;

    CustomData_bmesh_interp(
        &pbvh->bm->ldata, (const void **)lsrcs, lws, lws, 2, f_new->l_first->head.data);

    lsrcs[0] = lfirst->next->head.data;
    ;
    lws[0] = 1.0f;

    CustomData_bmesh_interp(
        &pbvh->bm->ldata, (const void **)lsrcs, lws, lws, 1, f_new->l_first->next->head.data);

    lsrcs[0] = lfirst->prev->head.data;
    lws[0] = 1.0f;

    CustomData_bmesh_interp(
        &pbvh->bm->ldata, (const void **)lsrcs, lws, lws, 1, f_new->l_first->prev->head.data);

    /* Delete original */
    pbvh_bmesh_face_remove(pbvh, f_adj);
    BM_face_kill(pbvh->bm, f_adj);

    /* Ensure new vertex is in the node */
    if (!BLI_table_gset_haskey(pbvh->nodes[ni].bm_unique_verts, v_new)) {
      BLI_table_gset_add(pbvh->nodes[ni].bm_other_verts, v_new);
    }
  }

  BM_edge_kill(pbvh->bm, e);
}

static bool pbvh_bmesh_subdivide_long_edges(EdgeQueueContext *eq_ctx,
                                            PBVH *pbvh,
                                            BLI_Buffer *edge_loops,
                                            int max_steps)
{
  bool any_subdivided = false;
  double time = PIL_check_seconds_timer();

  RNG *rng = BLI_rng_new((int)(time * 1000.0f));
  int step = 0;

  while (!BLI_heapsimple_is_empty(eq_ctx->q->heap)) {
    if (step++ > max_steps) {
      break;
    }

#ifdef DYNTOPO_TIME_LIMIT
    if (PIL_check_seconds_timer() - time > DYNTOPO_TIME_LIMIT) {
      break;
    }
#endif

#ifndef DYNTOPO_USE_HEAP
    if (eq_ctx->q->totelems == 0) {
      break;
    }

    int ri = BLI_rng_get_int(rng) % eq_ctx->q->totelems;

    BMVert **pair = eq_ctx->q->elems[ri];
    eq_ctx->q->elems[ri] = eq_ctx->q->elems[eq_ctx->q->totelems - 1];
    eq_ctx->q->totelems--;
#else
    BMVert **pair = BLI_heapsimple_pop_min(eq_ctx->q->heap);
#endif
    BMVert *v1 = pair[0], *v2 = pair[1];
    BMEdge *e;

    BLI_mempool_free(eq_ctx->pool, pair);
    pair = NULL;

    /* Check that the edge still exists */
    if (!(e = BM_edge_exists(v1, v2))) {
      continue;
    }

#ifdef USE_EDGEQUEUE_TAG
    EDGE_QUEUE_DISABLE(e);
#endif

    /* At the moment edges never get shorter (subdiv will make new edges)
     * unlike collapse where edges can become longer. */
#if 0
    if (len_squared_v3v3(v1->co, v2->co) <= eq_ctx->q->limit_len_squared) {
      continue;
    }
#else
    // BLI_assert(calc_weighted_edge_split(eq_ctx, v1->co, v2->co) > eq_ctx->q->limit_len_squared);
#endif

    /* Check that the edge's vertices are still in the PBVH. It's
     * possible that an edge collapse has deleted adjacent faces
     * and the node has been split, thus leaving wire edges and
     * associated vertices. */
    if ((BM_ELEM_CD_GET_INT(e->v1, eq_ctx->cd_vert_node_offset) == DYNTOPO_NODE_NONE) ||
        (BM_ELEM_CD_GET_INT(e->v2, eq_ctx->cd_vert_node_offset) == DYNTOPO_NODE_NONE)) {
      continue;
    }

    any_subdivided = true;

    pbvh_bmesh_split_edge(eq_ctx, pbvh, e, edge_loops);
  }

#if !defined(DYNTOPO_USE_HEAP) && defined(USE_EDGEQUEUE_TAG)
  for (int i = 0; i < eq_ctx->q->totelems; i++) {
    BMVert **pair = eq_ctx->q->elems[i];
    BMVert *v1 = pair[0], *v2 = pair[1];

    BMEdge *e = BM_edge_exists(v1, v2);

    if (e) {
      EDGE_QUEUE_DISABLE(e);
    }
  }
#endif

#ifdef USE_EDGEQUEUE_TAG_VERIFY
  pbvh_bmesh_edge_tag_verify(pbvh);
#endif

  BLI_rng_free(rng);

  return any_subdivided;
}

static void pbvh_bmesh_collapse_edge(PBVH *pbvh,
                                     BMEdge *e,
                                     BMVert *v1,
                                     BMVert *v2,
                                     GHash *deleted_verts,
                                     BLI_Buffer *deleted_faces,
                                     EdgeQueueContext *eq_ctx)
{
  BMVert *v_del, *v_conn;

  MDynTopoVert *mv1 = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, v1);
  MDynTopoVert *mv2 = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, v2);

  // customdata interpolation
  if (BM_elem_flag_test(e, BM_ELEM_SEAM)) {
    for (int step = 0; step < 2; step++) {
      int count = 0;
      BMVert *v = step ? v2 : v1;
      BMIter iter;
      BMEdge *e2;

      BM_ITER_ELEM (e2, &iter, v, BM_EDGES_OF_VERT) {
        if (BM_elem_flag_test(e2, BM_ELEM_SEAM)) {
          count++;
        }
      }

      if (count < 2) {
        return;
      }
    }
  }

  /* one of the two vertices may be masked, select the correct one for deletion */
  if (DYNTOPO_MASK(eq_ctx->cd_vert_mask_offset, v1) <
      DYNTOPO_MASK(eq_ctx->cd_vert_mask_offset, v2)) {
    v_del = v1;
    v_conn = v2;
  }
  else {
    v_del = v2;
    v_conn = v1;
  }

  /* Remove the merge vertex from the PBVH */
  pbvh_bmesh_vert_remove(pbvh, v_del);

  /* Remove all faces adjacent to the edge */
  BMLoop *l_adj;
  while ((l_adj = e->l)) {
    BMFace *f_adj = l_adj->f;

    int eflag = 0;

    // propegate flags to merged edges
    BMLoop *l = f_adj->l_first;
    do {
      BMEdge *e2 = l->e;

      if (e2 != e) {
        eflag |= e2->head.hflag & ~BM_ELEM_HIDDEN;
      }

      l = l->next;
    } while (l != f_adj->l_first);

    do {
      BMEdge *e2 = l->e;
      e2->head.hflag |= eflag;

      l = l->next;
    } while (l != f_adj->l_first);

    pbvh_bmesh_face_remove(pbvh, f_adj);
    BM_face_kill(pbvh->bm, f_adj);
  }

  /* Kill the edge */
  BLI_assert(BM_edge_is_wire(e));
  BM_edge_kill(pbvh->bm, e);

  /* For all remaining faces of v_del, create a new face that is the
   * same except it uses v_conn instead of v_del */
  /* Note: this could be done with BM_vert_splice(), but that
   * requires handling other issues like duplicate edges, so doesn't
   * really buy anything. */
  BLI_buffer_clear(deleted_faces);

  BMLoop *l;
  BMLoop **ls = NULL;
  void **blocks = NULL;
  float *ws = NULL;

  BLI_array_staticdeclare(ls, 64);
  BLI_array_staticdeclare(blocks, 64);
  BLI_array_staticdeclare(ws, 64);

  int totl = 0;

  BM_LOOPS_OF_VERT_ITER_BEGIN (l, v_del) {
    BLI_array_append(ls, l);
    totl++;
  }
  BM_LOOPS_OF_VERT_ITER_END;

  BM_LOOPS_OF_VERT_ITER_BEGIN (l, v_conn) {
    BLI_array_append(ls, l);
    totl++;
  }
  BM_LOOPS_OF_VERT_ITER_END;

  float w = totl > 0 ? 1.0f / (float)(totl) : 1.0f;

  for (int i = 0; i < totl; i++) {
    BLI_array_append(blocks, ls[i]->head.data);
    BLI_array_append(ws, w);
  }

  // snap customdata
  if (totl > 0) {
    CustomData_bmesh_interp(
        &pbvh->bm->ldata, (const void **)blocks, ws, NULL, totl, ls[0]->head.data);
    //*
    BM_LOOPS_OF_VERT_ITER_BEGIN (l, v_del) {
      BMLoop *l2 = l->v != v_del ? l->next : l;

      if (l2 == ls[0]) {
        continue;
      }

      CustomData_bmesh_copy_data(
          &pbvh->bm->ldata, &pbvh->bm->ldata, ls[0]->head.data, &l2->head.data);
    }
    BM_LOOPS_OF_VERT_ITER_END;

    BM_LOOPS_OF_VERT_ITER_BEGIN (l, v_conn) {
      BMLoop *l2 = l->v != v_conn ? l->next : l;

      if (l2 == ls[0]) {
        continue;
      }

      CustomData_bmesh_copy_data(
          &pbvh->bm->ldata, &pbvh->bm->ldata, ls[0]->head.data, &l2->head.data);
    }
    BM_LOOPS_OF_VERT_ITER_END;
    //*/
  }

  BM_LOOPS_OF_VERT_ITER_BEGIN (l, v_del) {
    BMFace *existing_face;

    /* Get vertices, replace use of v_del with v_conn */
    // BM_iter_as_array(NULL, BM_VERTS_OF_FACE, f, (void **)v_tri, 3);
    BMFace *f = l->f;

    /* Check if a face using these vertices already exists. If so,
     * skip adding this face and mark the existing one for
     * deletion as well. Prevents extraneous "flaps" from being
     * created. */
#if 0
    if (UNLIKELY(existing_face = BM_face_exists(v_tri, 3)))
#else
    if (UNLIKELY(existing_face = bm_face_exists_tri_from_loop_vert(l->next, v_conn)))
#endif
    {
      bool ok = true;

      // check we're not already in deleted_faces
      for (int i = 0; i < deleted_faces->count; i++) {
        if (BLI_buffer_at(deleted_faces, BMFace *, i) == existing_face) {
          ok = false;
          break;
        }
      }

      if (ok) {
        BLI_buffer_append(deleted_faces, BMFace *, existing_face);
      }
    }
    else
    {
      BMVert *v_tri[3] = {v_conn, l->next->v, l->prev->v};

      BLI_assert(!BM_face_exists(v_tri, 3));
      BMEdge *e_tri[3];
      PBVHNode *n = pbvh_bmesh_node_from_face(pbvh, f);
      int ni = n - pbvh->nodes;
      bm_edges_from_tri(pbvh->bm, v_tri, e_tri);
      BMFace *f2 = pbvh_bmesh_face_create(pbvh, ni, v_tri, e_tri, f, false, true);

      BMLoop *l2 = f2->l_first;

      // sync edge flags
      l2->e->head.hflag |= (l->e->head.hflag & ~BM_ELEM_HIDDEN);
      // l2->prev->e->head.hflag |= (l->prev->e->head.hflag & ~BM_ELEM_HIDDEN);

      pbvh_bmesh_copy_facedata(pbvh->bm, f2, f);

      CustomData_bmesh_copy_data(&pbvh->bm->ldata, &pbvh->bm->ldata, l->head.data, &l2->head.data);
      CustomData_bmesh_copy_data(
          &pbvh->bm->ldata, &pbvh->bm->ldata, l->next->head.data, &l2->next->head.data);
      CustomData_bmesh_copy_data(
          &pbvh->bm->ldata, &pbvh->bm->ldata, l->prev->head.data, &l2->prev->head.data);

      /* Ensure that v_conn is in the new face's node */
      if (!BLI_table_gset_haskey(n->bm_unique_verts, v_conn)) {
        BLI_table_gset_add(n->bm_other_verts, v_conn);
      }
    }

    BLI_buffer_append(deleted_faces, BMFace *, f);
  }
  BM_LOOPS_OF_VERT_ITER_END;

  /* Delete the tagged faces */
  for (int i = 0; i < deleted_faces->count; i++) {
    BMFace *f_del = BLI_buffer_at(deleted_faces, BMFace *, i);

    /* Get vertices and edges of face */
    BLI_assert(f_del->len == 3);
    BMLoop *l_iter = BM_FACE_FIRST_LOOP(f_del);
    BMVert *v_tri[3];
    BMEdge *e_tri[3];
    v_tri[0] = l_iter->v;
    e_tri[0] = l_iter->e;
    l_iter = l_iter->next;
    v_tri[1] = l_iter->v;
    e_tri[1] = l_iter->e;
    l_iter = l_iter->next;
    v_tri[2] = l_iter->v;
    e_tri[2] = l_iter->e;

    BMLoop *l1 = f_del->l_first;
    do {
      if (!l1->e) {
        printf("bmesh error!\n");
        l1->e = BM_edge_exists(l->v, l->next->v);
        if (!l1->e) {
          // create
          l1->e = BM_edge_create(pbvh->bm, l->v, l->next->v, NULL, 0);
        }
      }
      l1 = l1->next;
    } while (l1 != f_del->l_first);

    /* Remove the face */
    pbvh_bmesh_face_remove(pbvh, f_del);
    BM_face_kill(pbvh->bm, f_del);

    /* Check if any of the face's edges are now unused by any
     * face, if so delete them */
    for (int j = 0; j < 3; j++) {
      if (BM_edge_is_wire(e_tri[j])) {
        BM_edge_kill(pbvh->bm, e_tri[j]);
      }
    }

    /* Check if any of the face's vertices are now unused, if so
     * remove them from the PBVH */
    for (int j = 0; j < 3; j++) {
      if ((v_tri[j] != v_del) && (v_tri[j]->e == NULL)) {
        pbvh_bmesh_vert_remove(pbvh, v_tri[j]);

        BM_log_vert_removed(pbvh->bm_log, v_tri[j], eq_ctx->cd_vert_mask_offset);

        if (v_tri[j] == v_conn) {
          v_conn = NULL;
        }
        BLI_ghash_insert(deleted_verts, v_tri[j], NULL);
        BM_vert_kill(pbvh->bm, v_tri[j]);
      }
    }
  }

  /* Move v_conn to the midpoint of v_conn and v_del (if v_conn still exists, it
   * may have been deleted above) */
  if (v_conn != NULL) {
    // log vert in bmlog, but don't update original customata layers, we want them to be
    // interpolated
    BM_log_vert_before_modified(pbvh->bm_log, v_conn, eq_ctx->cd_vert_mask_offset, false);

    mid_v3_v3v3(v_conn->co, v_conn->co, v_del->co);
    add_v3_v3(v_conn->no, v_del->no);
    normalize_v3(v_conn->no);

    /* update boundboxes attached to the connected vertex
     * note that we can often get-away without this but causes T48779 */
    BM_LOOPS_OF_VERT_ITER_BEGIN (l, v_conn) {
      PBVHNode *f_node = pbvh_bmesh_node_from_face(pbvh, l->f);
      f_node->flag |= PBVH_UpdateDrawBuffers | PBVH_UpdateNormals | PBVH_UpdateBB |
                      PBVH_UpdateTris;
    }
    BM_LOOPS_OF_VERT_ITER_END;

    if (BM_vert_is_boundary(v_conn)) {
      MDynTopoVert *mv_conn = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, v_conn);
      mv_conn->flag |= DYNVERT_BOUNDARY;
    }
  }

  /* Delete v_del */
  BLI_assert(!BM_vert_face_check(v_del));
  BM_log_vert_removed(pbvh->bm_log, v_del, eq_ctx->cd_vert_mask_offset);
  /* v_conn == NULL is OK */
  BLI_ghash_insert(deleted_verts, v_del, v_conn);
  BM_vert_kill(pbvh->bm, v_del);

  BLI_array_free(ws);
  BLI_array_free(blocks);
  BLI_array_free(ls);
}

void BKE_pbvh_bmesh_update_origvert(
    PBVH *pbvh, BMVert *v, float **r_co, float **r_no, float **r_color, bool log_undo)
{
  float *co = NULL, *no = NULL;

  MDynTopoVert *mv = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, v);

  if (log_undo) {
    BM_log_vert_before_modified(pbvh->bm_log, v, pbvh->cd_vert_mask_offset, r_color != NULL);
  }

  if (r_co || r_no) {

    copy_v3_v3(mv->origco, v->co);
    copy_v3_v3(mv->origno, v->no);

    if (r_co) {
      *r_co = mv->origco;
    }

    if (r_no) {
      *r_no = mv->origno;
    }
  }

  if (r_color && pbvh->cd_vcol_offset >= 0) {
    MPropCol *ml1 = BM_ELEM_CD_GET_VOID_P(v, pbvh->cd_vcol_offset);

    copy_v4_v4(mv->origcolor, ml1->color);

    if (r_color) {
      *r_color = mv->origcolor;
    }
  }
  else if (r_color) {
    *r_color = NULL;
  }
}

static bool pbvh_bmesh_collapse_short_edges(EdgeQueueContext *eq_ctx,
                                            PBVH *pbvh,
                                            BLI_Buffer *deleted_faces,
                                            int max_steps)
{
  const float min_len_squared = pbvh->bm_min_edge_len * pbvh->bm_min_edge_len;
  bool any_collapsed = false;
  /* deleted verts point to vertices they were merged into, or NULL when removed. */
  GHash *deleted_verts = BLI_ghash_ptr_new("deleted_verts");

  double time = PIL_check_seconds_timer();
  RNG *rng = BLI_rng_new(time * 1000.0f);

//#define TEST_COLLAPSE
#ifdef TEST_COLLAPSE
  int _i = 0;
#endif

  int step = 0;

  while (!BLI_heapsimple_is_empty(eq_ctx->q->heap)) {
    if (step++ > max_steps) {
      break;
    }
#ifdef DYNTOPO_TIME_LIMIT
    if (PIL_check_seconds_timer() - time > DYNTOPO_TIME_LIMIT) {
      break;
    }
#endif

#ifndef DYNTOPO_USE_HEAP
    if (eq_ctx->q->totelems == 0) {
      break;
    }

    int ri = BLI_rng_get_int(rng) % eq_ctx->q->totelems;

    BMVert **pair = eq_ctx->q->elems[ri];
    eq_ctx->q->elems[ri] = eq_ctx->q->elems[eq_ctx->q->totelems - 1];
    eq_ctx->q->totelems--;
#else
    BMVert **pair = BLI_heapsimple_pop_min(eq_ctx->q->heap);
#endif
    BMVert *v1 = pair[0], *v2 = pair[1];
    BLI_mempool_free(eq_ctx->pool, pair);
    pair = NULL;

    /* Check the verts still exist */
    if (!(v1 = bm_vert_hash_lookup_chain(deleted_verts, v1)) ||
        !(v2 = bm_vert_hash_lookup_chain(deleted_verts, v2)) || (v1 == v2)) {
      continue;
    }

    /* Check that the edge still exists */
    BMEdge *e;
    if (!(e = BM_edge_exists(v1, v2))) {
      continue;
    }
#ifdef USE_EDGEQUEUE_TAG
    EDGE_QUEUE_DISABLE(e);
#endif

    if (calc_weighted_edge_collapse(eq_ctx, v1, v2) >= min_len_squared) {
      continue;
    }

    /* Check that the edge's vertices are still in the PBVH. It's
     * possible that an edge collapse has deleted adjacent faces
     * and the node has been split, thus leaving wire edges and
     * associated vertices. */
    if ((BM_ELEM_CD_GET_INT(e->v1, eq_ctx->cd_vert_node_offset) == DYNTOPO_NODE_NONE) ||
        (BM_ELEM_CD_GET_INT(e->v2, eq_ctx->cd_vert_node_offset) == DYNTOPO_NODE_NONE)) {
      continue;
    }

    any_collapsed = true;

    pbvh_bmesh_collapse_edge(pbvh, e, v1, v2, deleted_verts, deleted_faces, eq_ctx);

#ifdef TEST_COLLAPSE
    if (_i++ > 10) {
      break;
    }
#endif
  }

#if !defined(DYNTOPO_USE_HEAP) && defined(USE_EDGEQUEUE_TAG)
  for (int i = 0; i < eq_ctx->q->totelems; i++) {
    BMVert **pair = eq_ctx->q->elems[i];
    BMVert *v1 = pair[0], *v2 = pair[1];

    /* Check the verts still exist */
    if (!(v1 = bm_vert_hash_lookup_chain(deleted_verts, v1)) ||
        !(v2 = bm_vert_hash_lookup_chain(deleted_verts, v2)) || (v1 == v2)) {
      continue;
    }

    BMEdge *e = BM_edge_exists(v1, v2);
    if (e) {
      EDGE_QUEUE_DISABLE(e);
    }
  }
#endif
  BLI_rng_free(rng);
  BLI_ghash_free(deleted_verts, NULL, NULL);

  return any_collapsed;
}

/************************* Called from pbvh.c *************************/

bool BKE_pbvh_bmesh_check_origdata(PBVH *pbvh, BMVert *v, int stroke_id)
{
  MDynTopoVert *mv = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, v);

  if (mv->stroke_id != stroke_id) {
    float *dummy;

    BKE_pbvh_bmesh_update_origvert(pbvh, v, &dummy, &dummy, &dummy, false);
    mv->stroke_id = stroke_id;
    return true;
  }

  return false;
}

bool pbvh_bmesh_node_raycast(PBVH *pbvh,
                             PBVHNode *node,
                             const float ray_start[3],
                             const float ray_normal[3],
                             struct IsectRayPrecalc *isect_precalc,
                             float *depth,
                             bool use_original,
                             SculptVertRef *r_active_vertex_index,
                             SculptFaceRef *r_active_face_index,
                             float *r_face_normal,
                             int stroke_id)
{
  bool hit = false;
  float nearest_vertex_co[3] = {0.0f};
  float nearest_vertex_dist = 1e17;

  BKE_pbvh_bmesh_check_tris(pbvh, node);

  PBVHTriBuf *tribuf = node->tribuf;
  const int cd_dyn_vert = pbvh->cd_dyn_vert;

  for (int i = 0; i < node->tribuf->tottri; i++) {
    PBVHTri *tri = tribuf->tris + i;
    BMVert *v1 = (BMVert *)tribuf->verts[tri->v[0]].i;
    BMVert *v2 = (BMVert *)tribuf->verts[tri->v[1]].i;
    BMVert *v3 = (BMVert *)tribuf->verts[tri->v[2]].i;

    BMFace *f = (BMFace *)tri->f.i;

    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    float *co1, *co2, *co3;

    if (use_original) {
      BKE_pbvh_bmesh_check_origdata(pbvh, v1, stroke_id);
      BKE_pbvh_bmesh_check_origdata(pbvh, v2, stroke_id);
      BKE_pbvh_bmesh_check_origdata(pbvh, v3, stroke_id);

      co1 = BKE_PBVH_DYNVERT(cd_dyn_vert, v1)->origco;
      co2 = BKE_PBVH_DYNVERT(cd_dyn_vert, v2)->origco;
      co3 = BKE_PBVH_DYNVERT(cd_dyn_vert, v3)->origco;
    }
    else {
      co1 = v1->co;
      co2 = v2->co;
      co3 = v3->co;
    }
    bool hit2 = ray_face_intersection_tri(ray_start, isect_precalc, co1, co2, co3, depth);

    if (hit2) {
      // ensure sculpt active vertex is set r_active_vertex_index

      for (int j = 0; j < 3; j++) {
        BMVert *v = (BMVert *)tribuf->verts[tri->v[j]].i;
        float *co = BKE_PBVH_DYNVERT(cd_dyn_vert, v)->origco;

        float dist = len_squared_v3v3(co, ray_start);
        if (dist < nearest_vertex_dist) {
          nearest_vertex_dist = dist;
          copy_v3_v3(nearest_vertex_co, co);

          hit = true;
          if (r_active_vertex_index) {
            *r_active_vertex_index = tribuf->verts[tri->v[j]];
          }

          if (r_active_face_index) {
            *r_active_face_index = tri->f;
          }

          if (r_face_normal) {
            float no[3];

            if (use_original) {
              copy_v3_v3(no, BKE_PBVH_DYNVERT(cd_dyn_vert, v1)->origno);
              add_v3_v3(no, BKE_PBVH_DYNVERT(cd_dyn_vert, v2)->origno);
              add_v3_v3(no, BKE_PBVH_DYNVERT(cd_dyn_vert, v3)->origno);
              normalize_v3(no);
            }
            else {
              copy_v3_v3(no, tri->no);
            }

            copy_v3_v3(r_face_normal, no);
          }
        }
      }

      hit = true;
    }
  }

  return hit;
}

bool BKE_pbvh_bmesh_node_raycast_detail(PBVH *pbvh,
                                        PBVHNode *node,
                                        const float ray_start[3],
                                        struct IsectRayPrecalc *isect_precalc,
                                        float *depth,
                                        float *r_edge_length)
{
  if (node->flag & PBVH_FullyHidden) {
    return false;
  }

  bool hit = false;
  BMFace *f_hit = NULL;

  BKE_pbvh_bmesh_check_tris(pbvh, node);
  for (int i = 0; i < node->tribuf->tottri; i++) {
    PBVHTri *tri = node->tribuf->tris + i;
    BMVert *v1 = (BMVert *)node->tribuf->verts[tri->v[0]].i;
    BMVert *v2 = (BMVert *)node->tribuf->verts[tri->v[1]].i;
    BMVert *v3 = (BMVert *)node->tribuf->verts[tri->v[2]].i;
    BMFace *f = (BMFace *)tri->f.i;

    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    bool hit_local = ray_face_intersection_tri(
        ray_start, isect_precalc, v1->co, v2->co, v3->co, depth);

    if (hit_local) {
      float len1 = len_squared_v3v3(v1->co, v2->co);
      float len2 = len_squared_v3v3(v2->co, v3->co);
      float len3 = len_squared_v3v3(v3->co, v1->co);

      /* detail returned will be set to the maximum allowed size, so take max here */
      *r_edge_length = sqrtf(max_fff(len1, len2, len3));

      return true;
    }
  }

  return false;
}

bool pbvh_bmesh_node_nearest_to_ray(PBVH *pbvh,
                                    PBVHNode *node,
                                    const float ray_start[3],
                                    const float ray_normal[3],
                                    float *depth,
                                    float *dist_sq,
                                    bool use_original,
                                    int stroke_id)
{
  bool hit = false;

  BKE_pbvh_bmesh_check_tris(pbvh, node);
  PBVHTriBuf *tribuf = node->tribuf;
  const int cd_dyn_vert = pbvh->cd_dyn_vert;

  for (int i = 0; i < tribuf->tottri; i++) {
    PBVHTri *tri = tribuf->tris + i;
    BMFace *f = (BMFace *)tri->f.i;

    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      continue;
    }

    BMVert *v1 = (BMVert *)tribuf->verts[tri->v[0]].i;
    BMVert *v2 = (BMVert *)tribuf->verts[tri->v[1]].i;
    BMVert *v3 = (BMVert *)tribuf->verts[tri->v[2]].i;

    float *co1, *co2, *co3;

    if (use_original) {
      BKE_pbvh_bmesh_check_origdata(pbvh, v1, stroke_id);
      BKE_pbvh_bmesh_check_origdata(pbvh, v2, stroke_id);
      BKE_pbvh_bmesh_check_origdata(pbvh, v3, stroke_id);

      co1 = BKE_PBVH_DYNVERT(cd_dyn_vert, v1)->origco;
      co2 = BKE_PBVH_DYNVERT(cd_dyn_vert, v2)->origco;
      co3 = BKE_PBVH_DYNVERT(cd_dyn_vert, v3)->origco;
    }
    else {
      co1 = v1->co;
      co2 = v2->co;
      co3 = v3->co;
    }

    hit |= ray_face_nearest_tri(ray_start, ray_normal, co1, co2, co3, depth, dist_sq);
  }

  return hit;
}

typedef struct UpdateNormalsTaskData {
  PBVHNode **nodes;
  int totnode;
} UpdateNormalsTaskData;

static void pbvh_update_normals_task_cb(void *__restrict userdata,
                                        const int n,
                                        const TaskParallelTLS *__restrict UNUSED(tls))
{
  BMVert *v;
  BMFace *f;
  UpdateNormalsTaskData *data = (UpdateNormalsTaskData *)userdata;
  PBVHNode *node = data->nodes[n];

  node->flag |= PBVH_UpdateCurvatureDir;

  TGSET_ITER (f, node->bm_faces) {
    BM_face_normal_update(f);
  }
  TGSET_ITER_END

  TGSET_ITER (v, node->bm_unique_verts) {
    BM_vert_normal_update(v);
  }
  TGSET_ITER_END

  node->flag &= ~PBVH_UpdateNormals;
}

void pbvh_bmesh_normals_update(PBVHNode **nodes, int totnode)
{
  TaskParallelSettings settings;
  UpdateNormalsTaskData data = {nodes, totnode};

  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, pbvh_update_normals_task_cb, &settings);

#if 0  // in theory we shouldn't need to update normals in bm_other_verts.
  for (int i=0; i<totnode; i++) {
    PBVHNode *node = nodes[i];

    TGSET_ITER (v, node->bm_other_verts) {
      BM_vert_normal_update(v);
    }
    TGSET_ITER_END
  }
#endif
}

void pbvh_bmesh_normals_update_old(PBVHNode **nodes, int totnode)
{
  for (int n = 0; n < totnode; n++) {
    PBVHNode *node = nodes[n];

    if (node->flag & PBVH_UpdateNormals) {
      BMVert *v;
      BMFace *f;

      TGSET_ITER (f, node->bm_faces) {
        BM_face_normal_update(f);
      }
      TGSET_ITER_END

      TGSET_ITER (v, node->bm_unique_verts) {
        BM_vert_normal_update(v);
      }
      TGSET_ITER_END

      /* This should be unneeded normally */
      TGSET_ITER (v, node->bm_other_verts) {
        BM_vert_normal_update(v);
      }
      TGSET_ITER_END

      node->flag &= ~PBVH_UpdateNormals;
    }
  }
}

struct FastNodeBuildInfo {
  int totface; /* number of faces */
  int start;   /* start of faces in array */
  struct FastNodeBuildInfo *child1;
  struct FastNodeBuildInfo *child2;
};

/**
 * Recursively split the node if it exceeds the leaf_limit.
 * This function is multi-thread-able since each invocation applies
 * to a sub part of the arrays.
 */
static void pbvh_bmesh_node_limit_ensure_fast(
    PBVH *pbvh, BMFace **nodeinfo, BBC *bbc_array, struct FastNodeBuildInfo *node, MemArena *arena)
{
  struct FastNodeBuildInfo *child1, *child2;

  if (node->totface <= pbvh->leaf_limit) {
    return;
  }

  /* Calculate bounding box around primitive centroids */
  BB cb;
  BB_reset(&cb);
  for (int i = 0; i < node->totface; i++) {
    BMFace *f = nodeinfo[i + node->start];
    BBC *bbc = &bbc_array[BM_elem_index_get(f)];

    BB_expand(&cb, bbc->bcentroid);
  }

  /* initialize the children */

  /* Find widest axis and its midpoint */
  const int axis = BB_widest_axis(&cb);
  const float mid = (cb.bmax[axis] + cb.bmin[axis]) * 0.5f;

  int num_child1 = 0, num_child2 = 0;

  /* split vertices along the middle line */
  const int end = node->start + node->totface;
  for (int i = node->start; i < end - num_child2; i++) {
    BMFace *f = nodeinfo[i];
    BBC *bbc = &bbc_array[BM_elem_index_get(f)];

    if (bbc->bcentroid[axis] > mid) {
      int i_iter = end - num_child2 - 1;
      int candidate = -1;
      /* found a face that should be part of another node, look for a face to substitute with */

      for (; i_iter > i; i_iter--) {
        BMFace *f_iter = nodeinfo[i_iter];
        const BBC *bbc_iter = &bbc_array[BM_elem_index_get(f_iter)];
        if (bbc_iter->bcentroid[axis] <= mid) {
          candidate = i_iter;
          break;
        }

        num_child2++;
      }

      if (candidate != -1) {
        BMFace *tmp = nodeinfo[i];
        nodeinfo[i] = nodeinfo[candidate];
        nodeinfo[candidate] = tmp;
        /* increase both counts */
        num_child1++;
        num_child2++;
      }
      else {
        /* not finding candidate means second half of array part is full of
         * second node parts, just increase the number of child nodes for it */
        num_child2++;
      }
    }
    else {
      num_child1++;
    }
  }

  /* ensure at least one child in each node */
  if (num_child2 == 0) {
    num_child2++;
    num_child1--;
  }
  else if (num_child1 == 0) {
    num_child1++;
    num_child2--;
  }

  /* at this point, faces should have been split along the array range sequentially,
   * each sequential part belonging to one node only */
  BLI_assert((num_child1 + num_child2) == node->totface);

  node->child1 = child1 = BLI_memarena_alloc(arena, sizeof(struct FastNodeBuildInfo));
  node->child2 = child2 = BLI_memarena_alloc(arena, sizeof(struct FastNodeBuildInfo));

  child1->totface = num_child1;
  child1->start = node->start;
  child2->totface = num_child2;
  child2->start = node->start + num_child1;
  child1->child1 = child1->child2 = child2->child1 = child2->child2 = NULL;

  pbvh_bmesh_node_limit_ensure_fast(pbvh, nodeinfo, bbc_array, child1, arena);
  pbvh_bmesh_node_limit_ensure_fast(pbvh, nodeinfo, bbc_array, child2, arena);
}

static void pbvh_bmesh_create_nodes_fast_recursive(
    PBVH *pbvh, BMFace **nodeinfo, BBC *bbc_array, struct FastNodeBuildInfo *node, int node_index)
{
  PBVHNode *n = pbvh->nodes + node_index;
  /* two cases, node does not have children or does have children */
  if (node->child1) {
    int children_offset = pbvh->totnode;

    n->children_offset = children_offset;
    pbvh_grow_nodes(pbvh, pbvh->totnode + 2);
    pbvh_bmesh_create_nodes_fast_recursive(
        pbvh, nodeinfo, bbc_array, node->child1, children_offset);
    pbvh_bmesh_create_nodes_fast_recursive(
        pbvh, nodeinfo, bbc_array, node->child2, children_offset + 1);

    n = &pbvh->nodes[node_index];

    /* Update bounding box */
    BB_reset(&n->vb);
    BB_expand_with_bb(&n->vb, &pbvh->nodes[n->children_offset].vb);
    BB_expand_with_bb(&n->vb, &pbvh->nodes[n->children_offset + 1].vb);
    n->orig_vb = n->vb;
  }
  else {
    /* node does not have children so it's a leaf node, populate with faces and tag accordingly
     * this is an expensive part but it's not so easily thread-able due to vertex node indices */
    const int cd_vert_node_offset = pbvh->cd_vert_node_offset;
    const int cd_face_node_offset = pbvh->cd_face_node_offset;

    bool has_visible = false;

    n->flag = PBVH_Leaf | PBVH_UpdateTris;
    n->bm_faces = BLI_table_gset_new_ex("bm_faces", node->totface);

    /* Create vert hash sets */
    n->bm_unique_verts = BLI_table_gset_new("bm_unique_verts");
    n->bm_other_verts = BLI_table_gset_new("bm_other_verts");

    BB_reset(&n->vb);

    const int end = node->start + node->totface;

    for (int i = node->start; i < end; i++) {
      BMFace *f = nodeinfo[i];
      BBC *bbc = &bbc_array[BM_elem_index_get(f)];

      /* Update ownership of faces */
      BLI_table_gset_insert(n->bm_faces, f);
      BM_ELEM_CD_SET_INT(f, cd_face_node_offset, node_index);

      /* Update vertices */
      BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
      BMLoop *l_iter = l_first;
      do {
        BMVert *v = l_iter->v;
        if (!BLI_table_gset_haskey(n->bm_unique_verts, v)) {
          if (BM_ELEM_CD_GET_INT(v, cd_vert_node_offset) != DYNTOPO_NODE_NONE) {
            BLI_table_gset_add(n->bm_other_verts, v);
          }
          else {
            BLI_table_gset_insert(n->bm_unique_verts, v);
            BM_ELEM_CD_SET_INT(v, cd_vert_node_offset, node_index);
          }
        }
        /* Update node bounding box */
      } while ((l_iter = l_iter->next) != l_first);

      if (!BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        has_visible = true;
      }

      BB_expand_with_bb(&n->vb, (BB *)bbc);
    }

    BLI_assert(n->vb.bmin[0] <= n->vb.bmax[0] && n->vb.bmin[1] <= n->vb.bmax[1] &&
               n->vb.bmin[2] <= n->vb.bmax[2]);

    n->orig_vb = n->vb;

    /* Build GPU buffers for new node and update vertex normals */
    BKE_pbvh_node_mark_rebuild_draw(n);

    BKE_pbvh_node_fully_hidden_set(n, !has_visible);
    n->flag |= PBVH_UpdateNormals | PBVH_UpdateCurvatureDir;
  }
}

/***************************** Public API *****************************/

/*Used by symmetrize to update boundary flags*/
void BKE_pbvh_recalc_bmesh_boundary(PBVH *pbvh)
{
  BMVert *v;
  BMIter iter;

  BM_ITER_MESH (v, &iter, pbvh->bm, BM_VERTS_OF_MESH) {
    MDynTopoVert *mv = BKE_PBVH_DYNVERT(pbvh->cd_dyn_vert, v);

    if (BM_vert_is_boundary(v)) {
      mv->flag |= DYNVERT_BOUNDARY;
    }
    else {
      mv->flag &= ~DYNVERT_BOUNDARY;
    }
  }
}

/* Build a PBVH from a BMesh */
void BKE_pbvh_build_bmesh(PBVH *pbvh,
                          BMesh *bm,
                          bool smooth_shading,
                          BMLog *log,
                          const int cd_vert_node_offset,
                          const int cd_face_node_offset,
                          const int cd_dyn_vert)
{
  pbvh->cd_vert_node_offset = cd_vert_node_offset;
  pbvh->cd_face_node_offset = cd_face_node_offset;
  pbvh->cd_vert_mask_offset = CustomData_get_offset(&bm->vdata, CD_PAINT_MASK);
  pbvh->cd_dyn_vert = cd_dyn_vert;

  pbvh->bm = bm;

  BKE_pbvh_bmesh_detail_size_set(pbvh, 0.75f, 0.4f);

  pbvh->type = PBVH_BMESH;
  pbvh->bm_log = log;
  pbvh->cd_vcol_offset = CustomData_get_offset(&bm->vdata, CD_PROP_COLOR);
  pbvh->cd_faceset_offset = CustomData_get_offset(&bm->pdata, CD_SCULPT_FACE_SETS);

  /* TODO: choose leaf limit better */
  pbvh->leaf_limit = 1000;

  BMIter iter;
  BMVert *v;

  int cd_vcol_offset = CustomData_get_offset(&bm->vdata, CD_PROP_COLOR);

  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    MDynTopoVert *mv = BKE_PBVH_DYNVERT(cd_dyn_vert, v);

    mv->flag = 0;

    if (BM_vert_is_boundary(v)) {
      mv->flag |= DYNVERT_BOUNDARY;
    }

    copy_v3_v3(mv->origco, v->co);
    copy_v3_v3(mv->origno, v->no);

    if (cd_vcol_offset >= 0) {
      MPropCol *c1 = BM_ELEM_CD_GET_VOID_P(v, cd_vcol_offset);
      copy_v4_v4(mv->origcolor, c1->color);
    }
    else {
      zero_v4(mv->origcolor);
    }
  }
  if (smooth_shading) {
    pbvh->flags |= PBVH_DYNTOPO_SMOOTH_SHADING;
  }

  /* bounding box array of all faces, no need to recalculate every time */
  BBC *bbc_array = MEM_mallocN(sizeof(BBC) * bm->totface, "BBC");
  BMFace **nodeinfo = MEM_mallocN(sizeof(*nodeinfo) * bm->totface, "nodeinfo");
  MemArena *arena = BLI_memarena_new(BLI_MEMARENA_STD_BUFSIZE, "fast PBVH node storage");

  BMFace *f;
  int i;
  BM_ITER_MESH_INDEX (f, &iter, bm, BM_FACES_OF_MESH, i) {
    BBC *bbc = &bbc_array[i];
    BMLoop *l_first = BM_FACE_FIRST_LOOP(f);
    BMLoop *l_iter = l_first;

    BB_reset((BB *)bbc);
    do {
      BB_expand((BB *)bbc, l_iter->v->co);
    } while ((l_iter = l_iter->next) != l_first);
    BBC_update_centroid(bbc);

    /* so we can do direct lookups on 'bbc_array' */
    BM_elem_index_set(f, i); /* set_dirty! */
    nodeinfo[i] = f;
    BM_ELEM_CD_SET_INT(f, cd_face_node_offset, DYNTOPO_NODE_NONE);
  }
  /* Likely this is already dirty. */
  bm->elem_index_dirty |= BM_FACE;

  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    BM_ELEM_CD_SET_INT(v, cd_vert_node_offset, DYNTOPO_NODE_NONE);
  }

  /* setup root node */
  struct FastNodeBuildInfo rootnode = {0};
  rootnode.totface = bm->totface;

  /* start recursion, assign faces to nodes accordingly */
  pbvh_bmesh_node_limit_ensure_fast(pbvh, nodeinfo, bbc_array, &rootnode, arena);

  /* We now have all faces assigned to a node,
   * next we need to assign those to the gsets of the nodes. */

  /* Start with all faces in the root node */
  pbvh->nodes = MEM_callocN(sizeof(PBVHNode), "PBVHNode");
  pbvh->totnode = 1;

  /* take root node and visit and populate children recursively */
  pbvh_bmesh_create_nodes_fast_recursive(pbvh, nodeinfo, bbc_array, &rootnode, 0);

  BLI_memarena_free(arena);
  MEM_freeN(bbc_array);
  MEM_freeN(nodeinfo);
}

static double last_update_time[128] = {
    0,
};

bool BKE_pbvh_bmesh_update_topology_nodes(PBVH *pbvh,
                                          bool (*searchcb)(PBVHNode *node, void *data),
                                          void (*undopush)(PBVHNode *node, void *data),
                                          void *searchdata,
                                          PBVHTopologyUpdateMode mode,
                                          const float center[3],
                                          const float view_normal[3],
                                          float radius,
                                          const bool use_frontface,
                                          const bool use_projected,
                                          int sym_axis,
                                          bool updatePBVH)
{
  bool modified = false;

  for (int i = 0; i < pbvh->totnode; i++) {
    PBVHNode *node = pbvh->nodes + i;

    if (!(node->flag & PBVH_Leaf) || !searchcb(node, searchdata)) {
      continue;
    }

    if (node->flag & PBVH_Leaf) {
      node->flag |= PBVH_UpdateCurvatureDir;
      undopush(node, searchdata);

      BKE_pbvh_node_mark_topology_update(pbvh->nodes + i);
    }
  }

  modified = modified || BKE_pbvh_bmesh_update_topology(pbvh,
                                                        mode,
                                                        center,
                                                        view_normal,
                                                        radius,
                                                        use_frontface,
                                                        use_projected,
                                                        sym_axis,
                                                        updatePBVH);
  return modified;
}

static bool cleanup_valence_3_4(PBVH *pbvh,
                                const float center[3],
                                const float view_normal[3],
                                float radius,
                                const bool use_frontface,
                                const bool use_projected)
{
  bool modified = false;
  BMVert **relink_verts = NULL;
  BLI_array_staticdeclare(relink_verts, 1024);

  float radius2 = radius * 1.25;
  float rsqr = radius2 * radius2;

  for (int n = 0; n < pbvh->totnode; n++) {
    PBVHNode *node = pbvh->nodes + n;

    /* Check leaf nodes marked for topology update */
    bool ok = (node->flag & PBVH_Leaf) && (node->flag & PBVH_UpdateTopology);
    ok = ok && !(node->flag & PBVH_FullyHidden);

    if (!ok) {
      continue;
    }

    PBVHVertexIter vi;
    GSetIterator gi;
    BMVert *v;

    TGSET_ITER (v, node->bm_unique_verts) {
      if (len_squared_v3v3(v->co, center) >= rsqr) {
        continue;
      }

      const int val = BM_vert_edge_count(v);
      if (val < 3 || val > 4) {
        continue;
      }

      BMIter iter;
      BMLoop *l;
      BMLoop *ls[4];
      BMVert *vs[4];
      BMEdge *es[4];

      l = v->e->l;

      if (!l) {
        continue;
      }

      if (l->v != v) {
        l = l->next;
      }

      bool bad = false;
      int i = 0;

      for (int j = 0; j < val; j++) {
        ls[i++] = l->v == v ? l->next : l;

        l = l->prev->radial_next;

        if (l->v != v) {
          l = l->next;
        }

        if (l->radial_next == l || l->radial_next->radial_next != l) {
          bad = true;
          break;
        }

        for (int k = 0; k < j; k++) {
          if (ls[k]->v == ls[j]->v) {
            if (ls[j]->next->v != v) {
              ls[j] = ls[j]->next;
            }
            else {
              bad = true;
              break;
            }
          }

          if (ls[k]->f == ls[j]->f) {
            bad = true;
            break;
          }
        }
      }

      if (bad) {
        continue;
      }

      pbvh_bmesh_vert_remove(pbvh, v);
      BM_log_vert_removed(pbvh->bm_log, v, pbvh->cd_vert_mask_offset);

      BLI_array_clear(relink_verts);

      BMFace *f;
      BM_ITER_ELEM (f, &iter, v, BM_FACES_OF_VERT) {
        int ni2 = BM_ELEM_CD_GET_INT(f, pbvh->cd_face_node_offset);
        if (ni2 != DYNTOPO_NODE_NONE) {
          PBVHNode *node2 = pbvh->nodes + ni2;

          BLI_table_gset_remove(node2->bm_unique_verts, v, NULL);
          BLI_table_gset_remove(node2->bm_other_verts, v, NULL);

          pbvh_bmesh_face_remove(pbvh, f);
        }
      }

      modified = true;

      l = v->e->l;

      vs[0] = ls[0]->v;
      vs[1] = ls[1]->v;
      vs[2] = ls[2]->v;

      BMFace *f1 = NULL;
      if (vs[0] != vs[1] && vs[1] != vs[2] && vs[0] != vs[2]) {
        f1 = pbvh_bmesh_face_create(pbvh, n, vs, NULL, l->f, false, false);
      }

      if (val == 4 && vs[0] != vs[2] && vs[2] != vs[3] && vs[0] != vs[3]) {
        vs[0] = ls[0]->v;
        vs[1] = ls[2]->v;
        vs[2] = ls[3]->v;

        BMFace *f2 = pbvh_bmesh_face_create(pbvh, n, vs, NULL, v->e->l->f, false, false);
        SWAP(void *, f2->l_first->prev->head.data, ls[3]->head.data);

        CustomData_bmesh_copy_data(
            &pbvh->bm->ldata, &pbvh->bm->ldata, ls[0]->head.data, &f2->l_first->head.data);
        CustomData_bmesh_copy_data(
            &pbvh->bm->ldata, &pbvh->bm->ldata, ls[2]->head.data, &f2->l_first->next->head.data);

        BM_log_face_added(pbvh->bm_log, f2);
      }

      if (f1) {
        SWAP(void *, f1->l_first->head.data, ls[0]->head.data);
        SWAP(void *, f1->l_first->next->head.data, ls[1]->head.data);
        SWAP(void *, f1->l_first->prev->head.data, ls[2]->head.data);

        BM_log_face_added(pbvh->bm_log, f1);
      }

      BM_vert_kill(pbvh->bm, v);
#if 0
      for (int j = 0; j < pbvh->totnode; j++) {
        PBVHNode *node2 = pbvh->nodes + j;

        if (!node2->bm_unique_verts || !node2->bm_other_verts) {  //(node2->flag & PBVH_Leaf)) {
          continue;
        }

        BLI_table_gset_remove(node2->bm_unique_verts, v, NULL);
        BLI_table_gset_remove(node2->bm_other_verts, v, NULL);
      }
#endif
    }
    TGSET_ITER_END
  }

  BLI_array_free(relink_verts);

  if (modified) {
    pbvh->bm->elem_index_dirty |= BM_VERT | BM_FACE | BM_EDGE;
    pbvh->bm->elem_table_dirty |= BM_VERT | BM_FACE | BM_EDGE;
  }

  return modified;
}

/* Collapse short edges, subdivide long edges */
bool BKE_pbvh_bmesh_update_topology(PBVH *pbvh,
                                    PBVHTopologyUpdateMode mode,
                                    const float center[3],
                                    const float view_normal[3],
                                    float radius,
                                    const bool use_frontface,
                                    const bool use_projected,
                                    int sym_axis,
                                    bool updatePBVH)
{
  /*
  if (sym_axis >= 0 &&
      PIL_check_seconds_timer() - last_update_time[sym_axis] < DYNTOPO_RUN_INTERVAL) {
    return false;
  }

  if (sym_axis >= 0) {
    last_update_time[sym_axis] = PIL_check_seconds_timer();
  }*/

  /* 2 is enough for edge faces - manifold edge */
  BLI_buffer_declare_static(BMLoop *, edge_loops, BLI_BUFFER_NOP, 2);
  BLI_buffer_declare_static(BMFace *, deleted_faces, BLI_BUFFER_NOP, 32);
  const int cd_vert_mask_offset = CustomData_get_offset(&pbvh->bm->vdata, CD_PAINT_MASK);
  const int cd_vert_node_offset = pbvh->cd_vert_node_offset;
  const int cd_face_node_offset = pbvh->cd_face_node_offset;
  const int cd_dyn_vert = pbvh->cd_dyn_vert;
  float ratio = 1.0f;

  bool modified = false;

  if (view_normal) {
    BLI_assert(len_squared_v3(view_normal) != 0.0f);
  }

  EdgeQueueContext eq_ctx = {
      NULL,
      NULL,
      pbvh->bm,

      cd_dyn_vert,
      cd_vert_mask_offset,
      cd_vert_node_offset,
      cd_face_node_offset,
      .avg_elen = 0.0f,
      .max_elen = -1e17,
      .min_elen = 1e17,
      .totedge = 0.0f,
  };

#if 1
  if (mode & PBVH_Collapse) {
    EdgeQueue q;
    BLI_mempool *queue_pool = BLI_mempool_create(sizeof(BMVert *) * 2, 0, 128, BLI_MEMPOOL_NOP);

    eq_ctx.q = &q;
    eq_ctx.pool = queue_pool;

    short_edge_queue_create(
        &eq_ctx, pbvh, center, view_normal, radius, use_frontface, use_projected);

#  ifdef SKINNY_EDGE_FIX
    // prevent remesher thrashing by throttling edge splitting in pathological case of skinny edges
    float avg_elen = eq_ctx.avg_elen;
    if (eq_ctx.totedge > 0.0f) {
      avg_elen /= eq_ctx.totedge;

      float emax = eq_ctx.max_elen;
      if (emax == 0.0f) {
        emax = 0.0001f;
      }

      if (pbvh->bm_min_edge_len > 0.0f && avg_elen > 0.0f) {
        ratio = avg_elen / (pbvh->bm_min_edge_len * 0.5 + emax * 0.5);
        ratio = MAX2(ratio, 0.25f);
        ratio = MIN2(ratio, 5.0f);
      }
    }
#  endif

    int max_steps = (int)((float)DYNTOPO_MAX_ITER * ratio);

    modified |= pbvh_bmesh_collapse_short_edges(&eq_ctx, pbvh, &deleted_faces, max_steps);

    BLI_heapsimple_free(q.heap, NULL);
    if (q.elems) {
      MEM_freeN(q.elems);
    }
    BLI_mempool_destroy(queue_pool);
  }

  if (mode & PBVH_Subdivide) {
    EdgeQueue q;
    BLI_mempool *queue_pool = BLI_mempool_create(sizeof(BMVert *) * 2, 0, 128, BLI_MEMPOOL_NOP);

    eq_ctx.q = &q;
    eq_ctx.pool = queue_pool;

    long_edge_queue_create(
        &eq_ctx, pbvh, center, view_normal, radius, use_frontface, use_projected);

#  ifdef SKINNY_EDGE_FIX
    // prevent remesher thrashing by throttling edge splitting in pathological case of skinny edges
    float avg_elen = eq_ctx.avg_elen;
    if (eq_ctx.totedge > 0.0f) {
      avg_elen /= eq_ctx.totedge;

      float emin = eq_ctx.min_elen;
      if (emin == 0.0f) {
        emin = 0.0001f;
      }

      if (avg_elen > 0.0f) {
        ratio = (pbvh->bm_max_edge_len * 0.5 + emin * 0.5) / avg_elen;
        ratio = MAX2(ratio, 0.05f);
        ratio = MIN2(ratio, 1.0f);
      }
    }
#  endif

    int max_steps = (int)((float)DYNTOPO_MAX_ITER * ratio);

    modified |= pbvh_bmesh_subdivide_long_edges(&eq_ctx, pbvh, &edge_loops, max_steps);
    if (q.elems) {
      MEM_freeN(q.elems);
    }
    BLI_heapsimple_free(q.heap, NULL);
    BLI_mempool_destroy(queue_pool);
  }

#endif
  if (mode & PBVH_Cleanup) {
    modified |= cleanup_valence_3_4(
        pbvh, center, view_normal, radius, use_frontface, use_projected);
  }

  if (modified) {

#ifdef PROXY_ADVANCED
    for (int i = 0; i < pbvh->totnode; i++) {
      PBVHNode *node = pbvh->nodes + i;

      // ensure proxyvert arrays are rebuilt
      if (node->flag & PBVH_Leaf) {
        BKE_pbvh_free_proxyarray(pbvh, node);
      }
    }
#endif

    // avoid potential infinite loops
    const int totnode = pbvh->totnode;

    for (int i = 0; i < totnode; i++) {
      PBVHNode *node = pbvh->nodes + i;

      if ((node->flag & PBVH_Leaf) && (node->flag & PBVH_UpdateTopology) &&
          !(node->flag & PBVH_FullyHidden)) {

        node->flag &= ~PBVH_UpdateTopology;

        /* Recursively split nodes that have gotten too many
         * elements */
        if (updatePBVH) {
          pbvh_bmesh_node_limit_ensure(pbvh, i);
        }
      }
    }
  }
  else {  // still unmark nodes
    for (int i = 0; i < pbvh->totnode; i++) {
      PBVHNode *node = pbvh->nodes + i;

      if ((node->flag & PBVH_Leaf) && (node->flag & PBVH_UpdateTopology)) {
        node->flag &= ~PBVH_UpdateTopology;
      }
    }
  }

  BLI_buffer_free(&edge_loops);
  BLI_buffer_free(&deleted_faces);

#ifdef USE_VERIFY
  pbvh_bmesh_verify(pbvh);
#endif

  return modified;
}

PBVHTriBuf *BKE_pbvh_bmesh_get_tris(PBVH *pbvh, PBVHNode *node)
{
  BKE_pbvh_bmesh_check_tris(pbvh, node);

  return node->tribuf;
}

void BKE_pbvh_bmesh_free_tris(PBVH *pbvh, PBVHNode *node)
{
  if (node->tribuf) {
    MEM_SAFE_FREE(node->tribuf->verts);
    MEM_SAFE_FREE(node->tribuf->tris);
    MEM_SAFE_FREE(node->tribuf->loops);
    MEM_freeN(node->tribuf);
    node->tribuf = NULL;
  }
}

/*
generate triangle buffers with split uv islands.
currently unused (and untested).
*/
static bool pbvh_bmesh_split_tris(PBVH *pbvh, PBVHNode *node)
{
  BMFace *f;

  BM_mesh_elem_index_ensure(pbvh->bm, BM_VERT | BM_FACE);

  // split by uvs
  int layeri = CustomData_get_layer_index(&pbvh->bm->ldata, CD_MLOOPUV);
  if (layeri < 0) {
    return false;
  }

  int totlayer = 0;

  while (layeri < pbvh->bm->ldata.totlayer && pbvh->bm->ldata.layers[layeri].type == CD_MLOOPUV) {
    totlayer++;
    layeri++;
  }

  const int cd_uv = pbvh->bm->ldata.layers[layeri].offset;
  const int cd_size = CustomData_sizeof(CD_MLOOPUV);

  SculptVertRef *verts = NULL;
  PBVHTri *tris = NULL;
  intptr_t *loops = NULL;

  BLI_array_declare(verts);
  BLI_array_declare(tris);
  BLI_array_declare(loops);

  TGSET_ITER (f, node->bm_faces) {
    BMLoop *l = f->l_first;

    do {
      l->head.index = -1;
      l = l->next;
    } while (l != f->l_first);
  }
  TGSET_ITER_END

  int vi = 0;

  TGSET_ITER (f, node->bm_faces) {
    BMLoop *l = f->l_first;

    do {
      if (l->head.index >= 0) {
        continue;
      }

      l->head.index = vi++;
      BLI_array_append(loops, (intptr_t)l);

      SculptVertRef sv = {(intptr_t)l->v};
      BLI_array_append(verts, sv);

      BMIter iter;
      BMLoop *l2;

      BM_ITER_ELEM (l2, &iter, l, BM_LOOPS_OF_VERT) {
        bool ok = true;

        for (int i = 0; i < totlayer; i++) {
          MLoopUV *uv1 = BM_ELEM_CD_GET_VOID_P(l, cd_uv + cd_size * i);
          MLoopUV *uv2 = BM_ELEM_CD_GET_VOID_P(l2, cd_uv + cd_size * i);

          if (len_v3v3(uv1->uv, uv2->uv) > 0.001) {
            ok = false;
            break;
          }
        }

        if (ok) {
          l2->head.index = l->head.index;
        }
      }
    } while (l != f->l_first);
  }
  TGSET_ITER_END

  TGSET_ITER (f, node->bm_faces) {
    BMLoop *l1 = f->l_first, *l2 = f->l_first->next, *l3 = f->l_first->prev;

    PBVHTri tri;
    tri.f.i = (intptr_t)f;

    tri.v[0] = l1->head.index;
    tri.v[1] = l2->head.index;
    tri.v[2] = l3->head.index;

    copy_v3_v3(tri.no, f->no);
    BLI_array_append(tris, tri);
  }
  TGSET_ITER_END

  if (node->tribuf) {
    MEM_SAFE_FREE(node->tribuf->verts);
    MEM_SAFE_FREE(node->tribuf->tris);
    MEM_SAFE_FREE(node->tribuf->loops);

    node->tribuf->tottri = 0;
    node->tribuf->tris = NULL;
  }
  else {
    node->tribuf = MEM_callocN(sizeof(*node->tribuf), "node->tribuf");
  }

  node->tribuf->verts = verts;
  node->tribuf->loops = loops;
  node->tribuf->tris = tris;

  node->tribuf->tottri = BLI_array_len(tris);
  node->tribuf->totvert = BLI_array_len(verts);
  node->tribuf->totloop = BLI_array_len(loops);

  return true;
}
/* In order to perform operations on the original node coordinates
 * (currently just raycast), store the node's triangles and vertices.
 *
 * Skips triangles that are hidden. */
void BKE_pbvh_bmesh_check_tris(PBVH *pbvh, PBVHNode *node)
{
  BMesh *bm = pbvh->bm;

  if (!(node->flag & PBVH_UpdateTris) && node->tribuf) {
    return;
  }

  if (node->tribuf) {
    MEM_SAFE_FREE(node->tribuf->verts);
    MEM_SAFE_FREE(node->tribuf->tris);
    MEM_SAFE_FREE(node->tribuf->loops);

    node->tribuf->tottri = 0;
    node->tribuf->totvert = 0;
    node->tribuf->totloop = 0;
  }
  else {
    node->tribuf = MEM_callocN(sizeof(*node->tribuf), "node->tribuf");
    node->tribuf->loops = NULL;
    node->tribuf->totloop = 0;
  }

  node->flag &= ~PBVH_UpdateTris;
  PBVHTri *tris = NULL;
  SculptVertRef *verts = NULL;

  BLI_array_declare(tris);
  BLI_array_declare(verts);

  GHash *vmap = BLI_ghash_ptr_new("pbvh_bmesh.c vmap");
  BMFace *f;

  TGSET_ITER (f, node->bm_faces) {
    BMVert *v1 = f->l_first->v;
    BMVert *v2 = f->l_first->next->v;
    BMVert *v3 = f->l_first->prev->v;

    PBVHTri tri = {0};

    BMLoop *l = f->l_first;
    int j = 0;

    do {
      void **val = NULL;

      if (!BLI_ghash_ensure_p(vmap, l->v, &val)) {
        SculptVertRef sv = {(intptr_t)l->v};

        *val = (void *)BLI_array_len(verts);
        BLI_array_append(verts, sv);
      }

      tri.v[j] = (intptr_t)val[0];

      j++;

      if (j >= 3) {
        break;
      }

      l = l->next;
    } while (l != f->l_first);

    copy_v3_v3(tri.no, f->no);
    tri.f.i = (intptr_t)f;

    BLI_array_append(tris, tri);
  }
  TGSET_ITER_END

  bm->elem_index_dirty | BM_VERT;

  node->tribuf->tris = tris;
  node->tribuf->tottri = BLI_array_len(tris);
  node->tribuf->verts = verts;
  node->tribuf->totvert = BLI_array_len(verts);

  BLI_ghash_free(vmap, NULL, NULL);
}

static int pbvh_count_subtree_verts(PBVH *pbvh, PBVHNode *n)
{
  if (n->flag & PBVH_Leaf) {
    n->subtree_tottri = BLI_table_gset_len(
        n->bm_faces);  // n->tm_unique_verts->length + n->tm_other_verts->length;
    return n->subtree_tottri;
  }

  int ni = n->children_offset;

  int ret = pbvh_count_subtree_verts(pbvh, pbvh->nodes + ni);
  ret += pbvh_count_subtree_verts(pbvh, pbvh->nodes + ni + 1);

  n->subtree_tottri = ret;

  return ret;
}

static void pbvh_bmesh_join_subnodes(PBVH *pbvh, PBVHNode *node, PBVHNode *parent)
{
  if (!(node->flag & PBVH_Leaf)) {
    int ni = node->children_offset;

    if (ni > 0 && ni < pbvh->totnode - 1) {
      pbvh_bmesh_join_subnodes(pbvh, pbvh->nodes + ni, parent);
      pbvh_bmesh_join_subnodes(pbvh, pbvh->nodes + ni + 1, parent);
    }
    else {
      printf("node corruption: %d\n", ni);
      return;
    }
    if (node != parent) {
      node->flag |= PBVH_Delete;  // mark for deletion
    }

    return;
  }

  if (node != parent) {
    node->flag |= PBVH_Delete;  // mark for deletion
  }

  BMVert *v;

  TGSET_ITER (v, node->bm_unique_verts) {
    BLI_table_gset_add(parent->bm_unique_verts, v);

    BM_ELEM_CD_SET_INT(v, pbvh->cd_vert_node_offset, DYNTOPO_NODE_NONE);
  }
  TGSET_ITER_END

  // printf("  subtotface: %d\n", BLI_table_gset_len(node->bm_faces));

  BMFace *f;
  TGSET_ITER (f, node->bm_faces) {
    BLI_table_gset_add(parent->bm_faces, f);
    BM_ELEM_CD_SET_INT(f, pbvh->cd_face_node_offset, DYNTOPO_NODE_NONE);
  }
  TGSET_ITER_END
}

static void BKE_pbvh_bmesh_corect_tree(PBVH *pbvh, PBVHNode *node, PBVHNode *parent)
{
  const int size_lower = pbvh->leaf_limit - (pbvh->leaf_limit >> 1);
  const int size_higher = pbvh->leaf_limit + (pbvh->leaf_limit >> 1);

  if (node->flag & PBVH_Leaf) {
    // pbvh_trimesh_node_limit_ensure(pbvh, (int)(node - pbvh->nodes));
    return;

    // join nodes if subtree lacks verts, unless node is root
  }

  if (node->subtree_tottri < size_lower && node != pbvh->nodes) {
    node->bm_unique_verts = BLI_table_gset_new("bm_unique_verts");
    node->bm_other_verts = BLI_table_gset_new("bm_other_verts");
    node->bm_faces = BLI_table_gset_new("bm_faces");

    pbvh_bmesh_join_subnodes(pbvh, pbvh->nodes + node->children_offset, node);
    pbvh_bmesh_join_subnodes(pbvh, pbvh->nodes + node->children_offset + 1, node);

    node->children_offset = 0;
    node->flag |= PBVH_Leaf | PBVH_UpdateRedraw | PBVH_UpdateBB | PBVH_UpdateDrawBuffers |
                  PBVH_RebuildDrawBuffers | PBVH_UpdateOriginalBB | PBVH_UpdateMask |
                  PBVH_UpdateVisibility | PBVH_UpdateColor | PBVH_UpdateTopology |
                  PBVH_UpdateNormals | PBVH_UpdateTris;

    TableGSet *other = BLI_table_gset_new(__func__);
    BMVert *v;

    node->children_offset = 0;
    node->draw_buffers = NULL;

    // rebuild bm_other_verts
    BMFace *f;
    TGSET_ITER (f, node->bm_faces) {
      BMLoop *l = f->l_first;

      BM_ELEM_CD_SET_INT(f, pbvh->cd_face_node_offset, DYNTOPO_NODE_NONE);

      do {
        if (!BLI_table_gset_haskey(node->bm_unique_verts, l->v)) {
          BLI_table_gset_add(other, l->v);
        }
        l = l->next;
      } while (l != f->l_first);
    }
    TGSET_ITER_END

    BLI_table_gset_free(node->bm_other_verts, NULL);
    node->bm_other_verts = other;

    BB_reset(&node->vb);

#if 1
    TGSET_ITER (v, node->bm_unique_verts) {
      BB_expand(&node->vb, v->co);
    }
    TGSET_ITER_END

    TGSET_ITER (v, node->bm_other_verts) {
      BB_expand(&node->vb, v->co);
    }
    TGSET_ITER_END
#endif

    // printf("totface: %d\n", BLI_table_gset_len(node->bm_faces));
    node->orig_vb = node->vb;

    return;
  }

  int ni = node->children_offset;

  for (int i = 0; i < 2; i++, ni++) {
    PBVHNode *child = pbvh->nodes + ni;
    BKE_pbvh_bmesh_corect_tree(pbvh, child, node);
  }
}

static void pbvh_bmesh_join_nodes(PBVH *bvh)
{
  if (bvh->totnode < 2) {
    return;
  }

  pbvh_count_subtree_verts(bvh, bvh->nodes);
  BKE_pbvh_bmesh_corect_tree(bvh, bvh->nodes, NULL);

  // compact nodes
  int totnode = 0;
  for (int i = 0; i < bvh->totnode; i++) {
    PBVHNode *n = bvh->nodes + i;

    if (!(n->flag & PBVH_Delete)) {
      if (!(n->flag & PBVH_Leaf)) {
        PBVHNode *n1 = bvh->nodes + n->children_offset;
        PBVHNode *n2 = bvh->nodes + n->children_offset + 1;

        if ((n1->flag & PBVH_Delete) != (n2->flag & PBVH_Delete)) {
          printf("un-deleting an empty node\n");
          PBVHNode *n3 = n1->flag & PBVH_Delete ? n1 : n2;

          n3->flag = PBVH_Leaf | PBVH_UpdateTris;
          n3->bm_unique_verts = BLI_table_gset_new("bm_unique_verts");
          n3->bm_other_verts = BLI_table_gset_new("bm_other_verts");
          n3->bm_faces = BLI_table_gset_new("bm_faces");
          n3->tribuf = NULL;
        }
        else if ((n1->flag & PBVH_Delete) && (n2->flag & PBVH_Delete)) {
          n->children_offset = 0;
          n->flag |= PBVH_Leaf | PBVH_UpdateTris;

          if (!n->bm_unique_verts) {
            // should not happen
            n->bm_unique_verts = BLI_table_gset_new("bm_unique_verts");
            n->bm_other_verts = BLI_table_gset_new("bm_other_verts");
            n->bm_faces = BLI_table_gset_new("bm_faces");
            n->tribuf = NULL;
          }
        }
      }

      totnode++;
    }
  }

  int *map = MEM_callocN(sizeof(int) * bvh->totnode, "bmesh map temp");

  // build idx map for child offsets
  int j = 0;
  for (int i = 0; i < bvh->totnode; i++) {
    PBVHNode *n = bvh->nodes + i;

    if (!(n->flag & PBVH_Delete)) {
      map[i] = j++;
    }
    else if (1) {
      if (n->layer_disp) {
        MEM_freeN(n->layer_disp);
        n->layer_disp = NULL;
      }
      if (n->draw_buffers) {
        GPU_pbvh_buffers_free(n->draw_buffers);
        n->draw_buffers = NULL;
      }
      if (n->vert_indices) {
        MEM_freeN((void *)n->vert_indices);
        n->vert_indices = NULL;
      }
      if (n->face_vert_indices) {
        MEM_freeN((void *)n->face_vert_indices);
        n->face_vert_indices = NULL;
      }

      if (n->tribuf) {
        BKE_pbvh_bmesh_free_tris(bvh, n);
      }

      if (n->bm_unique_verts) {
        BLI_table_gset_free(n->bm_unique_verts, NULL);
        n->bm_unique_verts = NULL;
      }

      if (n->bm_other_verts) {
        BLI_table_gset_free(n->bm_other_verts, NULL);
        n->bm_other_verts = NULL;
      }

      if (n->bm_faces) {
        BLI_table_gset_free(n->bm_faces, NULL);
        n->bm_faces = NULL;
      }

#ifdef PROXY_ADVANCED
      BKE_pbvh_free_proxyarray(bvh, n);
#endif
    }
  }

  // compact node array
  j = 0;
  for (int i = 0; i < bvh->totnode; i++) {
    if (!(bvh->nodes[i].flag & PBVH_Delete)) {
      if (bvh->nodes[i].children_offset >= bvh->totnode - 1) {
        printf("error %i %i\n", i, bvh->nodes[i].children_offset);
        continue;
      }

      int i1 = map[bvh->nodes[i].children_offset];
      int i2 = map[bvh->nodes[i].children_offset + 1];

      if (bvh->nodes[i].children_offset >= bvh->totnode) {
        printf("bad child node reference %d->%d, totnode: %d\n",
               i,
               bvh->nodes[i].children_offset,
               bvh->totnode);
        continue;
      }

      if (bvh->nodes[i].children_offset && i2 != i1 + 1) {
        printf("      pbvh corruption during node join %d %d\n", i1, i2);
      }

      bvh->nodes[j] = bvh->nodes[i];
      bvh->nodes[j].children_offset = i1;

      j++;
    }
  }

  if (j != totnode) {
    printf("pbvh error: %s", __func__);
  }

  if (bvh->totnode != j) {
    memset(bvh->nodes + j, 0, sizeof(*bvh->nodes) * (bvh->totnode - j));
    bvh->node_mem_count = j;
  }

  bvh->totnode = j;

  BMVert *v;

  // set vert/face node indices again
  for (int i = 0; i < bvh->totnode; i++) {
    PBVHNode *n = bvh->nodes + i;

    if (!(n->flag & PBVH_Leaf)) {
      continue;
    }

    if (!n->bm_unique_verts) {
      printf("ERROR!\n");
      n->bm_unique_verts = BLI_table_gset_new("bleh");
      n->bm_other_verts = BLI_table_gset_new("bleh");
      n->bm_faces = BLI_table_gset_new("bleh");
    }

    BMVert *v;

    TGSET_ITER (v, n->bm_unique_verts) {
      BM_ELEM_CD_SET_INT(v, bvh->cd_vert_node_offset, i);
    }
    TGSET_ITER_END

    BMFace *f;

    TGSET_ITER (f, n->bm_faces) {
      BM_ELEM_CD_SET_INT(f, bvh->cd_face_node_offset, i);
    }
    TGSET_ITER_END
  }

  BMVert **scratch = NULL;
  BLI_array_declare(scratch);

  for (int i = 0; i < bvh->totnode; i++) {
    PBVHNode *n = bvh->nodes + i;

    if (!(n->flag & PBVH_Leaf)) {
      continue;
    }

    BLI_array_clear(scratch);
    BMVert *v;

    TGSET_ITER (v, n->bm_other_verts) {
      int ni = BM_ELEM_CD_GET_INT(v, bvh->cd_vert_node_offset);
      if (ni == DYNTOPO_NODE_NONE) {
        BLI_array_append(scratch, v);
      }
      // BM_ELEM_CD_SET_INT(v, bvh->cd_vert_node_offset, i);
    }
    TGSET_ITER_END

    int slen = BLI_array_len(scratch);
    for (int j = 0; j < slen; j++) {
      BMVert *v = scratch[j];

      BLI_table_gset_remove(n->bm_other_verts, v, NULL);
      BLI_table_gset_add(n->bm_unique_verts, v);
      BM_ELEM_CD_SET_INT(v, bvh->cd_vert_node_offset, i);
    }
  }

  BLI_array_free(scratch);
  MEM_freeN(map);
}

void BKE_pbvh_bmesh_after_stroke(PBVH *pbvh)
{
  check_heap();
  int totnode = pbvh->totnode;

  pbvh_bmesh_join_nodes(pbvh);

  check_heap();

  BKE_pbvh_update_bounds(pbvh, (PBVH_UpdateBB | PBVH_UpdateOriginalBB | PBVH_UpdateRedraw));

  totnode = pbvh->totnode;

  for (int i = 0; i < totnode; i++) {
    PBVHNode *n = pbvh->nodes + i;

    if (totnode != pbvh->totnode) {
#ifdef PROXY_ADVANCED
      BKE_pbvh_free_proxyarray(pbvh, n);
#endif
    }

    if (n->flag & PBVH_Leaf) {
      /* Recursively split nodes that have gotten too many
       * elements */
      pbvh_bmesh_node_limit_ensure(pbvh, i);
    }
  }
}

void BKE_pbvh_bmesh_detail_size_set(PBVH *pbvh, float detail_size, float detail_range)
{
  pbvh->bm_max_edge_len = detail_size;
  pbvh->bm_min_edge_len = pbvh->bm_max_edge_len * detail_range;
}

void BKE_pbvh_node_mark_topology_update(PBVHNode *node)
{
  node->flag |= PBVH_UpdateTopology;
}

TableGSet *BKE_pbvh_bmesh_node_unique_verts(PBVHNode *node)
{
  return node->bm_unique_verts;
}

TableGSet *BKE_pbvh_bmesh_node_other_verts(PBVHNode *node)
{
  return node->bm_other_verts;
}

struct TableGSet *BKE_pbvh_bmesh_node_faces(PBVHNode *node)
{
  return node->bm_faces;
}

/****************************** Debugging *****************************/

#if 0

static void pbvh_bmesh_print(PBVH *pbvh)
{
  fprintf(stderr, "\npbvh=%p\n", pbvh);
  fprintf(stderr, "bm_face_to_node:\n");

  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, pbvh->bm, BM_FACES_OF_MESH) {
    fprintf(stderr, "  %d -> %d\n", BM_elem_index_get(f), pbvh_bmesh_node_index_from_face(pbvh, f));
  }

  fprintf(stderr, "bm_vert_to_node:\n");
  BMVert *v;
  BM_ITER_MESH (v, &iter, pbvh->bm, BM_FACES_OF_MESH) {
    fprintf(stderr, "  %d -> %d\n", BM_elem_index_get(v), pbvh_bmesh_node_index_from_vert(pbvh, v));
  }

  for (int n = 0; n < pbvh->totnode; n++) {
    PBVHNode *node = &pbvh->nodes[n];
    if (!(node->flag & PBVH_Leaf)) {
      continue;
    }

    GSetIterator gs_iter;
    fprintf(stderr, "node %d\n  faces:\n", n);
    GSET_ITER (gs_iter, node->bm_faces)
      fprintf(stderr, "    %d\n", BM_elem_index_get((BMFace *)BLI_gsetIterator_getKey(&gs_iter)));
    fprintf(stderr, "  unique verts:\n");
    GSET_ITER (gs_iter, node->bm_unique_verts)
      fprintf(stderr, "    %d\n", BM_elem_index_get((BMVert *)BLI_gsetIterator_getKey(&gs_iter)));
    fprintf(stderr, "  other verts:\n");
    GSET_ITER (gs_iter, node->bm_other_verts)
      fprintf(stderr, "    %d\n", BM_elem_index_get((BMVert *)BLI_gsetIterator_getKey(&gs_iter)));
  }
}

static void print_flag_factors(int flag)
{
  printf("flag=0x%x:\n", flag);
  for (int i = 0; i < 32; i++) {
    if (flag & (1 << i)) {
      printf("  %d (1 << %d)\n", 1 << i, i);
    }
  }
}
#endif

#ifdef USE_VERIFY

static void pbvh_bmesh_verify(PBVH *pbvh)
{
  /* build list of faces & verts to lookup */
  GSet *faces_all = BLI_table_gset_new_ex(__func__, pbvh->bm->totface);
  BMIter iter;

  {
    BMFace *f;
    BM_ITER_MESH (f, &iter, pbvh->bm, BM_FACES_OF_MESH) {
      BLI_assert(BM_ELEM_CD_GET_INT(f, pbvh->cd_face_node_offset) != DYNTOPO_NODE_NONE);
      BLI_table_gset_insert(faces_all, f);
    }
  }

  GSet *verts_all = BLI_table_gset_new_ex(__func__, pbvh->bm->totvert);
  {
    BMVert *v;
    BM_ITER_MESH (v, &iter, pbvh->bm, BM_VERTS_OF_MESH) {
      if (BM_ELEM_CD_GET_INT(v, pbvh->cd_vert_node_offset) != DYNTOPO_NODE_NONE) {
        BLI_table_gset_insert(verts_all, v);
      }
    }
  }

  /* Check vert/face counts */
  {
    int totface = 0, totvert = 0;
    for (int i = 0; i < pbvh->totnode; i++) {
      PBVHNode *n = &pbvh->nodes[i];
      totface += n->bm_faces ? BLI_table_gset_len(n->bm_faces) : 0;
      totvert += n->bm_unique_verts ? BLI_table_gset_len(n->bm_unique_verts) : 0;
    }

    BLI_assert(totface == BLI_table_gset_len(faces_all));
    BLI_assert(totvert == BLI_table_gset_len(verts_all));
  }

  {
    BMFace *f;
    BM_ITER_MESH (f, &iter, pbvh->bm, BM_FACES_OF_MESH) {
      BMIter bm_iter;
      BMVert *v;
      PBVHNode *n = pbvh_bmesh_node_lookup(pbvh, f);

      /* Check that the face's node is a leaf */
      BLI_assert(n->flag & PBVH_Leaf);

      /* Check that the face's node knows it owns the face */
      BLI_assert(BLI_table_gset_haskey(n->bm_faces, f));

      /* Check the face's vertices... */
      BM_ITER_ELEM (v, &bm_iter, f, BM_VERTS_OF_FACE) {
        PBVHNode *nv;

        /* Check that the vertex is in the node */
        BLI_assert(BLI_table_gset_haskey(n->bm_unique_verts, v) ^
                   BLI_table_gset_haskey(n->bm_other_verts, v));

        /* Check that the vertex has a node owner */
        nv = pbvh_bmesh_node_lookup(pbvh, v);

        /* Check that the vertex's node knows it owns the vert */
        BLI_assert(BLI_table_gset_haskey(nv->bm_unique_verts, v));

        /* Check that the vertex isn't duplicated as an 'other' vert */
        BLI_assert(!BLI_table_gset_haskey(nv->bm_other_verts, v));
      }
    }
  }

  /* Check verts */
  {
    BMVert *v;
    BM_ITER_MESH (v, &iter, pbvh->bm, BM_VERTS_OF_MESH) {
      /* vertex isn't tracked */
      if (BM_ELEM_CD_GET_INT(v, pbvh->cd_vert_node_offset) == DYNTOPO_NODE_NONE) {
        continue;
      }

      PBVHNode *n = pbvh_bmesh_node_lookup(pbvh, v);

      /* Check that the vert's node is a leaf */
      BLI_assert(n->flag & PBVH_Leaf);

      /* Check that the vert's node knows it owns the vert */
      BLI_assert(BLI_table_gset_haskey(n->bm_unique_verts, v));

      /* Check that the vertex isn't duplicated as an 'other' vert */
      BLI_assert(!BLI_table_gset_haskey(n->bm_other_verts, v));

      /* Check that the vert's node also contains one of the vert's
       * adjacent faces */
      bool found = false;
      BMIter bm_iter;
      BMFace *f = NULL;
      BM_ITER_ELEM (f, &bm_iter, v, BM_FACES_OF_VERT) {
        if (pbvh_bmesh_node_lookup(pbvh, f) == n) {
          found = true;
          break;
        }
      }
      BLI_assert(found || f == NULL);

#  if 1
      /* total freak stuff, check if node exists somewhere else */
      /* Slow */
      for (int i = 0; i < pbvh->totnode; i++) {
        PBVHNode *n_other = &pbvh->nodes[i];
        if ((n != n_other) && (n_other->bm_unique_verts)) {
          BLI_assert(!BLI_table_gset_haskey(n_other->bm_unique_verts, v));
        }
      }
#  endif
    }
  }

#  if 0
  /* check that every vert belongs somewhere */
  /* Slow */
  BM_ITER_MESH (vi, &iter, pbvh->bm, BM_VERTS_OF_MESH) {
    bool has_unique = false;
    for (int i = 0; i < pbvh->totnode; i++) {
      PBVHNode *n = &pbvh->nodes[i];
      if ((n->bm_unique_verts != NULL) && BLI_table_gset_haskey(n->bm_unique_verts, vi)) {
        has_unique = true;
      }
    }
    BLI_assert(has_unique);
    vert_count++;
  }

  /* if totvert differs from number of verts inside the hash. hash-totvert is checked above  */
  BLI_assert(vert_count == pbvh->bm->totvert);
#  endif

  /* Check that node elements are recorded in the top level */
  for (int i = 0; i < pbvh->totnode; i++) {
    PBVHNode *n = &pbvh->nodes[i];
    if (n->flag & PBVH_Leaf) {
      GSetIterator gs_iter;

      GSET_ITER (gs_iter, n->bm_faces) {
        BMFace *f = BLI_gsetIterator_getKey(&gs_iter);
        PBVHNode *n_other = pbvh_bmesh_node_lookup(pbvh, f);
        BLI_assert(n == n_other);
        BLI_assert(BLI_table_gset_haskey(faces_all, f));
      }

      GSET_ITER (gs_iter, n->bm_unique_verts) {
        BMVert *v = BLI_gsetIterator_getKey(&gs_iter);
        PBVHNode *n_other = pbvh_bmesh_node_lookup(pbvh, v);
        BLI_assert(!BLI_table_gset_haskey(n->bm_other_verts, v));
        BLI_assert(n == n_other);
        BLI_assert(BLI_table_gset_haskey(verts_all, v));
      }

      GSET_ITER (gs_iter, n->bm_other_verts) {
        BMVert *v = BLI_gsetIterator_getKey(&gs_iter);
        /* this happens sometimes and seems harmless */
        // BLI_assert(!BM_vert_face_check(v));
        BLI_assert(BLI_table_gset_haskey(verts_all, v));
      }
    }
  }

  BLI_table_gset_free(faces_all, NULL);
  BLI_table_gset_free(verts_all, NULL);
}

#endif

void BKE_pbvh_update_offsets(PBVH *pbvh,
                             const int cd_vert_node_offset,
                             const int cd_face_node_offset,
                             const int cd_dyn_vert)
{
  pbvh->cd_face_node_offset = cd_face_node_offset;
  pbvh->cd_vert_node_offset = cd_vert_node_offset;
  pbvh->cd_vert_mask_offset = CustomData_get_offset(&pbvh->bm->vdata, CD_PAINT_MASK);
  pbvh->cd_vcol_offset = CustomData_get_offset(&pbvh->bm->vdata, CD_PROP_COLOR);
  pbvh->cd_dyn_vert = cd_dyn_vert;
}

static void scan_edge_split(BMesh *bm, BMEdge **edges, int totedge)
{
  BMFace **faces = NULL;
  BMEdge **newedges = NULL;
  BMVert **newverts = NULL;
  BMVert **fmap = NULL;  // newverts that maps to faces
  int *emap = NULL;

  BLI_array_declare(faces);
  BLI_array_declare(newedges);
  BLI_array_declare(newverts);
  BLI_array_declare(fmap);
  BLI_array_declare(emap);

  // remove e from radial list of e->v2
  for (int i = 0; i < totedge; i++) {
    BMEdge *e = edges[i];

    BMDiskLink *prev;
    BMDiskLink *next;

    if (e->v2_disk_link.prev->v1 == e->v2) {
      prev = &e->v2_disk_link.prev->v1_disk_link;
    }
    else {
      prev = &e->v2_disk_link.prev->v2_disk_link;
    }

    if (e->v2_disk_link.next->v1 == e->v2) {
      next = &e->v2_disk_link.next->v1_disk_link;
    }
    else {
      next = &e->v2_disk_link.next->v2_disk_link;
    }

    prev->next = e->v2_disk_link.next;
    next->prev = e->v2_disk_link.prev;
  }

  for (int i = 0; i < totedge; i++) {
    BMEdge *e = edges[i];

    BMVert *v2 = BLI_mempool_alloc(bm->vpool);
    memset(v2, 0, sizeof(*v2));
    v2->head.data = BLI_mempool_alloc(bm->vdata.pool);

    BLI_array_append(newverts, v2);

    BMEdge *e2 = BLI_mempool_alloc(bm->epool);
    BLI_array_append(newedges, e2);

    memset(e2, 0, sizeof(*e2));
    if (bm->edata.pool) {
      e2->head.data = BLI_mempool_alloc(bm->edata.pool);
    }

    BMLoop *l = e->l;

    if (!l) {
      continue;
    }

    do {
      BLI_array_append(faces, l->f);
      BMFace *f2 = BLI_mempool_alloc(bm->fpool);

      BLI_array_append(faces, l->f);
      BLI_array_append(fmap, v2);
      BLI_array_append(emap, i);

      BLI_array_append(faces, f2);
      BLI_array_append(fmap, v2);
      BLI_array_append(emap, i);

      memset(f2, 0, sizeof(*f2));
      f2->head.data = BLI_mempool_alloc(bm->ldata.pool);

      BMLoop *prev = NULL;
      BMLoop *l2;

      for (int j = 0; j < 3; j++) {
        l2 = BLI_mempool_alloc(bm->lpool);
        memset(l2, 0, sizeof(*l2));
        l2->head.data = BLI_mempool_alloc(bm->ldata.pool);

        l2->prev = prev;

        if (prev) {
          prev->next = l2;
        }
        else {
          f2->l_first = l2;
        }
      }

      f2->l_first->prev = l2;
      l2->next = f2->l_first;

      BLI_array_append(faces, f2);
      l = l->radial_next;
    } while (l != e->l);
  }

  for (int i = 0; i < BLI_array_len(newedges); i++) {
    BMEdge *e1 = edges[i];
    BMEdge *e2 = newedges[i];
    BMVert *v = newverts[i];

    add_v3_v3v3(v->co, e1->v1->co, e1->v2->co);
    mul_v3_fl(v->co, 0.5f);

    e2->v1 = v;
    e2->v2 = e1->v2;
    e1->v2 = v;

    v->e = e1;

    e1->v2_disk_link.next = e1->v2_disk_link.prev = e2;
    e2->v1_disk_link.next = e2->v1_disk_link.prev = e1;
  }

  for (int i = 0; i < BLI_array_len(faces); i += 2) {
    BMFace *f1 = faces[i], *f2 = faces[i + 1];
    BMEdge *e1 = edges[emap[i]];
    BMEdge *e2 = newedges[emap[i]];
    BMVert *nv = fmap[i];

    // make sure first loop points to e1->v1
    BMLoop *l = f1->l_first;
    do {
      if (l->v == e1->v1) {
        break;
      }
      l = l->next;
    } while (l != f1->l_first);

    f1->l_first = l;

    BMLoop *l2 = f2->l_first;

    l2->f = l2->next->f = l2->prev->f = f2;
    l2->v = nv;
    l2->next->v = l->next->v;
    l2->prev->v = l->prev->v;
    l2->e = e2;
    l2->next->e = l->next->e;
    l2->prev->e = l->prev->e;

    l->next->v = nv;
    l->next->e = e2;
  }

  BLI_array_free(newedges);
  BLI_array_free(newverts);
  BLI_array_free(faces);
  BLI_array_free(fmap);
}

BMesh *BKE_pbvh_reorder_bmesh(PBVH *pbvh)
{
  if (BKE_pbvh_type(pbvh) != PBVH_BMESH || pbvh->totnode == 0) {
    return pbvh->bm;
  }

  // try to group memory allocations by node
  struct {
    BMEdge **edges;
    int totedge;
    BMVert **verts;
    int totvert;
    BMFace **faces;
    int totface;
  } *nodedata = MEM_callocN(sizeof(*nodedata) * pbvh->totnode, "nodedata");

  BMIter iter;
  int types[3] = {BM_VERTS_OF_MESH, BM_EDGES_OF_MESH, BM_FACES_OF_MESH};

#define VISIT_TAG BM_ELEM_TAG

  BM_mesh_elem_index_ensure(pbvh->bm, BM_VERT | BM_EDGE | BM_FACE);
  BM_mesh_elem_table_ensure(pbvh->bm, BM_VERT | BM_EDGE | BM_FACE);

  for (int i = 0; i < 3; i++) {
    BMHeader *elem;

    BM_ITER_MESH (elem, &iter, pbvh->bm, types[i]) {
      elem->hflag &= ~VISIT_TAG;
    }
  }

  for (int i = 0; i < pbvh->totnode; i++) {
    PBVHNode *node = pbvh->nodes + i;

    if (!(node->flag & PBVH_Leaf)) {
      continue;
    }

    BMVert **verts = nodedata[i].verts;
    BMEdge **edges = nodedata[i].edges;
    BMFace **faces = nodedata[i].faces;

    BLI_array_declare(verts);
    BLI_array_declare(edges);
    BLI_array_declare(faces);

    BMVert *v;
    BMFace *f;

    TGSET_ITER (v, node->bm_unique_verts) {
      if (v->head.hflag & VISIT_TAG) {
        continue;
      }

      v->head.hflag |= VISIT_TAG;
      BLI_array_append(verts, v);

      BMEdge *e = v->e;
      do {
        if (!(e->head.hflag & VISIT_TAG)) {
          e->head.hflag |= VISIT_TAG;
          BLI_array_append(edges, e);
        }
        e = v == e->v1 ? e->v1_disk_link.next : e->v2_disk_link.next;
      } while (e != v->e);
    }
    TGSET_ITER_END;

    TGSET_ITER (f, node->bm_faces) {
      if (f->head.hflag & VISIT_TAG) {
        continue;
      }

      BLI_array_append(faces, f);
      f->head.hflag |= VISIT_TAG;
    }
    TGSET_ITER_END;

    nodedata[i].verts = verts;
    nodedata[i].edges = edges;
    nodedata[i].faces = faces;

    nodedata[i].totvert = BLI_array_len(verts);
    nodedata[i].totedge = BLI_array_len(edges);
    nodedata[i].totface = BLI_array_len(faces);
  }

  BMAllocTemplate templ = {
      pbvh->bm->totvert, pbvh->bm->totedge, pbvh->bm->totloop, pbvh->bm->totface};
  struct BMeshCreateParams params = {0};

  BMesh *bm2 = BM_mesh_create(&templ, &params);

  CustomData_copy_all_layout(&pbvh->bm->vdata, &bm2->vdata);
  CustomData_copy_all_layout(&pbvh->bm->edata, &bm2->edata);
  CustomData_copy_all_layout(&pbvh->bm->ldata, &bm2->ldata);
  CustomData_copy_all_layout(&pbvh->bm->pdata, &bm2->pdata);

  CustomData_bmesh_init_pool(&bm2->vdata, pbvh->bm->totvert, BM_VERT);
  CustomData_bmesh_init_pool(&bm2->edata, pbvh->bm->totedge, BM_EDGE);
  CustomData_bmesh_init_pool(&bm2->ldata, pbvh->bm->totloop, BM_LOOP);
  CustomData_bmesh_init_pool(&bm2->pdata, pbvh->bm->totface, BM_FACE);

  BMVert **verts = NULL;
  BMEdge **edges = NULL;
  BMFace **faces = NULL;
  BLI_array_declare(verts);
  BLI_array_declare(edges);
  BLI_array_declare(faces);

  for (int i = 0; i < pbvh->totnode; i++) {
    for (int j = 0; j < nodedata[i].totvert; j++) {
      BMVert *v1 = nodedata[i].verts[j];
      BMVert *v2 = BM_vert_create(bm2, v1->co, NULL, BM_CREATE_SKIP_CD);
      BM_elem_attrs_copy_ex(pbvh->bm, bm2, v1, v2, 0, 0L);

      v2->head.index = v1->head.index = BLI_array_len(verts);
      BLI_array_append(verts, v2);
    }
  }

  for (int i = 0; i < pbvh->totnode; i++) {
    for (int j = 0; j < nodedata[i].totedge; j++) {
      BMEdge *e1 = nodedata[i].edges[j];
      BMEdge *e2 = BM_edge_create(
          bm2, verts[e1->v1->head.index], verts[e1->v2->head.index], NULL, BM_CREATE_SKIP_CD);
      BM_elem_attrs_copy_ex(pbvh->bm, bm2, e1, e2, 0, 0L);

      e2->head.index = e1->head.index = BLI_array_len(edges);
      BLI_array_append(edges, e2);
    }
  }

  BMVert **fvs = NULL;
  BMEdge **fes = NULL;
  BLI_array_declare(fvs);
  BLI_array_declare(fes);

  for (int i = 0; i < pbvh->totnode; i++) {
    for (int j = 0; j < nodedata[i].totface; j++) {
      BMFace *f1 = nodedata[i].faces[j];

      BLI_array_clear(fvs);
      BLI_array_clear(fes);

      int totloop = 0;
      BMLoop *l1 = f1->l_first;
      do {
        BLI_array_append(fvs, verts[l1->v->head.index]);
        BLI_array_append(fes, edges[l1->e->head.index]);
        l1 = l1->next;
        totloop++;
      } while (l1 != f1->l_first);

      BMFace *f2 = BM_face_create(bm2, fvs, fes, totloop, NULL, BM_CREATE_SKIP_CD);
      f1->head.index = f2->head.index = BLI_array_len(faces);
      BLI_array_append(faces, f2);

      // CustomData_bmesh_copy_data(&pbvh->bm->pdata, &bm2->pdata, f1->head.data, &f2->head.data);
      BM_elem_attrs_copy_ex(pbvh->bm, bm2, f1, f2, 0, 0L);

      BMLoop *l2 = f2->l_first;
      do {
        BM_elem_attrs_copy_ex(pbvh->bm, bm2, l1, l2, 0, 0L);

        l1 = l1->next;
        l2 = l2->next;
      } while (l2 != f2->l_first);
    }
  }

  for (int i = 0; i < pbvh->totnode; i++) {
    PBVHNode *node = pbvh->nodes + i;

    if (!(node->flag & PBVH_Leaf)) {
      continue;
    }

    int totunique = node->bm_unique_verts->length;
    int totother = node->bm_other_verts->length;
    int totface = node->bm_faces->length;

    TableGSet *bm_faces = BLI_table_gset_new_ex("bm_faces", totface);
    TableGSet *bm_other_verts = BLI_table_gset_new_ex("bm_other_verts", totunique);
    TableGSet *bm_unique_verts = BLI_table_gset_new_ex("bm_unique_verts", totother);

    BMVert *v;
    BMFace *f;

    TGSET_ITER (v, node->bm_unique_verts) {
      BLI_table_gset_insert(bm_unique_verts, verts[v->head.index]);
    }
    TGSET_ITER_END;
    TGSET_ITER (v, node->bm_other_verts) {
      BLI_table_gset_insert(bm_other_verts, verts[v->head.index]);
    }
    TGSET_ITER_END;
    TGSET_ITER (f, node->bm_faces) {
      BLI_table_gset_insert(bm_faces, faces[f->head.index]);
    }
    TGSET_ITER_END;

    BLI_table_gset_free(node->bm_faces, NULL);
    BLI_table_gset_free(node->bm_other_verts, NULL);
    BLI_table_gset_free(node->bm_unique_verts, NULL);

    node->bm_faces = bm_faces;
    node->bm_other_verts = bm_other_verts;
    node->bm_unique_verts = bm_unique_verts;

    node->flag |= PBVH_UpdateTris | PBVH_UpdateRedraw;
  }

  MEM_SAFE_FREE(fvs);
  MEM_SAFE_FREE(fes);

  for (int i = 0; i < pbvh->totnode; i++) {
    MEM_SAFE_FREE(nodedata[i].verts);
    MEM_SAFE_FREE(nodedata[i].edges);
    MEM_SAFE_FREE(nodedata[i].faces);
  }

  MEM_SAFE_FREE(verts);
  MEM_SAFE_FREE(edges);
  MEM_SAFE_FREE(faces);

  MEM_freeN(nodedata);

  BM_mesh_free(pbvh->bm);
  pbvh->bm = bm2;

  return bm2;
}
