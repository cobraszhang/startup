/* gui_gtk.c - POSIX GTK+ configuration/status window.
 *
 * Direct port of the win32 structured GUI: application list + one
 * widget per parameter (name, command line, wait ms, dependency
 * multi-select), add/delete, save, dependency validation, ordered
 * launch. All model edits and serialization go through the shared
 * cfg_* helpers, so this GUI, the CLI and the Windows GUI keep the
 * exact same semantics. Core log output is routed to the status view.
 *
 * Requires gtk+-3.0 (Makefile enables this file automatically when
 * pkg-config finds it). Only started when a graphical session exists
 * (GNOME, or any X/Wayland display such as WSLg).
 *
 * C89-compatible code. Source encoding UTF-8 (GTK is UTF-8 native).
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "topo.h"
#include "log.h"
#include "cli.h"
#include "version.h"
#include "posix_app.h"

static app_t *g_apps = NULL;      /* in-memory model (shared struct) */
static int    g_n = 0;
static int    g_sel = -1;
static int    g_updating = 0;     /* guard for programmatic selections */
static int    g_selftest = 0;     /* scripted validation, then quit */
static int    g_rc = 0;

static char        g_confpath[4096];
static cli_run_fn  g_run_fn = NULL;

static GtkWidget   *w_app_tree, *w_deps_tree;
static GtkWidget   *e_name, *e_cmd, *sp_wait, *tv_status;
static GtkWidget   *g_win;                 /* top-level (dialog parent) */
static GtkListStore *store_apps, *store_deps;
static GtkTextBuffer *status_buf;

/* ------------------------------------------------------------------ */
/* status output                                                        */
/* ------------------------------------------------------------------ */

static void status_line(const char *line)
{
    GtkTextIter it;
    if (!status_buf) return;
    gtk_text_buffer_get_end_iter(status_buf, &it);
    gtk_text_buffer_insert(status_buf, &it, line, -1);
    gtk_text_buffer_get_end_iter(status_buf, &it);
    gtk_text_buffer_insert(status_buf, &it, "\n", -1);
    {
        GtkTextMark *m = gtk_text_buffer_get_insert(status_buf);
        gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(tv_status), m,
                                     0.0, FALSE, 0.0, 1.0);
    }
    if (g_selftest) fprintf(stderr, "[gui-status] %s\n", line);
}

static void log_sink(const char *line)
{
    status_line(line);
}

static void status_printf(const char *fmt, ...)
{
    char    buf[8192];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(buf, fmt, ap);
    va_end(ap);
    status_line(buf);
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* model <-> form                                                       */
/* ------------------------------------------------------------------ */

static void fill_form(int i);

static void model_load(void)
{
    if (cfg_load(g_confpath, &g_apps, &g_n) != 0) {
        g_apps = NULL;
        g_n = 0;
        status_printf("配置载入失败（详见日志），可从空列表开始编辑");
    } else {
        status_printf("已载入 %d 个应用：%s", g_n, g_confpath);
    }
}

static int tree_index_of_selection(GtkTreeView *tree)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tree);
    GtkTreeModel     *model = NULL;
    GtkTreeIter       it;
    GtkTreePath      *path;
    int               idx;

    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return -1;
    path = gtk_tree_model_get_path(model, &it);
    idx = *gtk_tree_path_get_indices(path);
    gtk_tree_path_free(path);
    return idx;
}

static void refresh_list(int newsel)
{
    GtkTreeIter it;
    int         i;

    gtk_list_store_clear(store_apps);
    for (i = 0; i < g_n; i++) {
        gtk_list_store_append(store_apps, &it);
        gtk_list_store_set(store_apps, &it, 0, g_apps[i].name, -1);
    }
    g_updating = 1;
    if (newsel >= 0 && newsel < g_n) {
        GtkTreePath *p = gtk_tree_path_new_from_indices(newsel, -1);
        gtk_tree_selection_select_path(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(w_app_tree)), p);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(w_app_tree), p, NULL, FALSE);
        gtk_tree_path_free(p);
        fill_form(newsel);
    } else {
        fill_form(-1);
    }
    g_updating = 0;
}

