/* win32_gui.c - structured configuration / status window, opened when
 * the executable is run with no arguments.
 *
 * Editor model: application list + one control per parameter
 * (name, command line, wait ms, dependencies multi-select), plus
 * add/delete, save, dependency validation, ordered launch and full
 * service control. All core log output is routed to the status box.
 * Model editing goes through the shared cfg_* helpers, so the GUI and
 * the CLI keep the exact same semantics.
 *
 * Raw Win32 ANSI API, strict C89 (compiles with MSVC 6.0 and MinGW-w64).
 * NOTE: this file is saved in GBK (code page 936) so the Chinese
 * control labels render correctly on zh-CN systems with both compilers.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "cfg.h"
#include "topo.h"
#include "log.h"
#include "win32_app.h"
#include "version.h"

#define ID_APPLIST   100
#define ID_DEPSLIST  101
#define ID_STATEDIT  102
#define ID_F_NAME    103
#define ID_F_CMD     104
#define ID_F_WAIT    105
#define ID_ADD       200
#define ID_DEL       201
#define ID_SAVE      202
#define ID_CHECK     203
#define ID_RUNNOW    204
#define ID_SVCI      205
#define ID_SVCD      206
#define ID_SVCS      207
#define ID_SVCT      208
#define ID_REFRESH   209

static HWND  g_list = NULL, g_depslb = NULL, g_stated = NULL;
static HWND  g_nameed = NULL, g_cmded = NULL, g_waited = NULL;
static HFONT g_font_fixed = NULL;
static char  g_confpath[MAX_PATH * 2];

static app_t *g_apps = NULL;   /* in-memory model (same struct as cfg.c) */
static int    g_n = 0;
static int    g_sel = -1;

/* ------------------------------------------------------------------ */
/* status output                                                        */
/* ------------------------------------------------------------------ */

static void status_line(const char *line)
{
    int len;
    if (!g_stated) return;
    len = GetWindowTextLengthA(g_stated);
    SendMessageA(g_stated, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(g_stated, EM_REPLACESEL, (WPARAM)FALSE, (LPARAM)line);
    SendMessageA(g_stated, EM_REPLACESEL, (WPARAM)FALSE, (LPARAM)"\r\n");
    SendMessageA(g_stated, EM_SCROLLCARET, 0, 0);
}

/* sink for log_msg() from the shared core */
static void log_sink(const char *line)
{
    status_line(line);
}

static void status_printf(const char *fmt, ...)
{
    char     buf[8192];
    va_list  ap;

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

static char *edit_text(HWND ed)
{
    int   n = GetWindowTextLengthA(ed);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) return NULL;
    GetWindowTextA(ed, buf, n + 1);
    return buf;
}

/* ------------------------------------------------------------------ */
/* service state                                                        */
/* ------------------------------------------------------------------ */

static const char *state_name(DWORD st)
{
    switch (st) {
    case SERVICE_STOPPED:           return "已停止 (STOPPED)";
    case SERVICE_START_PENDING:     return "正在启动…";
    case SERVICE_STOP_PENDING:      return "正在停止…";
    case SERVICE_RUNNING:           return "运行中 (RUNNING)";
    case SERVICE_CONTINUE_PENDING:  return "继续中…";
    case SERVICE_PAUSE_PENDING:     return "暂停中…";
    case SERVICE_PAUSED:            return "已暂停";
    default:                        return "未知状态";
    }
}

static const char *starttype_name(DWORD st)
{
    switch (st) {
    case SERVICE_AUTO_START:   return "自动（开机启动）";
    case SERVICE_DEMAND_START: return "手动";
    case SERVICE_DISABLED:     return "已禁用";
    default:                   return "其他";
    }
}

static void refresh_service(void)
{
    SC_HANDLE             scm, svc;
    SERVICE_STATUS        ss;
    QUERY_SERVICE_CONFIGA qsc;
    DWORD                 need = 0;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        status_printf("服务控制管理器：无法连接 err=%lu",
                      (unsigned long)GetLastError());
        return;
    }
    svc = OpenServiceA(scm, SERVICE_NAME,
                       SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!svc) {
        status_printf("服务 '%s'：未安装（点“安装服务”需管理员权限）",
                      SERVICE_NAME);
        CloseServiceHandle(scm);
        return;
    }
    if (QueryServiceConfigA(svc, &qsc, sizeof(qsc), &need))
        status_printf("服务 '%s'：启动类型 = %s", SERVICE_NAME,
                      starttype_name(qsc.dwStartType));
    if (QueryServiceStatus(svc, &ss))
        status_printf("服务状态：%s", state_name(ss.dwCurrentState));
    else
        status_printf("服务状态：查询失败 err=%lu",
                      (unsigned long)GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

/* ------------------------------------------------------------------ */
/* model <-> form  (model edits go through the shared cfg_* helpers)    */
/* ------------------------------------------------------------------ */

static void fill_form(int i);

static void model_load(void)
{
    if (cfg_load(g_confpath, &g_apps, &g_n) != 0) {
        g_apps = NULL;
        g_n = 0;
        status_printf("配置载入失败（详见上方输出），可从空列表开始编辑");
    } else {
        status_printf("已载入 %d 个应用：%s", g_n, g_confpath);
    }
}

static void refresh_list(int newsel)
{
    int i;
    SendMessageA(g_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_n; i++)
        SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)g_apps[i].name);
    if (newsel >= 0 && newsel < g_n) {
        SendMessageA(g_list, LB_SETCURSEL, (WPARAM)newsel, 0);
        fill_form(newsel);
    } else {
        fill_form(-1);
    }
}

