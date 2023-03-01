/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup render
 *
 * \brief The API itself is simple.
 * Blender sends a populated array of BakePixels to the renderer,
 * and gets back an array of floats with the result.
 *
 * \section bake_api Development Notes for External Engines
 *
 * The Bake API is fully implemented with Python rna functions.
 * The operator expects/call a function:
 *
 * `def bake(scene, object, pass_type, object_id, pixel_array, pixels_num, depth, result)`
 * - scene: current scene (Python object)
 * - object: object to render (Python object)
 * - pass_type: pass to render (string, e.g., "COMBINED", "AO", "NORMAL", ...)
 * - object_id: index of object to bake (to use with the pixel_array)
 * - pixel_array: list of primitive ids and barycentric coordinates to
 *   `bake(Python object, see bake_pixel)`.
 * - pixels_num: size of pixel_array, number of pixels to bake (int)
 * - depth: depth of pixels to return (int, assuming always 4 now)
 * - result: array to be populated by the engine (float array, PyLong_AsVoidPtr)
 *
 * \note Normals are expected to be in World Space and in the +X, +Y, +Z orientation.
 *
 * \subsection bake_pixel BakePixel data structure
 *
 * pixel_array is a Python object storing BakePixel elements:
 *
 * \code{.c}
 * struct BakePixel {
 *     int primitive_id, object_id;
 *     float uv[2];
 *     float du_dx, du_dy;
 *     float dv_dx, dv_dy;
 * };
 * \endcode
 *
 * In python you have access to:
 * - `primitive_id`, `object_id`, `uv`, `du_dx`, `du_dy`, `next`.
 * - `next()` is a function that returns the next #BakePixel in the array.
 *
 * \note Pixels that should not be baked have `primitive_id == -1`.
 *
 * For a complete implementation example look at the Cycles Bake commit.
 */

#include <climits>

#include "MEM_guardedalloc.h"

#include "BLI_math.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_bvhutils.h"
#include "BKE_customdata.h"
#include "BKE_image.h"
#include "BKE_lib_id.h"
#include "BKE_mesh.h"
#include "BKE_mesh_runtime.h"
#include "BKE_mesh_tangent.h"
#include "BKE_node.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "RE_bake.h"
#include "RE_texture_margin.h"

/* local include */
#include "zbuf.h"

struct BakeDataZSpan {
  BakePixel *pixel_array;
  int primitive_id;
  BakeImage *bk_image;
  ZSpan *zspan;
  float du_dx, du_dy;
  float dv_dx, dv_dy;
};

/**
 * struct wrapping up tangent space data
 */
struct TSpace {
  float tangent[3];
  float sign;
};

struct TriTessFace {
  const float *positions[3];
  const float *vert_normals[3];
  const TSpace *tspace[3];
  const float *loop_normal[3];
  float normal[3]; /* for flat faces */
  bool is_smooth;
};

static void store_bake_pixel(void *handle, int x, int y, float u, float v)
{
  BakeDataZSpan *bd = (BakeDataZSpan *)handle;
  BakePixel *pixel;

  const int width = bd->bk_image->width;
  const size_t offset = bd->bk_image->offset;
  const int i = offset + y * width + x;

  pixel = &bd->pixel_array[i];
  pixel->primitive_id = bd->primitive_id;

  /* At this point object_id is always 0, since this function runs for the
   * low-poly mesh only. The object_id lookup indices are set afterwards. */

  copy_v2_fl2(pixel->uv, u, v);

  pixel->du_dx = bd->du_dx;
  pixel->du_dy = bd->du_dy;
  pixel->dv_dx = bd->dv_dx;
  pixel->dv_dy = bd->dv_dy;
  pixel->object_id = 0;
  pixel->seed = i;
}

void RE_bake_mask_fill(const BakePixel pixel_array[], const size_t pixels_num, char *mask)
{
  size_t i;
  if (!mask) {
    return;
  }

  /* only extend to pixels outside the mask area */
  for (i = 0; i < pixels_num; i++) {
    if (pixel_array[i].primitive_id != -1) {
      mask[i] = FILTER_MASK_USED;
    }
  }
}

void RE_bake_margin(ImBuf *ibuf,
                    char *mask,
                    const int margin,
                    const char margin_type,
                    Mesh const *me,
                    char const *uv_layer,
                    const float uv_offset[2])
{
  /* margin */
  switch (margin_type) {
    case R_BAKE_ADJACENT_FACES:
      RE_generate_texturemargin_adjacentfaces(ibuf, mask, margin, me, uv_layer, uv_offset);
      break;
    default:
    /* fall through */
    case R_BAKE_EXTEND:
      IMB_filter_extend(ibuf, mask, margin);
      break;
  }

  if (ibuf->planes != R_IMF_PLANES_RGBA) {
    /* clear alpha added by filtering */
    IMB_rectfill_alpha(ibuf, 1.0f);
  }
}

