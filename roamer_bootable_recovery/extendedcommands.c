#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <reboot/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "../../external/yaffs2/yaffs2/utils/mkyaffs2image.h"
#include "../../external/yaffs2/yaffs2/utils/unyaffs.h"

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"
#include "flashutils/flashutils.h"
#include "edify/expr.h"
#include <libgen.h>
#include "mtdutils/mtdutils.h"
#include "bmlutils/bmlutils.h"

#define ABS_MT_POSITION_X 0x35  /* Center X ellipse position */

int signature_check_enabled = 0;
int script_assert_enabled = 0;
static const char *SDCARD_UPDATE_FILE = "/sdcard/update.zip";

int install_zip(const char* packagefilepath)
{
    ui_print("\n-- installing: %s\n", packagefilepath);
    if (device_flash_type() == MTD) {
        set_sdcard_update_bootloader_message();
    }
    int status = install_package(packagefilepath);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_print("installation aborted.\n");
        return 1;
    }
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_print("\ninstall complete.\n");
    return 0;
}

void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles)
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("couldn't open directory.\n");
        return NULL;
    }

    int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0)
            {
                total++;
                continue;
            }

            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("failed to close directory.");
    }

    if (total==0) {
        return NULL;
    }

    // sort the result
    if (files != NULL) {
        for (i = 0; i < total; i++) {
            int curMax = -1;
            int j;
            for (j = 0; j < total - i; j++) {
                if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
                    curMax = j;
            }
            char* temp = files[curMax];
            files[curMax] = files[total - i - 1];
            files[total - i - 1] = temp;
        }
    }

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, const char* headers[])
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0)
    {
        ui_print("no files found.\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;)
        {
            int chosen_item = get_menu_selection(headers, list, 0, 0);
            if (chosen_item == GO_BACK)
                break;
            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            }
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

void show_choose_zip_menu(const char *mount_point)
{
    if (ensure_path_mounted(mount_point) != 0) {
        LOGE ("can't mount %s\n", mount_point);
        return;
    }

    static char* headers[] = {  "          which one?",
				"",
                                "",
                                NULL
    };

    char* file = choose_file_menu(mount_point, ".zip", headers);
    if (file == NULL)
        return;
    static char* confirm_install  = "confirm install?";
    static char confirm[PATH_MAX];
    sprintf(confirm, "yes - Install %s", basename(file));
    if (confirm_selection(confirm_install, confirm))
        install_zip(file);
}

void show_nandroid_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE("can't mount %s\n", path);
        return;
    }

    static char* headers[] = {  "          which one?",
				"",
                                "",
                                NULL
    };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("confirm restore?", "yes - restore"))
        nandroid_restore(file, 1, 0, 1, 1, 1, 1);
}

#ifndef BOARD_UMS_LUNFILE
#define BOARD_UMS_LUNFILE	"/sys/devices/platform/usb_mass_storage/lun0/file"
#endif

void show_mount_usb_storage_menu()
{
    int fd;
    Volume *vol = volume_for_path("/sdcard");
    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("unable to open ums lunfile (%s)", strerror(errno));
        return -1;
    }

    if ((write(fd, vol->device, strlen(vol->device)) < 0) &&
        (!vol->device2 || (write(fd, vol->device, strlen(vol->device2)) < 0))) {
        LOGE("unable to write to ums lunfile (%s)", strerror(errno));
        close(fd);
        return -1;
    }
    static char* headers[] = {  "          usb mounted",
                                "",
                                "",
                                NULL
    };

    static char* list[] = { "unmount", NULL };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == 0)
            break;
    }

    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("unable to open ums lunfile (%s)", strerror(errno));
        return -1;
    }

    char ch = 0;
    if (write(fd, &ch, 1) < 0) {
        LOGE("unable to write to ums lunfile (%s)", strerror(errno));
        close(fd);
        return -1;
    }
}

