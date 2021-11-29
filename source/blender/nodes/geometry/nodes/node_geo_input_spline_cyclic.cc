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

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_input_spline_cyclic_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Bool>(N_("Cyclic")).field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<bool> cyclic_field = AttributeFieldInput::Create<bool>("cyclic");
  params.set_output("Cyclic", std::move(cyclic_field));
}

}  // namespace blender::nodes::node_geo_input_spline_cyclic_cc

void register_node_type_geo_input_spline_cyclic()
{
  namespace file_ns = blender::nodes::node_geo_input_spline_cyclic_cc;

  static bNodeType ntype;

  geo_node_type_base(
      &ntype, GEO_NODE_INPUT_SPLINE_CYCLIC, "Is Spline Cyclic", NODE_CLASS_INPUT, 0);
  ntype.geometry_node_execute = file_ns::node_geo_exec;
  ntype.declare = file_ns::node_declare;
  nodeRegisterType(&ntype);
}