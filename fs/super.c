/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);
void wait_for_keypress(void);

/* set_bit uses setb, as gas doesn't recognize setc */
#define set_bit(bitnr,addr) ({ \
register int __res __asm__("ax"); \
__asm__("bt %2,%3;setb %%al":"=a" (__res):"a" (0),"r" (bitnr),"m" (*(addr))); \
__res; })

// 是这个数组
struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

static void lock_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sb->s_lock = 1;
	sti();
}

static void free_super(struct super_block * sb)
{
	cli();
	sb->s_lock = 0;
	wake_up(&(sb->s_wait));
	sti();
}

static void wait_on_super(struct super_block * sb)
{
	cli();
	while (sb->s_lock)
		sleep_on(&(sb->s_wait));
	sti();
}

struct super_block * get_super(int dev)
{
	struct super_block * s;

	if (!dev)
		return NULL;
		//s指向28行的数组首元素
	s = 0+super_block;//数组的首地址，super_block是数组，不是struct
	while (s < NR_SUPER+super_block)
		if (s->s_dev == dev) {
			//和缓冲块的等待队列一样
			wait_on_super(s);
			if (s->s_dev == dev)
				return s;
				//如果在等待的过程中，超级块被释放了，那么就继续重新找
			s = 0+super_block;
		} else
			s++;
	return NULL;
}

void put_super(int dev)
{
	struct super_block * sb;
	struct m_inode * inode;
	int i;

	if (dev == ROOT_DEV) {
		printk("root diskette changed: prepare for armageddon\n\r");
		return;
	}
	if (!(sb = get_super(dev)))
		return;
	if (sb->s_imount) {
		printk("Mounted disk changed - tssk, tssk\n\r");
		return;
	}
	lock_super(sb);
	sb->s_dev = 0;
	for(i=0;i<I_MAP_SLOTS;i++)
		brelse(sb->s_imap[i]);
	for(i=0;i<Z_MAP_SLOTS;i++)
		brelse(sb->s_zmap[i]);
	free_super(sb);
	return;
}

static struct super_block * read_super(int dev)
{
	struct super_block * s;
	struct buffer_head * bh;
	int i,block;

	if (!dev)
		return NULL;
	check_disk_change(dev);
	//如果已经有了超级块，就直接返回
	//当然mount_root的时候，超级块还没有被初始化，所以会继续往下走