/**
 * This function returns the coordinate and normal of a barycentric u,v
 * for a face defined by the primitive_id index.
 * The returned normal is actually the direction from the same barycentric coordinate
 * in the cage to the base mesh
 * The returned coordinate is the point in the cage mesh
 */
static void calc_point_from_barycentric_cage(TriTessFace *triangles_low,
                                             TriTessFace *triangles_cage,
                                             const float mat_low[4][4],
                                             const float mat_cage[4][4],
                                             int primitive_id,
                                             float u,
                                             float v,
                                             float r_co[3],
                                             float r_dir[3])
{
  float data[2][3][3];
  float coord[2][3];
  float dir[3];
  int i;

  TriTessFace *triangle[2];

  triangle[0] = &triangles_low[primitive_id];
  triangle[1] = &triangles_cage[primitive_id];

  for (i = 0; i < 2; i++) {
    copy_v3_v3(data[i][0], triangle[i]->positions[0]);
    copy_v3_v3(data[i][1], triangle[i]->positions[1]);
    copy_v3_v3(data[i][2], triangle[i]->positions[2]);
    interp_barycentric_tri_v3(data[i], u, v, coord[i]);
  }

  /* convert from local to world space */
  mul_m4_v3(mat_low, coord[0]);
  mul_m4_v3(mat_cage, coord[1]);

  sub_v3_v3v3(dir, coord[0], coord[1]);
  normalize_v3(dir);

  copy_v3_v3(r_co, coord[1]);
  copy_v3_v3(r_dir, dir);
}

/**
 * This function returns the coordinate and normal of a barycentric u,v
 * for a face defined by the primitive_id index.
 * The returned coordinate is extruded along the normal by cage_extrusion
 */
static void calc_point_from_barycentric_extrusion(TriTessFace *triangles,
                                                  const float mat[4][4],
                                                  const float imat[4][4],
                                                  int primitive_id,
                                                  float u,
                                                  float v,
                                                  float cage_extrusion,
                                                  float r_co[3],
                                                  float r_dir[3],
                                                  const bool is_cage)
{
  float data[3][3];
  float coord[3];
  float dir[3];
  float cage[3];
  bool is_smooth;

  TriTessFace *triangle = &triangles[primitive_id];
  is_smooth = triangle->is_smooth || is_cage;

  copy_v3_v3(data[0], triangle->positions[0]);
  copy_v3_v3(data[1], triangle->positions[1]);
  copy_v3_v3(data[2], triangle->positions[2]);

  interp_barycentric_tri_v3(data, u, v, coord);

  if (is_smooth) {
    copy_v3_v3(data[0], triangle->vert_normals[0]);
    copy_v3_v3(data[1], triangle->vert_normals[1]);
    copy_v3_v3(data[2], triangle->vert_normals[2]);

    interp_barycentric_tri_v3(data, u, v, dir);
    normalize_v3(dir);
  }
  else {
    copy_v3_v3(dir, triangle->normal);
  }

  mul_v3_v3fl(cage, dir, cage_extrusion);
  add_v3_v3(coord, cage);

  normalize_v3(dir);
  negate_v3(dir);

  /* convert from local to world space */
  mul_m4_v3(mat, coord);
  mul_transposed_mat3_m4_v3(imat, dir);
  normalize_v3(dir);

  copy_v3_v3(r_co, coord);
  copy_v3_v3(r_dir, dir);
}

static void barycentric_differentials_from_position(const float co[3],
                                                    const float v1[3],
                                                    const float v2[3],
                                                    const float v3[3],
                                                    const float dxco[3],
                                                    const float dyco[3],
                                                    const float facenor[3],
                                                    const bool differentials,
                                                    float *u,
                                                    float *v,
                                                    float *dx_u,
                                                    float *dx_v,
                                                    float *dy_u,
                                                    float *dy_v)
{
  /* find most stable axis to project */
  int axis1, axis2;
  axis_dominant_v3(&axis1, &axis2, facenor);

  /* compute u,v and derivatives */
  float t00 = v3[axis1] - v1[axis1];
  float t01 = v3[axis2] - v1[axis2];
  float t10 = v3[axis1] - v2[axis1];
  float t11 = v3[axis2] - v2[axis2];

  float detsh = (t00 * t11 - t10 * t01);
  detsh = (detsh != 0.0f) ? 1.0f / detsh : 0.0f;
  t00 *= detsh;
  t01 *= detsh;
  t10 *= detsh;
  t11 *= detsh;

  *u = (v3[axis1] - co[axis1]) * t11 - (v3[axis2] - co[axis2]) * t10;
  *v = (v3[axis2] - co[axis2]) * t00 - (v3[axis1] - co[axis1]) * t01;
  if (differentials) {
    *dx_u = dxco[axis1] * t11 - dxco[axis2] * t10;
    *dx_v = dxco[axis2] * t00 - dxco[axis1] * t01;
    *dy_u = dyco[axis1] * t11 - dyco[axis2] * t10;
    *dy_v = dyco[axis2] * t00 - dyco[axis1] * t01;
  }
}

