#ifndef SFS_API_H
#define SFS_API_H
#include "disk_emu.h"
#include <stdint.h>
#define MAXIMUM_FILE_NAME 20
#define MAXFILENAME MAXIMUM_FILE_NAME

#define FREE(data, bit) \
    data = data | (1 << bit)

#define USE(data, bit) \
    data = data & ~(1 << bit)

// You can add more into this file.

void mksfs(int fresht);

int sfs_getnextfilename(char *fname);

int sfs_getfilesize(const char* path);

int sfs_fopen(char *name);

int sfs_fclose(int fileID);

int sfs_fwrite(int fileID, const char *buf, int length);

int sfs_fread(int fileID, char *buf, int length);

int sfs_fseek(int fileID, int loc);

int sfs_remove(char *file);

//HELPER FUNCTIONS
void set_index_bit(int index);
int get_index_bit();
void remove_index_bit(int index);
