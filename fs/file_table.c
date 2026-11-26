/*
 *  linux/fs/file_table.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/fs.h>

struct file file_table[NR_FILE];
//file_table可以重复，也就是多个进程filp指向同一个filetable，但是filetable指向的inode是不可以重复的