/* load model entry i (-1 = none) into the form widgets */
static void fill_form(int i)
{
    int j, k;

    g_sel = i;
    g_updating = 1;
    gtk_list_store_clear(store_deps);
    if (i < 0) {
        gtk_entry_set_text(GTK_ENTRY(e_name), "");
        gtk_entry_set_text(GTK_ENTRY(e_cmd), "");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_wait), 0);
        g_updating = 0;
        return;
    }
    gtk_entry_set_text(GTK_ENTRY(e_name), g_apps[i].name);
    gtk_entry_set_text(GTK_ENTRY(e_cmd), g_apps[i].cmd ? g_apps[i].cmd : "");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_wait),
                              (double)g_apps[i].wait_ms);

    /* dependency candidates: every other app; preselect current deps */
    for (j = 0; j < g_n; j++) {
        GtkTreeIter it;
        if (j == i) continue;
        gtk_list_store_append(store_deps, &it);
        gtk_list_store_set(store_deps, &it, 0, g_apps[j].name, -1);
    }
    {
        GtkTreeModel     *m = GTK_TREE_MODEL(store_deps);
        GtkTreeSelection *sel = gtk_tree_view_get_selection(
                                   GTK_TREE_VIEW(w_deps_tree));
        GtkTreeIter it;
        if (gtk_tree_model_get_iter_first(m, &it)) {
            do {
                char *nm = NULL;
                gtk_tree_model_get(m, &it, 0, &nm, -1);
                for (k = 0; nm && k < g_apps[i].ndeps; k++) {
                    if (strcmp(nm, g_apps[i].deps[k]) == 0) {
                        gtk_tree_selection_select_iter(sel, &it);
                        break;
                    }
                }
                g_free(nm);
            } while (gtk_tree_model_iter_next(m, &it));
        }
    }
    g_updating = 0;
}

/* write the current form back into model entry g_sel */
static void apply_form(void)
{
    const char *name, *cmd;
    char      **newdeps = NULL;
    int         i = g_sel, j, k, newn = 0;

    if (i < 0) return;

    name = gtk_entry_get_text(GTK_ENTRY(e_name));
    cmd  = gtk_entry_get_text(GTK_ENTRY(e_cmd));

    if (!*name) {
        status_printf("名称不能为空，已还原");
        gtk_entry_set_text(GTK_ENTRY(e_name), g_apps[i].name);
        return;
    }
    for (j = 0; j < g_n; j++) {
        if (j != i && strcmp(g_apps[j].name, name) == 0) break;
    }
    if (j < g_n) {
        status_printf("名称 '%s' 已存在，未改名", name);
        gtk_entry_set_text(GTK_ENTRY(e_name), g_apps[i].name);
        return;
    }
    if (strcmp(name, g_apps[i].name) != 0) {
        if (cfg_rename(g_apps, g_n, g_apps[i].name, name) != 0) {
            status_printf("改名失败");
            gtk_entry_set_text(GTK_ENTRY(e_name), g_apps[i].name);
            return;
        }
    }

    if (cfg_set_field(&g_apps[i], "cmd", cmd) != 0) {
        status_printf("应用 '%s' 的命令行不能为空", g_apps[i].name);
        return;
    }
    g_apps[i].wait_ms = (int)gtk_spin_button_get_value(
                            GTK_SPIN_BUTTON(sp_wait));
    if (g_apps[i].wait_ms < 0) g_apps[i].wait_ms = 0;

    /* dependencies from the multi-select tree */
    {
        GtkTreeModel     *m = GTK_TREE_MODEL(store_deps);
        GtkTreeSelection *sel = gtk_tree_view_get_selection(
                                   GTK_TREE_VIEW(w_deps_tree));
        GtkTreeIter it;
        if (gtk_tree_model_get_iter_first(m, &it)) {
            do {
                char *nm = NULL;
                if (!gtk_tree_selection_iter_is_selected(sel, &it))
                    continue;
                gtk_tree_model_get(m, &it, 0, &nm, -1);
                if (!nm) continue;
                {
                    char **nd = (char **)realloc(newdeps,
                                   (size_t)(newn + 1) * sizeof(char *));
                    if (nd) {
                        newdeps = nd;
                        newdeps[newn] = dup_str(nm);
                        if (newdeps[newn]) newn++;
                    }
                }
                g_free(nm);
            } while (gtk_tree_model_iter_next(m, &it));
        }
    }
    for (k = 0; k < g_apps[i].ndeps; k++) free(g_apps[i].deps[k]);
    free(g_apps[i].deps);
    g_apps[i].deps = newdeps;
    g_apps[i].ndeps = newn;
}

