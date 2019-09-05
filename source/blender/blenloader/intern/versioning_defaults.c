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
 * \ingroup blenloader
 */

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_string.h"
#include "BLI_system.h"

#include "DNA_camera_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_appdir.h"
#include "BKE_colortools.h"
#include "BKE_layer.h"
#include "BKE_library.h"
#include "BKE_main.h"
#include "BKE_node.h"
#include "BKE_screen.h"
#include "BKE_workspace.h"

#include "BLO_readfile.h"

/**
 * Rename if the ID doesn't exist.
 */
static ID *rename_id_for_versioning(Main *bmain,
                                    const short id_type,
                                    const char *name_src,
                                    const char *name_dst)
{
  /* We can ignore libraries */
  ListBase *lb = which_libbase(bmain, id_type);
  ID *id = NULL;
  for (ID *idtest = lb->first; idtest; idtest = idtest->next) {
    if (idtest->lib == NULL) {
      if (STREQ(idtest->name + 2, name_src)) {
        id = idtest;
      }
      if (STREQ(idtest->name + 2, name_dst)) {
        return NULL;
      }
    }
  }
  if (id != NULL) {
    BLI_strncpy(id->name + 2, name_dst, sizeof(id->name) - 2);
    /* We know it's unique, this just sorts. */
    BLI_libblock_ensure_unique_name(bmain, id->name);
  }
  return id;
}

static bool blo_is_builtin_template(const char *app_template)
{
  /* For all builtin templates shipped with Blender. */
  return (!app_template ||
          STR_ELEM(app_template, "2D_Animation", "Sculpting", "VFX", "Video_Editing"));
}

