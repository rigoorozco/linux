/*
 * Thin layer to provide access to FPGA fabric and allocate large buffers.
 *
 * Copyright (c) 2012 Ettus Research, LLC
 * Author: Philip Balister <philip@opensdr.com>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/dma-mapping.h>

#include <asm/page.h>

#define DRIVER_NAME "axi_fpga"

struct buffer_addr {
	void *virt_addr;
	unsigned long phys_addr;
	unsigned long size;
	int direction;
};

#define AXI_FPGA_GET_PAGE		_IOWR('a', 0x30, struct buffer_addr)
#define AXI_FPGA_GIVE_PAGE		_IOW('a', 0x31, struct buffer_addr)
#define AXI_FPGA_TAKE_PAGE		_IOW('a', 0x32, struct buffer_addr)


/**
 * struct axi_fpga_drvdata - Device Configuration driver structure
 *
 */
struct axi_fpga_drvdata {
	struct platform_device *pdev;
	struct miscdevice axi_fpga_device;
	int dev_open;
	atomic_t irq_happened;

	/* Name from device tree */
	char *name;

	/* AXI Master addressing (fpga is slave) */
	unsigned long dev_physaddr;
	unsigned long dev_size;

	/* fpga data transfer buffer */
	unsigned long slave_phys_addr;
	struct page *fpga_buffer_pages;
	unsigned int fpga_buffer_npages;

	/* Interrupt configuration */
	int irq_number;
	wait_queue_head_t wait;
};


/**
 * axi_fpga_irq_handler() - handle irq's
 */
static irqreturn_t axi_fpga_irq_handler(int irq, void *driv_data)
{
	struct axi_fpga_drvdata *d = driv_data;

	atomic_set(&d->irq_happened, 1);
	wake_up_interruptible(&d->wait);

	return IRQ_HANDLED;
}

/**
 * axi_fpga_open() - This is the driver open function.
 */
static int axi_fpga_open(struct inode *inode, struct file *filp)
{
	struct axi_fpga_drvdata *d = container_of(filp->private_data,
				struct axi_fpga_drvdata, axi_fpga_device);
	dev_info(&d->pdev->dev, "in axi_fpga_open, d->dev_open = %d\n",
			d->dev_open);

	if (d->dev_open)
		return -EBUSY;
	else
		d->dev_open = 1;

	if (request_irq(d->irq_number, axi_fpga_irq_handler, 0, d->name, d))
		return -EBUSY;

	return 0;
}

static unsigned int axi_fpga_poll(struct file *filp, poll_table *wait)
{
	struct axi_fpga_drvdata *d = container_of(filp->private_data,
				struct axi_fpga_drvdata, axi_fpga_device);
	unsigned int mask = 0;

	if (!d->dev_open)
		return -EIO;

	poll_wait(filp, &d->wait, wait);

	if (atomic_read(&d->irq_happened) == 1)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static ssize_t axi_fpga_read(struct file *filp, char *buf, size_t count,
				loff_t *ppos)
{
	struct axi_fpga_drvdata *d = container_of(filp->private_data,
				struct axi_fpga_drvdata, axi_fpga_device);

	if (!d->dev_open)
		return -EIO;

	// if (atomic_read(&d->irq_happened) == 1) {
	// 	atomic_set(&d->irq_happened, 0);
	// 	return 0;
	// }
	// interruptible_sleep_on(&d->wait);
	
	wait_event_interruptible(d->wait, atomic_read(&d->irq_happened) == 1);
	atomic_set(&d->irq_happened, 0);
	return 0;
}

static long axi_fpga_ioctl(struct file *file,
				unsigned int cmd, unsigned long arg)
{
	struct axi_fpga_drvdata *d = container_of(file->private_data,
				struct axi_fpga_drvdata, axi_fpga_device);
	struct buffer_addr buf_addr;
	struct buffer_addr __user *ubuf_addr;
	struct page *page;
	int res, retval;

	ubuf_addr = (struct buffer_addr __user *) arg;
	copy_from_user(&buf_addr, ubuf_addr, sizeof(struct buffer_addr));

	retval = 0;

	switch (cmd) {

	case AXI_FPGA_GET_PAGE:
		down_read(&current->mm->mmap_sem);
		res = get_user_pages((unsigned long) buf_addr.virt_addr, 1, 0, &page, NULL);

		up_read(&current->mm->mmap_sem);

		dev_info(&d->pdev->dev, "npages = %d\n", res);

		if (!res) 
			retval = -EINVAL;

		if (buf_addr.direction)
			buf_addr.phys_addr = dma_map_page(&d->pdev->dev, page, 0,
					buf_addr.size, DMA_TO_DEVICE);
		else
			buf_addr.phys_addr = dma_map_page(&d->pdev->dev, page, 0,
					buf_addr.size, DMA_FROM_DEVICE);

		copy_to_user(ubuf_addr, &buf_addr, sizeof(struct buffer_addr));
		return retval;

	case AXI_FPGA_GIVE_PAGE:
		if (buf_addr.direction)
			dma_sync_single_for_device(&d->pdev->dev, buf_addr.phys_addr,
				buf_addr.size, DMA_TO_DEVICE);
		else
			dma_sync_single_for_device(&d->pdev->dev, buf_addr.phys_addr,
				buf_addr.size, DMA_FROM_DEVICE);
		break;

	case AXI_FPGA_TAKE_PAGE:
		if (buf_addr.direction)
			dma_sync_single_for_cpu(&d->pdev->dev, buf_addr.phys_addr,
				buf_addr.size, DMA_TO_DEVICE);
		else
			dma_sync_single_for_cpu(&d->pdev->dev, buf_addr.phys_addr,
				buf_addr.size, DMA_FROM_DEVICE);
		break;

	default:
		return -ENOTTY;
	}
	return 0;
}

static const struct vm_operations_struct axi_fpga_mmap_ops = {
	.open	= NULL,
	.close	= NULL,
};


/**
 * axi_fpga_mmap() - Give the user access to the master and some ram.
 */
static int axi_fpga_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long size, expected_size, addr;
	struct axi_fpga_drvdata *d = container_of(filp->private_data,
				struct axi_fpga_drvdata, axi_fpga_device);
	dev_info(&d->pdev->dev, "in axi_fpga_mmap\n");

	if (!d->dev_open)
		return -EIO;

	if (vma->vm_pgoff)
		return -EINVAL;

	expected_size = d->dev_size + (1<<(d->fpga_buffer_npages)) * PAGE_SIZE;
	size = vma->vm_end - vma->vm_start;
	dev_info(&d->pdev->dev, "Size = %ld, expected size = %ld\n", size,
			expected_size);
	if (size != expected_size)
		return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	addr = vma->vm_start;

	if (remap_pfn_range(vma, addr,
				d->dev_physaddr >> PAGE_SHIFT,
				d->dev_size, vma->vm_page_prot))
		return -EIO;

	addr = d->dev_size;
	if (remap_pfn_range(vma, addr,
				page_to_pfn(d->fpga_buffer_pages),
				(1<<d->fpga_buffer_npages) * PAGE_SIZE,
				vma->vm_page_prot))
		return -EIO;

	vma->vm_ops = &axi_fpga_mmap_ops;

	return 0;
}



