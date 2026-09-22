/*
 * aro — scene.h
 *
 * One place decides which scene-graph implementation the project uses.
 *
 * SceneFX is a drop-in replacement for wlroots' scene API: same types, same
 * functions, plus rounded corners, shadows and blur. It reuses wlroots' own
 * include guard (WLR_TYPES_WLR_SCENE_H), so whichever header is included
 * first wins and the other becomes a no-op.
 *
 * That makes the choice fragile if it is scattered around, hence this file.
 * Every translation unit includes THIS instead of either scene header, and
 * includes it before any wlroots header.
 */
#ifndef ARO_SCENE_H
#define ARO_SCENE_H

#ifdef ARO_EFFECTS
#include <scenefx/types/fx/clipped_region.h>
#include <scenefx/types/wlr_scene.h>
#else
#include <wlr/types/wlr_scene.h>
#endif

#endif
