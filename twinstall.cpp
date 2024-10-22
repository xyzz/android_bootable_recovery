/*
	Copyright 2012 to 2017 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string.h>
#include <stdio.h>

#include "twcommon.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"

#ifdef USE_MINZIP
#include "minzip/SysUtil.h"
#else
#include "otautil/SysUtil.h"
#include <ziparchive/zip_archive.h>
#endif
#include "zipwrap.hpp"
#ifdef USE_OLD_VERIFIER
#include "verifier24/verifier.h"
#else
#include "verifier.h"
#endif
#include "variables.h"
#include "data.hpp"
#include "partitions.hpp"
#include "twrpDigestDriver.hpp"
#include "twrpDigest/twrpDigest.hpp"
#include "twrpDigest/twrpMD5.hpp"
#include "twrp-functions.hpp"
#include "gui/gui.hpp"
#include "gui/pages.hpp"
#include "legacy_property_service.h"
#include "twinstall.h"
#include "installcommand.h"
extern "C" {
	#include "gui/gui.h"
}

#define AB_OTA "payload_properties.txt"

static const char* properties_path = "/dev/__properties__";
static const char* properties_path_renamed = "/dev/__properties_kk__";
static bool legacy_props_env_initd = false;
static bool legacy_props_path_modified = false;

enum zip_type {
	UNKNOWN_ZIP_TYPE = 0,
	UPDATE_BINARY_ZIP_TYPE,
	AB_OTA_ZIP_TYPE,
	TWRP_THEME_ZIP_TYPE
};

// to support pre-KitKat update-binaries that expect properties in the legacy format
static int switch_to_legacy_properties()
{
	if (!legacy_props_env_initd) {
		if (legacy_properties_init() != 0)
			return -1;

		char tmp[32];
		int propfd, propsz;
		legacy_get_property_workspace(&propfd, &propsz);
		sprintf(tmp, "%d,%d", dup(propfd), propsz);
		setenv("ANDROID_PROPERTY_WORKSPACE", tmp, 1);
		legacy_props_env_initd = true;
	}

	if (TWFunc::Path_Exists(properties_path)) {
		// hide real properties so that the updater uses the envvar to find the legacy format properties
		if (rename(properties_path, properties_path_renamed) != 0) {
			LOGERR("Renaming %s failed: %s\n", properties_path, strerror(errno));
			return -1;
		} else {
			legacy_props_path_modified = true;
		}
	}

	return 0;
}

static int switch_to_new_properties()
{
	if (TWFunc::Path_Exists(properties_path_renamed)) {
		if (rename(properties_path_renamed, properties_path) != 0) {
			LOGERR("Renaming %s failed: %s\n", properties_path_renamed, strerror(errno));
			return -1;
		} else {
			legacy_props_path_modified = false;
		}
	}

	return 0;
}

static int Install_Theme(const char* path, ZipWrap *Zip) {
#ifdef TW_OEM_BUILD // We don't do custom themes in OEM builds
	Zip->Close();
	return INSTALL_CORRUPT;
#else
	if (!Zip->EntryExists("ui.xml")) {
		return INSTALL_CORRUPT;
	}
	Zip->Close();
	if (!PartitionManager.Mount_Settings_Storage(true))
		return INSTALL_ERROR;
	string theme_path = DataManager::GetSettingsStoragePath();
	theme_path += "/TWRP/theme";
	if (!TWFunc::Path_Exists(theme_path)) {
		if (!TWFunc::Recursive_Mkdir(theme_path)) {
			return INSTALL_ERROR;
		}
	}
	theme_path += "/ui.zip";
	if (TWFunc::copy_file(path, theme_path, 0644) != 0) {
		return INSTALL_ERROR;
	}
	LOGINFO("Installing custom theme '%s' to '%s'\n", path, theme_path.c_str());
	PageManager::RequestReload();
	return INSTALL_SUCCESS;
#endif
}

static int Prepare_Update_Binary(const char *path, ZipWrap *Zip, int* wipe_cache) {
	if (!Zip->ExtractEntry(ASSUMED_UPDATE_BINARY_NAME, TMP_UPDATER_BINARY_PATH, 0755)) {
		Zip->Close();
		LOGERR("Could not extract '%s'\n", ASSUMED_UPDATE_BINARY_NAME);
		return INSTALL_ERROR;
	}

	// If exists, extract file_contexts from the zip file
	if (!Zip->EntryExists("file_contexts")) {
		Zip->Close();
		LOGINFO("Zip does not contain SELinux file_contexts file in its root.\n");
	} else {
		const string output_filename = "/file_contexts";
		LOGINFO("Zip contains SELinux file_contexts file in its root. Extracting to %s\n", output_filename.c_str());
		if (!Zip->ExtractEntry("file_contexts", output_filename, 0644)) {
			Zip->Close();
			LOGERR("Could not extract '%s'\n", output_filename.c_str());
			return INSTALL_ERROR;
		}
	}
	Zip->Close();
	return INSTALL_SUCCESS;
}

static bool update_binary_has_legacy_properties(const char *binary) {
	const char str_to_match[] = "ANDROID_PROPERTY_WORKSPACE";
	int len_to_match = sizeof(str_to_match) - 1;
	bool found = false;

	int fd = open(binary, O_RDONLY);
	if (fd < 0) {
		LOGINFO("has_legacy_properties: Could not open %s: %s!\n", binary, strerror(errno));
		return false;
	}

	struct stat finfo;
	if (fstat(fd, &finfo) < 0) {
		LOGINFO("has_legacy_properties: Could not fstat %d: %s!\n", fd, strerror(errno));
		close(fd);
		return false;
	}

	void *data = mmap(NULL, finfo.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (data == MAP_FAILED) {
		LOGINFO("has_legacy_properties: mmap (size=%zu) failed: %s!\n", finfo.st_size, strerror(errno));
	} else {
		if (memmem(data, finfo.st_size, str_to_match, len_to_match)) {
			LOGINFO("has_legacy_properties: Found legacy property match!\n");
			found = true;
		}
		munmap(data, finfo.st_size);
	}
	close(fd);

	return found;
}

static int Run_Update_Binary(const char *path, ZipWrap *Zip, int* wipe_cache, zip_type ztype) {
	int ret_val, pipe_fd[2], status, zip_verify;
	char buffer[1024];
	FILE* child_data;

#ifndef TW_NO_LEGACY_PROPS
	if (!update_binary_has_legacy_properties(TMP_UPDATER_BINARY_PATH)) {
		LOGINFO("Legacy property environment not used in updater.\n");
	} else if (switch_to_legacy_properties() != 0) { /* Set legacy properties */
		LOGERR("Legacy property environment did not initialize successfully. Properties may not be detected.\n");
	} else {
		LOGINFO("Legacy property environment initialized.\n");
	}
