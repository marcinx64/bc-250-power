// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Translation catalogs of the web UI, gzip-compressed JSON, generated into
// i18n_data.cpp by touch_buildinfo.py from ui/i18n/*.json. The first entry is
// the language list ("index"); English is the UI's own text and has no entry.
#include <stddef.h>
#include <stdint.h>

struct I18nFile {
  const char *name;      // "index" or the language code, e.g. "pl": served as /i18n/<name>.json
  const char *etag;      // quoted, changes with the content
  const uint8_t *data;   // gzip
  size_t len;
};
extern const I18nFile I18N_FILES[];
extern const size_t I18N_FILE_COUNT;