int confirm_selection(const char* title, const char* confirm)
{
    struct stat info;
    if (0 == stat("/sdcard/clockworkmod/.no_confirm", &info))
        return 1;

    char* confirm_headers[] = { "         are you sure?", "", "", NULL };
		char* items[] = { "no",
				  "no",
				  confirm, //" Yes -- wipe partition",   // [2]
				  "no",
				  "no",
				  NULL };
		int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
		return chosen_item == 2;
}

#define MKE2FS_BIN      "/sbin/mke2fs"
#define TUNE2FS_BIN     "/sbin/tune2fs"
#define E2FSCK_BIN      "/sbin/e2fsck"

int format_device(const char *device, const char *path, const char *fs_type) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        // no /sdcard? let's assume /data/media
        if (strstr(path, "/sdcard") == path && is_data_media()) {
            return format_unknown_device(NULL, path, NULL);
        }
        // silent failure for sd-ext
        if (strcmp(path, "/sd-ext") == 0)
            return -1;
        LOGE("unknown volume \"%s\"\n", path);
        return -1;
    }
    if (strcmp(fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", path);
        return -1;
    }

    if (strcmp(fs_type, "rfs") == 0) {
        if (ensure_path_unmounted(path) != 0) {
            LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
            return -1;
        }
        if (0 != format_rfs_device(device, path)) {
            LOGE("format_volume: format_rfs_device failed on %s\n", device);
            return -1;
        }
        return 0;
    }
 
    if (strcmp(v->mount_point, path) != 0) {
        return format_unknown_device(v->device, path, NULL);
    }

    if (ensure_path_unmounted(path) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(fs_type, "yaffs2") == 0 || strcmp(fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n",device);
            return -1;
        }
        return 0;
    }

    if (strcmp(fs_type, "ext4") == 0) {
        reset_ext4fs_info();
        int result = make_ext4fs(device, NULL, NULL, 0, 0, 0);
        if (result != 0) {
            LOGE("format_volume: make_extf4fs failed on %s\n", device);
            return -1;
        }
        return 0;
    }

    return format_unknown_device(device, path, fs_type);
}

