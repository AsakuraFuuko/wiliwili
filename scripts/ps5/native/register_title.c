/*
 * wiliwili PS5 native application - title registration payload.
 * Copyright (C) 2026 wiliwili contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Registers an already-installed title directory with the shell database:
 * /system_ex/app/<TITLE_ID>/ holds the application image and
 * /user/app/<TITLE_ID>/sce_sys/param.json the user-side metadata. Upstream's
 * homebrew install helper relies on the same call (ps5-payload-sdk
 * samples/install_app); a payload has the privileges a title sandbox does not.
 *
 * usage: register-native.elf <TITLE_ID>
 */

#include <stdio.h>
#include <string.h>

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallTitleDir(const char *title_id, const char *dir,
                                     void *reserved);

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("usage: register-native.elf <TITLE_ID>\n");
    return 2;
  }
  if (strlen(argv[1]) != 9) {
    printf("title id must be nine characters: %s\n", argv[1]);
    return 2;
  }

  int error = sceAppInstUtilInitialize();
  if (error != 0) {
    printf("sceAppInstUtilInitialize: %#x\n", error);
    return 1;
  }

  error = sceAppInstUtilAppInstallTitleDir(argv[1], "/user/app/", 0);
  if (error != 0) {
    printf("sceAppInstUtilAppInstallTitleDir(%s): %#x\n", argv[1], error);
    return 1;
  }

  printf("registered %s\n", argv[1]);
  return 0;
}
