#pragma once
#include <cstdint>
using Result = uint32_t;
constexpr size_t FS_MAX_PATH = 0x301;
constexpr bool R_FAILED(Result rc) { return rc != 0; }
struct FsFileSystem { int id; };
enum FsDirEntryType { FsDirEntryType_Dir, FsDirEntryType_File };
int fsdevTranslatePath(const char *, FsFileSystem **, char *);
Result fsFsGetEntryType(FsFileSystem *, const char *, FsDirEntryType *);
Result fsFsDeleteFile(FsFileSystem *, const char *);
Result fsFsDeleteDirectoryRecursively(FsFileSystem *, const char *);
Result fsFsRenameFile(FsFileSystem *, const char *, const char *);
Result fsFsRenameDirectory(FsFileSystem *, const char *, const char *);
