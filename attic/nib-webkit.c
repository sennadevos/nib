/*
 * nib - a minimal browser with a real engine.
 *
 * Chrome-less WebKitGTK 6 (GTK4) browser: no toolbar, no menus, no buttons.
 * One hidden command bar, keyboard driven. The backend is stock WebKit,
 * multiprocess and sandboxed, with persistent cookies and cache.
 */

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <string.h>

#define APP_ID   "local.nib"
#define APP_NAME "nib"

typedef enum {
	BAR_HIDDEN,
	BAR_OPEN,
	BAR_FIND,
} BarMode;

typedef struct {
	GtkApplication        *app;
	GtkWindow             *win;
	GtkNotebook           *nb;
	GtkEntry              *bar;
	GtkLabel              *status;
	GtkProgressBar        *prog;
	BarMode                mode;
	gboolean               pending_g;   /* saw 'g', waiting for the second */
	WebKitWebContext      *ctx;
	WebKitNetworkSession  *session;
	WebKitSettings        *settings;
} Browser;

#define SCROLL_STEP 64   /* px per h/j/k/l */

static WebKitWebView *tab_new(Browser *b, const char *uri, gboolean focus);
static WebKitWebView *tab_adopt(Browser *b, WebKitWebView *view, gboolean focus);

/* ------------------------------------------------------------------ utils */

static const char *
env_or(const char *key, const char *fallback)
{
	const char *v = g_getenv(key);
	return (v && *v) ? v : fallback;
}