static void add_app(void)
{
    char nm[64];
    int  i, seq = 1, idx;

    apply_form();
    for (;;) {
        sprintf(nm, "new%d", seq);
        for (i = 0; i < g_n; i++)
            if (strcmp(g_apps[i].name, nm) == 0) break;
        if (i >= g_n) break;
        seq++;
    }
    idx = cfg_add(&g_apps, &g_n, nm);
    if (idx < 0) { status_printf("内存不足"); return; }
    refresh_list(idx);
    status_printf("已新增应用 '%s'：请填命令行、勾选依赖，然后保存", nm);
}

static void del_app(void)
{
    int i = g_sel;

    if (i < 0) return;
    apply_form();
    if (cfg_remove(&g_apps, &g_n, g_apps[i].name) != 0) return;
    refresh_list(g_n ? (i < g_n ? i : g_n - 1) : -1);
    status_printf("已删除应用（其他应用中对它的依赖引用已一并移除）");
}

/* basic model sanity before writing / validating */
static int model_ok(void)
{
    int i, j;
    for (i = 0; i < g_n; i++) {
        if (!g_apps[i].name || !*g_apps[i].name) {
            status_printf("第 %d 项名称为空", i + 1);
            return -1;
        }
        if (!g_apps[i].cmd || !*g_apps[i].cmd) {
            status_printf("应用 '%s' 的命令行不能为空", g_apps[i].name);
            return -1;
        }
        for (j = i + 1; j < g_n; j++) {
            if (strcmp(g_apps[i].name, g_apps[j].name) == 0) {
                status_printf("名称重复：%s", g_apps[i].name);
                return -1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* actions                                                              */
/* ------------------------------------------------------------------ */

static void do_save(void)
{
    apply_form();
    if (model_ok() != 0) return;
    if (cfg_save(g_confpath, g_apps, g_n) != 0)
        status_printf("保存失败：无法写入 %s", g_confpath);
    else
        status_printf("已保存：%s", g_confpath);
}

/* validate (and optionally launch) the model via a temp file */
static void check_or_run(int run_after)
{
    char   tmp[] = "/tmp/appboot_check.conf";
    app_t *apps = NULL;
    int   *order = NULL;
    int    n = 0, rc, i, cnt = 0;

    apply_form();
    if (model_ok() != 0) { g_rc = 1; return; }

    if (cfg_save(tmp, g_apps, g_n) != 0) {
        status_printf("无法写临时文件 %s", tmp);
        g_rc = 1;
        return;
    }
    if (cfg_load(tmp, &apps, &n) != 0) {   /* details via log sink */
        remove(tmp);
        g_rc = 1;
        return;
    }
    rc = topo_sort(apps, n, TOPO_ALL, &order, &cnt);
    if (rc != 0) {
        status_printf("依赖排序失败（代码 %d），详见日志", rc);
        cfg_free(apps, n);
        remove(tmp);
        g_rc = 1;
        return;
    }
    status_printf("校验通过：%d 个应用，启动顺序：", n);
    for (i = 0; i < cnt; i++)
        status_printf("  %d. %s", i + 1, apps[order[i]].name);
    free(order);
    cfg_free(apps, n);

    if (run_after) {
        status_printf("--- 按序启动应用 ---");
        g_run_fn(tmp);
    }
    remove(tmp);
}

/* ------------------------------------------------------------------ */
/* callbacks                                                            */
/* ------------------------------------------------------------------ */

static void on_app_sel_changed(GtkTreeSelection *sel, gpointer data)
{
    int cur;
    (void)sel; (void)data;
    if (g_updating) return;
    cur = tree_index_of_selection(GTK_TREE_VIEW(w_app_tree));
    apply_form();              /* commit edits of the previous app */
    fill_form(cur);
}

static void on_add(GtkButton *b, gpointer d)
    { (void)b; (void)d; add_app(); }
static void on_del(GtkButton *b, gpointer d)
    { (void)b; (void)d; del_app(); }
static void on_save(GtkButton *b, gpointer d)
    { (void)b; (void)d; do_save(); }
static void on_check(GtkButton *b, gpointer d)
    { (void)b; (void)d; check_or_run(0); }
static void on_runnow(GtkButton *b, gpointer d)
    { (void)b; (void)d; check_or_run(1); }

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    gtk_main_quit();
}

/* APPBOOT_GUI_SHOT=<file.png>: render the window into a PNG once
 * mapped, then quit (used by the test suite / debugging).
 * Cairo-based so it works on X11 and Wayland backends alike
 * (pixel readback from the server is not available under Wayland). */
static char g_shotpath[4096];

static gboolean take_shot(gpointer data)
{
    GtkWidget       *win = GTK_WIDGET(data);
    int              w = gtk_widget_get_allocated_width(win);
    int              h = gtk_widget_get_allocated_height(win);
    cairo_surface_t *surf;
    cairo_t         *cr;

    if (w > 0 && h > 0) {
        surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cr = cairo_create(surf);
        cairo_set_source_rgb(cr, 0.93, 0.93, 0.93); /* window bg */
        cairo_paint(cr);
        gtk_widget_draw(win, cr);
        cairo_destroy(cr);
        if (cairo_surface_write_to_png(surf, g_shotpath)
                != CAIRO_STATUS_SUCCESS)
            fprintf(stderr, "gui: screenshot write failed\n");
        cairo_surface_destroy(surf);
    }
    gtk_main_quit();
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* window                                                               */
/* ------------------------------------------------------------------ */

static GtkWidget *mk_button(const char *text, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static GtkWidget *mk_scrolled_tree(GtkListStore **store_out,
                                  gboolean multiple,
                                  int width, int height,
                                  GtkWidget **tree_out)
{
    GtkWidget       *sw, *tree;
    GtkCellRenderer *r;
    GtkListStore    *store;

    store = gtk_list_store_new(1, G_TYPE_STRING);
    *store_out = store;
    tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    *tree_out = tree;
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
    r = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(
        GTK_TREE_VIEW(tree), -1, "", r, "text", 0, NULL);

    {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(
                                   GTK_TREE_VIEW(tree));
        gtk_tree_selection_set_mode(sel,
            multiple ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_BROWSE);
    }

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    if (width > 0)
        gtk_scrolled_window_set_min_content_width(
            GTK_SCROLLED_WINDOW(sw), width);
    if (height > 0)
        gtk_scrolled_window_set_min_content_height(
            GTK_SCROLLED_WINDOW(sw), height);
    gtk_container_add(GTK_CONTAINER(sw), tree);
    return sw;
}

/* file picker for the command line field: the configuration keeps
 * only the quoted file path; the interpreter (if any) is prepended
 * later, at execution time (app_build_cmdline). */
static void on_browse(GtkWidget *b, gpointer u)
{
    static const char *pats[] =
        { "*.sh", "*.bash", "*.py", "*.pl", "*.rb", "*.js", "*.lua",
          NULL };
    GtkWidget *dlg;
    GtkFileFilter *flt;
    char       probe[4096];
    char       quoted[4096];
    char      *path;
    int        i;

    (void)b; (void)u;
    dlg = gtk_file_chooser_dialog_new("选择应用或脚本",
              GTK_WINDOW(g_win), GTK_FILE_CHOOSER_ACTION_OPEN,
              "取消", GTK_RESPONSE_CANCEL,
              "打开", GTK_RESPONSE_ACCEPT, NULL);

    flt = gtk_file_filter_new();
    gtk_file_filter_set_name(GTK_FILE_FILTER(flt),
        "脚本 (*.sh *.bash *.py *.pl *.rb *.js *.lua)");
    for (i = 0; pats[i]; i++)
        gtk_file_filter_add_pattern(GTK_FILE_FILTER(flt), pats[i]);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg),
                                GTK_FILE_FILTER(flt));
    flt = gtk_file_filter_new();
    gtk_file_filter_set_name(GTK_FILE_FILTER(flt), "所有文件 (*)");
    gtk_file_filter_add_pattern(GTK_FILE_FILTER(flt), "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg),
                                GTK_FILE_FILTER(flt));

    if (gtk_dialog_run(GTK_DIALOG(dlg)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(dlg);
        return;                            /* cancelled: keep as is */
    }
    path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);
    if (!path) return;

    sprintf(quoted, "\"%s\"", path);
    if (app_build_cmdline(quoted, probe, sizeof(probe)) != 0) {
        log_msg("浏览：系统中未找到可执行该脚本的方式（不可执行且无对应"
                "运行时），未写入命令行。");
        g_free(path);
        return;
    }
    gtk_entry_set_text(GTK_ENTRY(e_cmd), quoted);
    if (strcmp(probe, quoted) != 0)
        log_msg("浏览：已选择；执行时将实际运行为：%s", probe);
    else
        log_msg("浏览：已选择 %s", path);
    g_free(path);
}

int gui_main(const char *confpath, cli_run_fn run_fn)
{
    GtkWidget *win, *vroot, *hbox, *grid, *label, *sw, *hbtn;
    char       ttl[256];

    g_run_fn = run_fn;
    g_selftest = getenv("APPBOOT_GUI_SELFTEST") ? 1 : 0;
    strncpy(g_confpath, confpath, sizeof(g_confpath) - 1);
    g_confpath[sizeof(g_confpath) - 1] = '\0';

    if (!gtk_init_check(NULL, NULL)) {
        log_msg("gui: 无法连接图形显示（DISPLAY/WAYLAND_DISPLAY 不可用）");
        return 1;
    }

    /* monospace status view via CSS (no deprecated APIs) */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        if (gtk_css_provider_load_from_data(css,
                "textview, textview text { font-family: Monospace; }",
                -1, NULL)) {
            gtk_style_context_add_provider_for_screen(
                gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        g_object_unref(css);
    }

    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_win = win;
    sprintf(ttl, "开机启动应用管理器 - startup v%s",
            APPBOOT_VERSION);
    gtk_window_set_title(GTK_WINDOW(win), ttl);
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 620);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), NULL);

    vroot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(win), vroot);

    label = gtk_label_new("");
    {
        char lbl[5000];
        sprintf(lbl,
            "配置文件：%s（保存时按列表顺序重新生成）", g_confpath);
        gtk_label_set_text(GTK_LABEL(label), lbl);
    }
    gtk_box_pack_start(GTK_BOX(vroot), label, FALSE, FALSE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vroot), hbox, TRUE, TRUE, 0);

    /* left: application list + add/delete */
    {
        GtkWidget *lv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *swl = mk_scrolled_tree(&store_apps, FALSE, 200, 320,
                                          &w_app_tree);
        GtkWidget *bl = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
        g_signal_connect(gtk_tree_view_get_selection(
                             GTK_TREE_VIEW(w_app_tree)),
                         "changed", G_CALLBACK(on_app_sel_changed), NULL);
        gtk_box_pack_start(GTK_BOX(lv),
            gtk_label_new("应用列表："), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(lv), swl, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(bl),
            mk_button("新增应用", G_CALLBACK(on_add)));
        gtk_container_add(GTK_CONTAINER(bl),
            mk_button("删除应用", G_CALLBACK(on_del)));
        gtk_box_pack_start(GTK_BOX(lv), bl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), lv, FALSE, FALSE, 0);
    }

    /* right: per-parameter form */
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(hbox), grid, TRUE, TRUE, 0);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("名称："),
                    0, 0, 1, 1);
    e_name = gtk_entry_new();
    gtk_widget_set_size_request(e_name, 280, -1);
    gtk_grid_attach(GTK_GRID(grid), e_name, 1, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid),
        gtk_label_new("启动后等待（毫秒）："), 2, 0, 1, 1);
    sp_wait = gtk_spin_button_new_with_range(0, 3600000, 50);
    gtk_grid_attach(GTK_GRID(grid), sp_wait, 3, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("命令行："),
                    0, 1, 1, 1);
    e_cmd = gtk_entry_new();
    gtk_widget_set_hexpand(e_cmd, TRUE);
    gtk_grid_attach(GTK_GRID(grid), e_cmd, 1, 1, 2, 1);
    gtk_grid_attach(GTK_GRID(grid),
        mk_button("浏览...", G_CALLBACK(on_browse)), 3, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid),
        gtk_label_new("依赖（按住 Ctrl/Shift 多选）："), 0, 2, 1, 1);
    sw = mk_scrolled_tree(&store_deps, TRUE, 320, 140, &w_deps_tree);
    gtk_grid_attach(GTK_GRID(grid), sw, 1, 3, 1, 1);

    /* action buttons */
    hbtn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(hbtn),
        mk_button("保存配置", G_CALLBACK(on_save)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbtn),
        mk_button("校验依赖顺序", G_CALLBACK(on_check)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbtn),
        mk_button("按序启动应用", G_CALLBACK(on_runnow)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vroot), hbtn, FALSE, FALSE, 0);

    /* status log */
    gtk_box_pack_start(GTK_BOX(vroot),
        gtk_label_new("状态 / 输出："), FALSE, FALSE, 0);
    tv_status = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv_status), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv_status), FALSE);
    status_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv_status));
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(
        GTK_SCROLLED_WINDOW(sw), 180);
    gtk_container_add(GTK_CONTAINER(sw), tv_status);
    gtk_box_pack_start(GTK_BOX(vroot), sw, TRUE, TRUE, 0);

    log_set_sink(log_sink);
    model_load();
    refresh_list(g_n > 0 ? 0 : -1);

    gtk_widget_show_all(win);

    if (g_selftest) {
        /* scripted end-to-end check of load + form + topology */
        const char *exp = getenv("APPBOOT_GUI_EXPECT_N");
        if (exp && atoi(exp) != g_n) {
            status_printf("selftest: 期望 %s 个应用，实际 %d", exp, g_n);
            g_rc = 1;
        } else if (g_n > 0) {
            int nsel = gtk_tree_selection_count_selected_rows(
                gtk_tree_view_get_selection(GTK_TREE_VIEW(w_deps_tree)));
            if (nsel != g_apps[0].ndeps) {
                status_printf("selftest: 依赖预选不对（%d/%d）",
                              nsel, g_apps[0].ndeps);
                g_rc = 1;
            } else {
                status_printf("selftest: 依赖预选正确（%d 项）", nsel);
            }
        }
        check_or_run(0);
        /* no gtk_main() was entered; returning ends the process.
         * (gtk_main_quit() would hit a Gtk-CRITICAL assertion.) */
        return g_rc;
    }

    {
        const char *shot = getenv("APPBOOT_GUI_SHOT");
        if (shot && *shot) {
            strncpy(g_shotpath, shot, sizeof(g_shotpath) - 1);
            g_shotpath[sizeof(g_shotpath) - 1] = '\0';
            g_timeout_add(800, take_shot, win);
        }
    }

    gtk_main();
    return g_rc;
}
