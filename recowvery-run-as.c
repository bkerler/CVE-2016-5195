#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <selinux/selinux.h>

#define APP_NAME  "recowvery"
#define HOST_NAME "run-as"

#ifdef DEBUG
#include <android/log.h>
#define LOGV(...) { __android_log_print(ANDROID_LOG_INFO,  APP_NAME, __VA_ARGS__); printf(__VA_ARGS__); printf("\n"); }
#define LOGE(...) { __android_log_print(ANDROID_LOG_ERROR, APP_NAME, __VA_ARGS__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#else
#define LOGV(...) { printf(__VA_ARGS__); printf("\n"); }
#define LOGE(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#endif

#define SEP LOGV("------------")

#define CONTEXT_INIT "u:r:init:s0"

enum {
	DO_NOTHING = 0,
	DO_EXEC,
	DO_SU
};

static void capadd(struct __user_cap_data_struct *capdata, const int cap)
{
	capdata[CAP_TO_INDEX(cap)].effective   |= CAP_TO_MASK(cap);
	capdata[CAP_TO_INDEX(cap)].permitted   |= CAP_TO_MASK(cap);
	capdata[CAP_TO_INDEX(cap)].inheritable |= CAP_TO_MASK(cap);
}

int main(int argc, char **argv)
{
	int ret = 0;
	int run = DO_NOTHING;
	int uid, i, argc_exec;
	struct __user_cap_header_struct capheader;
	struct __user_cap_data_struct capdata[2];
	char **argv_exec;

	LOGV("Welcome to %s! (%s)", APP_NAME, "run-as");

	if (argc < 2)
		goto uid;

	if (!strcmp(argv[1], "exec")) {
		if (argc == 2) {
			LOGE("Not enough parameters for exec!");
			ret = EINVAL;
			goto usage;
		}
		run = DO_EXEC;
	} else
	if (!strcmp(argv[1], "su")) {
		run = DO_SU;
	} else {
		LOGE("Unknown parameter: %s", argv[1]);
		ret = EINVAL;
		goto usage;
	}
uid:
	SEP;

	uid = getuid();
	LOGV("Current uid: %d", uid);

	if (!uid)
		goto root;

	memset(&capheader, 0, sizeof(capheader));
	memset(&capdata, 0, sizeof(capdata));
	capheader.version = _LINUX_CAPABILITY_VERSION_3;
	capadd(capdata, CAP_SETGID);
	capadd(capdata, CAP_SETUID);

	LOGV("Setting capabilities");
	if (capset(&capheader, &capdata[0])) {
		ret = errno;
		LOGE("Could not set capabilities");
		goto oops;
	}

	LOGV("Attempting to escalate to root");

	if (setresgid(0, 0, 0)) {
		ret = errno;
		LOGE("setresgid failed");
		goto oops;
	}

	if (setresuid(0, 0, 0)) {
		ret = errno;
		LOGE("setresuid failed");
		goto oops;
	}

	uid = getuid();
	LOGV("Current uid: %d", uid);

	if (uid)
		goto oops;

	// less audits
	if (setcon(CONTEXT_INIT))
		LOGE("Warning: setcon transition to '%s' failed (is SELinux Enforcing?)", CONTEXT_INIT);

root:
	LOGV("We have root access!");

	if (prctl(PR_SET_KEEPCAPS, 1))
		LOGE("Warning: Could not set retain capabilities");

	if (run == DO_EXEC)
		goto exec;

	if (run == DO_SU)
		goto su;

	return 0;
exec:
	SEP;

	argc_exec = argc - 2; // drop name and -exec
	argv_exec = malloc((argc_exec + 1) * sizeof(void*)); // add 1 for NULL
	argv_exec[argc_exec] = 0;

	// place arguments after exec into argv_exec
	for (i = 2; i < argc; i++)
		argv_exec[i - 2] = argv[i];

	LOGV("Executing: '%s' with %d arguments\n",
		argv_exec[0], argc_exec - 1);
	execvp(argv_exec[0], argv_exec);

	// if we get this far, then execvp failed!
	free(argv_exec);
	ret = EPERM;
	goto oops;
su:
	SEP;

	argv_exec = malloc(2 * sizeof(void*)); // add 1 for NULL
	argv_exec[0] = "-sh";
	argv_exec[1] = 0;

	LOGV("Starting root shell");
	execve("/system/bin/sh", argv_exec, 0);

	// if we get this far, then execve failed!
	ret = EPERM;
	goto oops;
usage:
	LOGE("Usage for %s (%s):", argv[0], APP_NAME);
	LOGE("  Execute a command as root:");
	LOGE("    %s exec command [args...]", argv[0]);
	LOGE("  Start a root shell:");
	LOGE("    %s su", argv[0]);
	return EINVAL;
oops:
	LOGE("Error %d: %s", ret, strerror(ret));
	return ret;
}