#endif

	pipe(pipe_fd);

	std::vector<std::string> args;
    if (ztype == UPDATE_BINARY_ZIP_TYPE) {
		ret_val = update_binary_command(path, 0, pipe_fd[1], &args);
    } else if (ztype == AB_OTA_ZIP_TYPE) {
		ret_val = abupdate_binary_command(path, Zip, 0, pipe_fd[1], &args);
	} else {
		LOGERR("Unknown zip type %i\n", ztype);
		ret_val = INSTALL_CORRUPT;
	}
    if (ret_val) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return ret_val;
    }

	// Convert the vector to a NULL-terminated char* array suitable for execv.
	const char* chr_args[args.size() + 1];
	chr_args[args.size()] = NULL;
	for (size_t i = 0; i < args.size(); i++)
		chr_args[i] = args[i].c_str();

	pid_t pid = fork();
	if (pid == 0) {
		close(pipe_fd[0]);
		execve(chr_args[0], const_cast<char**>(chr_args), environ);
		printf("E:Can't execute '%s': %s\n", chr_args[0], strerror(errno));
		_exit(-1);
	}
	close(pipe_fd[1]);

	*wipe_cache = 0;

	DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
	child_data = fdopen(pipe_fd[0], "r");
	while (fgets(buffer, sizeof(buffer), child_data) != NULL) {
		char* command = strtok(buffer, " \n");
		if (command == NULL) {
			continue;
		} else if (strcmp(command, "progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			char* seconds_char = strtok(NULL, " \n");

			float fraction_float = strtof(fraction_char, NULL);
			int seconds_float = strtol(seconds_char, NULL, 10);

			if (zip_verify)
				DataManager::ShowProgress(fraction_float * (1 - VERIFICATION_PROGRESS_FRAC), seconds_float);
			else
				DataManager::ShowProgress(fraction_float, seconds_float);
		} else if (strcmp(command, "set_progress") == 0) {
			char* fraction_char = strtok(NULL, " \n");
			float fraction_float = strtof(fraction_char, NULL);
			DataManager::SetProgress(fraction_float);
		} else if (strcmp(command, "ui_print") == 0) {
			char* display_value = strtok(NULL, "\n");
			if (display_value) {
				gui_print("%s", display_value);
			} else {
				gui_print("\n");
			}
		} else if (strcmp(command, "wipe_cache") == 0) {
			*wipe_cache = 1;
		} else if (strcmp(command, "clear_display") == 0) {
			// Do nothing, not supported by TWRP
		} else if (strcmp(command, "log") == 0) {
			printf("%s\n", strtok(NULL, "\n"));
		} else {
			LOGERR("unknown command [%s]\n", command);
		}
	}
	fclose(child_data);

	int waitrc = TWFunc::Wait_For_Child(pid, &status, "Updater");

#ifndef TW_NO_LEGACY_PROPS
	/* Unset legacy properties */
	if (legacy_props_path_modified) {
		if (switch_to_new_properties() != 0) {
			LOGERR("Legacy property environment did not disable successfully. Legacy properties may still be in use.\n");
		} else {
			LOGINFO("Legacy property environment disabled.\n");
		}
	}
#endif

	if (waitrc != 0)
		return INSTALL_ERROR;

	return INSTALL_SUCCESS;
}

