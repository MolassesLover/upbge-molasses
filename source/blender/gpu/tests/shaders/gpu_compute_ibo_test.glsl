/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

void main()
{
  int store_index = int(gl_GlobalInvocationID.x);
  out_indices[store_index] = store_index;
}