	//有没有现成的超级块，也就是已经被分配设备的超级块
	if (s = get_super(dev))
		return s;
	//有没有空闲的超级块
	for (s = 0+super_block ;; s++) {
		if (s >= NR_SUPER+super_block)
			return NULL;
			//如果找到空闲的，设备号为0，表示没有被使用
		if (!s->s_dev)
			break;
	}
	//占用该设备号，绑定为当前的超级块
	s->s_dev = dev;
	s->s_isup = NULL;
	s->s_imount = NULL;
	s->s_time = 0;
	s->s_rd_only = 0;
	s->s_dirt = 0;
	lock_super(s);
	//从硬盘放到缓冲区，1表示是超级块，0是引导块
	if (!(bh = bread(dev,1))) {
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	*((struct d_super_block *) s) =
	//bh->b_data是缓冲区中的内容，大小是1k，d_super_block大小是20字节，
	//强制类型转换为d_super_block指针，解引用后赋值给s，s是super_block指针，大小是32字节，前20字节是d_super_block，
	//后12字节是内存中使用的内容
		*((struct d_super_block *) bh->b_data);
	//buffer relese 没有释放缓冲区的内容，只是把缓冲区的引用计数减1，表示不再使用该缓冲区
	//只有别人还要使用该缓冲区，才会继续保留该缓冲区的内容
	brelse(bh);
	//不用看
	if (s->s_magic != SUPER_MAGIC) {
		s->s_dev = 0;
		free_super(s);
		return NULL;
	}
	//如果将来文件系统加载之后，将inode位图块位图都放在缓冲区中
	for (i=0;i<I_MAP_SLOTS;i++)
		s->s_imap[i] = NULL;//inode位图，最多挂8个缓冲块，一个是1k，共8k，一共64k个bit，64k个块，64M上限
	for (i=0;i<Z_MAP_SLOTS;i++)
		s->s_zmap[i] = NULL;//数据块位图
	block=2;
	//实际有多少，加载inode位图
	for (i=0 ; i < s->s_imap_blocks ; i++)
		if (s->s_imap[i]=bread(dev,block))
			block++;
		else
			break;
			//加载数据块位图
	for (i=0 ; i < s->s_zmap_blocks ; i++)
	//bread 
		if (s->s_zmap[i]=bread(dev,block))
			block++;
		else
			break;
			//判断是否来的i节点的块号的位置
	if (block != 2+s->s_imap_blocks+s->s_zmap_blocks) {
		for(i=0;i<I_MAP_SLOTS;i++)
			brelse(s->s_imap[i]);
		for(i=0;i<Z_MAP_SLOTS;i++)
			brelse(s->s_zmap[i]);
		s->s_dev=0;
		free_super(s);
		return NULL;
	}
	//把imap位图的第一个数据置1，表示把inode的第一个置1
	//对于申请空闲 i 节点的函数来讲，如果设备上所有的 i 节点已经全被使用，则查找 
    // 函数会返回 0 值。因此 0 号 i 节点是不能用的，所以这里将位图中的最低位设置为 1，以防止文件 
    // 系统分配 0 号 i 节点。同样的道理，也将逻辑块位图的最低位设置为 1。 
	s->s_imap[0]->b_data[0] |= 1;
	s->s_zmap[0]->b_data[0] |= 1;
	free_super(s);
	return s;
}

int sys_umount(char * dev_name)
{
	struct m_inode * inode;
	struct super_block * sb;
	int dev;

	if (!(inode=namei(dev_name)))
		return -ENOENT;
	dev = inode->i_zone[0];
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	iput(inode);
	if (dev==ROOT_DEV)
		return -EBUSY;
	if (!(sb=get_super(dev)) || !(sb->s_imount))
		return -ENOENT;
	if (!sb->s_imount->i_mount)
		printk("Mounted inode has i_mount=0\n");
	for (inode=inode_table+0 ; inode<inode_table+NR_INODE ; inode++)
		if (inode->i_dev==dev && inode->i_count)
				return -EBUSY;
	sb->s_imount->i_mount=0;
	iput(sb->s_imount);
	sb->s_imount = NULL;
	iput(sb->s_isup);
	sb->s_isup = NULL;
	put_super(dev);
	sync_dev(dev);
	return 0;
}

int sys_mount(char * dev_name, char * dir_name, int rw_flag)
{
	struct m_inode * dev_i, * dir_i;
	struct super_block * sb;
	int dev;

	if (!(dev_i=namei(dev_name)))
		return -ENOENT;
	dev = dev_i->i_zone[0];
	if (!S_ISBLK(dev_i->i_mode)) {
		iput(dev_i);
		return -EPERM;
	}
	iput(dev_i);
	if (!(dir_i=namei(dir_name)))
		return -ENOENT;
	if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -EPERM;
	}
	if (!(sb=read_super(dev))) {
		iput(dir_i);
		return -EBUSY;
	}
	if (sb->s_imount) {
		iput(dir_i);
		return -EBUSY;
	}
	if (dir_i->i_mount) {
		iput(dir_i);
		return -EPERM;
	}
	sb->s_imount=dir_i;
	dir_i->i_mount=1;
	dir_i->i_dirt=1;		/* NOTE! we don't iput(dir_i) */
	return 0;			/* we do that in umount */
}

// 安装根文件系统
void mount_root(void)
{
	int i,free;
	struct super_block * p;
	struct m_inode * mi;

	//一个块里面有32个inode，一个块是1k，inode是32字节，所以一个块内有32个32字节的inode
	if (32 != sizeof (struct d_inode))
		panic("bad i-node size");
	//将file table清零
	for(i=0;i<NR_FILE;i++)
		file_table[i].f_count=0;
//关系不大
	if (MAJOR(ROOT_DEV) == 2) {
		printk("Insert root floppy and press ENTER");
		wait_for_keypress();
	}
//给超级块附初值
	for(p = &super_block[0] ; p < &super_block[NR_SUPER] ; p++) {
		p->s_dev = 0;//设备号
		p->s_lock = 0;
		p->s_wait = NULL;
	}
	//对于一个文件系统，前面的引导块和超级块是固定的，后面的inode和位图受格式化的影响，具体大小不确定
	if (!(p=read_super(ROOT_DEV)))
		panic("Unable to mount root");
	if (!(mi=iget(ROOT_DEV,ROOT_INO)))
		panic("Unable to read root i-node");
	mi->i_count += 3 ;	/* NOTE! it is logically used 4 times, not 1 */
	// p是根文件系统 isup和imount都指向同一个inode，安装根文件系统的安装方法
	p->s_isup = p->s_imount = mi;//这个mi是根文件系统的inode
	//安装普通的文件系统是sys_mount函数，安装根文件系统是mount_root函数
	current->pwd = mi;
	current->root = mi;
	free=0;
	i=p->s_nzones;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_zmap[i>>13]->b_data))
			free++;
	printk("%d/%d free blocks\n\r",free,p->s_nzones);
	free=0;
	i=p->s_ninodes+1;
	while (-- i >= 0)
		if (!set_bit(i&8191,p->s_imap[i>>13]->b_data))
			free++;
	printk("%d/%d free inodes\n\r",free,p->s_ninodes);
}