/* load model entry i (-1 = none) into the form controls */
static void fill_form(int i)
{
    int  j, k, idx;
    char num[16];

    g_sel = i;
    SendMessageA(g_depslb, LB_RESETCONTENT, 0, 0);
    if (i < 0) {
        SetWindowTextA(g_nameed, "");
        SetWindowTextA(g_cmded, "");
        SetWindowTextA(g_waited, "");
        return;
    }
    SetWindowTextA(g_nameed, g_apps[i].name);
    SetWindowTextA(g_cmded, g_apps[i].cmd ? g_apps[i].cmd : "");
    sprintf(num, "%d", g_apps[i].wait_ms);
    SetWindowTextA(g_waited, num);

    /* dependency candidates: every other app */
    for (j = 0; j < g_n; j++) {
        if (j == i) continue;
        SendMessageA(g_depslb, LB_ADDSTRING, 0, (LPARAM)g_apps[j].name);
    }
    for (k = 0; k < g_apps[i].ndeps; k++) {
        idx = (int)SendMessageA(g_depslb, LB_FINDSTRINGEXACT,
                                (WPARAM)-1, (LPARAM)g_apps[i].deps[k]);
        if (idx >= 0)
            SendMessageA(g_depslb, LB_SETSEL, (WPARAM)TRUE, (LPARAM)idx);
    }
}

