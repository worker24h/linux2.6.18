/*
 * file.c - operations for regular (text) files.
 */

#include <linux/module.h>
#include <linux/fsnotify.h>
#include <linux/kobject.h>
#include <linux/namei.h>
#include <linux/poll.h>
#include <asm/uaccess.h>
#include <asm/semaphore.h>

#include "sysfs.h"

#define to_subsys(k) container_of(k,struct subsystem,kset.kobj)
#define to_sattr(a) container_of(a,struct subsys_attribute,attr)

/*
 * Subsystem file operations.
 * These operations allow subsystems to have files that can be 
 * read/written. 
 */
static ssize_t 
subsys_attr_show(struct kobject * kobj, struct attribute * attr, char * page)
{
	struct subsystem * s = to_subsys(kobj);
	struct subsys_attribute * sattr = to_sattr(attr);
	ssize_t ret = -EIO;

	if (sattr->show)
		ret = sattr->show(s,page);
	return ret;
}

static ssize_t 
subsys_attr_store(struct kobject * kobj, struct attribute * attr, 
		  const char * page, size_t count)
{
	struct subsystem * s = to_subsys(kobj);
	struct subsys_attribute * sattr = to_sattr(attr);
	ssize_t ret = -EIO;

	if (sattr->store)
		ret = sattr->store(s,page,count);
	return ret;
}

static struct sysfs_ops subsys_sysfs_ops = {
	.show	= subsys_attr_show,
	.store	= subsys_attr_store,
};


struct sysfs_buffer {
	size_t			count;//实际内容
	loff_t			pos;
	char			* page;//4KB大小
	struct sysfs_ops	* ops;
	struct semaphore	sem;
	//1 --> 0: 表示buffer中没有数据，应该去读取
	//0 --> 1: 表示buffer中有数据，应该用于输出
	int			needs_read_fill;
	int			event;
};


/**
 *	fill_read_buffer - allocate and fill buffer from object.
 *	@dentry:	dentry pointer.
 *	@buffer:	data buffer for file.
 *
 *	Allocate @buffer->page, if it hasn't been already, then call the
 *	kobject's show() method to fill the buffer with this attribute's 
 *	data. 
 *	This is called only once, on the file's first read. 
 */
static int fill_read_buffer(struct dentry * dentry, struct sysfs_buffer * buffer)
{
	struct sysfs_dirent * sd = dentry->d_fsdata;
	struct attribute * attr = to_attr(dentry);
	struct kobject * kobj = to_kobj(dentry->d_parent);
	struct sysfs_ops * ops = buffer->ops;
	int ret = 0;
	ssize_t count;

	if (!buffer->page)
		buffer->page = (char *) get_zeroed_page(GFP_KERNEL);//申请一页大小的内存
	if (!buffer->page)
		return -ENOMEM;

	buffer->event = atomic_read(&sd->s_event);
	count = ops->show(kobj,attr,buffer->page);
	buffer->needs_read_fill = 0;//表示已经存在数据
	BUG_ON(count > (ssize_t)PAGE_SIZE);
	if (count >= 0)
		buffer->count = count;
	else
		ret = count;
	return ret;
}


/**
 *	flush_read_buffer - push buffer to userspace.
 *	@buffer:	data buffer for file.
 *	@buf:		user-passed buffer.
 *	@count:		number of bytes requested.
 *	@ppos:		file position.
 *
 *	Copy the buffer we filled in fill_read_buffer() to userspace.
 *	This is done at the reader's leisure, copying and advancing 
 *	the amount they specify each time.
 *	This may be called continuously until the buffer is empty.
 */
static int flush_read_buffer(struct sysfs_buffer * buffer, char __user * buf,
			     size_t count, loff_t * ppos)
{
	int error;

	if (*ppos > buffer->count)
		return 0;

	if (count > (buffer->count - *ppos))
		count = buffer->count - *ppos;
	//拷贝到用户空间中
	error = copy_to_user(buf,buffer->page + *ppos,count);
	if (!error)
		*ppos += count;//修改偏移量
	return error ? -EFAULT : count;
}

