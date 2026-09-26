/* extws: a bar's view of aro's workspaces; "expect STATE" checks it, "activate N" clicks one */
#include "common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct ws {
	struct ext_workspace_handle_v1 *h;
	char name[16];
	uint32_t state;
	int gone;
};

static struct ws wss[128];
static int nws;

static struct ws *find(struct ext_workspace_handle_v1 *h)
{
	for (int i = 0; i < nws; i++)
		if (wss[i].h == h)
			return &wss[i];
	return NULL;
}

static void w_id(void *d, struct ext_workspace_handle_v1 *h, const char *id)
{ (void)d; (void)h; (void)id; }
static void w_name(void *d, struct ext_workspace_handle_v1 *h, const char *name)
{ (void)d; struct ws *w = find(h); if (w) snprintf(w->name, sizeof w->name, "%s", name); }
static void w_coords(void *d, struct ext_workspace_handle_v1 *h, struct wl_array *a)
{ (void)d; (void)h; (void)a; }
static void w_state(void *d, struct ext_workspace_handle_v1 *h, uint32_t state)
{ (void)d; struct ws *w = find(h); if (w) w->state = state; }
static void w_caps(void *d, struct ext_workspace_handle_v1 *h, uint32_t caps)
{ (void)d; (void)h; (void)caps; }
static void w_removed(void *d, struct ext_workspace_handle_v1 *h)
{ (void)d; struct ws *w = find(h); if (w) w->gone = 1; }

static const struct ext_workspace_handle_v1_listener ws_listener = {
	w_id, w_name, w_coords, w_state, w_caps, w_removed,
};

static void m_group(void *d, struct ext_workspace_manager_v1 *m,
                    struct ext_workspace_group_handle_v1 *g)
{ (void)d; (void)m; (void)g; }
static void m_workspace(void *d, struct ext_workspace_manager_v1 *m,
                        struct ext_workspace_handle_v1 *h)
{
	(void)d; (void)m;
	if (nws == (int)(sizeof wss / sizeof wss[0]))
		fail("too many workspaces");
	wss[nws].h = h;
	ext_workspace_handle_v1_add_listener(h, &ws_listener, NULL);
	nws++;
}
static void m_done(void *d, struct ext_workspace_manager_v1 *m) { (void)d; (void)m; }
static void m_finished(void *d, struct ext_workspace_manager_v1 *m) { (void)d; (void)m; }

static const struct ext_workspace_manager_v1_listener mgr_listener = {
	m_group, m_workspace, m_done, m_finished,
};

/* "[1] 2 3 4": names in order, the active one in brackets */
static void describe(char *out, size_t len)
{
	out[0] = '\0';
	for (int i = 0; i < nws; i++) {
		if (wss[i].gone)
			continue;
		char one[24];
		snprintf(one, sizeof one,
		         wss[i].state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE ? "%s[%s]" : "%s%s",
		         out[0] ? " " : "", wss[i].name);
		strncat(out, one, len - strlen(out) - 1);
	}
}

int main(int argc, char **argv)
{
	if (argc < 3)
		fail("usage: extws expect STATE | extws activate NAME");

	struct globals g;
	connect_globals(&g);
	if (!g.workspaces)
		fail("aro does not offer ext-workspace");
	ext_workspace_manager_v1_add_listener(g.workspaces, &mgr_listener, NULL);
	roundtrip(&g);
	roundtrip(&g);

	if (!strcmp(argv[1], "activate")) {
		int found = 0;
		for (int i = 0; i < nws; i++)
			if (!wss[i].gone && !strcmp(wss[i].name, argv[2])) {
				ext_workspace_handle_v1_activate(wss[i].h);
				found = 1;
			}
		if (!found)
			fail("no workspace named %s", argv[2]);
		ext_workspace_manager_v1_commit(g.workspaces);
		roundtrip(&g);
		roundtrip(&g);
		return 0;
	}

	char got[512];
	describe(got, sizeof got);
	if (strcmp(got, argv[2]))
		fail("workspaces: expected \"%s\", got \"%s\"", argv[2], got);
	printf("workspaces: %s\n", got);
	return 0;
}