/**
 * This function populates pixel_array and returns TRUE if things are correct
 */
static bool cast_ray_highpoly(BVHTreeFromMesh *treeData,
                              TriTessFace *triangle_low,
                              TriTessFace *triangles[],
                              BakePixel *pixel_array_low,
                              BakePixel *pixel_array,
                              const float mat_low[4][4],
                              BakeHighPolyData *highpoly,
                              const float co[3],
                              const float dir[3],
                              const int pixel_id,
                              const int tot_highpoly,
                              const float max_ray_distance)
{
  int i;
  int hit_mesh = -1;
  float hit_distance_squared = max_ray_distance * max_ray_distance;
  if (hit_distance_squared == 0.0f) {
    /* No ray distance set, use maximum. */
    hit_distance_squared = FLT_MAX;
  }

  BVHTreeRayHit *hits;
  hits = static_cast<BVHTreeRayHit *>(
      MEM_mallocN(sizeof(BVHTreeRayHit) * tot_highpoly, "Bake Highpoly to Lowpoly: BVH Rays"));

  for (i = 0; i < tot_highpoly; i++) {
    float co_high[3], dir_high[3];

    hits[i].index = -1;
    /* TODO: we should use FLT_MAX here, but sweep-sphere code isn't prepared for that. */
    hits[i].dist = BVH_RAYCAST_DIST_MAX;

    /* Transform the ray from the world space to the `highpoly` space. */
    mul_v3_m4v3(co_high, highpoly[i].imat, co);

    /* rotates */
    mul_v3_mat3_m4v3(dir_high, highpoly[i].imat, dir);
    normalize_v3(dir_high);

    /* cast ray */
    if (treeData[i].tree) {
      BLI_bvhtree_ray_cast(treeData[i].tree,
                           co_high,
                           dir_high,
                           0.0f,
                           &hits[i],
                           treeData[i].raycast_callback,
                           &treeData[i]);
    }

    if (hits[i].index != -1) {
      /* distance comparison in world space */
      float hit_world[3];
      mul_v3_m4v3(hit_world, highpoly[i].obmat, hits[i].co);
      float distance_squared = len_squared_v3v3(hit_world, co);

      if (distance_squared < hit_distance_squared) {
        hit_mesh = i;
        hit_distance_squared = distance_squared;
      }
    }
  }

  if (hit_mesh != -1) {
    int primitive_id_high = hits[hit_mesh].index;
    TriTessFace *triangle_high = &triangles[hit_mesh][primitive_id_high];
    BakePixel *pixel_low = &pixel_array_low[pixel_id];
    BakePixel *pixel_high = &pixel_array[pixel_id];

    pixel_high->primitive_id = primitive_id_high;
    pixel_high->object_id = hit_mesh;
    pixel_high->seed = pixel_id;

    /* ray direction in high poly object space */
    float dir_high[3];
    mul_v3_mat3_m4v3(dir_high, highpoly[hit_mesh].imat, dir);
    normalize_v3(dir_high);

    /* compute position differentials on low poly object */
    float duco_low[3], dvco_low[3], dxco[3], dyco[3];
    sub_v3_v3v3(duco_low, triangle_low->positions[0], triangle_low->positions[2]);
    sub_v3_v3v3(dvco_low, triangle_low->positions[1], triangle_low->positions[2]);

    mul_v3_v3fl(dxco, duco_low, pixel_low->du_dx);
    madd_v3_v3fl(dxco, dvco_low, pixel_low->dv_dx);
    mul_v3_v3fl(dyco, duco_low, pixel_low->du_dy);
    madd_v3_v3fl(dyco, dvco_low, pixel_low->dv_dy);

    /* transform from low poly to high poly object space */
    mul_mat3_m4_v3(mat_low, dxco);
    mul_mat3_m4_v3(mat_low, dyco);
    mul_mat3_m4_v3(highpoly[hit_mesh].imat, dxco);
    mul_mat3_m4_v3(highpoly[hit_mesh].imat, dyco);

    /* transfer position differentials */
    float tmp[3];
    mul_v3_v3fl(tmp, dir_high, 1.0f / dot_v3v3(dir_high, triangle_high->normal));
    madd_v3_v3fl(dxco, tmp, -dot_v3v3(dxco, triangle_high->normal));
    madd_v3_v3fl(dyco, tmp, -dot_v3v3(dyco, triangle_high->normal));

    /* compute barycentric differentials from position differentials */
    barycentric_differentials_from_position(hits[hit_mesh].co,
                                            triangle_high->positions[0],
                                            triangle_high->positions[1],
                                            triangle_high->positions[2],
                                            dxco,
                                            dyco,
                                            triangle_high->normal,
                                            true,
                                            &pixel_high->uv[0],
                                            &pixel_high->uv[1],
                                            &pixel_high->du_dx,
                                            &pixel_high->dv_dx,
                                            &pixel_high->du_dy,
                                            &pixel_high->dv_dy);

    /* verify we have valid uvs */
    BLI_assert(pixel_high->uv[0] >= -1e-3f && pixel_high->uv[1] >= -1e-3f &&
               pixel_high->uv[0] + pixel_high->uv[1] <= 1.0f + 1e-3f);
  }
  else {
    pixel_array[pixel_id].primitive_id = -1;
    pixel_array[pixel_id].object_id = -1;
    pixel_array[pixel_id].seed = 0;
  }

  MEM_freeN(hits);
  return hit_mesh != -1;
}