/* write the current form back into model entry g_sel */
static void apply_form(void)
{
    char *name, *cmd, *waitstr;
    int   i = g_sel, j, cnt;

    if (i < 0) return;

    name = edit_text(g_nameed);
    cmd  = edit_text(g_cmded);
    waitstr = edit_text(g_waited);
    if (!name || !cmd || !waitstr) {
        free(name); free(cmd); free(waitstr);
        status_printf("内存不足");
        return;
    }

    if (!*name) {
        status_printf("名称不能为空，已还原");
        SetWindowTextA(g_nameed, g_apps[i].name);
        free(name); free(cmd); free(waitstr);
        return;
    }
    if (strcmp(name, g_apps[i].name) != 0) {
        /* rename via shared helper: references in other apps follow */
        if (cfg_rename(g_apps, g_n, g_apps[i].name, name) != 0) {
            status_printf("名称 '%s' 已存在或不合法，未改名", name);
            SetWindowTextA(g_nameed, g_apps[i].name);
            free(name); free(cmd); free(waitstr);
            return;
        }
    }

    if (cfg_set_field(&g_apps[i], "cmd", cmd) != 0) {
        status_printf("应用 '%s' 的命令行不能为空", g_apps[i].name);
        free(name); free(cmd); free(waitstr);
        return;
    }
    cfg_set_field(&g_apps[i], "wait", waitstr);

    /* dependencies from the multi-select listbox */
    cnt = (int)SendMessageA(g_depslb, LB_GETCOUNT, 0, 0);
    for (j = 0; j < g_apps[i].ndeps; j++) free(g_apps[i].deps[j]);
    free(g_apps[i].deps);
    g_apps[i].deps = NULL;
    g_apps[i].ndeps = 0;
    for (j = 0; j < cnt; j++) {
        char item[512];
        if ((int)SendMessageA(g_depslb, LB_GETSEL, (WPARAM)j, 0) <= 0)
            continue;
        if ((int)SendMessageA(g_depslb, LB_GETTEXT, (WPARAM)j,
                              (LPARAM)item) < 0)
            continue;
        {
            char **nd = (char **)realloc(g_apps[i].deps,
                           (size_t)(g_apps[i].ndeps + 1) * sizeof(char *));
            if (!nd) break;
            g_apps[i].deps = nd;
            g_apps[i].deps[g_apps[i].ndeps] = dup_str(item);
            if (!g_apps[i].deps[g_apps[i].ndeps]) break;
            g_apps[i].ndeps++;
        }
    }

    free(name); free(cmd); free(waitstr);
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
    char   tmp[MAX_PATH];
    app_t *apps = NULL;
    int   *order = NULL;
    int    n = 0, rc, i;

    apply_form();
    if (model_ok() != 0) return;

    if (!GetTempPathA(MAX_PATH, tmp) ||
        strlen(tmp) + 24 >= sizeof(tmp)) {
        status_printf("无法取得临时目录");
        return;
    }
    strcat(tmp, "appboot_check.conf");
    if (cfg_save(tmp, g_apps, g_n) != 0) {
        status_printf("无法写临时文件 %s", tmp);
        return;
    }

    if (cfg_load(tmp, &apps, &n) != 0) {   /* details via log sink */
        DeleteFileA(tmp);
        return;
    }
    rc = topo_sort(apps, n, &order);
    if (rc != 0) {
        status_printf("依赖排序失败（代码 %d），详见上方输出", rc);
        cfg_free(apps, n);
        DeleteFileA(tmp);
        return;
    }
    status_printf("校验通过：%d 个应用，启动顺序：", n);
    for (i = 0; i < n; i++)
        status_printf("  %d. %s", i + 1, apps[order[i]].name);
    free(order);
    cfg_free(apps, n);

    if (run_after) {
        status_printf("--- 按序启动应用 ---");
        run_all(tmp);
    }
    DeleteFileA(tmp);
}

/* ------------------------------------------------------------------ */
/* window                                                               */
/* ------------------------------------------------------------------ */

static HWND mk_button(HWND p, const char *text, int id,
                      int x, int y, int w, HFONT f)
{
    HWND b = CreateWindowExA(0, "BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, 26, p, (HMENU)(LONG_PTR)id, GetModuleHandleA(NULL), NULL);
    if (b) SendMessageA(b, WM_SETFONT, (WPARAM)f, TRUE);
    return b;
}

static HWND mk_label(HWND p, const char *text, int x, int y, int w, HFONT f)
{
    HWND s = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE,
        x, y, w, 16, p, NULL, GetModuleHandleA(NULL), NULL);
    if (s) SendMessageA(s, WM_SETFONT, (WPARAM)f, TRUE);
    return s;
}

static HWND mk_edit(HWND p, int id, int x, int y, int w, int h,
                    DWORD extra, HFONT f)
{
    HWND e = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra,
        x, y, w, h, p, (HMENU)(LONG_PTR)id, GetModuleHandleA(NULL), NULL);
    if (e) SendMessageA(e, WM_SETFONT, (WPARAM)f, TRUE);
    return e;
}

static HWND mk_listbox(HWND p, int id, int x, int y, int w, int h,
                       DWORD extra, HFONT f)
{
    HWND l = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | extra,
        x, y, w, h, p, (HMENU)(LONG_PTR)id, GetModuleHandleA(NULL), NULL);
    if (l) SendMessageA(l, WM_SETFONT, (WPARAM)f, TRUE);
    return l;
}

