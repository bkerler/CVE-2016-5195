#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

#define APP_NAME  "recowvery"
#define HOST_NAME "applypatch"

#ifdef DEBUG
#include <android/log.h>
#define LOGV(...) { __android_log_print(ANDROID_LOG_INFO,  APP_NAME, __VA_ARGS__); printf(__VA_ARGS__); printf("\n"); }
#define LOGE(...) { __android_log_print(ANDROID_LOG_ERROR, APP_NAME, __VA_ARGS__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#else
#define LOGV(...) { printf(__VA_ARGS__); printf("\n"); }
#define LOGE(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); }
#endif

#define SEP LOGV("------------")

#include "bootimg.h"

#define MiB 1048576

#define _FILE_OFFSET_BITS 64

/* directories to store content for working on
 * /data/local is r/w accessible by limited capabilities root
 * /cache is r/w accessible by u:r:install_recovery:s0 context as full capabilities root
 */
#define WORK_BOOT      "/data/local"
#define WORK_RECOVERY  "/cache"

/* msm8996 */
#define BLOCK_BOOT     "/dev/block/bootdevice/by-name/boot"
#define BLOCK_RECOVERY "/dev/block/bootdevice/by-name/recovery"

/* universal8890 */
//#define BLOCK_BOOT     "/dev/block/platform/155a0000.ufs/by-name/BOOT"
//#define BLOCK_RECOVERY "/dev/block/platform/155a0000.ufs/by-name/RECOVERY"

/* name of the init file we're going to overwrite */
static const char init_rc[] = "init.lge.fm.rc";
static const char init_rc_content[] =
/* this is the content of our new init file */
"on boot\n"
"    setprop ro.fm.module BCM\n"
"    setenforce 0\n"
"    write /sys/fs/selinux/enforce 0\n"
"\n"
"on property:sys.boot_completed=1\n"
"    setenforce 0\n"
"    write /sys/fs/selinux/enforce 0\n";
/* end of init file content */

static off_t seek_last_null(const int fd, const int reverse)
{
	char c[1];
	if (reverse)
		lseek(fd, -1, SEEK_CUR);
	while (read(fd, c, 1) > 0) {
		if (*c)
			return lseek(fd, reverse ? 1 : -1, SEEK_CUR); // go back to the last null
		if (reverse)
			lseek(fd, -2, SEEK_CUR);
	}
	return lseek(fd, 0, SEEK_CUR);
}

/* start cpio code */

#define CPIO_MAGIC "070701"
#define CPIO_TRAILER_MAGIC "TRAILER!!!"
#define CPIO_TRAILER_INNER "00000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000B00000000"

static const byte cpio_trailer[] = CPIO_MAGIC CPIO_TRAILER_INNER CPIO_TRAILER_MAGIC;

static byte *cpio_file(const char *file, const byte *content, const off_t content_len, off_t *cpio_len)
{
	struct stat st = {0};
	off_t sz = 0;
	const int flen = strlen(file) + 1; // null terminator
	byte *cpio, *c;

	st.st_mode |= S_IFREG; // is a file
	st.st_mode |= S_IRWXU | S_IRGRP | S_IXGRP; // permission mode 0750
	st.st_nlink = 1;
	st.st_mtime = time(0); // set modification to now
	st.st_size = content_len;

	// calculate length of the new cpio file
	*cpio_len = 110 + flen + 3 + content_len + 3;

	// allocate full memory needed to store cpio file
	c = cpio = malloc(*cpio_len);

	// write cpio header, content size, filename all in one go
	c += sprintf((char*)c,
		CPIO_MAGIC
		"%08X%08X%08X%08X%08X%08X"
		"%08X%08X%08X%08X%08X%08X"
		"00000000%s",
		(uint32_t)st.st_ino,
		(uint32_t)st.st_mode,
		(uint32_t)st.st_uid,
		(uint32_t)st.st_gid,
		(uint32_t)st.st_nlink,
		(uint32_t)st.st_mtime,
		(uint32_t)st.st_size,
		(uint32_t)major(st.st_dev),
		(uint32_t)minor(st.st_dev),
		(uint32_t)major(st.st_rdev),
		(uint32_t)minor(st.st_rdev),
		flen, file);

	// add null padding (+1 for filename null terminator)
	memset(c, 0, 4);
	c += 4;

	// write content to cpio file
	memcpy(c, content, content_len);
	c += content_len;

	// add null padding
	memset(c, 0, 3);
	c += 3;

//	assert((c - cpio) != *cpio_len));

	return cpio;
}