/**
 *	sysfs_read_file - read an attribute. 
 *	@file:	file pointer.
 *	@buf:	buffer to fill.
 *	@count:	number of bytes to read.
 *	@ppos:	starting offset in file.
 *
 *	Userspace wants to read an attribute file. The attribute descriptor
 *	is in the file's ->d_fsdata. The target object is in the directory's
 *	->d_fsdata.
 *
 *	We call fill_read_buffer() to allocate and fill the buffer from the
 *	object's show() method exactly once (if the read is happening from
 *	the beginning of the file). That should fill the entire buffer with
 *	all the data the object has to offer for that attribute.
 *	We then call flush_read_buffer() to copy the buffer to userspace
 *	in the increments specified.
 */

static ssize_t
sysfs_read_file(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct sysfs_buffer * buffer = file->private_data;
	ssize_t retval = 0;

	down(&buffer->sem);
	if (buffer->needs_read_fill) {//在check_perm的时候 needs_read_fill被赋值为1
		if ((retval = fill_read_buffer(file->f_dentry,buffer)))
			goto out;
	}
	pr_debug("%s: count = %d, ppos = %lld, buf = %s\n",
		 __FUNCTION__,count,*ppos,buffer->page);
	retval = flush_read_buffer(buffer,buf,count,ppos);
out:
	up(&buffer->sem);
	return retval;
}


/**
 *	fill_write_buffer - copy buffer from userspace.
 *	@buffer:	data buffer for file.
 *	@buf:		data from user.
 *	@count:		number of bytes in @userbuf.
 *
 *	Allocate @buffer->page if it hasn't been already, then
 *	copy the user-supplied buffer into it.
 */

static int 
fill_write_buffer(struct sysfs_buffer * buffer, const char __user * buf, size_t count)
{
	int error;

	if (!buffer->page)
		buffer->page = (char *)get_zeroed_page(GFP_KERNEL);
	if (!buffer->page)
		return -ENOMEM;

	if (count >= PAGE_SIZE)
		count = PAGE_SIZE - 1;
	error = copy_from_user(buffer->page,buf,count);//从用户空间写入到buffer中
	buffer->needs_read_fill = 1;
	return error ? -EFAULT : count;
}


/**
 *	flush_write_buffer - push buffer to kobject.
 *	@dentry:	dentry to the attribute
 *	@buffer:	data buffer for file.
 *	@count:		number of bytes
 *
 *	Get the correct pointers for the kobject and the attribute we're
 *	dealing with, then call the store() method for the attribute, 
 *	passing the buffer that we acquired in fill_write_buffer().
 */

static int 
flush_write_buffer(struct dentry * dentry, struct sysfs_buffer * buffer, size_t count)
{
	struct attribute * attr = to_attr(dentry);
	struct kobject * kobj = to_kobj(dentry->d_parent);
	struct sysfs_ops * ops = buffer->ops;

	return ops->store(kobj,attr,buffer->page,count);//真正写入
}


/**
 *	sysfs_write_file - write an attribute.
 *	@file:	file pointer
 *	@buf:	data to write
 *	@count:	number of bytes
 *	@ppos:	starting offset
 *
 *	Similar to sysfs_read_file(), though working in the opposite direction.
 *	We allocate and fill the data from the user in fill_write_buffer(),
 *	then push it to the kobject in flush_write_buffer().
 *	There is no easy way for us to know if userspace is only doing a partial
 *	write, so we don't support them. We expect the entire buffer to come
 *	on the first write. 
 *	Hint: if you're writing a value, first read the file, modify only the
 *	the value you're changing, then write entire buffer back. 
 */

static ssize_t
sysfs_write_file(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct sysfs_buffer * buffer = file->private_data;
	ssize_t len;

	down(&buffer->sem);
	len = fill_write_buffer(buffer, buf, count);
	if (len > 0)
		len = flush_write_buffer(file->f_dentry, buffer, len);
	if (len > 0)
		*ppos += len;
	up(&buffer->sem);
	return len;
}