const char *karnak_boot_part = "/dev/block/platform/soc/11230000.mmc/by-name/boot";

#define EXPLOIT_TAG "[amonet] "

static int unpatch_boot() {
  FILE *fp = NULL;
  uint8_t boot_data[0x800];
  int ret = -1;

  gui_print_color("highlight", EXPLOIT_TAG "Remove boot patch...");

  fp = fopen(karnak_boot_part, "r+b");
  if (!fp) {
    gui_print_color("highlight", EXPLOIT_TAG "Failed to open the boot device");
    goto cleanup;
  }

  if (fread(boot_data, sizeof(boot_data), 1, fp) != 1) {
    gui_print_color("highlight", EXPLOIT_TAG "Failed to read data");
    goto cleanup;
  }

  if (memcmp(boot_data + 0x400, "ANDROID!", 8) != 0) {
    // Exploit not installed yet, but that's okay
    gui_print_color("highlight", EXPLOIT_TAG "NOT_INSTALLED");
    ret = 0;
    goto cleanup;
  }

  // Assume exploit is installed. Uninstall it by copying the second 0x400 over the first 0x400
  memcpy(boot_data, boot_data + 0x400, 0x400);
  // and zero out the second 0x400
  memset(boot_data + 0x400, 0, 0x400);

  if (fseek(fp, 0, SEEK_SET) != 0) {
    gui_print_color("highlight", EXPLOIT_TAG "Failed to seek");
    goto cleanup;
  }

  if (fwrite(boot_data, sizeof(boot_data), 1, fp) != 1) {
    gui_print_color("highlight", EXPLOIT_TAG "Failed to write data");
    goto cleanup;
  }

  gui_print_color("highlight", EXPLOIT_TAG "OK");
  ret = 0;

cleanup:
  if (fp) {
    fclose(fp);
    fp = NULL;
  }

  return ret;
}