static int cpio_append(const char *file, const byte *cpio, const off_t cpio_len)
{
	int fd, ret = 0;
	off_t sz;
	byte tmp[] = CPIO_TRAILER_MAGIC;

	fd = open(file, O_RDWR);
	if (fd < 0) {
		LOGE("Could not open cpio archive '%s' as r/w!", file);
		goto oops;
	}

	sz = lseek(fd, 0, SEEK_END);
	if (sz < 0)
		goto oops;

	LOGV("Opened cpio archive '%s' (%lld bytes)", file, (long long)sz);

	sz = seek_last_null(fd, 1);
	// search for cpio trailer magic
	lseek(fd, -sizeof(tmp), SEEK_CUR);
	read(fd, tmp, sizeof(tmp));
	if (memcmp(CPIO_TRAILER_MAGIC, tmp, sizeof(tmp))) {
		lseek(fd, sz, SEEK_SET); // didn't find trailer magic, go back to first null
		goto append;
	}
	// seek to the start of trailer
	lseek(fd, -sizeof(cpio_trailer), SEEK_CUR);
append:
	if (write(fd, cpio, cpio_len) != cpio_len) {
		ret = EIO;
		goto trailer;
	}

	LOGV("Wrote new file (%lld bytes) to cpio archive,", (long long)cpio_len);
trailer:
	if (write(fd, cpio_trailer, sizeof(cpio_trailer)) != sizeof(cpio_trailer)) {
		ret = EIO;
		goto oops;
	}

	// final 3 byte null padding
	write(fd, "\0\0\0", 3);

	sz = lseek(fd, 0, SEEK_CUR);
	// make sure there's no trailing garbage
	ftruncate(fd, sz);

	LOGV("Final size: %lld bytes", (long long)sz);
oops:
	if (fd >= 0)
		close(fd);

	return ret;
}

/* end cpio code */

static int valid_filesize(const char *file, const off_t size)
{
	int fd;
	off_t sz;

	LOGV("Checking '%s' for validity (size >= %lld bytes)", file, (long long)size);
	fd = open(file, O_RDONLY);
	if (fd < 0) {
		LOGE("Couldn't open file for reading!");
		return ENOENT;
	}
	sz = lseek(fd, 0, SEEK_END);
	LOGV("'%s': %lld bytes", file, (long long)sz);
	if (sz < size) {
		LOGE("File is not at least %lld bytes, must not be valid", (long long)size);
		close(fd);
		return EINVAL;
	}
	LOGV("File OK");
	close(fd);
	return 0;
}

static int decompress_ramdisk(const char *ramdisk, const char *cpio)
{
	int ret = 0;
	char cmd[100];

	LOGV("Decompressing ramdisk (gzip -d)");

	sprintf(cmd, "gzip -d < \"%s\" > \"%s\"", ramdisk, cpio);
	system(cmd);

	ret = valid_filesize(cpio, 4*MiB);
	if (ret)
		goto oops;

	LOGV("Decompression of ramdisk successful");

	LOGV("Deleting '%s' (no longer needed)", ramdisk);
	remove(ramdisk);

	return 0;
oops:
	LOGE("Ramdisk decompression failed!");
	return ret;
}

static int compress_ramdisk(const char *cpio, const char *ramdisk)
{
	int ret = 0;
	char cmd[100];

	LOGV("Compressing cpio to ramdisk (gzip -9 -c)");

	sprintf(cmd, "gzip -9 -c < \"%s\" > \"%s\"", cpio, ramdisk);
	system(cmd);

	ret = valid_filesize(ramdisk, 2*MiB);
	if (ret)
		goto oops;

	LOGV("Compression of ramdisk successful");

	LOGV("Deleting '%s' (no longer needed)", cpio);
	remove(cpio);

	return 0;
oops:
	LOGE("Ramdisk compression failed!");
	return ret;
}