/**
 * This function populates an array of verts for the triangles of a mesh
 * Tangent and Normals are also stored
 */
static TriTessFace *mesh_calc_tri_tessface(Mesh *me, bool tangent, Mesh *me_eval)
{
  int i;

  const int tottri = poly_to_tri_count(me->totpoly, me->totloop);
  MLoopTri *looptri;
  TriTessFace *triangles;

  /* calculate normal for each polygon only once */
  uint mpoly_prev = UINT_MAX;
  float no[3];

  const float(*positions)[3] = BKE_mesh_vert_positions(me);
  const blender::Span<MPoly> polys = me->polys();
  const blender::Span<MLoop> loops = me->loops();

  looptri = static_cast<MLoopTri *>(MEM_mallocN(sizeof(*looptri) * tottri, __func__));
  triangles = static_cast<TriTessFace *>(MEM_callocN(sizeof(TriTessFace) * tottri, __func__));

  const float(*precomputed_normals)[3] = BKE_mesh_poly_normals_are_dirty(me) ?
                                             nullptr :
                                             BKE_mesh_poly_normals_ensure(me);
  const bool calculate_normal = precomputed_normals ? false : true;

  if (precomputed_normals != nullptr) {
    BKE_mesh_recalc_looptri_with_normals(loops.data(),
                                         polys.data(),
                                         positions,
                                         me->totloop,
                                         me->totpoly,
                                         looptri,
                                         precomputed_normals);
  }
  else {
    BKE_mesh_recalc_looptri(
        loops.data(), polys.data(), positions, me->totloop, me->totpoly, looptri);
  }

  const TSpace *tspace = nullptr;
  const float(*loop_normals)[3] = nullptr;
  if (tangent) {
    BKE_mesh_ensure_normals_for_display(me_eval);
    BKE_mesh_calc_normals_split(me_eval);
    BKE_mesh_calc_loop_tangents(me_eval, true, nullptr, 0);

    tspace = static_cast<const TSpace *>(CustomData_get_layer(&me_eval->ldata, CD_TANGENT));
    BLI_assert(tspace);

    loop_normals = static_cast<const float(*)[3]>(
        CustomData_get_layer(&me_eval->ldata, CD_NORMAL));
  }

  const float(*vert_normals)[3] = BKE_mesh_vert_normals_ensure(me);
  for (i = 0; i < tottri; i++) {
    const MLoopTri *lt = &looptri[i];
    const MPoly *mp = &polys[lt->poly];

    triangles[i].positions[0] = positions[loops[lt->tri[0]].v];
    triangles[i].positions[1] = positions[loops[lt->tri[1]].v];
    triangles[i].positions[2] = positions[loops[lt->tri[2]].v];
    triangles[i].vert_normals[0] = vert_normals[loops[lt->tri[0]].v];
    triangles[i].vert_normals[1] = vert_normals[loops[lt->tri[1]].v];
    triangles[i].vert_normals[2] = vert_normals[loops[lt->tri[2]].v];
    triangles[i].is_smooth = (mp->flag & ME_SMOOTH) != 0;

    if (tangent) {
      triangles[i].tspace[0] = &tspace[lt->tri[0]];
      triangles[i].tspace[1] = &tspace[lt->tri[1]];
      triangles[i].tspace[2] = &tspace[lt->tri[2]];
    }

    if (loop_normals) {
      triangles[i].loop_normal[0] = loop_normals[lt->tri[0]];
      triangles[i].loop_normal[1] = loop_normals[lt->tri[1]];
      triangles[i].loop_normal[2] = loop_normals[lt->tri[2]];
    }

    if (calculate_normal) {
      if (lt->poly != mpoly_prev) {
        BKE_mesh_calc_poly_normal(mp, &loops[mp->loopstart], positions, no);
        mpoly_prev = lt->poly;
      }
      copy_v3_v3(triangles[i].normal, no);
    }
    else {
      copy_v3_v3(triangles[i].normal, precomputed_normals[lt->poly]);
    }
  }

  MEM_freeN(looptri);

  return triangles;
}