/**
 * axi_fpga_release() - This is the driver release function.
 */
static int axi_fpga_release(struct inode *inode, struct file *filp)
{
	struct axi_fpga_drvdata *d = container_of(filp->private_data,
				struct axi_fpga_drvdata, axi_fpga_device);

	dev_info(&d->pdev->dev, "in axi_fpga_release\n");

	if (!d->dev_open)
		return -EIO;

	free_irq(d->irq_number, d);

	d->dev_open = 0;

	return 0;
}

/*
 * /sys/axi_fpgaX
 * /phys_addr		read-only	Physical address of the buffer
 * /buffer_length	read-only	length of buffer
 * /control_length	read-only	length of control address space
 */

static ssize_t axi_fpga_get_phys_addr(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct axi_fpga_drvdata *d = platform_get_drvdata(pdev);

	return sprintf(buf, "%ld\n", d->slave_phys_addr);
}

static ssize_t axi_fpga_get_buffer_length(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct axi_fpga_drvdata *d = platform_get_drvdata(pdev);

	return sprintf(buf, "%ld\n", (1 << d->fpga_buffer_npages) * PAGE_SIZE);
}

static ssize_t axi_fpga_get_control_length(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct axi_fpga_drvdata *d = platform_get_drvdata(pdev);

	return sprintf(buf, "%ld\n", d->dev_size);
}

static DEVICE_ATTR(phys_addr, S_IRUGO, axi_fpga_get_phys_addr, NULL);
static DEVICE_ATTR(buffer_length, S_IRUGO, axi_fpga_get_buffer_length, NULL);
static DEVICE_ATTR(control_length, S_IRUGO, axi_fpga_get_control_length, NULL);

static struct attribute *axi_fpga_sysfs_entries[] = {
	&dev_attr_phys_addr.attr,
	&dev_attr_buffer_length.attr,
	&dev_attr_control_length.attr,
	NULL
};

static struct attribute_group axi_fpga_attribute_group = {
		.name = NULL,
		.attrs = axi_fpga_sysfs_entries,
};

static const struct file_operations axi_fpga_fops = {
	.owner		=	THIS_MODULE,
	.open		=	axi_fpga_open,
	.read		=	axi_fpga_read,
	.poll		=	axi_fpga_poll,
	.mmap		=	axi_fpga_mmap,
	.unlocked_ioctl =	axi_fpga_ioctl,
	.release	=	axi_fpga_release,
};

/**
 * axi_fpga_drv_probe() - Probe call for the device
 */
