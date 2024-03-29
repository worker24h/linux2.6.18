/*
 * dir.c - Operations for sysfs directories.
 */

#undef DEBUG

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/namei.h>
#include "sysfs.h"

DECLARE_RWSEM(sysfs_rename_sem);

static void sysfs_d_iput(struct dentry * dentry, struct inode * inode)
{
	struct sysfs_dirent * sd = dentry->d_fsdata;

	if (sd) {
		BUG_ON(sd->s_dentry != dentry);
		sd->s_dentry = NULL;
		sysfs_put(sd);
	}
	iput(inode);
}

static struct dentry_operations sysfs_dentry_ops = {
	.d_iput		= sysfs_d_iput,
};

/*
 * Allocates a new sysfs_dirent and links it to the parent sysfs_dirent
 */
static struct sysfs_dirent * sysfs_new_dirent(struct sysfs_dirent * parent_sd,
						void * element)
{
	struct sysfs_dirent * sd;

	sd = kmem_cache_alloc(sysfs_dir_cachep, GFP_KERNEL);
	if (!sd)
		return NULL;

	memset(sd, 0, sizeof(*sd));
	atomic_set(&sd->s_count, 1);
	atomic_set(&sd->s_event, 0);
	INIT_LIST_HEAD(&sd->s_children);
	list_add(&sd->s_sibling, &parent_sd->s_children);//插入到父节点中
	sd->s_element = element;

	return sd;
}

/*
 *
 * Return -EEXIST if there is already a sysfs element with the same name for
 * the same parent.
 *
 * called with parent inode's i_mutex held
 */
int sysfs_dirent_exist(struct sysfs_dirent *parent_sd,
			  const unsigned char *new)
{
	struct sysfs_dirent * sd;

	list_for_each_entry(sd, &parent_sd->s_children, s_sibling) {
		if (sd->s_element) {
			const unsigned char *existing = sysfs_get_name(sd);
			if (strcmp(existing, new))
				continue;
			else
				return -EEXIST;
		}
	}

	return 0;
}

/**
 * 创建sysfs_dirent对象
 * @parent_sd 父节点
 * @dentry 要创建sysfs_dirent对象需要绑定一个dentry对象
 * @element 私有数据 此处实际为 kobject
 * @mode 文件属性：可读、可写、可执行等
 * @type 文件类型： SYSFS_DIR, SYSFS_KOBJ_ATTR
 */
int sysfs_make_dirent(struct sysfs_dirent * parent_sd, struct dentry * dentry,
			void * element, umode_t mode, int type)
{
	struct sysfs_dirent * sd;

	sd = sysfs_new_dirent(parent_sd, element);
	if (!sd)
		return -ENOMEM;

	sd->s_mode = mode;
	sd->s_type = type;
	sd->s_dentry = dentry;
	if (dentry) {
		dentry->d_fsdata = sysfs_get(sd);
		dentry->d_op = &sysfs_dentry_ops;
	}

	return 0;
}

static int init_dir(struct inode * inode)
{
	inode->i_op = &sysfs_dir_inode_operations;
	inode->i_fop = &sysfs_dir_operations;

	/* directory inodes start off with i_nlink == 2 (for "." entry) */
	inode->i_nlink++;
	return 0;
}

static int init_file(struct inode * inode)
{
	inode->i_size = PAGE_SIZE;
	inode->i_fop = &sysfs_file_operations;
	return 0;
}

static int init_symlink(struct inode * inode)
{
	inode->i_op = &sysfs_symlink_inode_operations;
	return 0;
}

/**
 * 为sysfs文件系统创建dentry对象
 * @k kobject对象
 * @p 父dentry对象
 * @n 目录名字
 * @d 输出参数 新dentry对象
 */
