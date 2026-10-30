/* Chamows FAT32 file system on the USB disk. Names are 8.3 ("APPS/CALC.ELF");
 * long names written by other systems show up as their short form. */
#ifndef CHAMOWS_FAT_H
#define CHAMOWS_FAT_H
#include "kernel.h"

typedef struct { char name[13]; u32 size; u8 is_dir; } fat_entry_t;

int fat_mount(void);                       /* 0 when the USB disk has FAT32 */
extern int fat_mounted;
extern char fat_label[12];
u32 fat_free_mb(void);
int fat_list(const char *dir, fat_entry_t *out, int max);    /* "" = root; returns count or -1 */
int fat_read(const char *path, u8 **data, u32 *size);        /* kmalloc'd data; free with kfree */
int fat_write(const char *path, const void *data, u32 size); /* creates or replaces the file */
int fat_mkdir(const char *path);                             /* 0 also when it already exists */
int fat_delete(const char *path);                            /* files only */

#endif
