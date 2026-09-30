// SPDX-License-Identifier: GPL-2.0-or-later
//
// A memory-backed stand-in for the FatFs API (ff.h) in the headless mixer.
//
// EdgeTX reads and writes models and radio settings as files on the SD card
// (storage/sdcard_yaml.cpp). The headless build has no filesystem at run time:
// the API puts the YAML the host passed in into a memory file, lets EdgeTX's
// own reader open it by path, and collects what EdgeTX's own writer produces
// the same way. Paths are case-insensitive, as they are on the radio's FAT
// card. Directories are implicit: every directory exists and is empty unless a
// file below it does. Anything else FatFs offers answers FR_NO_FILE or FR_OK.

#include "etx_port_impl.h"

#include "ff.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

namespace {

struct MemFile {
  std::vector<uint8_t> data;
};

struct OpenFile {
  std::string key;
  bool write;
};

std::map<std::string, MemFile>& files()
{
  static std::map<std::string, MemFile> table;
  return table;
}

std::string normalise(const TCHAR* path)
{
  std::string out;
  if (!path) return out;
  if (path[0] != '/') out.push_back('/');
  for (const TCHAR* p = path; *p; p++) {
    char c = (char)tolower((unsigned char)*p);
    if (c == '\\') c = '/';
    if (c == '/' && !out.empty() && out.back() == '/') continue;
    out.push_back(c);
  }
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

OpenFile* handle(FIL* fil)
{
  return fil ? reinterpret_cast<OpenFile*>(fil->obj.fs) : nullptr;
}

MemFile* fileOf(FIL* fil)
{
  auto h = handle(fil);
  if (!h) return nullptr;
  auto it = files().find(h->key);
  return it == files().end() ? nullptr : &it->second;
}

bool isDirectory(const std::string& key)
{
  // A directory exists when a file lives below it (or it is the root).
  if (key == "/") return true;
  std::string prefix = key + "/";
  auto it = files().lower_bound(prefix);
  return it != files().end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

// ---- host side ------------------------------------------------------------

void etxFsPut(const char* path, const uint8_t* data, uint32_t len)
{
  auto& f = files()[normalise(path)];
  f.data.assign(data, data + len);
}

bool etxFsGet(const char* path, const uint8_t** data, uint32_t* len)
{
  auto it = files().find(normalise(path));
  if (it == files().end()) return false;
  *data = it->second.data.data();
  *len = (uint32_t)it->second.data.size();
  return true;
}

void etxFsRemove(const char* path) { files().erase(normalise(path)); }

// ---- FatFs API --------------------------------------------------------------

FRESULT f_mount(FATFS*, const TCHAR*, BYTE) { return FR_OK; }

FRESULT f_open(FIL* fil, const TCHAR* name, BYTE mode)
{
  if (!fil) return FR_INVALID_OBJECT;
  memset(fil, 0, sizeof(FIL));
  std::string key = normalise(name);
  auto& table = files();
  auto it = table.find(key);

  if (mode & (FA_WRITE | FA_CREATE_ALWAYS | FA_CREATE_NEW | FA_OPEN_ALWAYS | FA_OPEN_APPEND)) {
    if ((mode & FA_CREATE_NEW) && it != table.end()) return FR_EXIST;
    if (it == table.end()) {
      if (!(mode & (FA_CREATE_ALWAYS | FA_CREATE_NEW | FA_OPEN_ALWAYS | FA_OPEN_APPEND)))
        return FR_NO_FILE;
      it = table.emplace(key, MemFile{}).first;
    } else if (mode & FA_CREATE_ALWAYS) {
      it->second.data.clear();
    }
  } else if (it == table.end()) {
    return isDirectory(key) ? FR_NO_FILE : FR_NO_FILE;
  }

  auto h = new OpenFile{key, (mode & FA_WRITE) != 0};
  fil->obj.fs = reinterpret_cast<FATFS*>(h);
  fil->obj.objsize = it->second.data.size();
  fil->fptr = (mode & FA_OPEN_APPEND) ? it->second.data.size() : 0;
  return FR_OK;
}

FRESULT f_close(FIL* fil)
{
  auto h = handle(fil);
  if (h) {
    delete h;
    fil->obj.fs = nullptr;
  }
  return FR_OK;
}

FRESULT f_read(FIL* fil, void* buff, UINT btr, UINT* br)
{
  if (br) *br = 0;
  auto f = fileOf(fil);
  if (!f) return FR_INVALID_OBJECT;
  size_t size = f->data.size();
  size_t pos = fil->fptr;
  UINT n = pos >= size ? 0 : (UINT)std::min<size_t>(btr, size - pos);
  if (n) memcpy(buff, f->data.data() + pos, n);
  fil->fptr += n;
  if (br) *br = n;
  return FR_OK;
}

FRESULT f_write(FIL* fil, const void* buff, UINT btw, UINT* bw)
{
  if (bw) *bw = 0;
  auto h = handle(fil);
  auto f = fileOf(fil);
  if (!f || !h->write) return FR_DENIED;
  size_t pos = fil->fptr;
  if (f->data.size() < pos + btw) f->data.resize(pos + btw);
  memcpy(f->data.data() + pos, buff, btw);
  fil->fptr += btw;
  fil->obj.objsize = f->data.size();
  if (bw) *bw = btw;
  return FR_OK;
}

FRESULT f_lseek(FIL* fil, FSIZE_t ofs)
{
  auto f = fileOf(fil);
  if (!f) return FR_INVALID_OBJECT;
  if (ofs > f->data.size()) {
    if (!handle(fil)->write) ofs = f->data.size();
    else f->data.resize(ofs);
  }
  fil->fptr = ofs;
  fil->obj.objsize = f->data.size();
  return FR_OK;
}

UINT f_size(FIL* fil)
{
  auto f = fileOf(fil);
  return f ? (UINT)f->data.size() : 0;
}

FRESULT f_truncate(FIL* fil)
{
  auto f = fileOf(fil);
  if (!f) return FR_INVALID_OBJECT;
  f->data.resize(fil->fptr);
  fil->obj.objsize = f->data.size();
  return FR_OK;
}

FRESULT f_sync(FIL*) { return FR_OK; }

FRESULT f_stat(const TCHAR* path, FILINFO* fno)
{
  std::string key = normalise(path);
  auto it = files().find(key);
  if (fno) memset(fno, 0, sizeof(FILINFO));
  if (it != files().end()) {
    if (fno) {
      fno->fsize = it->second.data.size();
      auto slash = key.rfind('/');
      strncpy(fno->fname, key.c_str() + slash + 1, sizeof(fno->fname) - 1);
    }
    return FR_OK;
  }
  if (isDirectory(key)) {
    if (fno) fno->fattrib = AM_DIR;
    return FR_OK;
  }
  return FR_NO_FILE;
}

FRESULT f_unlink(const TCHAR* path)
{
  return files().erase(normalise(path)) ? FR_OK : FR_NO_FILE;
}

FRESULT f_rename(const TCHAR* oldname, const TCHAR* newname)
{
  auto& table = files();
  auto it = table.find(normalise(oldname));
  if (it == table.end()) return FR_NO_FILE;
  std::string to = normalise(newname);
  if (table.count(to)) return FR_EXIST;
  MemFile moved = std::move(it->second);
  table.erase(it);
  table.emplace(to, std::move(moved));
  return FR_OK;
}

FRESULT f_mkdir(const TCHAR*) { return FR_OK; }
FRESULT f_chdir(const TCHAR*) { return FR_OK; }

FRESULT f_getcwd(TCHAR* buff, UINT len)
{
  if (!buff || len < 2) return FR_NOT_ENOUGH_CORE;
  strcpy(buff, "/");
  return FR_OK;
}

FRESULT f_utime(const TCHAR*, const FILINFO*) { return FR_OK; }

// Directory listing is not offered: nothing in the mixer path enumerates the
// card, and a listing of our handful of memory files would only invite the
// model list and sound scanners to go looking for more.
FRESULT f_opendir(DIR* dp, const TCHAR*)
{
  if (dp) memset(dp, 0, sizeof(DIR));
  return FR_NO_PATH;
}

FRESULT f_closedir(DIR*) { return FR_OK; }

FRESULT f_readdir(DIR*, FILINFO* fno)
{
  if (fno) fno->fname[0] = '\0';
  return FR_OK;
}

FRESULT f_getfree(const TCHAR*, DWORD* nclst, FATFS** fatfs)
{
  if (nclst) *nclst = 0;
  if (fatfs) *fatfs = nullptr;
  return FR_OK;
}

FRESULT f_mkfs(const TCHAR*, const MKFS_PARM*, void*, UINT) { return FR_OK; }

int f_putc(TCHAR c, FIL* fil)
{
  UINT bw;
  return f_write(fil, &c, 1, &bw) == FR_OK ? 1 : -1;
}

TCHAR* f_gets(TCHAR* buff, int len, FIL* fil)
{
  int n = 0;
  while (n < len - 1) {
    UINT br;
    TCHAR c;
    if (f_read(fil, &c, 1, &br) != FR_OK || br == 0) break;
    buff[n++] = c;
    if (c == '\n') break;
  }
  buff[n] = '\0';
  return n ? buff : nullptr;
}