static void
status_set(Browser *b, const char *fmt, ...)
{
	if (!fmt) {
		gtk_label_set_text(b->status, "");
		gtk_widget_set_visible(GTK_WIDGET(b->status), FALSE);
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	char *s = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	gtk_label_set_text(b->status, s);
	gtk_widget_set_visible(GTK_WIDGET(b->status), TRUE);
	g_free(s);
}

/*
 * Turn whatever the user typed into something loadable. Explicit schemes and
 * filesystem paths pass through; a bare "host.tld" or "localhost:8080" gets
 * https://; anything else is a search query.
 */
static char *
resolve_input(const char *in)
{
	char *s = g_strstrip(g_strdup(in));

	if (!*s) {
		g_free(s);
		return NULL;
	}
	if (strstr(s, "://") || g_str_has_prefix(s, "about:") ||
	    g_str_has_prefix(s, "data:") || g_str_has_prefix(s, "mailto:"))
		return s;

	if (*s == '/' || *s == '~' || g_str_has_prefix(s, "./")) {
		char *exp = (*s == '~') ? g_build_filename(g_get_home_dir(), s + 1, NULL)
		                        : g_strdup(s);
		char *abs = g_canonicalize_filename(exp, NULL);
		char *uri = g_filename_to_uri(abs, NULL, NULL);
		g_free(s); g_free(exp); g_free(abs);
		return uri ? uri : g_strdup("about:blank");
	}

	/* host-like: no spaces, and either a dotted label or an explicit port */
	if (!strchr(s, ' ')) {
		const char *dot = strchr(s, '.');
		gboolean dotted = dot && dot != s && *(dot + 1) && *(dot + 1) != '.';
		gboolean ported = strchr(s, ':') != NULL;
		if (dotted || ported || g_str_has_prefix(s, "localhost")) {
			char *uri = g_strconcat("https://", s, NULL);
			g_free(s);
			return uri;
		}
	}

	char *esc = g_uri_escape_string(s, NULL, TRUE);
	char *uri = g_strdup_printf(env_or("NIB_SEARCH",
	    "https://duckduckgo.com/?q=%s"), esc);
	g_free(s); g_free(esc);
	return uri;
}

static WebKitWebView *
current_view(Browser *b)
{
	int page = gtk_notebook_get_current_page(b->nb);
	if (page < 0)
		return NULL;
	return WEBKIT_WEB_VIEW(gtk_notebook_get_nth_page(b->nb, page));
}

static void
bar_show(Browser *b, BarMode mode, const char *text)
{
	b->mode = mode;
	gtk_editable_set_text(GTK_EDITABLE(b->bar), text ? text : "");
	gtk_entry_set_placeholder_text(b->bar,
	    mode == BAR_FIND ? "find" : "open");
	gtk_widget_set_visible(GTK_WIDGET(b->bar), TRUE);
	gtk_widget_grab_focus(GTK_WIDGET(b->bar));
	gtk_editable_select_region(GTK_EDITABLE(b->bar), 0, -1);
}

static void
bar_hide(Browser *b)
{
	b->mode = BAR_HIDDEN;
	gtk_widget_set_visible(GTK_WIDGET(b->bar), FALSE);
	WebKitWebView *v = current_view(b);
	if (v)
		gtk_widget_grab_focus(GTK_WIDGET(v));
}

/* --------------------------------------------------------------- chrome-ish */

static void
refresh_chrome(Browser *b, WebKitWebView *v)
{
	if (v != current_view(b))
		return;

	const char *title = webkit_web_view_get_title(v);
	const char *uri = webkit_web_view_get_uri(v);
	if (!title || !*title)
		title = uri;
	char *t = g_strdup_printf("%s — %s", title ? title : "blank", APP_NAME);
	gtk_window_set_title(b->win, t);
	g_free(t);

	double p = webkit_web_view_get_estimated_load_progress(v);
	gboolean loading = webkit_web_view_is_loading(v);
	gtk_progress_bar_set_fraction(b->prog, loading ? p : 0.0);
	gtk_widget_set_visible(GTK_WIDGET(b->prog), loading);
}

static void
tab_label_update(Browser *b, WebKitWebView *v)
{
	GtkLabel *lbl = g_object_get_data(G_OBJECT(v), "nib-tab-label");
	if (!lbl)
		return;
	const char *title = webkit_web_view_get_title(v);
	if (!title || !*title)
		title = webkit_web_view_get_uri(v);
	gtk_label_set_text(lbl, (title && *title) ? title : "blank");
	gtk_widget_set_tooltip_text(GTK_WIDGET(lbl), title ? title : "");
}

static void
tabs_sync(Browser *b)
{
	int n = gtk_notebook_get_n_pages(b->nb);
	gtk_notebook_set_show_tabs(b->nb, n > 1);
}

/* ----------------------------------------------------------- view callbacks */

static void
on_notify_title(WebKitWebView *v, GParamSpec *ps, Browser *b)
{
	tab_label_update(b, v);
	refresh_chrome(b, v);
}

static void
on_notify_progress(WebKitWebView *v, GParamSpec *ps, Browser *b)
{
	refresh_chrome(b, v);
}

static void
on_load_changed(WebKitWebView *v, WebKitLoadEvent ev, Browser *b)
{
	if (ev == WEBKIT_LOAD_COMMITTED || ev == WEBKIT_LOAD_FINISHED) {
		tab_label_update(b, v);
		refresh_chrome(b, v);
	}
	if (ev == WEBKIT_LOAD_FINISHED && v == current_view(b))
		status_set(b, NULL);
}

static gboolean
on_load_failed(WebKitWebView *v, WebKitLoadEvent ev, const char *uri,
               GError *err, Browser *b)
{
	if (err->code == WEBKIT_NETWORK_ERROR_CANCELLED)
		return FALSE;
	if (v == current_view(b))
		status_set(b, "load failed: %s", err->message);
	return FALSE;
}

static gboolean
on_tls_error(WebKitWebView *v, const char *failing_uri,
             GTlsCertificate *cert, GTlsCertificateFlags errs, Browser *b)
{
	/* Never silently accept a bad certificate; report and stop. */
	if (v == current_view(b))
		status_set(b, "TLS error (0x%x) for %s — not loaded",
		    (unsigned)errs, failing_uri);
	return TRUE;
}

static void
on_mouse_target(WebKitWebView *v, WebKitHitTestResult *hit, guint mods,
                Browser *b)
{
	if (v != current_view(b))
		return;
	if (webkit_hit_test_result_context_is_link(hit))
		status_set(b, "%s", webkit_hit_test_result_get_link_uri(hit));
	else if (b->mode == BAR_HIDDEN)
		status_set(b, NULL);
}

static void
on_ready_to_show(WebKitWebView *v, Browser *b)
{
	tab_adopt(b, v, TRUE);
}

/* target=_blank and window.open() land here */
static GtkWidget *
on_create(WebKitWebView *v, WebKitNavigationAction *action, Browser *b)
{
	WebKitWebView *nv = g_object_new(WEBKIT_TYPE_WEB_VIEW,
	    "related-view", v, NULL);
	g_signal_connect(nv, "ready-to-show", G_CALLBACK(on_ready_to_show), b);
	g_signal_connect(nv, "create", G_CALLBACK(on_create), b);
	return GTK_WIDGET(nv);
}

static void
on_close_view(WebKitWebView *v, Browser *b)
{
	int page = gtk_notebook_page_num(b->nb, GTK_WIDGET(v));
	if (page >= 0) {
		gtk_notebook_remove_page(b->nb, page);
		tabs_sync(b);
	}
	if (gtk_notebook_get_n_pages(b->nb) == 0)
		gtk_window_close(b->win);
}

static gboolean
on_permission(WebKitWebView *v, WebKitPermissionRequest *req, Browser *b)
{
	/* Minimal by default: deny everything the page asks for. */
	webkit_permission_request_deny(req);
	if (v == current_view(b))
		status_set(b, "denied a permission request");
	return TRUE;
}

/* ------------------------------------------------------------------- tabs */

static void
wire_view(Browser *b, WebKitWebView *v)
{
	g_signal_connect(v, "notify::title", G_CALLBACK(on_notify_title), b);
	g_signal_connect(v, "notify::uri", G_CALLBACK(on_notify_title), b);
	g_signal_connect(v, "notify::estimated-load-progress",
	    G_CALLBACK(on_notify_progress), b);
	g_signal_connect(v, "load-changed", G_CALLBACK(on_load_changed), b);
	g_signal_connect(v, "load-failed", G_CALLBACK(on_load_failed), b);
	g_signal_connect(v, "load-failed-with-tls-errors",
	    G_CALLBACK(on_tls_error), b);
	g_signal_connect(v, "mouse-target-changed",
	    G_CALLBACK(on_mouse_target), b);
	g_signal_connect(v, "permission-request", G_CALLBACK(on_permission), b);
	g_signal_connect(v, "close", G_CALLBACK(on_close_view), b);
	if (!g_signal_handler_find(v, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
	    (gpointer)on_create, NULL))
		g_signal_connect(v, "create", G_CALLBACK(on_create), b);
}

static WebKitWebView *
tab_adopt(Browser *b, WebKitWebView *view, gboolean focus)
{
	GtkWidget *lbl = gtk_label_new("blank");
	gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
	gtk_label_set_width_chars(GTK_LABEL(lbl), 12);
	gtk_label_set_max_width_chars(GTK_LABEL(lbl), 24);
	g_object_set_data(G_OBJECT(view), "nib-tab-label", lbl);

	gtk_widget_set_vexpand(GTK_WIDGET(view), TRUE);
	int page = gtk_notebook_append_page(b->nb, GTK_WIDGET(view), lbl);
	wire_view(b, view);
	tabs_sync(b);
	if (focus) {
		gtk_notebook_set_current_page(b->nb, page);
		gtk_widget_grab_focus(GTK_WIDGET(view));
	}
	tab_label_update(b, view);
	return view;
}

static WebKitWebView *
tab_new(Browser *b, const char *uri, gboolean focus)
{
	WebKitWebView *v = g_object_new(WEBKIT_TYPE_WEB_VIEW,
	    "web-context",    b->ctx,
	    "network-session", b->session,
	    "settings",       b->settings,
	    NULL);
	tab_adopt(b, v, focus);
	if (uri && *uri)
		webkit_web_view_load_uri(v, uri);
	return v;
}

static void
tab_close(Browser *b, WebKitWebView *v)
{
	if (!v)
		return;
	if (gtk_notebook_get_n_pages(b->nb) <= 1) {
		gtk_window_close(b->win);
		return;
	}
	int page = gtk_notebook_page_num(b->nb, GTK_WIDGET(v));
	if (page >= 0)
		gtk_notebook_remove_page(b->nb, page);
	tabs_sync(b);
	WebKitWebView *n = current_view(b);
	if (n)
		gtk_widget_grab_focus(GTK_WIDGET(n));
}

static void
on_switch_page(GtkNotebook *nb, GtkWidget *page, guint n, Browser *b)
{
	if (WEBKIT_IS_WEB_VIEW(page)) {
		/* refresh_chrome checks current_view, which lags this signal */
		WebKitWebView *v = WEBKIT_WEB_VIEW(page);
		const char *title = webkit_web_view_get_title(v);
		const char *uri = webkit_web_view_get_uri(v);
		char *t = g_strdup_printf("%s — %s",
		    (title && *title) ? title : (uri ? uri : "blank"), APP_NAME);
		gtk_window_set_title(b->win, t);
		g_free(t);
		status_set(b, NULL);
	}
}

/* --------------------------------------------------------------- commands */

static void
on_bar_activate(GtkEntry *entry, Browser *b)
{
	const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
	WebKitWebView *v = current_view(b);
	if (!v)
		return;

	if (b->mode == BAR_FIND) {
		WebKitFindController *fc = webkit_web_view_get_find_controller(v);
		webkit_find_controller_search(fc, text,
		    WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE |
		    WEBKIT_FIND_OPTIONS_WRAP_AROUND, G_MAXUINT);
		/* keep the bar up so Enter/Ctrl-G can step through matches */
		return;
	}

	char *uri = resolve_input(text);
	bar_hide(b);
	if (uri) {
		webkit_web_view_load_uri(v, uri);
		g_free(uri);
	}
}

static void
zoom_by(Browser *b, double delta, gboolean reset)
{
	WebKitWebView *v = current_view(b);
	if (!v)
		return;
	double z = reset ? 1.0
	                 : CLAMP(webkit_web_view_get_zoom_level(v) + delta, 0.3, 5.0);
	webkit_web_view_set_zoom_level(v, z);
	status_set(b, "zoom %.0f%%", z * 100);
}

static gboolean
on_key(GtkEventControllerKey *kc, guint keyval, guint keycode,
       GdkModifierType state, Browser *b)
{
	gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
	gboolean shift = (state & GDK_SHIFT_MASK) != 0;
	gboolean alt = (state & GDK_ALT_MASK) != 0;
	guint key = gdk_keyval_to_lower(keyval);
	WebKitWebView *v = current_view(b);

	if (keyval == GDK_KEY_Escape) {
		if (b->mode != BAR_HIDDEN) {
			if (b->mode == BAR_FIND && v)
				webkit_find_controller_search_finish(
				    webkit_web_view_get_find_controller(v));
			bar_hide(b);
			status_set(b, NULL);
			return TRUE;
		}
		if (v)
			webkit_web_view_stop_loading(v);
		status_set(b, NULL);
		return TRUE;
	}

	/* while typing in the bar, let the entry have everything else */
	if (b->mode != BAR_HIDDEN &&
	    gtk_widget_has_focus(GTK_WIDGET(b->bar)) && !(ctrl || alt))
		return FALSE;

	if (alt && !ctrl) {
		if (keyval == GDK_KEY_Left) {
			if (v) webkit_web_view_go_back(v);
			return TRUE;
		}
		if (keyval == GDK_KEY_Right) {
			if (v) webkit_web_view_go_forward(v);
			return TRUE;
		}
		if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9) {
			int want = keyval - GDK_KEY_1;
			int n = gtk_notebook_get_n_pages(b->nb);
			gtk_notebook_set_current_page(b->nb, MIN(want, n - 1));
			return TRUE;
		}
	}

	if (!ctrl)
		return FALSE;

	switch (key) {
	case GDK_KEY_l:
		bar_show(b, BAR_OPEN,
		    v ? webkit_web_view_get_uri(v) : NULL);
		return TRUE;
	case GDK_KEY_f:
		bar_show(b, BAR_FIND, NULL);
		return TRUE;
	case GDK_KEY_g:
		if (v) {
			WebKitFindController *fc =
			    webkit_web_view_get_find_controller(v);
			if (shift)
				webkit_find_controller_search_previous(fc);
			else
				webkit_find_controller_search_next(fc);
		}
		return TRUE;
	case GDK_KEY_r:
		if (v) {
			if (shift)
				webkit_web_view_reload_bypass_cache(v);
			else
				webkit_web_view_reload(v);
		}
		return TRUE;
	case GDK_KEY_t:
		tab_new(b, NULL, TRUE);
		bar_show(b, BAR_OPEN, NULL);
		return TRUE;
	case GDK_KEY_w:
		tab_close(b, v);
		return TRUE;
	case GDK_KEY_q:
		gtk_window_close(b->win);
		return TRUE;
	case GDK_KEY_bracketleft:
		if (v) webkit_web_view_go_back(v);
		return TRUE;
	case GDK_KEY_bracketright:
		if (v) webkit_web_view_go_forward(v);
		return TRUE;
	case GDK_KEY_Tab:
	case GDK_KEY_ISO_Left_Tab:
		if (shift)
			gtk_notebook_prev_page(b->nb);
		else
			gtk_notebook_next_page(b->nb);
		return TRUE;
	case GDK_KEY_plus:
	case GDK_KEY_equal:
		zoom_by(b, 0.1, FALSE);
		return TRUE;
	case GDK_KEY_minus:
		zoom_by(b, -0.1, FALSE);
		return TRUE;
	case GDK_KEY_0:
		zoom_by(b, 0, TRUE);
		return TRUE;
	case GDK_KEY_i:
		if (shift && v) {
			WebKitWebInspector *insp =
			    webkit_web_view_get_inspector(v);
			webkit_web_inspector_show(insp);
			return TRUE;
		}
		return FALSE;
	case GDK_KEY_p:
		if (v) {
			WebKitPrintOperation *op = webkit_print_operation_new(v);
			webkit_print_operation_run_dialog(op, b->win);
			g_object_unref(op);
		}
		return TRUE;
	}
	return FALSE;
}