static int axi_fpga_drv_probe(struct platform_device *pdev)
{
	struct resource *control, *buffer, *irq;
	struct axi_fpga_drvdata *d;
	int err, ret = 0;

	dev_info(&pdev->dev, "in axi_fpga_dev_probe\n");

	control = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!control) {
		dev_err(&pdev->dev, "Invalid address\n");
		return -ENODEV;
	}

	buffer = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!buffer) {
		dev_err(&pdev->dev, "Invalid address\n");
		return -ENODEV;
	}

	irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (!irq) {
		dev_err(&pdev->dev, "No IRQ found\n");
		return -ENODEV;
	}

	dev_info(&pdev->dev, "Reg 1 start: %08X, reg 1 end: %08X, reg 1 name: %s\n",
		control->start, control->end, control->name);
	dev_info(&pdev->dev, "Reg 2 start: %08X, reg 2 end: %08X, reg 2 name: %s\n",
		buffer->start, buffer->end, buffer->name);

	dev_info(&pdev->dev, "IRQ start: %d, IRQ stop %d, IRQ name, %s\n",
		irq->start, irq->end, irq->name);


	d = devm_kzalloc(&pdev->dev, sizeof(struct axi_fpga_drvdata),
				GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	dev_set_drvdata(&pdev->dev, (void *)d);

	d->pdev = pdev;

	/* Get driver name from device tree */
	d->name = kstrdup(pdev->name, GFP_KERNEL);
	dev_info(&pdev->dev, "Dev name: %s\n", d->name);

	/* Setup interrupt */
	d->irq_number = irq->start;
	init_waitqueue_head(&d->wait);

	atomic_set(&d->irq_happened, 0);

	/* Setup memory for AXI window */

	d->dev_physaddr = control->start;
	d->dev_size = control->end - control->start + 1;

	if (!devm_request_mem_region(&pdev->dev, d->dev_physaddr,
			d->dev_size, control->name)) {
		dev_err(&pdev->dev, "can't reserve IO memory at 0x%0lx\n",
				d->dev_physaddr);
		return -ENODEV;
	}

	/* Setup contiguous RAM buffer for fpga */
	d->fpga_buffer_npages = buffer->end + 1;
	d->fpga_buffer_pages = alloc_pages(GFP_ATOMIC, d->fpga_buffer_npages);
	if (!d->fpga_buffer_pages) {
		dev_err(&pdev->dev, "Failed to allocate buffer\n");
		return -1;
	}
	split_page(d->fpga_buffer_pages, d->fpga_buffer_npages);
	d->slave_phys_addr = page_to_phys(d->fpga_buffer_pages);


	/* Initialize driver flags */
	d->dev_open = 0;

/* Turn on userspace access to cycle counter DELETE */
	/* enable user-mode access to the performance counter*/
	asm ("MCR p15, 0, %0, C9, C14, 0\n\t" : : "r"(1));

	/* disable counter overflow interrupts (just in case)*/
	asm ("MCR p15, 0, %0, C9, C14, 2\n\t" : : "r"(0x8000000f));

	d->axi_fpga_device.name = "axi_fpga",
	d->axi_fpga_device.fops = &axi_fpga_fops,
	d->axi_fpga_device.minor = MISC_DYNAMIC_MINOR,
	err = misc_register(&(d->axi_fpga_device));
	if (err)
		dev_err(&pdev->dev, "OMFG something bad happened!!!!");

	err = sysfs_create_group(&pdev->dev.kobj, &axi_fpga_attribute_group);
	if (ret)
		dev_err(&pdev->dev, "OMFG something bad happened, creating sysfs!!!!");

	return 0;
}

/**
 * axi_fpga_drv_remove() - Remove call for the device.
 */
static int axi_fpga_drv_remove(struct platform_device *pdev)
{
	struct axi_fpga_drvdata *d;

	dev_info(&pdev->dev, "in axi_fpga_drv_remove\n");

	d = dev_get_drvdata(&pdev->dev);

	misc_deregister(&(d->axi_fpga_device));

	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id axi_fpga_of_match[] = {
	{ .compatible = "xlnx,ps7-axi-fpga-1.00.a", },
	{ /* end of table */}
};
MODULE_DEVICE_TABLE(of, axi_fpga_of_match);
#else
#define axi_fpga_of_match NULL
#endif /* CONFIG_OF */

/* Driver Structure */
static struct platform_driver axi_fpga_platform_driver = {
	.probe = axi_fpga_drv_probe,
	.remove = axi_fpga_drv_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = DRIVER_NAME,
		.of_match_table = axi_fpga_of_match,
	},
};

/**
 * axi_fpga_module_init() - register the Device Configuration.
 */
static int __init axi_fpga_module_init(void)
{
	pr_info("in axi_fpga_module_init\n");
	return platform_driver_register(&axi_fpga_platform_driver);
}

/**
 * axi_fpga_module_exit - Unregister the Device Configuration.
 */
static void axi_fpga_module_exit(void)
{
	pr_info("in axi_fpga_module_exit\n");
	platform_driver_unregister(&axi_fpga_platform_driver);
}

module_init(axi_fpga_module_init);
module_exit(axi_fpga_module_exit);

MODULE_AUTHOR("philip@opensdr.com <Philip Balister>");
MODULE_DESCRIPTION("Streaming data to/from FPGA via AXI");
MODULE_LICENSE("GPL");
