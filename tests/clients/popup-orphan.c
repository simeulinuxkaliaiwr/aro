/* popup-orphan: a popup with no parent, the way layer-shell clients start a tooltip */
#include "common.h"

#include <stdio.h>

int main(void)
{
	struct globals g;
	connect_globals(&g);

	for (int i = 0; i < 3; i++) {
		struct wl_surface *surf = wl_compositor_create_surface(g.compositor);
		struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(g.wm, surf);
		struct xdg_positioner *pos = xdg_wm_base_create_positioner(g.wm);
		xdg_positioner_set_size(pos, 100, 30);
		xdg_positioner_set_anchor_rect(pos, 0, 0, 1, 1);
		struct xdg_popup *pop = xdg_surface_get_popup(xs, NULL, pos);
		roundtrip(&g);
		xdg_popup_destroy(pop);
		xdg_positioner_destroy(pos);
		xdg_surface_destroy(xs);
		wl_surface_destroy(surf);
		roundtrip(&g);
	}
	puts("aro survived three parentless popups");
	return 0;
}
