#ifndef _DIRENT_H_STUB
#define _DIRENT_H_STUB
struct dirent { char d_name[256]; };
typedef struct { int dummy; } DIR;
static inline DIR* opendir(const char* name) { (void)name; return 0; }
static inline struct dirent* readdir(DIR* dir) { (void)dir; return 0; }
static inline int closedir(DIR* dir) { (void)dir; return -1; }
#endif