bool RE_bake_pixels_populate_from_objects(Mesh *me_low,
                                          BakePixel pixel_array_from[],
                                          BakePixel pixel_array_to[],
                                          BakeHighPolyData highpoly[],
                                          const int tot_highpoly,
                                          const size_t pixels_num,
                                          const bool is_custom_cage,
                                          const float cage_extrusion,
                                          const float max_ray_distance,
                                          float mat_low[4][4],
                                          float mat_cage[4][4],
                                          Mesh *me_cage)
{
  size_t i;
  int primitive_id;
  float u, v;
  float imat_low[4][4];
  bool is_cage = me_cage != nullptr;
  bool result = true;

  Mesh *me_eval_low = nullptr;
  Mesh **me_highpoly;
  BVHTreeFromMesh *treeData;

  /* NOTE: all coordinates are in local space. */
  TriTessFace *tris_low = nullptr;
  TriTessFace *tris_cage = nullptr;
  TriTessFace **tris_high;

  /* Assume all low-poly tessfaces can be quads. */
  tris_high = MEM_cnew_array<TriTessFace *>(tot_highpoly, "MVerts Highpoly Mesh Array");

  /* Assume all high-poly tessfaces are triangles. */
  me_highpoly = static_cast<Mesh **>(
      MEM_mallocN(sizeof(Mesh *) * tot_highpoly, "Highpoly Derived Meshes"));
  treeData = MEM_cnew_array<BVHTreeFromMesh>(tot_highpoly, "Highpoly BVH Trees");

  if (!is_cage) {
    me_eval_low = BKE_mesh_copy_for_eval(me_low, false);
    tris_low = mesh_calc_tri_tessface(me_low, true, me_eval_low);
  }
  else if (is_custom_cage) {
    tris_low = mesh_calc_tri_tessface(me_low, false, nullptr);
    tris_cage = mesh_calc_tri_tessface(me_cage, false, nullptr);
  }
  else {
    tris_cage = mesh_calc_tri_tessface(me_cage, false, nullptr);
  }

  invert_m4_m4(imat_low, mat_low);

  for (i = 0; i < tot_highpoly; i++) {
    tris_high[i] = mesh_calc_tri_tessface(highpoly[i].me, false, nullptr);

    me_highpoly[i] = highpoly[i].me;

    if (BKE_mesh_runtime_looptri_len(me_highpoly[i]) != 0) {
      /* Create a BVH-tree for each `highpoly` object. */
      BKE_bvhtree_from_mesh_get(&treeData[i], me_highpoly[i], BVHTREE_FROM_LOOPTRI, 2);

      if (treeData[i].tree == nullptr) {
        printf("Baking: out of memory while creating BHVTree for object \"%s\"\n",
               highpoly[i].ob->id.name + 2);
        result = false;
        goto cleanup;
      }
    }
  }

  for (i = 0; i < pixels_num; i++) {
    float co[3];
    float dir[3];
    TriTessFace *tri_low;

    primitive_id = pixel_array_from[i].primitive_id;

    if (primitive_id == -1) {
      pixel_array_to[i].primitive_id = -1;
      continue;
    }

    u = pixel_array_from[i].uv[0];
    v = pixel_array_from[i].uv[1];

    /* calculate from low poly mesh cage */
    if (is_custom_cage) {
      calc_point_from_barycentric_cage(
          tris_low, tris_cage, mat_low, mat_cage, primitive_id, u, v, co, dir);
      tri_low = &tris_cage[primitive_id];
    }
    else if (is_cage) {
      calc_point_from_barycentric_extrusion(
          tris_cage, mat_low, imat_low, primitive_id, u, v, cage_extrusion, co, dir, true);
      tri_low = &tris_cage[primitive_id];
    }
    else {
      calc_point_from_barycentric_extrusion(
          tris_low, mat_low, imat_low, primitive_id, u, v, cage_extrusion, co, dir, false);
      tri_low = &tris_low[primitive_id];
    }

    /* cast ray */
    if (!cast_ray_highpoly(treeData,
                           tri_low,
                           tris_high,
                           pixel_array_from,
                           pixel_array_to,
                           mat_low,
                           highpoly,
                           co,
                           dir,
                           i,
                           tot_highpoly,
                           max_ray_distance)) {
      /* if it fails mask out the original pixel array */
      pixel_array_from[i].primitive_id = -1;
    }
  }

  /* garbage collection */
cleanup:
  for (i = 0; i < tot_highpoly; i++) {
    free_bvhtree_from_mesh(&treeData[i]);

    if (tris_high[i]) {
      MEM_freeN(tris_high[i]);
    }
  }

  MEM_freeN(tris_high);
  MEM_freeN(treeData);
  MEM_freeN(me_highpoly);

  if (me_eval_low) {
    BKE_id_free(nullptr, me_eval_low);
  }
  if (tris_low) {
    MEM_freeN(tris_low);
  }
  if (tris_cage) {
    MEM_freeN(tris_cage);
  }

  return result;
}

