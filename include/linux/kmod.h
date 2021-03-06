/*
	kmod header
*/

#include <linux/config.h>

#ifdef CONFIG_KMOD
extern int request_module(const char * name);
extern int exec_usermodehelper(char *program_path, char *argv[], char *envp[]);
#else
#include <linux/errno.h>

#define request_module(x) do {} while(0)
static inline int exec_usermodehelper(char *program_path, char *argv[], char *envp[])
{
        return -EACCES;
}
#endif