static int create_dir(struct kobject * k, struct dentry * p,
		      const char * n, struct dentry ** d)
{
	int error;
	umode_t mode = S_IFDIR| S_IRWXU | S_IRUGO | S_IXUGO;

	mutex_lock(&p->d_inode->i_mutex);
	*d = lookup_one_len(n, p, strlen(n));//按照名字查找 存在则返回 不存在则创建
	if (!IS_ERR(*d)) {
 		if (sysfs_dirent_exist(p->d_fsdata, n))//判断sysfs_dirent对象是否存在
  			error = -EEXIST;
  		else
			error = sysfs_make_dirent(p->d_fsdata, *d, k, mode,
								SYSFS_DIR);
		if (!error) {
			error = sysfs_create(*d, mode, init_dir);//为dentry创建inode对象
			if (!error) {
				p->d_inode->i_nlink++;
				(*d)->d_op = &sysfs_dentry_ops;
				d_rehash(*d);//存储到dentry cache中
			}
		}
		if (error && (error != -EEXIST)) {//回收资源
			struct sysfs_dirent *sd = (*d)->d_fsdata;
			if (sd) {
 				list_del_init(&sd->s_sibling);
				sysfs_put(sd);
			}
			d_drop(*d);
		}
		dput(*d);
	} else
		error = PTR_ERR(*d);
	mutex_unlock(&p->d_inode->i_mutex);
	return error;
}


int sysfs_create_subdir(struct kobject * k, const char * n, struct dentry ** d)
{
	return create_dir(k,k->dentry,n,d);
}

/**
 *	sysfs_create_dir - create a directory for an object.
 *	@parent:	parent parent object.
 *	@kobj:		object we're creating directory for. 
 */

int sysfs_create_dir(struct kobject * kobj)
{
	struct dentry * dentry = NULL;
	struct dentry * parent;
	int error = 0;

	BUG_ON(!kobj);

	if (kobj->parent)
		parent = kobj->parent->dentry;//获取父kobject所在的dentry 做为parent
	else if (sysfs_mount && sysfs_mount->mnt_sb)
		parent = sysfs_mount->mnt_sb->s_root;//将sysfs根dentry 做为parent
	else
		return -EFAULT;

	error = create_dir(kobj,parent,kobject_name(kobj),&dentry);
	if (!error)
		kobj->dentry = dentry;
	return error;
}

/* attaches attribute's sysfs_dirent to the dentry corresponding to the
 * attribute file
 */
static int sysfs_attach_attr(struct sysfs_dirent * sd, struct dentry * dentry)
{
	struct attribute * attr = NULL;
	struct bin_attribute * bin_attr = NULL;
	int (* init) (struct inode *) = NULL;//初始化inode函数指针
	int error = 0;

        if (sd->s_type & SYSFS_KOBJ_BIN_ATTR) {
                bin_attr = sd->s_element;
                attr = &bin_attr->attr;
        } else {
                attr = sd->s_element;
                init = init_file;
        }

	dentry->d_fsdata = sysfs_get(sd);
	sd->s_dentry = dentry;
	error = sysfs_create(dentry, (attr->mode & S_IALLUGO) | S_IFREG, init);
	if (error) {
		sysfs_put(sd);
		return error;
	}

        if (bin_attr) {
		dentry->d_inode->i_size = bin_attr->size;
		dentry->d_inode->i_fop = &bin_fops;
	}
	dentry->d_op = &sysfs_dentry_ops;
	d_rehash(dentry);

	return 0;
}

static int sysfs_attach_link(struct sysfs_dirent * sd, struct dentry * dentry)
{
	int err = 0;

	dentry->d_fsdata = sysfs_get(sd);
	sd->s_dentry = dentry;
	err = sysfs_create(dentry, S_IFLNK|S_IRWXUGO, init_symlink);
	if (!err) {
		dentry->d_op = &sysfs_dentry_ops;
		d_rehash(dentry);
	} else
		sysfs_put(sd);

	return err;
}

/**
 * 进入这个函数说明 要处理的是文件而不是目录
 */
static struct dentry * sysfs_lookup(struct inode *dir, struct dentry *dentry,
				struct nameidata *nd)
{
	struct sysfs_dirent * parent_sd = dentry->d_parent->d_fsdata;
	struct sysfs_dirent * sd;
	int err = 0;