static void bake_differentials(BakeDataZSpan *bd,
                               const float *uv1,
                               const float *uv2,
                               const float *uv3)
{
  float A;

  /* assumes dPdu = P1 - P3 and dPdv = P2 - P3 */
  A = (uv2[0] - uv1[0]) * (uv3[1] - uv1[1]) - (uv3[0] - uv1[0]) * (uv2[1] - uv1[1]);

  if (fabsf(A) > FLT_EPSILON) {
    A = 0.5f / A;

    bd->du_dx = (uv2[1] - uv3[1]) * A;
    bd->dv_dx = (uv3[1] - uv1[1]) * A;

    bd->du_dy = (uv3[0] - uv2[0]) * A;
    bd->dv_dy = (uv1[0] - uv3[0]) * A;
  }
  else {
    bd->du_dx = bd->du_dy = 0.0f;
    bd->dv_dx = bd->dv_dy = 0.0f;
  }
}

void RE_bake_pixels_populate(Mesh *me,
                             BakePixel pixel_array[],
                             const size_t pixels_num,
                             const BakeTargets *targets,
                             const char *uv_layer)
{
  const float(*mloopuv)[2];
  if ((uv_layer == nullptr) || (uv_layer[0] == '\0')) {
    mloopuv = static_cast<const float(*)[2]>(CustomData_get_layer(&me->ldata, CD_PROP_FLOAT2));
  }
  else {
    int uv_id = CustomData_get_named_layer(&me->ldata, CD_PROP_FLOAT2, uv_layer);
    mloopuv = static_cast<const float(*)[2]>(
        CustomData_get_layer_n(&me->ldata, CD_PROP_FLOAT2, uv_id));
  }

  if (mloopuv == nullptr) {
    return;
  }

  BakeDataZSpan bd;
  bd.pixel_array = pixel_array;
  bd.zspan = MEM_cnew_array<ZSpan>(targets->images_num, "bake zspan");

  /* initialize all pixel arrays so we know which ones are 'blank' */
  for (int i = 0; i < pixels_num; i++) {
    pixel_array[i].primitive_id = -1;
    pixel_array[i].object_id = 0;
  }

  for (int i = 0; i < targets->images_num; i++) {
    zbuf_alloc_span(&bd.zspan[i], targets->images[i].width, targets->images[i].height);
  }

  const int tottri = poly_to_tri_count(me->totpoly, me->totloop);
  MLoopTri *looptri = static_cast<MLoopTri *>(MEM_mallocN(sizeof(*looptri) * tottri, __func__));

  const float(*positions)[3] = BKE_mesh_vert_positions(me);
  const blender::Span<MPoly> polys = me->polys();
  const blender::Span<MLoop> loops = me->loops();
  BKE_mesh_recalc_looptri(
      loops.data(), polys.data(), positions, me->totloop, me->totpoly, looptri);

  const int *material_indices = BKE_mesh_material_indices(me);
  const int materials_num = targets->materials_num;

  for (int i = 0; i < tottri; i++) {
    const MLoopTri *lt = &looptri[i];

    bd.primitive_id = i;

    /* Find images matching this material. */
    const int material_index = (material_indices && materials_num) ?
                                   clamp_i(material_indices[lt->poly], 0, materials_num - 1) :
                                   0;
    Image *image = targets->material_to_image[material_index];
    for (int image_id = 0; image_id < targets->images_num; image_id++) {
      BakeImage *bk_image = &targets->images[image_id];
      if (bk_image->image != image) {
        continue;
      }

      /* Compute triangle vertex UV coordinates. */
      float vec[3][2];
      for (int a = 0; a < 3; a++) {
        const float *uv = mloopuv[lt->tri[a]];

        /* NOTE(@ideasman42): workaround for pixel aligned UVs which are common and can screw
         * up our intersection tests where a pixel gets in between 2 faces or the middle of a quad,
         * camera aligned quads also have this problem but they are less common.
         * Add a small offset to the UVs, fixes bug #18685. */
        vec[a][0] = (uv[0] - bk_image->uv_offset[0]) * float(bk_image->width) - (0.5f + 0.001f);
        vec[a][1] = (uv[1] - bk_image->uv_offset[1]) * float(bk_image->height) - (0.5f + 0.002f);
      }

      /* Rasterize triangle. */
      bd.bk_image = bk_image;
      bake_differentials(&bd, vec[0], vec[1], vec[2]);
      zspan_scanconvert(
          &bd.zspan[image_id], (void *)&bd, vec[0], vec[1], vec[2], store_bake_pixel);
    }
  }

  for (int i = 0; i < targets->images_num; i++) {
    zbuf_free_span(&bd.zspan[i]);
  }

  MEM_freeN(looptri);
  MEM_freeN(bd.zspan);
}