static uint8_t microloader_bin[1024] = {
    0x41, 0x4E, 0x44, 0x52, 0x4F, 0x49, 0x44, 0x21, 0x00, 0x10, 0x00, 0x00, 0xF0, 0xBF, 0xD5, 0x4B,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x40,
    0x00, 0x00, 0x00, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x11, 0x04, 0x0E,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x62, 0x6F, 0x6F, 0x74, 0x6F, 0x70, 0x74, 0x3D, 0x36, 0x34, 0x53, 0x33, 0x2C, 0x33, 0x32, 0x4E,
    0x32, 0x2C, 0x33, 0x32, 0x4E, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0xC0, 0xD5, 0x4B, 0x20, 0x33, 0xD4, 0x4B, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0xC0, 0xD5, 0x4B, 0x00, 0x00, 0x00, 0x00,
    0x23, 0x84, 0xD1, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x33, 0x01, 0xD5, 0x4B, 0x00, 0xC1, 0xD5, 0x4B, 0x00, 0x02, 0x00, 0x00, 0xAD, 0xDE, 0x00, 0x00,
    0x90, 0x4C, 0xD2, 0x4B, 0xAD, 0xDE, 0x00, 0x00, 0xAD, 0xDE, 0x00, 0x00, 0x9B, 0x5E, 0xD2, 0x4B,
    0xAD, 0xDE, 0x00, 0x00, 0x00, 0xC1, 0xD5, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x42, 0xD4, 0xA0, 0xE3, 0x12, 0x00, 0x00, 0xFA, 0x03, 0x4A, 0x13, 0x68, 0x9B, 0x06, 0xFC, 0xD5,
    0x02, 0x4B, 0x18, 0x60, 0x70, 0x47, 0x00, 0xBF, 0x14, 0x20, 0x00, 0x11, 0x00, 0x20, 0x00, 0x11,
    0x0A, 0x28, 0x08, 0xB5, 0x01, 0x46, 0x02, 0xD1, 0x0D, 0x20, 0xFF, 0xF7, 0xED, 0xFF, 0x08, 0x46,
    0xFF, 0xF7, 0xEA, 0xFF, 0x08, 0xBD, 0x38, 0xB5, 0x45, 0x1E, 0x15, 0xF8, 0x01, 0x4F, 0x24, 0xB9,
    0x0A, 0x20, 0xFF, 0xF7, 0xED, 0xFF, 0x20, 0x46, 0x38, 0xBD, 0x20, 0x46, 0xFF, 0xF7, 0xE8, 0xFF,
    0xF3, 0xE7, 0x00, 0xBF, 0x7F, 0xB5, 0x4F, 0xF0, 0x82, 0x44, 0x0E, 0x4E, 0x4F, 0xF4, 0x00, 0x15,
    0x0D, 0x48, 0xFF, 0xF7, 0xE8, 0xFF, 0x33, 0x68, 0x98, 0x47, 0x01, 0x23, 0x4F, 0xF4, 0x00, 0x12,
    0x02, 0x93, 0x00, 0x23, 0x01, 0x95, 0x00, 0x94, 0x01, 0x69, 0x88, 0x47, 0x73, 0x68, 0x29, 0x46,
    0x20, 0x46, 0x98, 0x47, 0x05, 0x48, 0xFF, 0xF7, 0xD6, 0xFF, 0xA0, 0x47, 0x04, 0x48, 0xFF, 0xF7,
    0xD2, 0xFF, 0xFE, 0xE7, 0xFC, 0xC1, 0xD5, 0x4B, 0xA4, 0xC1, 0xD5, 0x4B, 0xC8, 0xC1, 0xD5, 0x4B,
    0xDC, 0xC1, 0xD5, 0x4B, 0x6D, 0x69, 0x63, 0x72, 0x6F, 0x6C, 0x6F, 0x61, 0x64, 0x65, 0x72, 0x20,
    0x62, 0x79, 0x20, 0x78, 0x79, 0x7A, 0x2E, 0x20, 0x43, 0x6F, 0x70, 0x79, 0x72, 0x69, 0x67, 0x68,
    0x74, 0x20, 0x32, 0x30, 0x31, 0x39, 0x2E, 0x00, 0x4A, 0x75, 0x6D, 0x70, 0x20, 0x74, 0x6F, 0x20,
    0x74, 0x68, 0x65, 0x20, 0x70, 0x61, 0x79, 0x6C, 0x6F, 0x61, 0x64, 0x00, 0x53, 0x6F, 0x6D, 0x65,
    0x74, 0x68, 0x69, 0x6E, 0x67, 0x20, 0x77, 0x65, 0x6E, 0x74, 0x20, 0x68, 0x6F, 0x72, 0x72, 0x69,
    0x62, 0x6C, 0x79, 0x20, 0x77, 0x72, 0x6F, 0x6E, 0x67, 0x21, 0x00, 0x00, 0x99, 0xEC, 0xD1, 0x4B,
    0x90, 0x4C, 0xD2, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 
};