int format_unknown_device(const char *device, const char* path, const char *fs_type)
{
    LOGI("formatting unknown device.\n");

    if (fs_type != NULL && get_flash_type(fs_type) != UNSUPPORTED)
        return erase_raw_partition(fs_type, device);

    // if this is SDEXT:, don't worry about it if it does not exist.
    if (0 == strcmp(path, "/sd-ext"))
    {
        struct stat st;
        Volume *vol = volume_for_path("/sd-ext");
        if (vol == NULL || 0 != stat(vol->device, &st))
        {
            ui_print("skipping format of /sd-ext.\n");
            return 0;
        }
    }

    if (NULL != fs_type) {
        if (strcmp("ext3", fs_type) == 0) {
            LOGI("formatting ext3 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext3_device(device);
        }

        if (strcmp("ext2", fs_type) == 0) {
            LOGI("formatting ext2 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext2_device(device);
        }
    }

    if (0 != ensure_path_mounted(path))
    {
        ui_print("error mounting %s!\n", path);
        ui_print("skipping format...\n");
        return 0;
    }

    static char tmp[PATH_MAX];
    if (strcmp(path, "/data") == 0) {
        sprintf(tmp, "cd /data ; for f in $(ls -a | grep -v ^media$); do rm -rf $f; done");
        __system(tmp);
    }
    else {
        sprintf(tmp, "rm -rf %s/*", path);
        __system(tmp);
        sprintf(tmp, "rm -rf %s/.*", path);
        __system(tmp);
    }

    ensure_path_unmounted(path);
    return 0;
}

//#define MOUNTABLE_COUNT 5
//#define DEVICE_COUNT 4
//#define MMC_COUNT 2

typedef struct {
    char mount[255];
    char unmount[255];
    Volume* v;
} MountMenuEntry;

typedef struct {
    char txt[255];
    Volume* v;
} FormatMenuEntry;

int is_safe_to_format(char* name)
{
    char str[255];
    char* partition;
    property_get("ro.cwm.forbid_format", str, "/misc,/radio,/bootloader,/recovery,/efs");

    partition = strtok(str, ", ");
    while (partition != NULL) {
        if (strcmp(name, partition) == 0) {
            return 0;
        }
        partition = strtok(NULL, ", ");
    }

    return 1;
}

void show_mount_menu()
{
    static char* headers[] = {  "          what to do?",
                                "",
				"",
                                NULL
    };

    static MountMenuEntry* mount_menue = NULL;
    static FormatMenuEntry* format_menue = NULL;

    typedef char* string;

    int i, mountable_volumes, formatable_volumes;
    int num_volumes;
    Volume* device_volumes;

    num_volumes = get_num_volumes();
    device_volumes = get_device_volumes();

    string options[255];

    if(!device_volumes)
		return;

		mountable_volumes = 0;
		formatable_volumes = 0;

		mount_menue = malloc(num_volumes * sizeof(MountMenuEntry));
		format_menue = malloc(num_volumes * sizeof(FormatMenuEntry));

		for (i = 0; i < num_volumes; ++i) {
			Volume* v = &device_volumes[i];
			if(strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) != 0 && strcmp("emmc", v->fs_type) != 0 && strcmp("bml", v->fs_type) != 0)
			{
				sprintf(&mount_menue[mountable_volumes].mount, "mount %s", v->mount_point);
				sprintf(&mount_menue[mountable_volumes].unmount, "unmount %s", v->mount_point);
				mount_menue[mountable_volumes].v = &device_volumes[i];
				++mountable_volumes;
				if (is_safe_to_format(v->mount_point)) {
					sprintf(&format_menue[formatable_volumes].txt, "format %s", v->mount_point);
					format_menue[formatable_volumes].v = &device_volumes[i];
					++formatable_volumes;
				}
		    }
		    else if (strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) == 0 && is_safe_to_format(v->mount_point))
		    {
				sprintf(&format_menue[formatable_volumes].txt, "format %s", v->mount_point);
				format_menue[formatable_volumes].v = &device_volumes[i];
				++formatable_volumes;
			}
		}


    static char* confirm_format  = "confirm format?";
    static char* confirm = "yes - format";
    char confirm_string[255];

    for (;;)
    {

		for (i = 0; i < mountable_volumes; i++)
		{
			MountMenuEntry* e = &mount_menue[i];
			Volume* v = e->v;
			if(is_path_mounted(v->mount_point))
				options[i] = e->unmount;
			else
				options[i] = e->mount;
		}

		for (i = 0; i < formatable_volumes; i++)
		{
			FormatMenuEntry* e = &format_menue[i];

			options[mountable_volumes+i] = e->txt;
		}

        options[mountable_volumes+formatable_volumes] = "mount usb storage";
        options[mountable_volumes+formatable_volumes + 1] = NULL;

        int chosen_item = get_menu_selection(headers, &options, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        if (chosen_item == (mountable_volumes+formatable_volumes))
        {
            show_mount_usb_storage_menu();
        }
        else if (chosen_item < mountable_volumes)
        {
			MountMenuEntry* e = &mount_menue[chosen_item];
            Volume* v = e->v;

            if (is_path_mounted(v->mount_point))
            {
                if (0 != ensure_path_unmounted(v->mount_point))
                    ui_print("error unmounting %s!\n", v->mount_point);
            }
            else
            {
                if (0 != ensure_path_mounted(v->mount_point))
                    ui_print("error mounting %s!\n",  v->mount_point);
            }
        }
        else if (chosen_item < (mountable_volumes + formatable_volumes))
        {
            chosen_item = chosen_item - mountable_volumes;
            FormatMenuEntry* e = &format_menue[chosen_item];
            Volume* v = e->v;

            sprintf(confirm_string, "%s - %s", v->mount_point, confirm_format);

            if (!confirm_selection(confirm_string, confirm))
                continue;
            ui_print("formatting %s...\n", v->mount_point);
            if (0 != format_volume(v->mount_point))
                ui_print("error formatting %s!\n", v->mount_point);
            else
                ui_print("done.\n");
        }
    }

    free(mount_menue);
    free(format_menue);

}

void show_nandroid_advanced_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE ("can't mount sdcard\n");
        return;
    }

    static char* advancedheaders[] = {  "  restore from which nandroid?",
                                	"",
                                	"",
                                	NULL
    };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, advancedheaders);
    if (file == NULL)
        return;

    static char* headers[] = {  "         restore what?",
                                "",
				"",
                                NULL
    };

    static char* list[] = { "restore boot",
                            "restore recovery",
                            "restore system",
                            "restore data",
                            "restore cache",
                            "restore sd-ext",
                            NULL
    };
    
    static char* confirm_restore  = "confirm restore?";

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "yes - restore boot"))
                nandroid_restore(file, 1, 0, 0, 0, 0, 0);
            break;
        case 1:
            if (confirm_selection(confirm_restore, "yes - restore recovery"))
                nandroid_restore(file, 0, 1, 0, 0, 0, 0);
            break;
        case 2:
            if (confirm_selection(confirm_restore, "yes - restore system"))
                nandroid_restore(file, 0, 0, 1, 0, 0, 0);
            break;
        case 3:
            if (confirm_selection(confirm_restore, "yes - restore data"))
                nandroid_restore(file, 0, 0, 0, 1, 0, 0);
            break;
        case 4:
            if (confirm_selection(confirm_restore, "yes - restore cache"))
                nandroid_restore(file, 0, 0, 0, 0, 1, 0);
            break;
        case 5:
            if (confirm_selection(confirm_restore, "yes - restore sd-ext"))
                nandroid_restore(file, 0, 0, 0, 0, 0, 1);
            break;
    }
}

