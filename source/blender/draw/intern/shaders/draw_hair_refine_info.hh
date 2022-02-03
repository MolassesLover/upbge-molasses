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
 * The Original Code is Copyright (C) 2022 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup draw
 */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(draw_hair_refine_compute)
    .local_group_size(1, 1)
    .storage_buf(0, Qualifier::WRITE, "vec4", "posTime[]")
    .sampler(0, ImageType::FLOAT_BUFFER, "hairPointBuffer")
    .sampler(1, ImageType::UINT_BUFFER, "hairStrandBuffer")
    .sampler(2, ImageType::UINT_BUFFER, "hairStrandSegBuffer")
    .push_constant(Type::VEC4, "hairDupliMatrix", 4)
    .push_constant(Type::BOOL, "hairCloseTip")
    .push_constant(Type::FLOAT, "hairRadShape")
    .push_constant(Type::FLOAT, "hairRadTip")
    .push_constant(Type::FLOAT, "hairRadRoot")
    .push_constant(Type::INT, "hairThicknessRes")
    .push_constant(Type::INT, "hairStrandsRes")
    .push_constant(Type::INT, "hairStrandOffset")
    .compute_source("common_hair_refine_comp.glsl")
    .define("HAIR_PHASE_SUBDIV")
    .do_static_compilation(true);