static int flash_permissive_boot(const int to_boot)
{
	int ret = 0;
	off_t sz;
	boot_img *image;
	const char *ramdisk, *cpio, *flash_block;

	if (to_boot) {
		flash_block = BLOCK_BOOT;
		ramdisk = WORK_BOOT "/ramdisk.gz";
		cpio = WORK_BOOT "/ramdisk.cpio";
	} else {
		flash_block = BLOCK_RECOVERY;
		ramdisk = WORK_RECOVERY "/ramdisk.gz";
		cpio = WORK_RECOVERY "/ramdisk.cpio";
	}

	SEP;
/* start read boot image */

	LOGV("Loading boot image from block device '%s'...", BLOCK_BOOT);
	image = load_boot_image(BLOCK_BOOT);
	if (!image) {
		LOGE("Failed to load boot image!");
		ret = EINVAL;
		goto oops;
	}
	LOGV("Loaded boot image!");

/* end read boot image */
	SEP;
/* start ramdisk modification */

	LOGV("Saving old ramdisk to file");
	ret = bootimg_save_ramdisk(image, ramdisk);
	if (ret)
		goto oops;

	ret = decompress_ramdisk(ramdisk, cpio);
	if (ret)
		goto oops;

	SEP;
/* start add modified init.lge.fm.rc to ramdisk cpio */

	byte* cpiodata = cpio_file(init_rc, (byte*)init_rc_content, strlen(init_rc_content), &sz);

	ret = cpio_append(cpio, cpiodata, sz);
	if (ret) {
		LOGE("Failed to append '%s' to the cpio file", init_rc);
		goto oops;
	}

/* end add modified init.lge.fm.rc to ramdisk cpio */
	SEP;

	ret = compress_ramdisk(cpio, ramdisk);
	if (ret)
		goto oops;

	LOGV("Loading new ramdisk into boot image");
	ret = bootimg_load_ramdisk(image, ramdisk);
	if (ret)
		goto oops;

/* end ramdisk modification */
	SEP;
/* start cmdline set */

	LOGV("cmdline: \"%s\"", image->hdr.cmdline);
	LOGV("Setting permissive arguments on cmdline");
	bootimg_set_cmdline_arg(image, "androidboot.selinux", "permissive");
	bootimg_set_cmdline_arg(image, "enforcing", "0");
	LOGV("cmdline: \"%s\"", image->hdr.cmdline);

/* end cmdline set */
	SEP;
/* start flash boot image */
	LOGV("Updating boot image hash");
	bootimg_update_hash(image);

	LOGV("Writing modified boot image to block device '%s'...", flash_block);
	ret = write_boot_image(image, flash_block);
	if (ret) {
		LOGE("Failed to write boot image: %s", strerror(ret));
		goto oops;
	}
	LOGV("Done!");

/* end flash boot image */
	SEP;

	LOGV("Permissive boot has been has been flashed to %s successfully!", flash_block)
	LOGV("You may use '%s' now to enter a permissive system.",
		to_boot ? "reboot" : "reboot recovery");

	if (!to_boot) {
		SEP;
		LOGV("Warning: If you don't reboot now, this will continue to run every 3 minutes!");
	}

	LOGV("***********************************************");
	LOGV("*       give jcadduono a hug, will ya?        *");
	LOGV("***********************************************");

	ret = 0;
oops:
	free_boot_image(image);
	return ret;
}

int main(int argc, char **argv)
{
	int ret = 0;

	LOGV("Welcome to %s! (%s)", APP_NAME, HOST_NAME);

	if (argc > 1 && !strcmp(argv[1], "boot"))
		ret = flash_permissive_boot(1);
	else
		ret = flash_permissive_boot(0);
	if (ret)
		goto oops;

	return 0;
oops:
	LOGE("Error %d: %s", ret, strerror(ret));
	LOGE("Exiting...");
	return ret;
}
