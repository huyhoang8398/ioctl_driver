#include <linux/cdev.h>
#include <linux/fcntl.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <asm/atomic.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include "ioctl_dev.h"
#include "ioctl.h"

/* store the major number extracted by dev_t */
int ioctl_d_interface_major = 0;
int ioctl_d_interface_minor = 0;

#define DEVICE_NAME "ioctl_d"
char *ioctl_d_interface_name = DEVICE_NAME;

ioctl_d_interface_dev ioctl_d_interface;

struct file_operations ioctl_d_interface_fops = {
	.owner = THIS_MODULE,
	.read = NULL,
	.write = NULL,
	.open = ioctl_d_interface_open,
	.unlocked_ioctl = ioctl_d_interface_ioctl,
	.release = ioctl_d_interface_release};

/* Private API */
static int ioctl_d_interface_dev_init(ioctl_d_interface_dev *ioctl_d_interface);
static void ioctl_d_interface_dev_del(ioctl_d_interface_dev *ioctl_d_interface);
static int ioctl_d_interface_setup_cdev(ioctl_d_interface_dev *ioctl_d_interface);
static int ioctl_d_interface_init(void);
static void ioctl_d_interface_exit(void);

static int ioctl_d_interface_dev_init(ioctl_d_interface_dev *ioctl_d_interface)
{
	int result = 0;
	memset(ioctl_d_interface, 0, sizeof(ioctl_d_interface_dev));
	atomic_set(&ioctl_d_interface->available, 1);
	sema_init(&ioctl_d_interface->sem, 1);

	return result;
}

static void ioctl_d_interface_dev_del(ioctl_d_interface_dev *ioctl_d_interface) {}

static int ioctl_d_interface_setup_cdev(ioctl_d_interface_dev *ioctl_d_interface)
{
	int error = 0;
	dev_t devno = MKDEV(ioctl_d_interface_major, ioctl_d_interface_minor);

	cdev_init(&ioctl_d_interface->cdev, &ioctl_d_interface_fops);
	ioctl_d_interface->cdev.owner = THIS_MODULE;
	ioctl_d_interface->cdev.ops = &ioctl_d_interface_fops;
	error = cdev_add(&ioctl_d_interface->cdev, devno, 1);

	return error;
}

static int ioctl_d_interface_init(void)
{
	dev_t devno = 0;
	int result = 0;

	ioctl_d_interface_dev_init(&ioctl_d_interface);

	/* register char device
	 * we will get the major number dynamically this is recommended see
	 * book : ldd3
	 */
	result = alloc_chrdev_region(&devno, ioctl_d_interface_minor, 1, ioctl_d_interface_name);
	ioctl_d_interface_major = MAJOR(devno);
	if (result < 0)
	{
		printk(KERN_WARNING "ioctl_d_interface: can't get major number %d\n", ioctl_d_interface_major);
		goto fail;
	}

	result = ioctl_d_interface_setup_cdev(&ioctl_d_interface);
	if (result < 0)
	{
		printk(KERN_WARNING "ioctl_d_interface: error %d adding ioctl_d_interface", result);
		goto fail;
	}

	printk(KERN_INFO "ioctl_d_interface: module loaded\n");
	return 0;

fail:
	ioctl_d_interface_exit();
	return result;
}

static void ioctl_d_interface_exit(void)
{
	dev_t devno = MKDEV(ioctl_d_interface_major, ioctl_d_interface_minor);

	cdev_del(&ioctl_d_interface.cdev);
	unregister_chrdev_region(devno, 1);
	ioctl_d_interface_dev_del(&ioctl_d_interface);

	printk(KERN_INFO "ioctl_d_interface: module unloaded\n");
}

/* Public API */
int ioctl_d_interface_open(struct inode *inode, struct file *filp)
{
	ioctl_d_interface_dev *ioctl_d_interface;

	ioctl_d_interface = container_of(inode->i_cdev, ioctl_d_interface_dev, cdev);
	filp->private_data = ioctl_d_interface;

	if (!atomic_dec_and_test(&ioctl_d_interface->available))
	{
		atomic_inc(&ioctl_d_interface->available);
		printk(KERN_ALERT "open ioctl_d_interface : the device has been opened by some other device, unable to open lock\n");
		return -EBUSY; /* already open */
	}

	return 0;
}

int ioctl_d_interface_release(struct inode *inode, struct file *filp)
{
	ioctl_d_interface_dev *ioctl_d_interface = filp->private_data;
	atomic_inc(&ioctl_d_interface->available); /* release the device */
	return 0;
}

/*
 * vpif_uservirt_to_phys: This function is used to convert user
 * space virtual address to physical address.
 */
static u32 vpif_uservirt_to_phys(u32 virtp)
{
	struct mm_struct *mm = current->mm;
	unsigned long physp = 0;
	struct vm_area_struct *vma;

	vma = find_vma(mm, virtp);

	/* For kernel direct-mapped memory, take the easy way */
	if (virtp >= PAGE_OFFSET)
	{
		physp = virt_to_phys((void *)virtp);
	}
	else if (vma && (vma->vm_flags & VM_IO) && (vma->vm_pgoff))
	{
		/* this will catch, kernel-allocated, mmaped-to-usermode addr */
		physp = (vma->vm_pgoff << PAGE_SHIFT) + (virtp - vma->vm_start);
	}
	else
	{
		/* otherwise, use get_user_pages() for general userland pages */
		int res, nr_pages = 1;
		struct page *pages;

		down_read(&current->mm->mmap_lock);

		res = get_user_pages(virtp, nr_pages, 1, &pages, NULL);
		up_read(&current->mm->mmap_lock);

		if (res == nr_pages)
		{
			physp = __pa(page_address(&pages[0]) +
						 (virtp & ~PAGE_MASK));
		}
		else
		{
			pr_err("get_user_pages failed\n");
			return 0;
		}
	}

	return physp;
}

long ioctl_d_interface_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd)
	{
	// Get the number of channel found
	case IOCTL_BASE_GET_MUIR:
		printk(KERN_INFO "<%s> ioctl: IOCTL_BASE_GET_MUIR\n", DEVICE_NAME);

		// struct mesg *msg = kmalloc(sizeof(struct mesg), GFP_DMA);

		// uint32_t value = 0x12345678;
		u32 *addr = kmalloc(sizeof(u32), GFP_DMA);

		/*
		 *if (copy_from_user(msg, (void *)arg, sizeof(struct mesg)))
		 *{
		 *	return -EFAULT;
		 *}
		 */

		if (copy_from_user(addr, (u32 *) arg, sizeof(u32)))
		{
			return -EFAULT;
		}
		// printk("Debug %lu %lu\n", msg->addr, msg->len);
		printk("Debug %u \n", *addr);

		u32 ret = vpif_uservirt_to_phys(*addr);
		printk("Debug 2 %u \n", ret);

		break;

	default:
		break;
	}

	return 0;
}

module_init(ioctl_d_interface_init);
module_exit(ioctl_d_interface_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DO Duy Huy Hoang");
MODULE_DESCRIPTION("IOCTL Interface driver");
MODULE_VERSION("0.1");
