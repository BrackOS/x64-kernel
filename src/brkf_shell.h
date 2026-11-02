#ifndef BRKF_SHELL_H
#define BRKF_SHELL_H

/* "brkf <subcommand> ..." from the shell */
void brkf_shell_command(const char *args);
/* Mount the second disk at boot if it holds a BRKF volume */
void brkf_boot_mount(void);

#endif