void show_nandroid_menu()
{
    static char* headers[] = {  "       Backup or Restore?",
				"",
                                "",
                                NULL
    };

    static char* list[] = { "backup",
                            "restore",
                            "advanced restore",
                            "backup to internal sdcard",
                            "restore from internal sdcard",
                            "advanced restore from internal sdcard",
                            NULL
    };

    if (volume_for_path("/emmc") == NULL)
        list[3] = NULL;

	static char* confirm_backup  = "confirm backup?";

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            {
		if (confirm_selection(confirm_backup, "yes - backup"))
		{
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/sdcard/clockworkmod/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
                }
                nandroid_backup(backup_path);
            }
	}
            break;
        case 1:
            show_nandroid_restore_menu("/sdcard");
            break;
        case 2:
            show_nandroid_advanced_restore_menu("/sdcard");
            break;
        case 3:
            {
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/emmc/clockworkmod/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/emmc/clockworkmod/backup/%F.%H.%M.%S", tmp);
                }
                nandroid_backup(backup_path);
            }
            break;
        case 4:
            show_nandroid_restore_menu("/emmc");
            break;
        case 5:
            show_nandroid_advanced_restore_menu("/emmc");
            break;
    }
}

void wipe_battery_stats()
{
    ensure_path_mounted("/data");
    remove("/data/system/batterystats.bin");
    ensure_path_unmounted("/data");
    ui_print("Battery Stats wiped.\n");
}