static void blo_update_defaults_screen(bScreen *screen,
                                       const char *app_template,
                                       const char *workspace_name)
{
  /* For all app templates. */
  for (ScrArea *sa = screen->areabase.first; sa; sa = sa->next) {
    for (ARegion *ar = sa->regionbase.first; ar; ar = ar->next) {
      /* Some toolbars have been saved as initialized,
       * we don't want them to have odd zoom-level or scrolling set, see: T47047 */
      if (ELEM(ar->regiontype, RGN_TYPE_UI, RGN_TYPE_TOOLS, RGN_TYPE_TOOL_PROPS)) {
        ar->v2d.flag &= ~V2D_IS_INITIALISED;
      }
    }

    /* Set default folder. */
    for (SpaceLink *sl = sa->spacedata.first; sl; sl = sl->next) {
      if (sl->spacetype == SPACE_FILE) {
        SpaceFile *sfile = (SpaceFile *)sl;
        if (sfile->params) {
          const char *dir_default = BKE_appdir_folder_default();
          if (dir_default) {
            STRNCPY(sfile->params->dir, dir_default);
            sfile->params->file[0] = '\0';
          }
        }
      }
    }
  }

  /* For builtin templates only. */
  if (!blo_is_builtin_template(app_template)) {
    return;
  }

  for (ScrArea *sa = screen->areabase.first; sa; sa = sa->next) {
    for (ARegion *ar = sa->regionbase.first; ar; ar = ar->next) {
      /* Remove all stored panels, we want to use defaults
       * (order, open/closed) as defined by UI code here! */
      BKE_area_region_panels_free(&ar->panels);
      BLI_freelistN(&ar->panels_category_active);

      /* Reset size so it uses consistent defaults from the region types. */
      ar->sizex = 0;
      ar->sizey = 0;
    }

    if (sa->spacetype == SPACE_IMAGE) {
      if (STREQ(workspace_name, "UV Editing")) {
        SpaceImage *sima = sa->spacedata.first;
        if (sima->mode == SI_MODE_VIEW) {
          sima->mode = SI_MODE_UV;
        }
      }
    }
    else if (sa->spacetype == SPACE_ACTION) {
      /* Show marker lines, hide channels and collapse summary in timelines. */
      SpaceAction *saction = sa->spacedata.first;
      saction->flag |= SACTION_SHOW_MARKER_LINES;

      if (saction->mode == SACTCONT_TIMELINE) {
        saction->ads.flag |= ADS_FLAG_SUMMARY_COLLAPSED;

        for (ARegion *ar = sa->regionbase.first; ar; ar = ar->next) {
          if (ar->regiontype == RGN_TYPE_CHANNELS) {
            ar->flag |= RGN_FLAG_HIDDEN;
          }
        }
      }
    }
    else if (sa->spacetype == SPACE_GRAPH) {
      SpaceGraph *sipo = sa->spacedata.first;
      sipo->flag |= SIPO_MARKER_LINES;
    }
    else if (sa->spacetype == SPACE_NLA) {
      SpaceNla *snla = sa->spacedata.first;
      snla->flag |= SNLA_SHOW_MARKER_LINES;
    }
    else if (sa->spacetype == SPACE_TEXT) {
      /* Show syntax and line numbers in Script workspace text editor. */
      SpaceText *stext = sa->spacedata.first;
      stext->showsyntax = true;
      stext->showlinenrs = true;
    }
    else if (sa->spacetype == SPACE_VIEW3D) {
      View3D *v3d = sa->spacedata.first;
      /* Screen space cavity by default for faster performance. */
      v3d->shading.cavity_type = V3D_SHADING_CAVITY_CURVATURE;
      v3d->shading.flag |= V3D_SHADING_SPECULAR_HIGHLIGHT;
      v3d->overlay.texture_paint_mode_opacity = 1.0f;
      v3d->overlay.weight_paint_mode_opacity = 1.0f;
      v3d->overlay.vertex_paint_mode_opacity = 1.0f;
      /* Use dimmed selected edges. */
      v3d->overlay.edit_flag &= ~V3D_OVERLAY_EDIT_EDGES;
      /* grease pencil settings */
      v3d->vertex_opacity = 1.0f;
      v3d->gp_flag |= V3D_GP_SHOW_EDIT_LINES;
      /* Remove dither pattern in wireframe mode. */
      v3d->shading.xray_alpha_wire = 0.0f;
      /* Skip startups that use the viewport color by default. */
      if (v3d->shading.background_type != V3D_SHADING_BACKGROUND_VIEWPORT) {
        copy_v3_fl(v3d->shading.background_color, 0.05f);
      }
    }
    else if (sa->spacetype == SPACE_CLIP) {
      SpaceClip *sclip = sa->spacedata.first;
      sclip->around = V3D_AROUND_CENTER_MEDIAN;
    }
  }

  /* Show toopbar for sculpt/paint modes. */
  const bool show_tool_header = STR_ELEM(
      workspace_name, "Sculpting", "Texture Paint", "2D Animation", "2D Full Canvas");

  if (show_tool_header) {
    for (ScrArea *sa = screen->areabase.first; sa; sa = sa->next) {
      for (SpaceLink *sl = sa->spacedata.first; sl; sl = sl->next) {
        ListBase *regionbase = (sl == sa->spacedata.first) ? &sa->regionbase : &sl->regionbase;
        for (ARegion *ar = regionbase->first; ar; ar = ar->next) {
          if (ar->regiontype == RGN_TYPE_TOOL_HEADER) {
            ar->flag &= ~(RGN_FLAG_HIDDEN | RGN_FLAG_HIDDEN_BY_USER);
          }
        }
      }
    }
  }
}

void BLO_update_defaults_workspace(WorkSpace *workspace, const char *app_template)
{
  ListBase *layouts = BKE_workspace_layouts_get(workspace);
  for (WorkSpaceLayout *layout = layouts->first; layout; layout = layout->next) {
    if (layout->screen) {
      blo_update_defaults_screen(layout->screen, app_template, workspace->id.name + 2);
    }
  }

  if (blo_is_builtin_template(app_template)) {
    /* Clear all tools to use default options instead, ignore the tool saved in the file. */
    while (!BLI_listbase_is_empty(&workspace->tools)) {
      BKE_workspace_tool_remove(workspace, workspace->tools.first);
    }

    /* For 2D animation template. */
    if (STREQ(workspace->id.name + 2, "Drawing")) {
      workspace->object_mode = OB_MODE_PAINT_GPENCIL;
    }
  }
}