/* ******************** NORMALS ************************ */

static void normal_compress(float out[3],
                            const float in[3],
                            const eBakeNormalSwizzle normal_swizzle[3])
{
  const int swizzle_index[6] = {
      0, /* R_BAKE_POSX */
      1, /* R_BAKE_POSY */
      2, /* R_BAKE_POSZ */
      0, /* R_BAKE_NEGX */
      1, /* R_BAKE_NEGY */
      2, /* R_BAKE_NEGZ */
  };
  const float swizzle_sign[6] = {
      +1.0f, /* R_BAKE_POSX */
      +1.0f, /* R_BAKE_POSY */
      +1.0f, /* R_BAKE_POSZ */
      -1.0f, /* R_BAKE_NEGX */
      -1.0f, /* R_BAKE_NEGY */
      -1.0f, /* R_BAKE_NEGZ */
  };

  int i;

  for (i = 0; i < 3; i++) {
    int index;
    float sign;

    sign = swizzle_sign[normal_swizzle[i]];
    index = swizzle_index[normal_swizzle[i]];

    /*
     * There is a small 1e-5f bias for precision issues. otherwise
     * we randomly get 127 or 128 for neutral colors in tangent maps.
     * we choose 128 because it is the convention flat color. *
     */

    out[i] = sign * in[index] / 2.0f + 0.5f + 1e-5f;
  }
}

void RE_bake_normal_world_to_tangent(const BakePixel pixel_array[],
                                     const size_t pixels_num,
                                     const int depth,
                                     float result[],
                                     Mesh *me,
                                     const eBakeNormalSwizzle normal_swizzle[3],
                                     float mat[4][4])
{
  size_t i;

  TriTessFace *triangles;

  Mesh *me_eval = BKE_mesh_copy_for_eval(me, false);

  triangles = mesh_calc_tri_tessface(me, true, me_eval);

  BLI_assert(pixels_num >= 3);

  for (i = 0; i < pixels_num; i++) {
    TriTessFace *triangle;
    float tangents[3][3];
    float normals[3][3];
    float signs[3];
    int j;

    float tangent[3];
    float normal[3];
    float binormal[3];
    float sign;
    float u, v, w;

    float tsm[3][3]; /* tangent space matrix */
    float itsm[3][3];

    size_t offset;
    float nor[3]; /* texture normal */

    bool is_smooth;

    int primitive_id = pixel_array[i].primitive_id;

    offset = i * depth;

    if (primitive_id == -1) {
      if (depth == 4) {
        copy_v4_fl4(&result[offset], 0.5f, 0.5f, 1.0f, 1.0f);
      }
      else {
        copy_v3_fl3(&result[offset], 0.5f, 0.5f, 1.0f);
      }
      continue;
    }

    triangle = &triangles[primitive_id];
    is_smooth = triangle->is_smooth;

    for (j = 0; j < 3; j++) {
      const TSpace *ts;

      if (is_smooth) {
        if (triangle->loop_normal[j]) {
          copy_v3_v3(normals[j], triangle->loop_normal[j]);
        }
        else {
          copy_v3_v3(normals[j], triangle->vert_normals[j]);
        }
      }

      ts = triangle->tspace[j];
      copy_v3_v3(tangents[j], ts->tangent);
      signs[j] = ts->sign;
    }

    u = pixel_array[i].uv[0];
    v = pixel_array[i].uv[1];
    w = 1.0f - u - v;

    /* normal */
    if (is_smooth) {
      interp_barycentric_tri_v3(normals, u, v, normal);
    }
    else {
      copy_v3_v3(normal, triangle->normal);
    }

    /* tangent */
    interp_barycentric_tri_v3(tangents, u, v, tangent);

    /* sign */
    /* The sign is the same at all face vertices for any non degenerate face.
     * Just in case we clamp the interpolated value though. */
    sign = (signs[0] * u + signs[1] * v + signs[2] * w) < 0 ? (-1.0f) : 1.0f;

    /* binormal */
    /* `B = sign * cross(N, T)` */
    cross_v3_v3v3(binormal, normal, tangent);
    mul_v3_fl(binormal, sign);

    /* populate tangent space matrix */
    copy_v3_v3(tsm[0], tangent);
    copy_v3_v3(tsm[1], binormal);
    copy_v3_v3(tsm[2], normal);

    /* texture values */
    copy_v3_v3(nor, &result[offset]);

    /* converts from world space to local space */
    mul_transposed_mat3_m4_v3(mat, nor);

    invert_m3_m3(itsm, tsm);
    mul_m3_v3(itsm, nor);
    normalize_v3(nor);

    /* save back the values */
    normal_compress(&result[offset], nor, normal_swizzle);
  }

  /* garbage collection */
  MEM_freeN(triangles);

  if (me_eval) {
    BKE_id_free(nullptr, me_eval);
  }
}

