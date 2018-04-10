#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int mkdir_p(const char *path)
{
    int ret = 0;
    char *temp = strdup(path);
    char *pos = temp;
    char tmp_ch = '\0';

    if (!path || !temp) {
        return -1;
    }

    if (!strncmp(temp, "/", 1) || !strncmp(temp, "\\", 1)) {
        pos++;
    } else if (!strncmp(temp, "./", 2) || !strncmp(temp, ".\\", 2)) {
        pos += 2;
    }

    for ( ; *pos != '\0'; ++pos) {
        if (*pos == '/' || *pos == '\\') {
            tmp_ch = *pos;
            *pos = '\0';
            ret = mkdir(temp, 0755);
            *pos = tmp_ch;
        }
    }

    if ((*(pos - 1) != '/') || (*(pos - 1) != '\\')) {
        ret = mkdir(temp, 0755);
    }

    free(temp);
    return ret;
}
