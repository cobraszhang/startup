/* posix_app.h - internal interface shared by posix_main.c and gui_gtk.c */
#ifndef POSIX_APP_H
#define POSIX_APP_H

#include "cli.h"

/* validate config and start everything in dependency order */
int run_all(const char *confpath);

/* GTK configuration/status window (gui_gtk.c; built only when GTK
 * development files are present, guarded by HAVE_GUI) */
int gui_main(const char *confpath, cli_run_fn run_fn);

#endif /* POSIX_APP_H */
