#ifndef _SYS_DIRENT_H_
#define _SYS_DIRENT_H_

#include <sys/_types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _INO_T_DECLARED
typedef __ino_t ino_t;
#define _INO_T_DECLARED
#endif

struct dirent {
    ino_t          d_fileno;
    unsigned short d_reclen;
    unsigned char  d_type;
    unsigned char  d_namlen;
    char           d_name[256];
};

#define d_ino d_fileno

#define DT_UNKNOWN   0
#define DT_FIFO      1
#define DT_CHR       2
#define DT_DIR       4
#define DT_BLK       6
#define DT_REG       8
#define DT_LNK       10
#define DT_SOCK      12
#define DT_WHT       14

#define MAXNAMLEN    255

typedef struct _dirdesc {
    int dd_fd;
    long dd_loc;
    long dd_size;
    long dd_bufsize;
    char *dd_buf;
    int dd_flags;
} DIR;

DIR *opendir(const char *);
struct dirent *readdir(DIR *);
void rewinddir(DIR *);
int closedir(DIR *);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_DIRENT_H_ */