void show_apps_menu()
{
    static char* headers[] = {  "       What to do?",
				"",
                                "",
                                NULL
    };

    static char* list[] = { "system app backup",
                            "system app restore",
                            "user app backup",
                            "user app restore",
                            NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            {
		if (confirm_selection("confirm backup?", "yes - backup"))
		{
		ensure_path_mounted("/system");
		ensure_path_unmounted("/data");
		ensure_path_mounted("/sdcard");
		ui_print("removing last backup...\n");
		__system("rm /sdcard/clockworkmod/system_app-last.tar");
		ui_print("creating new backup...\n");
		__system("tar cvf /sdcard/clockworkmod/system_app-last.tar /system/app/");
		ui_print("backup system app finished...\n");
            break;
        case 1:
		if (confirm_selection("confirm restore?", "yes - restore"))
		{
		ensure_path_mounted("/system");
		ensure_path_mounted("/data");
		ensure_path_mounted("/sdcard");
		ensure_path_mounted("/cache");
		ensure_path_mounted("/sd-ext");
		ui_print("cleaning...\n");
		__system("rm -r /system/app");
		__system("rm -r /data/dalvik-cache");
		__system("rm -r /cache/dalvik-cache");
		__system("rm -r /sd-ext/dalvik-cache");
		ensure_path_unmounted("/data");
		ui_print("restoring last backup...\n");
		__system("tar xvf /sdcard/clockworkmod/system_app-last.tar");
		ui_print("restore system app finished...\n");
            break;
        case 2:
		if (confirm_selection("confirm backup?", "yes - backup"))
		{
		ensure_path_mounted("/data");
		ensure_path_mounted("/sdcard");
		ensure_path_unmounted("/system");
		ui_print("removing last backup...\n");
		__system("rm /sdcard/clockworkmod/data_app-last.tar");
		ui_print("creating new backup...\n");
		__system("tar cvf /sdcard/clockworkmod/data_app-last.tar /data/app/");
		ui_print("backup user app finished...\n");
            break;
        case 3:
		if (confirm_selection("confirm restore?", "yes - restore"))
		{
		ensure_path_mounted("/data");
		ensure_path_mounted("/sdcard");
		ensure_path_mounted("/cache");
		ensure_path_mounted("/sd-ext");
		ensure_path_unmounted("/system");
		ui_print("cleaning...\n");
		__system("rm -r /data/app");
		__system("rm -r /data/dalvik-cache");
		__system("rm -r /cache/dalvik-cache");
		__system("rm -r /sd-ext/dalvik-cache");
		ui_print("restoring last backup...\n");
		__system("tar xvf /sdcard/clockworkmod/data_app-last.tar");
		ui_print("restore user app finished...\n");
            break;
}
}
}
}
}
}
}

void show_config_menu()
{
    static char* headers[] = {  "       What to do?",
				"",
                                "",
                                NULL
    };

    static char* list[] = { "backup rom settings config",
                            "restore rom settings config",
                            "backup all app data",
                            "restore all app data",
                            NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            {
		if (confirm_selection("confirm backup?", "yes - backup"))
		{
		ensure_path_mounted("/data");
		ensure_path_mounted("/sdcard");
		ui_print("removing last backup...\n");
		__system("rm /sdcard/clockworkmod/rom-settings-last.tar");
		ui_print("creating new backup...\n");
		__system("tar cvf /sdcard/clockworkmod/rom-settings-last.tar /data/data/com.android.providers.settings");
		ui_print("backup rom settings finished...\n");
            break;
        case 1:
		if (confirm_selection("confirm restore?", "yes - restore"))
		{
		ensure_path_mounted("/data");
		ensure_path_mounted("/sdcard");
		ensure_path_unmounted("/system");
		__system("rm -r /data/data/com.android.providers.settings");
		ui_print("restoring last backup...\n");
		__system("tar xvf /sdcard/clockworkmod/rom-settings-last.tar");
		ui_print("restore rom settings finished...\n");
            break;
        case 2:
		if (confirm_selection("confirm backup?", "yes - backup"))
		{
		ensure_path_mounted("/data");
		ensure_path_mounted("/sdcard");
		ui_print("removing last backup...\n");
		__system("rm /sdcard/clockworkmod/data_data-last.tar");
		ui_print("creating new backup...\n");
		__system("tar cvf /sdcard/clockworkmod/data_data-last.tar /data/data/");
		ui_print("backup all app data finished...\n");
            break;
        case 3:
		if (confirm_selection("confirm restore?", "yes - restore"))
		{
		ensure_path_mounted("/data");
		ensure_path_mounted("/sdcard");
		ensure_path_mounted("/cache");
		ensure_path_mounted("/sd-ext");
		ensure_path_unmounted("/system");
		ui_print("cleaning...\n");
		__system("rm -r /data/data");
		__system("rm -r /data/dalvik-cache");
		__system("rm -r /cache/dalvik-cache");
		__system("rm -r /sd-ext/dalvik-cache");
		ui_print("restoring last backup...\n");
		__system("tar xvf /sdcard/clockworkmod/data_data-last.tar");
		ui_print("restore all app data finished...\n");
            break;
}
}
}
}
}
}
}