/* --------------------------------------------------------------- downloads */

static gboolean
on_decide_destination(WebKitDownload *dl, const char *suggested, Browser *b)
{
	const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
	if (!dir)
		dir = g_get_home_dir();

	char *base = g_path_get_basename(suggested && *suggested ? suggested : "download");
	char *path = g_build_filename(dir, base, NULL);
	for (int i = 1; g_file_test(path, G_FILE_TEST_EXISTS) && i < 1000; i++) {
		g_free(path);
		char *alt = g_strdup_printf("%d-%s", i, base);
		path = g_build_filename(dir, alt, NULL);
		g_free(alt);
	}
	char *uri = g_filename_to_uri(path, NULL, NULL);
	webkit_download_set_destination(dl, uri);
	status_set(b, "downloading → %s", path);
	g_free(base); g_free(path); g_free(uri);
	return TRUE;
}

static void
on_download_finished(WebKitDownload *dl, Browser *b)
{
	const char *dest = webkit_download_get_destination(dl);
	char *path = dest ? g_filename_from_uri(dest, NULL, NULL) : NULL;
	status_set(b, "saved %s", path ? path : "download");
	g_free(path);
}

static void
on_download_failed(WebKitDownload *dl, GError *err, Browser *b)
{
	status_set(b, "download failed: %s", err->message);
}