static int check_perm(struct inode * inode, struct file * file)
{
	//从struct dentry对中d_fsdata成员获取 struct sysfs_dirent对象 再从
	//sysfs_dirent对象中获取kobject对象
	struct kobject *kobj = sysfs_get_kobject(file->f_dentry->d_parent);
	struct attribute * attr = to_attr(file->f_dentry);
	struct sysfs_buffer * buffer;
	struct sysfs_ops * ops = NULL;
	int error = 0;

	if (!kobj || !attr)
		goto Einval;

	/* Grab the module reference for this attribute if we have one */
	if (!try_module_get(attr->owner)) {
		error = -ENODEV;
		goto Done;
	}

	/* if the kobject has no ktype, then we assume that it is a subsystem
	 * itself, and use ops for it.
	 * 真正读写文件 需要sysfs子系统来处理 /sys/目录下面所有目录都属于sysfs子系统
	 * 通过decl_subsys decl_subsys_name 两个宏来声明sysfs子系统
	 * 子系统注册一定会通过：subsystem_register函数来注册，可以通过搜索该关键字进行确定
	 * 例如：drivers/base/core.c 注册devices
	 */
	if (kobj->kset && kobj->kset->ktype)
		ops = kobj->kset->ktype->sysfs_ops;
	else if (kobj->ktype)
		ops = kobj->ktype->sysfs_ops;
	else
		ops = &subsys_sysfs_ops;//设置sysfs子系统读写函数为默认

	/* No sysfs operations, either from having no subsystem,
	 * or the subsystem have no operations.
	 */
	if (!ops)
		goto Eaccess;

	/* File needs write support.
	 * The inode's perms must say it's ok, 
	 * and we must have a store method.
	 * 检查写权限
	 */
	if (file->f_mode & FMODE_WRITE) {

		if (!(inode->i_mode & S_IWUGO) || !ops->store)
			goto Eaccess;

	}

	/* File needs read support.
	 * The inode's perms must say it's ok, and we there
	 * must be a show method for it.
	 * 检查读权限
	 */
	if (file->f_mode & FMODE_READ) {
		if (!(inode->i_mode & S_IRUGO) || !ops->show)
			goto Eaccess;
	}

	/* No error? Great, allocate a buffer for the file, and store it
	 * it in file->private_data for easy access.
	 */
	buffer = kzalloc(sizeof(struct sysfs_buffer), GFP_KERNEL);
	if (buffer) {
		init_MUTEX(&buffer->sem);
		buffer->needs_read_fill = 1;
		buffer->ops = ops;
		file->private_data = buffer;
	} else
		error = -ENOMEM;
	goto Done;

 Einval:
	error = -EINVAL;
	goto Done;
 Eaccess:
	error = -EACCES;
	module_put(attr->owner);
 Done:
	if (error && kobj)
		kobject_put(kobj);
	return error;
}

/**
 * 注意：
 * sysfs文件系统 在调用sysfs_create_file的时只创建struct dirent对象，并没有创建
 * struct dentry和struct inode对象，而是推迟到open file时才创建这两个对象
 * 但是我们在阅读sys_open_file里面并没有发现创建struct dentry、struct inode对象
 * 其实在调用sys_open_file之前就已经把这两个对象创建好了
 * 系统调用sys_open-->do_filp_open
 *                    | -->open_namei-->do_path_lookup-->link_path_walk-->do_lookup-->real_lookup（这里创建struct dentry）-->
 *                    |              sysfs_lookup-->sysfs_attach_attr-->sysfs_create(这里创建inode)
 *                    | -->nameidata_to_filp-->__dentry_open--> open --> sysfs_open
 * sysfs有普通文件和二进制文件，这里是普通文件处理方法，二进制文件可参考sysfs/bin.c
 */
