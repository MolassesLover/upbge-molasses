/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 */

/** \file blender/modifiers/intern/MOD_wireframe.c
 *  \ingroup modifiers
 */

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_utildefines.h"

#include "BKE_deform.h"
#include "BKE_mesh.h"

#include "MOD_modifiertypes.h"

#include "bmesh.h"
#include "tools/bmesh_wireframe.h"

static void initData(ModifierData *md)
{
	WireframeModifierData *wmd = (WireframeModifierData *)md;
	wmd->offset = 0.02f;
	wmd->flag = MOD_WIREFRAME_REPLACE | MOD_WIREFRAME_OFS_EVEN;
	wmd->crease_weight = 1.0f;
}

static CustomDataMask requiredDataMask(Object *UNUSED(ob), ModifierData *md)
{
	WireframeModifierData *wmd = (WireframeModifierData *)md;
	CustomDataMask dataMask = 0;

	/* ask for vertexgroups if we need them */
	if (wmd->defgrp_name[0]) dataMask |= CD_MASK_MDEFORMVERT;

	return dataMask;

}

static bool dependsOnNormals(ModifierData *UNUSED(md))
{
	return true;
}

static Mesh *WireframeModifier_do(WireframeModifierData *wmd, Object *ob, Mesh *mesh)
{
	Mesh *result;
	BMesh *bm;

	const int defgrp_index = defgroup_name_index(ob, wmd->defgrp_name);

	bm = BKE_mesh_to_bmesh_ex(
	        mesh,
	        &(struct BMeshCreateParams){0},
	        &(struct BMeshFromMeshParams){
	            .calc_face_normal = true,
	            .add_key_index = false,
	            .use_shapekey = true,
	            .active_shapekey = ob->shapenr,
	        });

	BM_mesh_wireframe(
	       bm,
	       wmd->offset, wmd->offset_fac, wmd->offset_fac_vg,
	       (wmd->flag & MOD_WIREFRAME_REPLACE) != 0,
	       (wmd->flag & MOD_WIREFRAME_BOUNDARY) != 0,
	       (wmd->flag & MOD_WIREFRAME_OFS_EVEN) != 0,
	       (wmd->flag & MOD_WIREFRAME_OFS_RELATIVE) != 0,
	       (wmd->flag & MOD_WIREFRAME_CREASE) != 0,
	       wmd->crease_weight,
	       defgrp_index,
	       (wmd->flag & MOD_WIREFRAME_INVERT_VGROUP) != 0,
	       wmd->mat_ofs,
	       MAX2(ob->totcol - 1, 0),
	       false);

	result = BKE_mesh_from_bmesh_nomain(bm, &(struct BMeshToMeshParams){0});
	BM_mesh_free(bm);

	result->runtime.cd_dirty_vert |= CD_MASK_NORMAL;

	return result;

}

static Mesh *applyModifier(
        ModifierData *md,
        const struct ModifierEvalContext *ctx,
        struct Mesh *mesh)
{
	return WireframeModifier_do((WireframeModifierData *)md, ctx->object, mesh);
}


ModifierTypeInfo modifierType_Wireframe = {
	/* name */              "Wireframe",
	/* structName */        "WireframeModifierData",
	/* structSize */        sizeof(WireframeModifierData),
	/* type */              eModifierTypeType_Constructive,
	/* flags */             eModifierTypeFlag_AcceptsMesh |
	                        eModifierTypeFlag_SupportsEditmode,

	/* copyData */          modifier_copyData_generic,

	/* deformVerts_DM */    NULL,
	/* deformMatrices_DM */ NULL,
	/* deformVertsEM_DM */  NULL,
	/* deformMatricesEM_DM*/NULL,
	/* applyModifier_DM */  NULL,
	/* applyModifierEM_DM */NULL,

	/* deformVerts */       NULL,
	/* deformMatrices */    NULL,
	/* deformVertsEM */     NULL,
	/* deformMatricesEM */  NULL,
	/* applyModifier */     applyModifier,
	/* applyModifierEM */   NULL,

	/* initData */          initData,
	/* requiredDataMask */  requiredDataMask,
	/* freeData */          NULL,
	/* isDisabled */        NULL,
	/* updateDepgraph */    NULL,
	/* dependsOnTime */     NULL,
	/* dependsOnNormals */  dependsOnNormals,
	/* foreachObjectLink */ NULL,
	/* foreachIDLink */     NULL,
	/* foreachTexLink */    NULL,
};