static void
on_download_started(WebKitNetworkSession *s, WebKitDownload *dl, Browser *b)
{
	g_signal_connect(dl, "decide-destination",
	    G_CALLBACK(on_decide_destination), b);
	g_signal_connect(dl, "finished", G_CALLBACK(on_download_finished), b);
	g_signal_connect(dl, "failed", G_CALLBACK(on_download_failed), b);
}

/* ------------------------------------------------------------------ setup */

static void
backend_init(Browser *b)
{
	char *data = g_build_filename(g_get_user_data_dir(), APP_NAME, NULL);
	char *cache = g_build_filename(g_get_user_cache_dir(), APP_NAME, NULL);
	g_mkdir_with_parents(data, 0700);
	g_mkdir_with_parents(cache, 0700);

	b->ctx = webkit_web_context_new();
	b->session = webkit_network_session_new(data, cache);

	WebKitCookieManager *cm =
	    webkit_network_session_get_cookie_manager(b->session);
	char *jar = g_build_filename(data, "cookies.sqlite", NULL);
	webkit_cookie_manager_set_persistent_storage(cm, jar,
	    WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
	webkit_cookie_manager_set_accept_policy(cm,
	    WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);

	/* Intelligent Tracking Prevention: WebKit's built-in tracker blocking */
	webkit_network_session_set_itp_enabled(b->session, TRUE);

	g_signal_connect(b->session, "download-started",
	    G_CALLBACK(on_download_started), b);

	b->settings = webkit_settings_new();
	g_object_set(b->settings,
	    "enable-developer-extras",                    TRUE,
	    "enable-smooth-scrolling",                    TRUE,
	    "enable-back-forward-navigation-gestures",    TRUE,
	    "enable-webgl",                               TRUE,
	    "enable-media-stream",                        FALSE,
	    "javascript-can-open-windows-automatically",  FALSE,
	    "media-playback-requires-user-gesture",       TRUE,
	    "default-charset",                            "utf-8",
	    NULL);
	const char *ua = g_getenv("NIB_UA");
	if (ua && *ua)
		webkit_settings_set_user_agent(b->settings, ua);

	g_free(data); g_free(cache); g_free(jar);
}

static void
style_init(void)
{
	static const char *css =
	    "entry.nib-bar { border-radius:0; border:none; padding:2px 6px;"
	    "  font-family:monospace; min-height:0; }"
	    "label.nib-status { padding:1px 6px; font-family:monospace;"
	    "  font-size:0.8em; opacity:0.75; }"
	    "progressbar.nib-prog trough, progressbar.nib-prog progress {"
	    "  min-height:2px; border-radius:0; }"
	    "notebook.nib-tabs > header { min-height:0; }"
	    "notebook.nib-tabs > header > tabs > tab { min-height:0;"
	    "  padding:1px 6px; font-size:0.85em; }";

	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_string(p, css);
	gtk_style_context_add_provider_for_display(gdk_display_get_default(),
	    GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);
}

static Browser *
browser_new(GtkApplication *app)
{
	Browser *b = g_new0(Browser, 1);
	b->app = app;
	backend_init(b);

	b->win = GTK_WINDOW(gtk_application_window_new(app));
	gtk_window_set_default_size(b->win, 1280, 800);
	gtk_window_set_title(b->win, APP_NAME);

	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	b->nb = GTK_NOTEBOOK(gtk_notebook_new());
	gtk_widget_add_css_class(GTK_WIDGET(b->nb), "nib-tabs");
	gtk_notebook_set_show_tabs(b->nb, FALSE);
	gtk_notebook_set_show_border(b->nb, FALSE);
	gtk_notebook_set_scrollable(b->nb, TRUE);
	gtk_widget_set_vexpand(GTK_WIDGET(b->nb), TRUE);
	g_signal_connect(b->nb, "switch-page", G_CALLBACK(on_switch_page), b);

	b->prog = GTK_PROGRESS_BAR(gtk_progress_bar_new());
	gtk_widget_add_css_class(GTK_WIDGET(b->prog), "nib-prog");
	gtk_widget_set_visible(GTK_WIDGET(b->prog), FALSE);

	b->bar = GTK_ENTRY(gtk_entry_new());
	gtk_widget_add_css_class(GTK_WIDGET(b->bar), "nib-bar");
	gtk_widget_set_visible(GTK_WIDGET(b->bar), FALSE);
	g_signal_connect(b->bar, "activate", G_CALLBACK(on_bar_activate), b);

	b->status = GTK_LABEL(gtk_label_new(""));
	gtk_widget_add_css_class(GTK_WIDGET(b->status), "nib-status");
	gtk_label_set_xalign(b->status, 0.0);
	gtk_label_set_ellipsize(b->status, PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_visible(GTK_WIDGET(b->status), FALSE);

	gtk_box_append(GTK_BOX(box), GTK_WIDGET(b->nb));
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(b->prog));
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(b->bar));
	gtk_box_append(GTK_BOX(box), GTK_WIDGET(b->status));
	gtk_window_set_child(b->win, box);

	GtkEventController *kc = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
	g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), b);
	gtk_widget_add_controller(GTK_WIDGET(b->win), kc);

	return b;
}