static int sysfs_open_file(struct inode * inode, struct file * filp)
{
	return check_perm(inode,filp);
}

static int sysfs_release(struct inode * inode, struct file * filp)
{
	struct kobject * kobj = to_kobj(filp->f_dentry->d_parent);
	struct attribute * attr = to_attr(filp->f_dentry);
	struct module * owner = attr->owner;
	struct sysfs_buffer * buffer = filp->private_data;

	if (kobj) 
		kobject_put(kobj);
	/* After this point, attr should not be accessed. */
	module_put(owner);

	if (buffer) {
		if (buffer->page)
			free_page((unsigned long)buffer->page);
		kfree(buffer);
	}
	return 0;
}

/* Sysfs attribute files are pollable.  The idea is that you read
 * the content and then you use 'poll' or 'select' to wait for
 * the content to change.  When the content changes (assuming the
 * manager for the kobject supports notification), poll will
 * return POLLERR|POLLPRI, and select will return the fd whether
 * it is waiting for read, write, or exceptions.
 * Once poll/select indicates that the value has changed, you
 * need to close and re-open the file, as simply seeking and reading
 * again will not get new data, or reset the state of 'poll'.
 * Reminder: this only works for attributes which actively support
 * it, and it is not possible to test an attribute from userspace
 * to see if it supports poll (Nether 'poll' or 'select' return
 * an appropriate error code).  When in doubt, set a suitable timeout value.
 */
static unsigned int sysfs_poll(struct file *filp, poll_table *wait)
{
	struct sysfs_buffer * buffer = filp->private_data;
	struct kobject * kobj = to_kobj(filp->f_dentry->d_parent);
	struct sysfs_dirent * sd = filp->f_dentry->d_fsdata;
	int res = 0;

	poll_wait(filp, &kobj->poll, wait);

	if (buffer->event != atomic_read(&sd->s_event)) {
		res = POLLERR|POLLPRI;
		buffer->needs_read_fill = 1;
	}

	return res;
}


static struct dentry *step_down(struct dentry *dir, const char * name)
{
	struct dentry * de;

	if (dir == NULL || dir->d_inode == NULL)
		return NULL;

	mutex_lock(&dir->d_inode->i_mutex);
	de = lookup_one_len(name, dir, strlen(name));
	mutex_unlock(&dir->d_inode->i_mutex);
	dput(dir);
	if (IS_ERR(de))
		return NULL;
	if (de->d_inode == NULL) {
		dput(de);
		return NULL;
	}
	return de;
}

void sysfs_notify(struct kobject * k, char *dir, char *attr)
{
	struct dentry *de = k->dentry;
	if (de)
		dget(de);
	if (de && dir)
		de = step_down(de, dir);
	if (de && attr)
		de = step_down(de, attr);
	if (de) {
		struct sysfs_dirent * sd = de->d_fsdata;
		if (sd)
			atomic_inc(&sd->s_event);
		wake_up_interruptible(&k->poll);
		dput(de);
	}
}
EXPORT_SYMBOL_GPL(sysfs_notify);

/**
 * syfs文件系统 文件操作函数
 */
const struct file_operations sysfs_file_operations = {
	.read		= sysfs_read_file,
	.write		= sysfs_write_file,
	.llseek		= generic_file_llseek,
	.open		= sysfs_open_file,
	.release	= sysfs_release,
	.poll		= sysfs_poll,
};

/**
 * 在dir目录下面创建一个文件
 * @dir 目录
 * @attr 文件属性
 * @type 类型 取值为SYSFS_KOBJ_ATTR、SYSFS_KOBJ_BIN_ATTR等
 */