static void blo_update_defaults_scene(Main *bmain, Scene *scene)
{
  BLI_strncpy(scene->r.engine, RE_engine_id_BLENDER_EEVEE, sizeof(scene->r.engine));

  scene->r.cfra = 1.0f;
  scene->r.displaymode = R_OUTPUT_WINDOW;

  /* Don't enable compositing nodes. */
  if (scene->nodetree) {
    ntreeFreeNestedTree(scene->nodetree);
    MEM_freeN(scene->nodetree);
    scene->nodetree = NULL;
    scene->use_nodes = false;
  }

  /* Rename render layers. */
  BKE_view_layer_rename(bmain, scene, scene->view_layers.first, "View Layer");

  /* New EEVEE defaults. */
  scene->eevee.bloom_intensity = 0.05f;
  scene->eevee.bloom_clamp = 0.0f;
  scene->eevee.motion_blur_shutter = 0.5f;

  copy_v3_v3(scene->display.light_direction, (float[3]){M_SQRT1_3, M_SQRT1_3, M_SQRT1_3});
  copy_v2_fl2(scene->safe_areas.title, 0.1f, 0.05f);
  copy_v2_fl2(scene->safe_areas.action, 0.035f, 0.035f);

  /* Change default cubemap quality. */
  scene->eevee.gi_filter_quality = 3.0f;

  /* Enable Soft Shadows by default. */
  scene->eevee.flag |= SCE_EEVEE_SHADOW_SOFT;

  /* Be sure curfalloff and primitive are initializated */
  ToolSettings *ts = scene->toolsettings;
  if (ts->gp_sculpt.cur_falloff == NULL) {
    ts->gp_sculpt.cur_falloff = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
    CurveMapping *gp_falloff_curve = ts->gp_sculpt.cur_falloff;
    BKE_curvemapping_initialize(gp_falloff_curve);
    BKE_curvemap_reset(gp_falloff_curve->cm,
                       &gp_falloff_curve->clipr,
                       CURVE_PRESET_GAUSS,
                       CURVEMAP_SLOPE_POSITIVE);
  }
  if (ts->gp_sculpt.cur_primitive == NULL) {
    ts->gp_sculpt.cur_primitive = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
    CurveMapping *gp_primitive_curve = ts->gp_sculpt.cur_primitive;
    BKE_curvemapping_initialize(gp_primitive_curve);
    BKE_curvemap_reset(gp_primitive_curve->cm,
                       &gp_primitive_curve->clipr,
                       CURVE_PRESET_BELL,
                       CURVEMAP_SLOPE_POSITIVE);
  }

  /* Correct default startup UV's. */
  Mesh *me = BLI_findstring(&bmain->meshes, "Cube", offsetof(ID, name) + 2);
  if (me && (me->totloop == 24) && (me->mloopuv != NULL)) {
    const float uv_values[24][2] = {
        {0.625, 0.50}, {0.875, 0.50}, {0.875, 0.75}, {0.625, 0.75}, {0.375, 0.75}, {0.625, 0.75},
        {0.625, 1.00}, {0.375, 1.00}, {0.375, 0.00}, {0.625, 0.00}, {0.625, 0.25}, {0.375, 0.25},
        {0.125, 0.50}, {0.375, 0.50}, {0.375, 0.75}, {0.125, 0.75}, {0.375, 0.50}, {0.625, 0.50},
        {0.625, 0.75}, {0.375, 0.75}, {0.375, 0.25}, {0.625, 0.25}, {0.625, 0.50}, {0.375, 0.50},
    };
    for (int i = 0; i < ARRAY_SIZE(uv_values); i++) {
      copy_v2_v2(me->mloopuv[i].uv, uv_values[i]);
    }
  }
}

/**
 * Update defaults in startup.blend, without having to save and embed the file.
 * This function can be emptied each time the startup.blend is updated. */