	list_for_each_entry(sd, &parent_sd->s_children, s_sibling) {
		if (sd->s_type & SYSFS_NOT_PINNED) {//只处理文件
			const unsigned char * name = sysfs_get_name(sd);

			if (strcmp(name, dentry->d_name.name))//strcmp相等返回0 非0表示不等
				continue;

			if (sd->s_type & SYSFS_KOBJ_LINK)
				err = sysfs_attach_link(sd, dentry);//软连接文件
			else
				err = sysfs_attach_attr(sd, dentry);//普通文件
			break;
		}
	}

	return ERR_PTR(err);
}

struct inode_operations sysfs_dir_inode_operations = {
	.lookup		= sysfs_lookup,
	.setattr	= sysfs_setattr,
};

static void remove_dir(struct dentry * d)
{
	struct dentry * parent = dget(d->d_parent);
	struct sysfs_dirent * sd;

	mutex_lock(&parent->d_inode->i_mutex);
	d_delete(d);
	sd = d->d_fsdata;
 	list_del_init(&sd->s_sibling);
	sysfs_put(sd);
	if (d->d_inode)
		simple_rmdir(parent->d_inode,d);

	pr_debug(" o %s removing done (%d)\n",d->d_name.name,
		 atomic_read(&d->d_count));

	mutex_unlock(&parent->d_inode->i_mutex);
	dput(parent);
}

void sysfs_remove_subdir(struct dentry * d)
{
	remove_dir(d);
}


/**
 *	sysfs_remove_dir - remove an object's directory.
 *	@kobj:	object. 
 *
 *	The only thing special about this is that we remove any files in 
 *	the directory before we remove the directory, and we've inlined
 *	what used to be sysfs_rmdir() below, instead of calling separately.
 */

void sysfs_remove_dir(struct kobject * kobj)
{
	struct dentry * dentry = dget(kobj->dentry);
	struct sysfs_dirent * parent_sd;
	struct sysfs_dirent * sd, * tmp;

	if (!dentry)
		return;

	pr_debug("sysfs %s: removing dir\n",dentry->d_name.name);
	mutex_lock(&dentry->d_inode->i_mutex);
	parent_sd = dentry->d_fsdata;
	list_for_each_entry_safe(sd, tmp, &parent_sd->s_children, s_sibling) {
		if (!sd->s_element || !(sd->s_type & SYSFS_NOT_PINNED))
			continue;
		list_del_init(&sd->s_sibling);
		sysfs_drop_dentry(sd, dentry);
		sysfs_put(sd);
	}
	mutex_unlock(&dentry->d_inode->i_mutex);

	remove_dir(dentry);
	/**
	 * Drop reference from dget() on entrance.
	 */
	dput(dentry);
	kobj->dentry = NULL;
}

int sysfs_rename_dir(struct kobject * kobj, const char *new_name)
{
	int error = 0;
	struct dentry * new_dentry, * parent;

	if (!strcmp(kobject_name(kobj), new_name))
		return -EINVAL;

	if (!kobj->parent)
		return -EINVAL;

	down_write(&sysfs_rename_sem);
	parent = kobj->parent->dentry;

	mutex_lock(&parent->d_inode->i_mutex);

	new_dentry = lookup_one_len(new_name, parent, strlen(new_name));
	if (!IS_ERR(new_dentry)) {
  		if (!new_dentry->d_inode) {
			error = kobject_set_name(kobj, "%s", new_name);
			if (!error) {
				d_add(new_dentry, NULL);
				d_move(kobj->dentry, new_dentry);
			}
			else
				d_drop(new_dentry);
		} else
			error = -EEXIST;
		dput(new_dentry);
	}
	mutex_unlock(&parent->d_inode->i_mutex);
	up_write(&sysfs_rename_sem);

	return error;
}

