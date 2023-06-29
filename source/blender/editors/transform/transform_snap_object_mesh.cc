/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include "BLI_math.h"
#include "BLI_math_matrix.hh"

#include "BKE_bvhutils.h"
#include "BKE_editmesh.h"
#include "BKE_mesh.hh"
#include "BKE_object.h"

#include "ED_transform_snap_object_context.h"

#include "transform_snap_object.hh"

#ifdef DEBUG_SNAP_TIME
#  if WIN32 and NDEBUG
#    pragma optimize("O", on)
#  endif
#endif

using namespace blender;

/* -------------------------------------------------------------------- */
/** \name Snap Object Data
 * \{ */

static void snap_object_data_mesh_get(const Mesh *me_eval,
                                      bool use_hide,
                                      BVHTreeFromMesh *r_treedata)
{
  const Span<float3> vert_positions = me_eval->vert_positions();
  const blender::OffsetIndices polys = me_eval->polys();
  const Span<int> corner_verts = me_eval->corner_verts();

  /* The BVHTree from looptris is always required. */
  BKE_bvhtree_from_mesh_get(
      r_treedata, me_eval, use_hide ? BVHTREE_FROM_LOOPTRI_NO_HIDDEN : BVHTREE_FROM_LOOPTRI, 4);

  BLI_assert(reinterpret_cast<const float3 *>(r_treedata->vert_positions) ==
             vert_positions.data());
  BLI_assert(r_treedata->corner_verts == corner_verts.data());
  BLI_assert(!polys.data() || r_treedata->looptri);
  BLI_assert(!r_treedata->tree || r_treedata->looptri);

  UNUSED_VARS_NDEBUG(vert_positions, polys, corner_verts);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Ray Cast Functions
 * \{ */

/* Store all ray-hits
 * Support for storing all depths, not just the first (ray-cast 'all'). */

/* Callback to ray-cast with back-face culling (#Mesh). */
static void mesh_looptri_raycast_backface_culling_cb(void *userdata,
                                                     int index,
                                                     const BVHTreeRay *ray,
                                                     BVHTreeRayHit *hit)
{
  const BVHTreeFromMesh *data = (BVHTreeFromMesh *)userdata;
  const float(*vert_positions)[3] = data->vert_positions;
  const MLoopTri *lt = &data->looptri[index];
  const float *vtri_co[3] = {
      vert_positions[data->corner_verts[lt->tri[0]]],
      vert_positions[data->corner_verts[lt->tri[1]]],
      vert_positions[data->corner_verts[lt->tri[2]]],
  };
  float dist = bvhtree_ray_tri_intersection(ray, hit->dist, UNPACK3(vtri_co));

  if (dist >= 0 && dist < hit->dist) {
    float no[3];
    if (raycast_tri_backface_culling_test(ray->direction, UNPACK3(vtri_co), no)) {
      hit->index = index;
      hit->dist = dist;
      madd_v3_v3v3fl(hit->co, ray->origin, ray->direction, dist);
      normalize_v3_v3(hit->no, no);
    }
  }
}

static bool raycastMesh(SnapObjectContext *sctx,
                        Object *ob_eval,
                        const Mesh *me_eval,
                        const float obmat[4][4],
                        const uint ob_index,
                        bool use_hide)
{
  bool retval = false;

  if (me_eval->totpoly == 0) {
    return retval;
  }

  float imat[4][4];
  float ray_start_local[3], ray_normal_local[3];
  float local_scale, local_depth, len_diff = 0.0f;

  invert_m4_m4(imat, obmat);

  copy_v3_v3(ray_start_local, sctx->runtime.ray_start);
  copy_v3_v3(ray_normal_local, sctx->runtime.ray_dir);

  mul_m4_v3(imat, ray_start_local);
  mul_mat3_m4_v3(imat, ray_normal_local);

  /* local scale in normal direction */
  local_scale = normalize_v3(ray_normal_local);
  local_depth = sctx->ret.ray_depth_max;
  if (local_depth != BVH_RAYCAST_DIST_MAX) {
    local_depth *= local_scale;
  }

  /* Test BoundBox */
  if (ob_eval->data == me_eval) {
    const BoundBox *bb = BKE_object_boundbox_get(ob_eval);
    if (bb) {
      /* was BKE_boundbox_ray_hit_check, see: cf6ca226fa58 */
      if (!isect_ray_aabb_v3_simple(
              ray_start_local, ray_normal_local, bb->vec[0], bb->vec[6], &len_diff, nullptr))
      {
        return retval;
      }
    }
  }

  /* We pass a temp ray_start, set from object's boundbox, to avoid precision issues with
   * very far away ray_start values (as returned in case of ortho view3d), see #50486, #38358.
   */
  if (len_diff > 400.0f) {
    /* Make temporary start point a bit away from bounding-box hit point. */
    len_diff -= local_scale;
    madd_v3_v3fl(ray_start_local, ray_normal_local, len_diff);
    local_depth -= len_diff;
  }
  else {
    len_diff = 0.0f;
  }

  BVHTreeFromMesh treedata;
  snap_object_data_mesh_get(me_eval, use_hide, &treedata);

  const blender::Span<int> looptri_polys = me_eval->looptri_polys();

  if (treedata.tree == nullptr) {
    return retval;
  }

  BLI_assert(treedata.raycast_callback != nullptr);
  if (sctx->ret.hit_list) {
    RayCastAll_Data data;

    data.bvhdata = &treedata;
    data.raycast_callback = treedata.raycast_callback;
    data.obmat = obmat;
    data.len_diff = len_diff;
    data.local_scale = local_scale;
    data.ob_uuid = ob_index;
    data.hit_list = sctx->ret.hit_list;

    void *hit_last_prev = data.hit_list->last;
    BLI_bvhtree_ray_cast_all(treedata.tree,
                             ray_start_local,
                             ray_normal_local,
                             0.0f,
                             sctx->ret.ray_depth_max,
                             raycast_all_cb,
                             &data);

    retval = hit_last_prev != data.hit_list->last;
  }
  else {
    BVHTreeRayHit hit{};
    hit.index = -1;
    hit.dist = local_depth;

    if (BLI_bvhtree_ray_cast(treedata.tree,
                             ray_start_local,
                             ray_normal_local,
                             0.0f,
                             &hit,
                             sctx->runtime.params.use_backface_culling ?
                                 mesh_looptri_raycast_backface_culling_cb :
                                 treedata.raycast_callback,
                             &treedata) != -1)
    {
      hit.dist += len_diff;
      hit.dist /= local_scale;
      if (hit.dist <= sctx->ret.ray_depth_max) {
        copy_v3_v3(sctx->ret.loc, hit.co);
        copy_v3_v3(sctx->ret.no, hit.no);

        mul_m4_v3(obmat, sctx->ret.loc);

        mul_transposed_mat3_m4_v3(imat, sctx->ret.no);
        normalize_v3(sctx->ret.no);

        sctx->ret.ray_depth_max = hit.dist;
        sctx->ret.index = looptri_polys[hit.index];
        retval = true;
      }
    }
  }

  return retval;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface Snap Functions
 * \{ */

static bool nearest_world_mesh(SnapObjectContext *sctx,
                               Object *ob_eval,
                               const Mesh *me_eval,
                               const float4x4 obmat,
                               bool use_hide)
{
  BVHTreeFromMesh treedata;
  snap_object_data_mesh_get(me_eval, use_hide, &treedata);
  if (treedata.tree == nullptr) {
    return false;
  }

  float3 init_co = math::transform_point(obmat, float3(sctx->runtime.init_co));
  float3 curr_co = math::transform_point(obmat, float3(sctx->runtime.curr_co));

  BVHTreeNearest nearest{};
  nearest.dist_sq = sctx->ret.dist_px_sq;
  if (nearest_world_tree(
          sctx, treedata.tree, treedata.nearest_callback, init_co, curr_co, &treedata, &nearest))
  {
    SnapData::register_result(sctx, ob_eval, &me_eval->id, obmat, &nearest);
    return true;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Subclass for Snapping to Edges or Points of a Mesh
 * \{ */

class SnapData_Mesh : public SnapData {
 public:
  const float3 *vert_positions;
  const float3 *vert_normals;
  const int2 *edges; /* only used for #BVHTreeFromMeshEdges */
  const int *corner_verts;
  const int *corner_edges;
  const MLoopTri *looptris;

  SnapData_Mesh(SnapObjectContext *sctx, const Mesh *mesh_eval, const float4x4 &obmat)
      : SnapData(sctx, obmat)
  {
    this->vert_positions = mesh_eval->vert_positions().data();
    this->vert_normals = mesh_eval->vert_normals().data();
    this->edges = mesh_eval->edges().data();
    this->corner_verts = mesh_eval->corner_verts().data();
    this->corner_edges = mesh_eval->corner_edges().data();
    this->looptris = mesh_eval->looptris().data();
  };

  void get_vert_co(const int index, const float **r_co)
  {
    *r_co = this->vert_positions[index];
  }

  void get_edge_verts_index(const int index, int r_v_index[2])
  {
    const blender::int2 &edge = this->edges[index];
    r_v_index[0] = edge[0];
    r_v_index[1] = edge[1];
  }

  void get_tri_verts_index(const int index, int r_v_index[3])
  {
    const int *corner_verts = this->corner_verts;
    const MLoopTri *looptri = &this->looptris[index];
    r_v_index[0] = corner_verts[looptri->tri[0]];
    r_v_index[1] = corner_verts[looptri->tri[1]];
    r_v_index[2] = corner_verts[looptri->tri[2]];
  }

  void get_tri_edges_index(const int index, int r_e_index[3])
  {
    const blender::int2 *edges = this->edges;
    const int *corner_verts = this->corner_verts;
    const int *corner_edges = this->corner_edges;
    const MLoopTri *lt = &this->looptris[index];
    for (int j = 2, j_next = 0; j_next < 3; j = j_next++) {
      const blender::int2 &edge = edges[corner_edges[lt->tri[j]]];
      const int tri_edge[2] = {corner_verts[lt->tri[j]], corner_verts[lt->tri[j_next]]};
      if (ELEM(edge[0], tri_edge[0], tri_edge[1]) && ELEM(edge[1], tri_edge[0], tri_edge[1])) {
        // printf("real edge found\n");
        r_e_index[j] = corner_edges[lt->tri[j]];
      }
      else {
        r_e_index[j] = -1;
      }
    }
  }

  void copy_vert_no(const int index, float r_no[3])
  {
    copy_v3_v3(r_no, this->vert_normals[index]);
  }
};

static void cb_snap_edge_verts(void *userdata,
                               int index,
                               const DistProjectedAABBPrecalc *precalc,
                               const float (*clip_plane)[4],
                               const int clip_plane_len,
                               BVHTreeNearest *nearest)
{
  SnapData_Mesh *data = static_cast<SnapData_Mesh *>(userdata);

  int vindex[2];
  data->get_edge_verts_index(index, vindex);

  for (int i = 2; i--;) {
    if (vindex[i] == nearest->index) {
      continue;
    }
    cb_snap_vert(userdata, vindex[i], precalc, clip_plane, clip_plane_len, nearest);
  }
}

static void cb_snap_tri_verts(void *userdata,
                              int index,
                              const DistProjectedAABBPrecalc *precalc,
                              const float (*clip_plane)[4],
                              const int clip_plane_len,
                              BVHTreeNearest *nearest)
{
  SnapData_Mesh *data = static_cast<SnapData_Mesh *>(userdata);

  int vindex[3];
  data->get_tri_verts_index(index, vindex);

  if (data->use_backface_culling) {
    const float *t0, *t1, *t2;
    data->get_vert_co(vindex[0], &t0);
    data->get_vert_co(vindex[1], &t1);
    data->get_vert_co(vindex[2], &t2);
    float dummy[3];
    if (raycast_tri_backface_culling_test(precalc->ray_direction, t0, t1, t2, dummy)) {
      return;
    }
  }

  for (int i = 3; i--;) {
    if (vindex[i] == nearest->index) {
      continue;
    }
    cb_snap_vert(userdata, vindex[i], precalc, clip_plane, clip_plane_len, nearest);
  }
}

static void cb_snap_tri_edges(void *userdata,
                              int index,
                              const DistProjectedAABBPrecalc *precalc,
                              const float (*clip_plane)[4],
                              const int clip_plane_len,
                              BVHTreeNearest *nearest)
{
  SnapData_Mesh *data = static_cast<SnapData_Mesh *>(userdata);

  if (data->use_backface_culling) {
    int vindex[3];
    data->get_tri_verts_index(index, vindex);

    const float *t0, *t1, *t2;
    data->get_vert_co(vindex[0], &t0);
    data->get_vert_co(vindex[1], &t1);
    data->get_vert_co(vindex[2], &t2);
    float dummy[3];
    if (raycast_tri_backface_culling_test(precalc->ray_direction, t0, t1, t2, dummy)) {
      return;
    }
  }

  int eindex[3];
  data->get_tri_edges_index(index, eindex);
  for (int i = 3; i--;) {
    if (eindex[i] != -1) {
      if (eindex[i] == nearest->index) {
        continue;
      }
      cb_snap_edge(userdata, eindex[i], precalc, clip_plane, clip_plane_len, nearest);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal Object Snapping API
 * \{ */

eSnapMode snap_polygon_mesh(SnapObjectContext *sctx,
                            Object *ob_eval,
                            const ID *id,
                            const float obmat[4][4],
                            eSnapMode snap_to_flag,
                            int polygon)
{
  eSnapMode elem = SCE_SNAP_TO_NONE;

  const Mesh *mesh_eval = reinterpret_cast<const Mesh *>(id);

  SnapData_Mesh nearest2d(sctx, mesh_eval, float4x4(obmat));
  nearest2d.clip_planes_enable(sctx);

  BVHTreeNearest nearest{};
  nearest.index = -1;
  nearest.dist_sq = sctx->ret.dist_px_sq;

  const blender::IndexRange poly = mesh_eval->polys()[polygon];

  if (snap_to_flag & SCE_SNAP_TO_EDGE) {
    elem = SCE_SNAP_TO_EDGE;
    BLI_assert(nearest2d.edges != nullptr);
    const int *poly_edges = &nearest2d.corner_edges[poly.start()];
    for (int i = poly.size(); i--;) {
      cb_snap_edge(&nearest2d,
                   poly_edges[i],
                   &nearest2d.nearest_precalc,
                   reinterpret_cast<float(*)[4]>(nearest2d.clip_planes.data()),
                   nearest2d.clip_planes.size(),
                   &nearest);
    }
  }
  else {
    elem = SCE_SNAP_TO_VERTEX;
    const int *poly_verts = &nearest2d.corner_verts[poly.start()];
    for (int i = poly.size(); i--;) {
      cb_snap_vert(&nearest2d,
                   poly_verts[i],
                   &nearest2d.nearest_precalc,
                   reinterpret_cast<float(*)[4]>(nearest2d.clip_planes.data()),
                   nearest2d.clip_planes.size(),
                   &nearest);
    }
  }

  if (nearest.index != -1) {
    nearest2d.nearest_point = nearest;
    nearest2d.register_result(sctx, ob_eval, id);
    return elem;
  }

  return SCE_SNAP_TO_NONE;
}

eSnapMode snap_edge_points_mesh(SnapObjectContext *sctx,
                                Object *ob_eval,
                                const ID *id,
                                const float obmat[4][4],
                                float dist_pex_sq_orig,
                                int edge)
{
  SnapData_Mesh nearest2d(sctx, reinterpret_cast<const Mesh *>(id), float4x4(obmat));
  eSnapMode elem = nearest2d.snap_edge_points_impl(sctx, edge, dist_pex_sq_orig);
  if (nearest2d.nearest_point.index != -2) {
    nearest2d.register_result(sctx, ob_eval, id);
  }
  return elem;
}

static eSnapMode mesh_snap_mode_supported(const Mesh *mesh)
{
  eSnapMode snap_mode_supported = mesh->loose_verts().count ? SCE_SNAP_TO_POINT : SCE_SNAP_TO_NONE;
  if (mesh->totpoly) {
    snap_mode_supported |= SCE_SNAP_TO_FACE | SCE_SNAP_INDIVIDUAL_NEAREST | SNAP_TO_EDGE_ELEMENTS;
  }
  else if (mesh->totedge) {
    snap_mode_supported |= SNAP_TO_EDGE_ELEMENTS;
  }

  return snap_mode_supported;
}

static eSnapMode snapMesh(SnapObjectContext *sctx,
                          Object *ob_eval,
                          const Mesh *me_eval,
                          const float obmat[4][4],
                          bool use_hide,
                          eSnapMode snap_to)
{
  BLI_assert(snap_to != SCE_SNAP_TO_FACE);
  SnapData_Mesh nearest2d(sctx, me_eval, float4x4(obmat));

  if (ob_eval->data == me_eval) {
    const BoundBox *bb = BKE_mesh_boundbox_get(ob_eval);
    if (!nearest2d.snap_boundbox(bb->vec[0], bb->vec[6])) {
      return SCE_SNAP_TO_NONE;
    }
  }

  snap_to &= mesh_snap_mode_supported(me_eval) & (SNAP_TO_EDGE_ELEMENTS | SCE_SNAP_TO_POINT);
  if (snap_to == SCE_SNAP_TO_NONE) {
    return SCE_SNAP_TO_NONE;
  }

  BVHTreeFromMesh treedata, treedata_dummy;
  snap_object_data_mesh_get(me_eval, use_hide, &treedata);

  BVHTree *bvhtree[2] = {nullptr};
  bvhtree[0] = BKE_bvhtree_from_mesh_get(&treedata_dummy, me_eval, BVHTREE_FROM_LOOSEEDGES, 2);
  BLI_assert(treedata_dummy.cached);
  if (snap_to & SCE_SNAP_TO_POINT) {
    bvhtree[1] = BKE_bvhtree_from_mesh_get(&treedata_dummy, me_eval, BVHTREE_FROM_LOOSEVERTS, 2);
    BLI_assert(treedata_dummy.cached);
  }

  nearest2d.clip_planes_enable(sctx);

  BVHTreeNearest nearest{};
  nearest.index = -1;
  nearest.dist_sq = sctx->ret.dist_px_sq;

  int last_index = nearest.index;
  eSnapMode elem = SCE_SNAP_TO_POINT;

  if (bvhtree[1]) {
    BLI_assert(snap_to & SCE_SNAP_TO_POINT);
    /* snap to loose verts */
    BLI_bvhtree_find_nearest_projected(bvhtree[1],
                                       nearest2d.pmat_local.ptr(),
                                       sctx->runtime.win_size,
                                       sctx->runtime.mval,
                                       reinterpret_cast<float(*)[4]>(nearest2d.clip_planes.data()),
                                       nearest2d.clip_planes.size(),
                                       &nearest,
                                       cb_snap_vert,
                                       &nearest2d);

    last_index = nearest.index;
  }

  if (snap_to & (SNAP_TO_EDGE_ELEMENTS & ~SCE_SNAP_TO_EDGE_ENDPOINT)) {
    if (bvhtree[0]) {
      /* Snap to loose edges. */
      BLI_bvhtree_find_nearest_projected(
          bvhtree[0],
          nearest2d.pmat_local.ptr(),
          sctx->runtime.win_size,
          sctx->runtime.mval,
          reinterpret_cast<float(*)[4]>(nearest2d.clip_planes.data()),
          nearest2d.clip_planes.size(),
          &nearest,
          cb_snap_edge,
          &nearest2d);
    }

    if (treedata.tree) {
      /* Snap to looptris. */
      BLI_bvhtree_find_nearest_projected(
          treedata.tree,
          nearest2d.pmat_local.ptr(),
          sctx->runtime.win_size,
          sctx->runtime.mval,
          reinterpret_cast<float(*)[4]>(nearest2d.clip_planes.data()),
          nearest2d.clip_planes.size(),
          &nearest,
          cb_snap_tri_edges,
          &nearest2d);
    }

    if (last_index != nearest.index) {
      elem = SCE_SNAP_TO_EDGE;
    }
  }
  else {
    BLI_assert(snap_to & SCE_SNAP_TO_EDGE_ENDPOINT);
    if (bvhtree[0]) {
      /* Snap to loose edge verts. */
      BLI_bvhtree_find_nearest_projected(
          bvhtree[0],
          nearest2d.pmat_local.ptr(),
          sctx->runtime.win_size,
          sctx->runtime.mval,
          reinterpret_cast<float(*)[4]>(nearest2d.clip_planes.data()),
          nearest2d.clip_planes.size(),
          &nearest,
          cb_snap_edge_verts,
          &nearest2d);
    }

    if (treedata.tree) {
      /* Snap to looptri verts. */
      BLI_bvhtree_find_nearest_projected(
          treedata.tree,
          nearest2d.pmat_local.ptr(),
          sctx->runtime.win_size,
          sctx->runtime.mval,
          reinterpret_cast<float(*)[4]>(nearest2d.clip_planes.data()),
          nearest2d.clip_planes.size(),
          &nearest,
          cb_snap_tri_verts,
          &nearest2d);
    }
  }

  if (nearest.index != -1) {
    nearest2d.nearest_point = nearest;
    nearest2d.register_result(sctx, ob_eval, &me_eval->id);
    return elem;
  }

  return SCE_SNAP_TO_NONE;
}

/** \} */

eSnapMode snap_object_mesh(SnapObjectContext *sctx,
                           Object *ob_eval,
                           const ID *id,
                           const float obmat[4][4],
                           eSnapMode snap_to_flag,
                           bool use_hide)
{
  eSnapMode elem = SCE_SNAP_TO_NONE;
  const Mesh *mesh_eval = reinterpret_cast<const Mesh *>(id);

  if (snap_to_flag & (SNAP_TO_EDGE_ELEMENTS | SCE_SNAP_TO_POINT)) {
    elem = snapMesh(sctx, ob_eval, mesh_eval, obmat, use_hide, snap_to_flag);
    if (elem) {
      return elem;
    }
  }

  if (snap_to_flag & SCE_SNAP_TO_FACE) {
    if (raycastMesh(sctx, ob_eval, mesh_eval, obmat, sctx->runtime.object_index++, use_hide)) {
      return SCE_SNAP_TO_FACE;
    }
  }

  if (snap_to_flag & SCE_SNAP_INDIVIDUAL_NEAREST) {
    if (nearest_world_mesh(sctx, ob_eval, mesh_eval, float4x4(obmat), use_hide)) {
      return SCE_SNAP_INDIVIDUAL_NEAREST;
    }
  }

  return SCE_SNAP_TO_NONE;
}
