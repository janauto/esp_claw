#include <stdio.h>
#include <dirent.h>
#include <string.h>
static int directories, manifests, indexes;
void app_test_reset_io(void)
{
    directories = manifests = indexes = 0;
}
int app_test_directories(void)
{
    return directories;
}
int app_test_manifests(void)
{
    return manifests;
}
int app_test_indexes(void)
{
    return indexes;
}
DIR *app_probe_opendir(const char *path)
{
    directories++;
    return opendir(path);
}
FILE *app_probe_fopen(const char *path, const char *mode)
{
    if (strstr(path, "/app.json"))
        manifests++;
    if (strstr(path, "/index.json"))
        indexes++;
    return fopen(path, mode);
}
