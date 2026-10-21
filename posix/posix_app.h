/* posix_app.h - internal interface shared by posix_main.c and gui_gtk.c */
#ifndef POSIX_APP_H
#define POSIX_APP_H

#include "cli.h"

/* validate config and start everything in dependency order */
int run_all(const char *confpath);

/* Build the ACTUAL command line for launching, from the stored config
 * value (POSIX counterpart of the Win32 helper). A file path that is
 * executable runs as stored (the kernel honors its shebang). A file
 * path without the execute bit gets an interpreter prepended at
 * execution time from its extension (.sh/.bash use /bin/sh, .py, .pl,
 * .rb, .js, .lua use the matching runtime; it must be installed or
 * the launch is refused). The interpreter never appears in the saved
 * configuration. Commands with arguments and missing files pass
 * through unchanged. Returns 0 with the result in out, or -1 when no
 * interpreter is available for the file. */
int app_build_cmdline(const char *stored, char *out, size_t outsz);

/* GTK configuration/status window (gui_gtk.c; built only when GTK
 * development files are present, guarded by HAVE_GUI) */
int gui_main(const char *confpath, cli_run_fn run_fn);

#endif /* POSIX_APP_H */
