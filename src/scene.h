/* scene.h: choose scene implementation */
#ifndef ARO_SCENE_H
#define ARO_SCENE_H

#ifdef ARO_EFFECTS
#include <scenefx/types/fx/clipped_region.h>
#include <scenefx/types/wlr_scene.h>
#else
#include <wlr/types/wlr_scene.h>
#endif

#endif
