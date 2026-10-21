/* cli.h - shared command-line management interface (portable C89).
 *
 * Platform front-ends dispatch their unknown positional command here:
 *   win32:  startup.exe <subcommand> [args] [-c FILE]
 *   posix:  bootlauncher [-c FILE] <subcommand> [args]
 *
 * "run" is delegated to a platform-provided launcher (run_all).
 */
#ifndef CLI_H
#define CLI_H

typedef int (*cli_run_fn)(const char *confpath);

/* Non-zero when cmd is one of the management subcommands. */
int  cli_is_known(const char *cmd);

/* Print subcommand help to stderr. */
void cli_usage(const char *prog);

/* Execute the management subcommand argv[0] with argv[1..argc-1].
 * Returns process exit code: 0 ok, 1 error, 2 usage. */
int  cli_main(int argc, char **argv, const char *prog,
              const char *confpath, cli_run_fn run_fn);

#endif /* CLI_H */