static const char usage_text[] =
"usage: nib [URL|FILE ...]\n"
"\n"
"A minimal keyboard-driven WebKit browser. Each argument opens in a tab;\n"
"with none, NIB_HOME is opened. Non-URL text is sent to the search engine.\n"
"\n"
"keys:\n"
"  C-l          open URL bar (prefilled)      C-t / C-w   new / close tab\n"
"  C-f          find; C-g / C-S-g next / prev C-Tab       next tab\n"
"  C-r / C-S-r  reload / bypass cache         M-1..9      nth tab\n"
"  C-[ / C-]    back / forward                M-Left/Right back / forward\n"
"  C-+ / C-- / C-0  zoom in / out / reset     C-S-i       web inspector\n"
"  C-p          print                         Escape      stop / hide bar\n"
"  C-q          quit\n"
"\n"
"env:\n"
"  NIB_HOME     start page          (default https://duckduckgo.com)\n"
"  NIB_SEARCH   search URL, %s slot (default DuckDuckGo)\n"
"  NIB_UA       override user agent\n"
"\n"
"state: cookies and cache under ~/.local/share/nib and ~/.cache/nib\n";

static int
on_command_line(GApplication *app, GApplicationCommandLine *cl, gpointer u)
{
	int argc = 0;
	char **argv = g_application_command_line_get_arguments(cl, &argc);

	for (int i = 1; i < argc; i++) {
		if (!g_strcmp0(argv[i], "-h") || !g_strcmp0(argv[i], "--help")) {
			g_application_command_line_print(cl, "%s", usage_text);
			g_strfreev(argv);
			return 0;
		}
		if (!g_strcmp0(argv[i], "-v") || !g_strcmp0(argv[i], "--version")) {
			g_application_command_line_print(cl, "%s (WebKitGTK %d.%d.%d)\n",
			    APP_NAME, webkit_get_major_version(),
			    webkit_get_minor_version(), webkit_get_micro_version());
			g_strfreev(argv);
			return 0;
		}
	}

	style_init();
	Browser *b = browser_new(GTK_APPLICATION(app));

	gboolean opened = FALSE;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-')
			continue;
		char *uri = resolve_input(argv[i]);
		if (uri) {
			tab_new(b, uri, !opened);
			g_free(uri);
			opened = TRUE;
		}
	}
	if (!opened) {
		char *uri = resolve_input(env_or("NIB_HOME", "https://duckduckgo.com"));
		tab_new(b, uri, TRUE);
		g_free(uri);
	}

	gtk_window_present(b->win);
	g_strfreev(argv);
	return 0;
}

int
main(int argc, char **argv)
{
	GtkApplication *app = gtk_application_new(APP_ID,
	    G_APPLICATION_HANDLES_COMMAND_LINE | G_APPLICATION_NON_UNIQUE);
	g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);
	int rc = g_application_run(G_APPLICATION(app), argc, argv);
	g_object_unref(app);
	return rc;
}