void show_wipes_menu()
{
    static char* headers[] = {  "       Wipe What?",
				"",
                                "",
                                NULL
    };

    static char* list[] = { "wipe /system",
                            "wipe /data all",
                            "wipe /data/app",
                            "wipe /data/data",
                            "wipe /data/media",
                            "wipe /cache",
                            "wipe dalvik-cache",
                            "wipe /sdcard",
                            "wipe /sd-ext",
                            "wipe battery stats",
                            NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            {
		if (confirm_selection("confirm wipe?", "yes - wipe /system"))
		{
		ensure_path_mounted("/system");
		ui_print("wiping /system...\n");
		__system("rm -r /system");
		ui_print("/system wiped...\n");
            break;

        case 1:
		if (confirm_selection("confirm wipe?", "yes - wipe /data all"))
		{
		ensure_path_mounted("/data");
		ui_print("wiping /data...\n");
		__system("rm -r /data");
		ui_print("/data completely wiped...\n");
            break;

        case 2:
		if (confirm_selection("confirm wipe?", "yes - wipe /data/app"))
		{
		ensure_path_mounted("/data");
		ui_print("wiping /data/app...\n");
		__system("rm -r /data/app");
		ui_print("/data/app wiped...\n");
            break;

        case 3:
		if (confirm_selection("confirm wipe?", "yes - wipe /data/data"))
		{
		ensure_path_mounted("/data");
		ui_print("wiping /data/data...\n");
		__system("rm -r /data/data");
		ui_print("/data/data wiped...\n");
            break;

        case 4:
		if (confirm_selection("confirm wipe?", "yes - wipe /data/media"))
		{
		ensure_path_mounted("/data");
		ui_print("wiping /data/media...\n");
		__system("rm -r /data/media");
		ui_print("/data/media wiped...\n");
            break;

        case 5:
		if (confirm_selection("confirm wipe?", "yes - wipe /cache"))
		{
		ensure_path_mounted("/cache");
		ui_print("wiping /cache...\n");
		__system("rm -r /cache");
		ui_print("/cache wiped...\n");
            break;

	case 6:
		if (confirm_selection("confirm wipe?", "yes - wipe dalvik-cache"))
		{
		ensure_path_mounted("/sd-ext");
		ensure_path_mounted("/cache");
		ensure_path_mounted("/data");
		ui_print("wiping dalvik-cache...\n");
		    __system("rm -r /data/dalvik-cache");
		    __system("rm -r /cache/dalvik-cache");
		    __system("rm -r /sd-ext/dalvik-cache");
		    ui_print("dalvik-cache wiped.\n");
		break;

        case 7:
		if (confirm_selection("confirm wipe?", "yes - wipe /sdcard"))
		{
		ensure_path_mounted("/sdcard");
		ui_print("wiping /sdcard...\n");
		__system("rm -r /sdcard");
		ui_print("/sdcard wiped...\n");
            break;

        case 8:
		if (confirm_selection("confirm wipe?", "yes - wipe /sdcard"))
		{
		ensure_path_mounted("/sd-ext");
		ui_print("wiping /sd-ext...\n");
		__system("rm -r /sd-ext");
		ui_print("/sd-ext wiped...\n");
            break;
	case 9:
		wipe_battery_stats();
		ui_print("battery stats wiped...\n");
	    break;
} } }
} } }
} } }
} } }

void show_mounts_menu()
{
    static char* headers[] = {  "       Mount What?",
				"",
                                "",
                                NULL
    };

    static char* list[] = { "mount /system",
                            "mount /data",
                            "mount /cache",
                            "mount /sdcard",
                            NULL
    };

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
		ensure_path_mounted("/system");
		ui_print("/system mounted...\n");
            break;

        case 1:
		ensure_path_mounted("/data");
		ui_print("/data mounted...\n");
	    break;

        case 2:
		ensure_path_mounted("/cache");
		ui_print("/cache mounted...\n");
	    break;

        case 3:
		ensure_path_mounted("/sdcard");
		ui_print("/sdcard mounted...\n");
	    break;

} 
}


