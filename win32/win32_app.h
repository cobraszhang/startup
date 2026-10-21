/* win32_app.h - internal glue between the service entry and the GUI. */
#ifndef WIN32_APP_H
#define WIN32_APP_H

#include <stddef.h>

#define SERVICE_NAME "AppBootLauncher"

/* default config path: <exe dir>\..\conf\startup.conf */
void default_conf_path(const char *exepath, char *out, size_t outsz);

/* load config, order by dependencies, launch everything */
int  run_all(const char *confpath);

/* install / uninstall / start / stop via the SCM; returns 0 on success */
int  svc_control(const char *what, int needs_create);

/* open the configuration/status window (implemented in win32_gui.c) */
int  gui_main(const char *confpath);

#endif /* WIN32_APP_H */