void RE_bake_normal_world_to_object(const BakePixel pixel_array[],
                                    const size_t pixels_num,
                                    const int depth,
                                    float result[],
                                    Object *ob,
                                    const eBakeNormalSwizzle normal_swizzle[3])
{
  size_t i;
  float iobmat[4][4];

  invert_m4_m4(iobmat, ob->object_to_world);

  for (i = 0; i < pixels_num; i++) {
    size_t offset;
    float nor[3];

    if (pixel_array[i].primitive_id == -1) {
      continue;
    }

    offset = i * depth;
    copy_v3_v3(nor, &result[offset]);

    /* rotates only without translation */
    mul_mat3_m4_v3(iobmat, nor);
    normalize_v3(nor);

    /* save back the values */
    normal_compress(&result[offset], nor, normal_swizzle);
  }
}

void RE_bake_normal_world_to_world(const BakePixel pixel_array[],
                                   const size_t pixels_num,
                                   const int depth,
                                   float result[],
                                   const eBakeNormalSwizzle normal_swizzle[3])
{
  size_t i;

  for (i = 0; i < pixels_num; i++) {
    size_t offset;
    float nor[3];

    if (pixel_array[i].primitive_id == -1) {
      continue;
    }

    offset = i * depth;
    copy_v3_v3(nor, &result[offset]);

    /* save back the values */
    normal_compress(&result[offset], nor, normal_swizzle);
  }
}

void RE_bake_ibuf_clear(Image *image, const bool is_tangent)
{
  ImBuf *ibuf;
  void *lock;

  const float vec_alpha[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  const float vec_solid[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  const float nor_alpha[4] = {0.5f, 0.5f, 1.0f, 0.0f};
  const float nor_solid[4] = {0.5f, 0.5f, 1.0f, 1.0f};

  ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
  BLI_assert(ibuf);

  if (is_tangent) {
    IMB_rectfill(ibuf, (ibuf->planes == R_IMF_PLANES_RGBA) ? nor_alpha : nor_solid);
  }
  else {
    IMB_rectfill(ibuf, (ibuf->planes == R_IMF_PLANES_RGBA) ? vec_alpha : vec_solid);
  }

  BKE_image_release_ibuf(image, ibuf, lock);
}

/* ************************************************************* */

int RE_pass_depth(const eScenePassType pass_type)
{
  /* IMB_buffer_byte_from_float assumes 4 channels
   * making it work for now - XXX */
  return 4;

  switch (pass_type) {
    case SCE_PASS_Z:
    case SCE_PASS_AO:
    case SCE_PASS_MIST: {
      return 1;
    }
    case SCE_PASS_UV: {
      return 2;
    }
    case SCE_PASS_COMBINED:
    case SCE_PASS_SHADOW:
    case SCE_PASS_POSITION:
    case SCE_PASS_NORMAL:
    case SCE_PASS_VECTOR:
    case SCE_PASS_INDEXOB: /* XXX double check */
    case SCE_PASS_EMIT:
    case SCE_PASS_ENVIRONMENT:
    case SCE_PASS_INDEXMA:
    case SCE_PASS_DIFFUSE_DIRECT:
    case SCE_PASS_DIFFUSE_INDIRECT:
    case SCE_PASS_DIFFUSE_COLOR:
    case SCE_PASS_GLOSSY_DIRECT:
    case SCE_PASS_GLOSSY_INDIRECT:
    case SCE_PASS_GLOSSY_COLOR:
    case SCE_PASS_TRANSM_DIRECT:
    case SCE_PASS_TRANSM_INDIRECT:
    case SCE_PASS_TRANSM_COLOR:
    case SCE_PASS_SUBSURFACE_DIRECT:
    case SCE_PASS_SUBSURFACE_INDIRECT:
    case SCE_PASS_SUBSURFACE_COLOR:
    default: {
      return 3;
    }
  }
}