void show_advanced_menu()
{
    static char* headers[] = {  "Advanced and Debugging Menu",
                                "",
                                NULL
    };

    static char* list[] = { "Reboot Recovery",
                            "Wipe Dalvik Cache",
                            "Wipe Battery Stats",
                            "Report Error",
                            "Key Test",
                            "Show log",
#ifndef BOARD_HAS_SMALL_RECOVERY
                            "Partition SD Card",
                            "Fix Permissions",
#ifdef BOARD_HAS_SDCARD_INTERNAL
                            "Partition Internal SD Card",
#endif
#endif
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0:
            {
                reboot_wrapper("recovery");
                break;
            }
            case 1:
            {
                if (0 != ensure_path_mounted("/data"))
                    break;
                ensure_path_mounted("/sd-ext");
                ensure_path_mounted("/cache");
                if (confirm_selection( "confirm wipe?", "yes - wipe dalvik-cache")) {
                    __system("rm -r /data/dalvik-cache");
                    __system("rm -r /cache/dalvik-cache");
                    __system("rm -r /sd-ext/dalvik-cache");
                    ui_print("dalvik-cache wiped.\n");
                }
                ensure_path_unmounted("/data");
                break;
            }
            case 2:
            {
                if (confirm_selection( "confirm wipe?", "yes - wipe battery stats"))
                    wipe_battery_stats();
                break;
            }
            case 3:
                handle_failure(1);
                break;
            case 4:
            {
                ui_print("outputting key codes.\n");
                ui_print("go back to end debugging.\n");
                struct keyStruct{
					int code;
					int x;
					int y;
				}*key;
                int action;
                do
                {
                    key = ui_wait_key();
					if(key->code == ABS_MT_POSITION_X)
					{
				        action = device_handle_mouse(key, 1);
						ui_print("touch: X: %d\tY: %d\n", key->x, key->y);
					}
					else
					{
				        action = device_handle_key(key->code, 1);
						ui_print("key: %x\n", key->code);
					}
                }
                while (action != GO_BACK);
                break;
            }
            case 5:
            {
                ui_printlogtail(12);
                break;
            }
            case 6:
            {
                static char* ext_sizes[] = { "128M",
                                             "256M",
                                             "512M",
                                             "1024M",
                                             "2048M",
                                             "4096M",
                                             NULL };

                static char* swap_sizes[] = { "0M",
                                              "32M",
                                              "64M",
                                              "128M",
                                              "256M",
                                              NULL };

                static char* ext_headers[] = { "ext size", "", NULL };
                static char* swap_headers[] = { "swap size", "", NULL };

                int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
                if (ext_size == GO_BACK)
                    continue;

                int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0);
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                Volume *vol = volume_for_path("/sdcard");
                strcpy(sddevice, vol->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", ext_sizes[ext_size], swap_sizes[swap_size]);
                ui_print("partitioning sdcard... please wait...\n");
                if (0 == __system(cmd))
                    ui_print("Done!\n");
                else
                    ui_print("an error occured while partitioning your sdcard.\n");
                break;
            }
            case 7:
            {
                ensure_path_mounted("/system");
                ensure_path_mounted("/data");
                ui_print("fixing permissions...\n");
                __system("fix_permissions");
                ui_print("done!\n");
                break;
            }
            case 8:
            {
                static char* ext_sizes[] = { "128M",
                                             "256M",
                                             "512M",
                                             "1024M",
                                             "2048M",
                                             "4096M",
                                             NULL };

                static char* swap_sizes[] = { "0M",
                                              "32M",
                                              "64M",
                                              "128M",
                                              "256M",
                                              NULL };

                static char* ext_headers[] = { "data Size", "", NULL };
                static char* swap_headers[] = { "swap Size", "", NULL };

                int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
                if (ext_size == GO_BACK)
                    continue;

                int swap_size = 0;
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                Volume *vol = volume_for_path("/emmc");
                strcpy(sddevice, vol->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs ext3 -s", ext_sizes[ext_size], swap_sizes[swap_size]);
                ui_print("partitioning internal sdcard... please wait...\n");
                if (0 == __system(cmd))
                    ui_print("done!\n");
                else
                    ui_print("an error occured while partitioning your Internal sdcard.\n");
                break;
            }
        }
    }
}

