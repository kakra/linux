// SPDX-License-Identifier: GPL-2.0-only
/*
 * winesync.c - Kernel driver for Wine synchronization primitives
 *
 * Copyright (C) 2021 Zebediah Figura
 */

#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <uapi/linux/winesync.h>

#define WINESYNC_NAME	"winesync"

enum winesync_type {
	WINESYNC_TYPE_SEM,
};

struct winesync_obj {
	struct kref refcount;
	spinlock_t lock;

	enum winesync_type type;

	/* The following fields are protected by the object lock. */
	union {
		struct {
			__u32 count;
			__u32 max;
		} sem;
	} u;
};

struct winesync_device {
	struct mutex table_lock;
	struct idr objects;
};

static struct winesync_obj *get_obj(struct winesync_device *dev, int id)
{
	struct winesync_obj *obj;

	rcu_read_lock();
	obj = idr_find(&dev->objects, id);
	if (obj && !kref_get_unless_zero(&obj->refcount))
		obj = NULL;
	rcu_read_unlock();

	return obj;
}

static void destroy_obj(struct kref *ref)
{
	struct winesync_obj *obj;

	obj = container_of(ref, struct winesync_obj, refcount);
	kfree(obj);
}

static void put_obj(struct winesync_obj *obj)
{
	kref_put(&obj->refcount, destroy_obj);
}

static int winesync_char_open(struct inode *inode, struct file *file)
{
	struct winesync_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	idr_init(&dev->objects);
	mutex_init(&dev->table_lock);

	file->private_data = dev;
	return nonseekable_open(inode, file);
}

static int winesync_char_release(struct inode *inode, struct file *file)
{
	struct winesync_device *dev = file->private_data;
	struct winesync_obj *obj;
	int id;

	mutex_lock(&dev->table_lock);
	idr_for_each_entry(&dev->objects, obj, id) {
		idr_remove(&dev->objects, id);
		synchronize_rcu();
		put_obj(obj);
	}
	mutex_unlock(&dev->table_lock);

	kfree(dev);

	return 0;
}

static void winesync_init_obj(struct winesync_obj *obj)
{
	kref_init(&obj->refcount);
	spin_lock_init(&obj->lock);
}

static int winesync_create_sem(struct winesync_device *dev, void __user *argp)
{
	struct winesync_sem_args __user *user_args = argp;
	struct winesync_sem_args args;
	struct winesync_obj *sem;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	if (args.count > args.max)
		return -EINVAL;

	sem = kzalloc(sizeof(*sem), GFP_KERNEL);
	if (!sem)
		return -ENOMEM;

	winesync_init_obj(sem);
	sem->type = WINESYNC_TYPE_SEM;
	sem->u.sem.count = args.count;
	sem->u.sem.max = args.max;

	mutex_lock(&dev->table_lock);
	ret = idr_alloc(&dev->objects, sem, 0, 0, GFP_KERNEL);
	mutex_unlock(&dev->table_lock);

	if (ret < 0) {
		kfree(sem);
		return ret;
	}

	return put_user(ret, &user_args->sem);
}

static int winesync_delete(struct winesync_device *dev, void __user *argp)
{
	struct winesync_obj *obj;
	__u32 id;

	if (get_user(id, (__u32 __user *)argp))
		return -EFAULT;

	mutex_lock(&dev->table_lock);
	obj = idr_remove(&dev->objects, id);
	mutex_unlock(&dev->table_lock);

	if (!obj)
		return -EINVAL;

	put_obj(obj);
	return 0;
}

/*
 * Actually change the semaphore state, returning -EOVERFLOW if it is made
 * invalid.
 */
static int put_sem_state(struct winesync_obj *sem, __u32 count)
{
	lockdep_assert_held(&sem->lock);

	if (sem->u.sem.count + count < sem->u.sem.count ||
	    sem->u.sem.count + count > sem->u.sem.max)
		return -EOVERFLOW;

	sem->u.sem.count += count;
	return 0;
}

static int winesync_put_sem(struct winesync_device *dev, void __user *argp)
{
	struct winesync_sem_args __user *user_args = argp;
	struct winesync_sem_args args;
	struct winesync_obj *sem;
	__u32 prev_count;
	int ret;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	sem = get_obj(dev, args.sem);
	if (!sem)
		return -EINVAL;
	if (sem->type != WINESYNC_TYPE_SEM) {
		put_obj(sem);
		return -EINVAL;
	}

	spin_lock(&sem->lock);
	ret = put_sem_state(sem, args.count);
	spin_unlock(&sem->lock);

	put_obj(sem);

	if (!ret && put_user(prev_count, &user_args->count))
		ret = -EFAULT;

	return ret;
}

static long winesync_char_ioctl(struct file *file, unsigned int cmd,
				unsigned long parm)
{
	struct winesync_device *dev = file->private_data;
	void __user *argp = (void __user *)parm;

	switch (cmd) {
	case WINESYNC_IOC_CREATE_SEM:
		return winesync_create_sem(dev, argp);
	case WINESYNC_IOC_DELETE:
		return winesync_delete(dev, argp);
	case WINESYNC_IOC_PUT_SEM:
		return winesync_put_sem(dev, argp);
	default:
		return -ENOSYS;
	}
}

static const struct file_operations winesync_fops = {
	.owner		= THIS_MODULE,
	.open		= winesync_char_open,
	.release	= winesync_char_release,
	.unlocked_ioctl	= winesync_char_ioctl,
	.compat_ioctl	= winesync_char_ioctl,
	.llseek		= no_llseek,
};

static struct miscdevice winesync_misc = {
	.minor		= WINESYNC_MINOR,
	.name		= WINESYNC_NAME,
	.fops		= &winesync_fops,
};

static int __init winesync_init(void)
{
	return misc_register(&winesync_misc);
}

static void __exit winesync_exit(void)
{
	misc_deregister(&winesync_misc);
}

module_init(winesync_init);
module_exit(winesync_exit);

MODULE_AUTHOR("Zebediah Figura");
MODULE_DESCRIPTION("Kernel driver for Wine synchronization primitives");
MODULE_LICENSE("GPL");
MODULE_ALIAS("devname:" WINESYNC_NAME);
MODULE_ALIAS_MISCDEV(WINESYNC_MINOR);