static int sysfs_dir_open(struct inode *inode, struct file *file)
{
	struct dentry * dentry = file->f_dentry;
	struct sysfs_dirent * parent_sd = dentry->d_fsdata;

	mutex_lock(&dentry->d_inode->i_mutex);
	file->private_data = sysfs_new_dirent(parent_sd, NULL);
	mutex_unlock(&dentry->d_inode->i_mutex);

	return file->private_data ? 0 : -ENOMEM;

}

static int sysfs_dir_close(struct inode *inode, struct file *file)
{
	struct dentry * dentry = file->f_dentry;
	struct sysfs_dirent * cursor = file->private_data;

	mutex_lock(&dentry->d_inode->i_mutex);
	list_del_init(&cursor->s_sibling);
	mutex_unlock(&dentry->d_inode->i_mutex);

	release_sysfs_dirent(cursor);

	return 0;
}

/* Relationship between s_mode and the DT_xxx types */
static inline unsigned char dt_type(struct sysfs_dirent *sd)
{
	return (sd->s_mode >> 12) & 15;
}

static int sysfs_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	struct dentry *dentry = filp->f_dentry;
	struct sysfs_dirent * parent_sd = dentry->d_fsdata;
	struct sysfs_dirent *cursor = filp->private_data;
	struct list_head *p, *q = &cursor->s_sibling;
	ino_t ino;
	int i = filp->f_pos;

	switch (i) {
		case 0:
			ino = dentry->d_inode->i_ino;
			if (filldir(dirent, ".", 1, i, ino, DT_DIR) < 0)
				break;
			filp->f_pos++;
			i++;
			/* fallthrough */
		case 1:
			ino = parent_ino(dentry);
			if (filldir(dirent, "..", 2, i, ino, DT_DIR) < 0)
				break;
			filp->f_pos++;
			i++;
			/* fallthrough */
		default:
			if (filp->f_pos == 2)
				list_move(q, &parent_sd->s_children);

			for (p=q->next; p!= &parent_sd->s_children; p=p->next) {
				struct sysfs_dirent *next;
				const char * name;
				int len;

				next = list_entry(p, struct sysfs_dirent,
						   s_sibling);
				if (!next->s_element)
					continue;

				name = sysfs_get_name(next);
				len = strlen(name);
				if (next->s_dentry)
					ino = next->s_dentry->d_inode->i_ino;
				else
					ino = iunique(sysfs_sb, 2);

				if (filldir(dirent, name, len, filp->f_pos, ino,
						 dt_type(next)) < 0)
					return 0;

				list_move(q, p);
				p = q;
				filp->f_pos++;
			}
	}
	return 0;
}

static loff_t sysfs_dir_lseek(struct file * file, loff_t offset, int origin)
{
	struct dentry * dentry = file->f_dentry;

	mutex_lock(&dentry->d_inode->i_mutex);
	switch (origin) {
		case 1:
			offset += file->f_pos;
		case 0:
			if (offset >= 0)
				break;
		default:
			mutex_unlock(&file->f_dentry->d_inode->i_mutex);
			return -EINVAL;
	}
	if (offset != file->f_pos) {
		file->f_pos = offset;
		if (file->f_pos >= 2) {
			struct sysfs_dirent *sd = dentry->d_fsdata;
			struct sysfs_dirent *cursor = file->private_data;
			struct list_head *p;
			loff_t n = file->f_pos - 2;

			list_del(&cursor->s_sibling);
			p = sd->s_children.next;
			while (n && p != &sd->s_children) {
				struct sysfs_dirent *next;
				next = list_entry(p, struct sysfs_dirent,
						   s_sibling);
				if (next->s_element)
					n--;
				p = p->next;
			}
			list_add_tail(&cursor->s_sibling, p);
		}
	}
	mutex_unlock(&dentry->d_inode->i_mutex);
	return offset;
}

const struct file_operations sysfs_dir_operations = {
	.open		= sysfs_dir_open,
	.release	= sysfs_dir_close,
	.llseek		= sysfs_dir_lseek,
	.read		= generic_read_dir,
	.readdir	= sysfs_readdir,
};