void write_fstab_root(char *path, FILE *file)
{
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGW("unable to get recovery.fstab info for %s during fstab generation!\n", path);
        return;
    }

    char device[200];
    if (vol->device[0] != '/')
        get_partition_device(vol->device, device);
    else
        strcpy(device, vol->device);

    fprintf(file, "%s ", device);
    fprintf(file, "%s ", path);
    // special case rfs cause auto will mount it as vfat on samsung.
    fprintf(file, "%s rw\n", vol->fs_type2 != NULL && strcmp(vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

void create_fstab()
{
    struct stat info;
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("unable to create /etc/fstab!\n");
        return;
    }
    Volume *vol = volume_for_path("/boot");
    if (NULL != vol && strcmp(vol->fs_type, "mtd") != 0 && strcmp(vol->fs_type, "emmc") != 0 && strcmp(vol->fs_type, "bml") != 0)
         write_fstab_root("/boot", file);
    write_fstab_root("/cache", file);
    write_fstab_root("/data", file);
    write_fstab_root("/datadata", file);
    write_fstab_root("/emmc", file);
    write_fstab_root("/system", file);
    write_fstab_root("/sdcard", file);
    write_fstab_root("/sd-ext", file);
    fclose(file);
    LOGI("completed outputting fstab.\n");
}

int bml_check_volume(const char *path) {
    ui_print("checking %s...\n", path);
    ensure_path_unmounted(path);
    if (0 == ensure_path_mounted(path)) {
        ensure_path_unmounted(path);
        return 0;
    }
    
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGE("unable process volume! Skipping...\n");
        return 0;
    }
    
    ui_print("%s may be rfs. Checking...\n", path);
    char tmp[PATH_MAX];
    sprintf(tmp, "mount -t rfs %s %s", vol->device, path);
    int ret = __system(tmp);
    printf("%d\n", ret);
    return ret == 0 ? 1 : 0;
}

void process_volumes() {
    create_fstab();

    if (is_data_media()) {
        setup_data_media();
    }

    return;

    // dead code.
    if (device_flash_type() != BML)
        return;

    ui_print("checking for ext4 partitions...\n");
    int ret = 0;
    ret = bml_check_volume("/system");
    ret |= bml_check_volume("/data");
    if (has_datadata())
        ret |= bml_check_volume("/datadata");
    ret |= bml_check_volume("/cache");
    
    if (ret == 0) {
        ui_print("done!\n");
        return;
    }
    
    char backup_path[PATH_MAX];
    time_t t = time(NULL);
    char backup_name[PATH_MAX];
    struct timeval tp;
    gettimeofday(&tp, NULL);
    sprintf(backup_name, "before-ext4-convert-%d", tp.tv_sec);
    sprintf(backup_path, "/sdcard/clockworkmod/backup/%s", backup_name);

    ui_set_show_text(1);
    ui_print("filesystems need to be converted to ext4.\n");
    ui_print("a backup and restore will now take place.\n");
    ui_print("if anything goes wrong, your backup will be\n");
    ui_print("named %s. try restoring it\n", backup_name);
    ui_print("in case of error.\n");

    nandroid_backup(backup_path);
    nandroid_restore(backup_path, 1, 1, 1, 1, 1, 0);
    ui_set_show_text(0);
}

void handle_failure(int ret)
{
    if (ret == 0)
        return;
    if (0 != ensure_path_mounted("/sdcard"))
        return;
    mkdir("/sdcard/clockworkmod", S_IRWXU);
    __system("cp /tmp/recovery.log /sdcard/clockworkmod/recovery.log");
    ui_print("/tmp/recovery.log was copied to /sdcard/clockworkmod/recovery.log.\n");
}

int is_path_mounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return 0;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 1;
    }
    return 0;
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    return vol != NULL;
}

int volume_main(int argc, char **argv) {
    load_volume_table();
    return 0;
}