int sysfs_add_file(struct dentry * dir, const struct attribute * attr, int type)
{
	struct sysfs_dirent * parent_sd = dir->d_fsdata;
	umode_t mode = (attr->mode & S_IALLUGO) | S_IFREG;//常规文件
	int error = -EEXIST;
	/**
	 * 注意：sysfs文件系统在创建文件的时候只创建了struct dirent对象
	 *      但并没有创建dentry和inode 那么在什么地方创建呢？
	 * 答案：在打开文件的时候创建dentry和inode
	 */
	mutex_lock(&dir->d_inode->i_mutex);
	if (!sysfs_dirent_exist(parent_sd, attr->name))
		error = sysfs_make_dirent(parent_sd, NULL, (void *)attr,
					  mode, type);
	mutex_unlock(&dir->d_inode->i_mutex);

	return error;
}


/**
 *	sysfs_create_file - create an attribute file for an object.
 *	@kobj:	object we're creating for. 
 *	@attr:	atrribute descriptor.
 *	为sysfs文件系统中创建一个属性文件
 */
int sysfs_create_file(struct kobject * kobj, const struct attribute * attr)
{
	BUG_ON(!kobj || !kobj->dentry || !attr);

	return sysfs_add_file(kobj->dentry, attr, SYSFS_KOBJ_ATTR);

}


/**
 * sysfs_update_file - update the modified timestamp on an object attribute.
 * @kobj: object we're acting for.
 * @attr: attribute descriptor.
 */
int sysfs_update_file(struct kobject * kobj, const struct attribute * attr)
{
	struct dentry * dir = kobj->dentry;
	struct dentry * victim;
	int res = -ENOENT;

	mutex_lock(&dir->d_inode->i_mutex);
	victim = lookup_one_len(attr->name, dir, strlen(attr->name));
	if (!IS_ERR(victim)) {
		/* make sure dentry is really there */
		if (victim->d_inode && 
		    (victim->d_parent->d_inode == dir->d_inode)) {
			victim->d_inode->i_mtime = CURRENT_TIME;
			fsnotify_modify(victim);

			/**
			 * Drop reference from initial sysfs_get_dentry().
			 */
			dput(victim);
			res = 0;
		} else
			d_drop(victim);
		
		/**
		 * Drop the reference acquired from sysfs_get_dentry() above.
		 */
		dput(victim);
	}
	mutex_unlock(&dir->d_inode->i_mutex);

	return res;
}


/**
 * sysfs_chmod_file - update the modified mode value on an object attribute.
 * @kobj: object we're acting for.
 * @attr: attribute descriptor.
 * @mode: file permissions.
 *
 */
int sysfs_chmod_file(struct kobject *kobj, struct attribute *attr, mode_t mode)
{
	struct dentry *dir = kobj->dentry;
	struct dentry *victim;
	struct inode * inode;
	struct iattr newattrs;
	int res = -ENOENT;

	mutex_lock(&dir->d_inode->i_mutex);
	victim = lookup_one_len(attr->name, dir, strlen(attr->name));
	if (!IS_ERR(victim)) {
		if (victim->d_inode &&
		    (victim->d_parent->d_inode == dir->d_inode)) {
			inode = victim->d_inode;
			mutex_lock(&inode->i_mutex);
			newattrs.ia_mode = (mode & S_IALLUGO) |
						(inode->i_mode & ~S_IALLUGO);
			newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
			res = notify_change(victim, &newattrs);
			mutex_unlock(&inode->i_mutex);
		}
		dput(victim);
	}
	mutex_unlock(&dir->d_inode->i_mutex);

	return res;
}
EXPORT_SYMBOL_GPL(sysfs_chmod_file);


/**
 *	sysfs_remove_file - remove an object attribute.
 *	@kobj:	object we're acting for.
 *	@attr:	attribute descriptor.
 *
 *	Hash the attribute name and kill the victim.
 */

void sysfs_remove_file(struct kobject * kobj, const struct attribute * attr)
{
	sysfs_hash_and_remove(kobj->dentry,attr->name);
}


EXPORT_SYMBOL_GPL(sysfs_create_file);
EXPORT_SYMBOL_GPL(sysfs_remove_file);
EXPORT_SYMBOL_GPL(sysfs_update_file);