static int repatch_boot() {
  FILE *fp = NULL;
  uint8_t boot_data[0x800];
  int ret = -1;

  gui_print_color("highlight", EXPLOIT_TAG "Install boot patch... ");

  fp = fopen(karnak_boot_part, "r+b");
  if (!fp) {
    gui_print_color("highlight", EXPLOIT_TAG "Failed to open the boot device");
    goto cleanup;
  }

  if (fread(boot_data, sizeof(boot_data), 1, fp) != 1) {
    gui_print_color("highlight", EXPLOIT_TAG "Failed to read data");
    goto cleanup;
  }

  if (memcmp(boot_data + 0x400, "ANDROID!", 8) == 0) {
    // Exploit not installed yet, but that's okay
    gui_print_color("highlight", EXPLOIT_TAG "ALREADY_INSTALLED"); // If the rom author injected the boot image herself
    ret = 0;
    goto cleanup;
  }

  // Copy first half to the second half, replace first half with the microloader
  memcpy(boot_data + 0x400, boot_data, 0x400);
  memcpy(boot_data, microloader_bin, 0x400);

  if (fseek(fp, 0, SEEK_SET) != 0) {
    gui_print_color("highlight", EXPLOIT_TAG "Failed to seek");
    goto cleanup;
  }

  if (fwrite(boot_data, sizeof(boot_data), 1, fp) != 1) {
    gui_print_color("highlight", EXPLOIT_TAG "Failed to write data");
    goto cleanup;
  }

  gui_print_color("highlight", EXPLOIT_TAG "OK");
  ret = 0;

cleanup:
  if (fp) {
    fclose(fp);
    fp = NULL;
  }

  return ret;
}