static void on_create(HWND h)
{
    HFONT fui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    char  lbl[MAX_PATH * 3];

    g_font_fixed = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");

    sprintf(lbl, "配置文件：%s（保存时按列表顺序重新生成）", g_confpath);
    mk_label(h, lbl, 10, 8, 864, fui);

    /* left: application list */
    mk_label(h, "应用列表：", 10, 30, 120, fui);
    g_list = mk_listbox(h, ID_APPLIST, 10, 48, 200, 220, 0, fui);
    mk_button(h, "新增应用", ID_ADD, 10, 274, 95, fui);
    mk_button(h, "删除应用", ID_DEL, 115, 274, 95, fui);

    /* right: per-parameter form */
    mk_label(h, "名称：", 225, 30, 120, fui);
    g_nameed = mk_edit(h, ID_F_NAME, 225, 48, 280, 24, 0, fui);
    mk_label(h, "启动后等待（毫秒）：", 525, 30, 200, fui);
    g_waited = mk_edit(h, ID_F_WAIT, 525, 48, 100, 24, 0, fui);
    mk_label(h, "命令行：", 225, 84, 200, fui);
    g_cmded = mk_edit(h, ID_F_CMD, 225, 102, 649, 24, 0, g_font_fixed);
    mk_label(h, "依赖（多选：Ctrl/Shift 点击）：", 225, 138, 300, fui);
    g_depslb = mk_listbox(h, ID_DEPSLIST, 225, 156, 300, 142,
                          LBS_MULTIPLESEL, fui);

    /* actions */
    mk_button(h, "保存配置",     ID_SAVE,    10,  310, 90,  fui);
    mk_button(h, "校验依赖顺序", ID_CHECK,   106, 310, 110, fui);
    mk_button(h, "按序启动应用", ID_RUNNOW,  222, 310, 110, fui);

    mk_button(h, "安装服务", ID_SVCI,    10,  344, 90, fui);
    mk_button(h, "卸载服务", ID_SVCD,    106, 344, 90, fui);
    mk_button(h, "启动服务", ID_SVCS,    202, 344, 90, fui);
    mk_button(h, "停止服务", ID_SVCT,    298, 344, 90, fui);
    mk_button(h, "刷新状态", ID_REFRESH, 394, 344, 90, fui);

    mk_label(h, "状态 / 输出：", 10, 378, 200, fui);
    g_stated = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
        ES_WANTRETURN | WS_VSCROLL | WS_HSCROLL,
        10, 394, 864, 190, h, (HMENU)(LONG_PTR)ID_STATEDIT,
        GetModuleHandleA(NULL), NULL);
    if (g_stated)
        SendMessageA(g_stated, WM_SETFONT, (WPARAM)g_font_fixed, TRUE);

    log_set_sink(log_sink);
    model_load();
    refresh_list(g_n > 0 ? 0 : -1);
    refresh_service();
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        on_create(h);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == ID_APPLIST && code == LBN_SELCHANGE) {
            int cur = (int)SendMessageA(g_list, LB_GETCURSEL, 0, 0);
            apply_form();          /* commit edits of the previous app */
            fill_form(cur);
            return 0;
        }
        switch (id) {
        case ID_ADD:     add_app();       break;
        case ID_DEL:     del_app();       break;
        case ID_SAVE:    do_save();       break;
        case ID_CHECK:   check_or_run(0); break;
        case ID_RUNNOW:  check_or_run(1); break;
        case ID_SVCI:    svc_control("install", 1);   refresh_service(); break;
        case ID_SVCD:    svc_control("uninstall", 0); refresh_service(); break;
        case ID_SVCS:    svc_control("start", 0);     refresh_service(); break;
        case ID_SVCT:    svc_control("stop", 0);      refresh_service(); break;
        case ID_REFRESH: refresh_service(); break;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_font_fixed) DeleteObject(g_font_fixed);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int gui_main(const char *confpath)
{
    WNDCLASSA  wc;
    MSG        msg;
    HWND       hwnd;

    strncpy(g_confpath, confpath, sizeof(g_confpath) - 1);
    g_confpath[sizeof(g_confpath) - 1] = '\0';

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.style         = CS_OWNDC;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPSTR)IDC_ARROW);
    wc.hIcon         = LoadIconA(NULL, (LPSTR)IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "AppBootLauncherGui";
    if (!RegisterClassA(&wc)) return 1;

    {
        char ttl[256];
        sprintf(ttl, "开机启动应用管理器 - startup v%s",
                APPBOOT_VERSION);
        hwnd = CreateWindowExA(0, wc.lpszClassName, ttl,
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
            NULL, NULL, wc.hInstance, NULL);
    }
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