void BLO_update_defaults_startup_blend(Main *bmain, const char *app_template)
{
  /* For all app templates. */
  for (WorkSpace *workspace = bmain->workspaces.first; workspace; workspace = workspace->id.next) {
    BLO_update_defaults_workspace(workspace, app_template);
  }

  /* For builtin templates only. */
  if (!blo_is_builtin_template(app_template)) {
    return;
  }

  /* Workspaces. */
  wmWindow *win = ((wmWindowManager *)bmain->wm.first)->windows.first;
  for (WorkSpace *workspace = bmain->workspaces.first; workspace; workspace = workspace->id.next) {
    WorkSpaceLayout *layout = BKE_workspace_hook_layout_for_workspace_get(win->workspace_hook,
                                                                          workspace);

    /* Name all screens by their workspaces (avoids 'Default.###' names). */
    /* Default only has one window. */
    if (layout->screen) {
      bScreen *screen = layout->screen;
      BLI_strncpy(screen->id.name + 2, workspace->id.name + 2, sizeof(screen->id.name) - 2);
      BLI_libblock_ensure_unique_name(bmain, screen->id.name);
    }

    /* For some reason we have unused screens, needed until re-saving.
     * Clear unused layouts because they're visible in the outliner & Python API. */
    LISTBASE_FOREACH_MUTABLE (WorkSpaceLayout *, layout_iter, &workspace->layouts) {
      if (layout != layout_iter) {
        BKE_workspace_layout_remove(bmain, workspace, layout_iter);
      }
    }
  }

  /* Scenes */
  for (Scene *scene = bmain->scenes.first; scene; scene = scene->id.next) {
    blo_update_defaults_scene(bmain, scene);

    if (app_template && STREQ(app_template, "Video_Editing")) {
      /* Filmic is too slow, use standard until it is optimized. */
      STRNCPY(scene->view_settings.view_transform, "Standard");
      STRNCPY(scene->view_settings.look, "None");
    }
    else {
      /* AV Sync break physics sim caching, disable until that is fixed. */
      scene->audio.flag &= ~AUDIO_SYNC;
      scene->flag &= ~SCE_FRAME_DROP;
    }
  }

  /* Objects */
  rename_id_for_versioning(bmain, ID_OB, "Lamp", "Light");
  rename_id_for_versioning(bmain, ID_LA, "Lamp", "Light");

  if (app_template && STREQ(app_template, "2D_Animation")) {
    for (Object *object = bmain->objects.first; object; object = object->id.next) {
      if (object->type == OB_GPENCIL) {
        /* Set grease pencil object in drawing mode */
        bGPdata *gpd = (bGPdata *)object->data;
        object->mode = OB_MODE_PAINT_GPENCIL;
        gpd->flag |= GP_DATA_STROKE_PAINTMODE;
        break;
      }
    }
  }

  for (Mesh *mesh = bmain->meshes.first; mesh; mesh = mesh->id.next) {
    /* Match default for new meshes. */
    mesh->smoothresh = DEG2RADF(30);
  }

  for (Camera *camera = bmain->cameras.first; camera; camera = camera->id.next) {
    /* Initialize to a useful value. */
    camera->dof.focus_distance = 10.0f;
    camera->dof.aperture_fstop = 2.8f;
  }

  for (Light *light = bmain->lights.first; light; light = light->id.next) {
    /* Fix lights defaults. */
    light->clipsta = 0.05f;
    light->att_dist = 40.0f;
  }

  /* Materials */
  for (Material *ma = bmain->materials.first; ma; ma = ma->id.next) {
    /* Update default material to be a bit more rough. */
    ma->roughness = 0.4f;

    if (ma->nodetree) {
      for (bNode *node = ma->nodetree->nodes.first; node; node = node->next) {
        if (node->type == SH_NODE_BSDF_PRINCIPLED) {
          bNodeSocket *roughness_socket = nodeFindSocket(node, SOCK_IN, "Roughness");
          bNodeSocketValueFloat *roughness_data = roughness_socket->default_value;
          roughness_data->value = 0.4f;
        }
      }
    }
  }

  /* Brushes */
  {
    /* Enable for UV sculpt (other brush types will be created as needed),
     * without this the grab brush will be active but not selectable from the list. */
    Brush *brush = BLI_findstring(&bmain->brushes, "Grab", offsetof(ID, name) + 2);
    if (brush) {
      brush->ob_mode |= OB_MODE_EDIT;
    }
  }

  for (Brush *brush = bmain->brushes.first; brush; brush = brush->id.next) {
    brush->blur_kernel_radius = 2;
  }

  {
    /* Change the spacing of the Smear brush to 3.0% */
    Brush *brush = BLI_findstring(&bmain->brushes, "Smear", offsetof(ID, name) + 2);
    if (brush) {
      brush->spacing = 3.0;
    }
  }

  /*********************Game engine transition*********************/
  // WARNING: ALWAYS KEEP THIS IN BLO_update_defaults_startup_blend
  for (Scene *sce = bmain->scenes.first; sce; sce = sce->id.next) {
    /* game data */
    sce->gm.stereoflag = STEREO_NOSTEREO;
    sce->gm.stereomode = STEREO_ANAGLYPH;
    sce->gm.eyeseparation = 0.10;
    sce->gm.xplay = 640;
    sce->gm.yplay = 480;
    sce->gm.freqplay = 60;
    sce->gm.depth = 32;
    sce->gm.gravity = 9.8f;
    sce->gm.physicsEngine = WOPHY_BULLET;
    //sce->gm.mode = WO_ACTIVITY_CULLING | WO_DBVT_CULLING;
    sce->gm.occlusionRes = 128;
    sce->gm.ticrate = 60;
    sce->gm.maxlogicstep = 5;
    sce->gm.physubstep = 1;
    sce->gm.maxphystep = 5;
    //sce->gm.timeScale = 1.0f;
    sce->gm.lineardeactthreshold = 0.8f;
    sce->gm.angulardeactthreshold = 1.0f;
    sce->gm.deactivationtime = 0.0f;

    sce->gm.obstacleSimulation = OBSTSIMULATION_NONE;
    sce->gm.levelHeight = 2.f;

    sce->gm.recastData.cellsize = 0.3f;
    sce->gm.recastData.cellheight = 0.2f;
    sce->gm.recastData.agentmaxslope = M_PI_4;
    sce->gm.recastData.agentmaxclimb = 0.9f;
    sce->gm.recastData.agentheight = 2.0f;
    sce->gm.recastData.agentradius = 0.6f;
    sce->gm.recastData.edgemaxlen = 12.0f;
    sce->gm.recastData.edgemaxerror = 1.3f;
    sce->gm.recastData.regionminsize = 8.f;
    sce->gm.recastData.regionmergesize = 20.f;
    sce->gm.recastData.vertsperpoly = 6;
    sce->gm.recastData.detailsampledist = 6.0f;
    sce->gm.recastData.detailsamplemaxerror = 1.0f;

    sce->gm.exitkey = 218; // Blender key code for ESC

    sce->gm.flag |= GAME_USE_UNDO;

    //sce->gm.pythonkeys[0] = LEFTCTRLKEY;
    //sce->gm.pythonkeys[1] = LEFTSHIFTKEY;
    //sce->gm.pythonkeys[2] = LEFTALTKEY;
    //sce->gm.pythonkeys[3] = TKEY;
  }
  for (Object *ob = bmain->objects.first; ob; ob = ob->id.next) {
    /* Game engine defaults*/
    ob->mass = ob->inertia = 1.0f;
    ob->formfactor = 0.4f;
    ob->damping = 0.04f;
    ob->rdamping = 0.1f;
    ob->anisotropicFriction[0] = 1.0f;
    ob->anisotropicFriction[1] = 1.0f;
    ob->anisotropicFriction[2] = 1.0f;
    ob->gameflag = OB_PROP | OB_COLLISION;
    ob->gameflag2 = 0;
    ob->margin = 0.04f;
    ob->friction = 0.5;
    ob->init_state = 1;
    ob->state = 1;
    ob->obstacleRad = 1.0f;
    ob->step_height = 0.15f;
    ob->jump_speed = 10.0f;
    ob->fall_speed = 55.0f;
    ob->max_jumps = 1;
    //ob->max_slope = M_PI_2;
    ob->col_group = 0x01;
    ob->col_mask = 0xffff;
    ob->preview = NULL;
    ob->duplicator_visibility_flag = OB_DUPLI_FLAG_VIEWPORT | OB_DUPLI_FLAG_RENDER;
  }
  /***********************End of Game engine transition**********************/
}