int TWinstall_zip(const char* path, int* wipe_cache) {
	int ret_val, zip_verify = 1;

	if (strcmp(path, "error") == 0) {
		LOGERR("Failed to get adb sideload file: '%s'\n", path);
		return INSTALL_CORRUPT;
	}

	gui_msg(Msg("installing_zip=Installing zip file '{1}'")(path));
	if (strlen(path) < 9 || strncmp(path, "/sideload", 9) != 0) {
		string digest_str;
		string Full_Filename = path;
		string digest_file = path;
		string defmd5file = digest_file + ".md5sum";

		if (TWFunc::Path_Exists(defmd5file)) {
			digest_file += ".md5sum";
		}
		else {
			digest_file += ".md5";
		}

		gui_msg("check_for_digest=Checking for Digest file...");
		if (!TWFunc::Path_Exists(digest_file)) {
			gui_msg("no_digest=Skipping Digest check: no Digest file found");
		}
		else {
			if (TWFunc::read_file(digest_file, digest_str) != 0) {
				LOGERR("Skipping MD5 check: MD5 file unreadable\n");
			}
			else {
				twrpDigest *digest = new twrpMD5();
				if (!twrpDigestDriver::stream_file_to_digest(Full_Filename, digest)) {
					delete digest;
					return INSTALL_CORRUPT;
				}
				string digest_check = digest->return_digest_string();
				if (digest_str == digest_check) {
					gui_msg(Msg("digest_matched=Digest matched for '{1}'.")(path));
				}
				else {
					LOGERR("Aborting zip install: Digest verification failed\n");
					delete digest;
					return INSTALL_CORRUPT;
				}
				delete digest;
			}
		}
	}

#ifndef TW_OEM_BUILD
	DataManager::GetValue(TW_SIGNED_ZIP_VERIFY_VAR, zip_verify);
#endif
	DataManager::SetProgress(0);

	MemMapping map;
#ifdef USE_MINZIP
	if (sysMapFile(path, &map) != 0) {
#else
	if (!map.MapFile(path)) {
#endif
		gui_msg(Msg(msg::kError, "fail_sysmap=Failed to map file '{1}'")(path));
		return -1;
	}

	if (zip_verify) {
		gui_msg("verify_zip_sig=Verifying zip signature...");
#ifdef USE_OLD_VERIFIER
		ret_val = verify_file(map.addr, map.length);
#else
		std::vector<Certificate> loadedKeys;
		if (!load_keys("/res/keys", loadedKeys)) {
			LOGINFO("Failed to load keys");
			gui_err("verify_zip_fail=Zip signature verification failed!");
#ifdef USE_MINZIP
			sysReleaseMap(&map);
#endif
			return -1;
		}
		ret_val = verify_file(map.addr, map.length, loadedKeys, std::bind(&DataManager::SetProgress, std::placeholders::_1));
#endif
		if (ret_val != VERIFY_SUCCESS) {
			LOGINFO("Zip signature verification failed: %i\n", ret_val);
			gui_err("verify_zip_fail=Zip signature verification failed!");
#ifdef USE_MINZIP
			sysReleaseMap(&map);
#endif
			return -1;
		} else {
			gui_msg("verify_zip_done=Zip signature verified successfully.");
		}
	}
	ZipWrap Zip;
	if (!Zip.Open(path, &map)) {
		gui_err("zip_corrupt=Zip file is corrupt!");
#ifdef USE_MINZIP
			sysReleaseMap(&map);
#endif
		return INSTALL_CORRUPT;
	}

	time_t start, stop;
	time(&start);
	if (Zip.EntryExists(ASSUMED_UPDATE_BINARY_NAME)) {
		LOGINFO("Update binary zip\n");
		// Additionally verify the compatibility of the package.
		if (!verify_package_compatibility(&Zip)) {
			gui_err("zip_compatible_err=Zip Treble compatibility error!");
			Zip.Close();
#ifdef USE_MINZIP
			sysReleaseMap(&map);
#endif
			ret_val = INSTALL_CORRUPT;
		} else {
			ret_val = Prepare_Update_Binary(path, &Zip, wipe_cache);
			if (ret_val == INSTALL_SUCCESS) {
				if (unpatch_boot() < 0)
					ret_val = INSTALL_ERROR;
				if (ret_val == INSTALL_SUCCESS)
					ret_val = Run_Update_Binary(path, &Zip, wipe_cache, UPDATE_BINARY_ZIP_TYPE);
				if (repatch_boot() < 0)
					ret_val = INSTALL_ERROR;
			}
		}
	} else {
		if (Zip.EntryExists(AB_OTA)) {
			LOGINFO("AB zip\n");
			ret_val = Run_Update_Binary(path, &Zip, wipe_cache, AB_OTA_ZIP_TYPE);
		} else {
			if (Zip.EntryExists("ui.xml")) {
				LOGINFO("TWRP theme zip\n");
				ret_val = Install_Theme(path, &Zip);
			} else {
				Zip.Close();
				ret_val = INSTALL_CORRUPT;
			}
		}
	}
	time(&stop);
	int total_time = (int) difftime(stop, start);
	if (ret_val == INSTALL_CORRUPT) {
		gui_err("invalid_zip_format=Invalid zip file format!");
	} else {
		LOGINFO("Install took %i second(s).\n", total_time);
	}
#ifdef USE_MINZIP
	sysReleaseMap(&map);
#endif
	return ret_val;
}
