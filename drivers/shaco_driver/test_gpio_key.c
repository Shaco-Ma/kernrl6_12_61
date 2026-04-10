#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio_keys.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/spinlock.h>

#define DEV_NAME											"test-gpio-key"

typedef struct
{
	struct device											*dev;
	struct class											*class;
	struct cdev												char_dev;
	dev_t													devno;
	struct gpio_desc										*gpio_desc;
	int														irq;
	struct input_dev										*input_dev;
}test_gpio_ctrl_t;

static int test_gpio_key_open(struct inode *inode, struct file *filp);
static int test_gpio_key_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos);
static int test_gpio_key_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos);
static int test_gpio_key_release(struct inode *inode, struct file *filp);

static const struct file_operations test_gpio_key_fops = 
{
	.owner		= THIS_MODULE,
	.open		= test_gpio_key_open,
	.read		= test_gpio_key_read,
	.write		= test_gpio_key_write,
	.release	= test_gpio_key_release,
};

static int test_gpio_key_open(struct inode *inode, struct file *filp)
{
	test_gpio_key_dev *char_handle = container_of(inode->i_cdev, test_gpio_key_dev, char_dev);
	if(char_handle->has_open)
	{
		return -EBUSY;
	}
	char_handle->has_open = 1;
	filp->private_data = char_handle;
	dev_info(char_handle->dev, "Open %s device\n", DEV_NAME);
	return 0;
}
static int test_gpio_key_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
	test_gpio_key_dev *char_handle = filp->private_data;
	if (copy_to_user(buf, &char_handle->write_val, 1))
	{
		return -EFAULT;
	}
	return 1;
}
static int test_gpio_key_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
	test_gpio_key_dev *char_handle = filp->private_data;
	if(count > 1)
	{
		return -ENOSPC;
	}
	else if(count == 0)
	{
		return 0;
	}
	if (copy_from_user(&char_handle->write_val, buf, count)) {
		return -EFAULT;
	}
	return 1;
}
static int test_gpio_key_release(struct inode *inode, struct file *filp)
{
	test_gpio_key_dev *char_handle = container_of(inode->i_cdev, test_gpio_key_dev, char_dev);
	char_handle->has_open = 0;
	dev_info(char_handle->dev, "Close %s device\n", DEV_NAME);
	return 0;
}

static int test_gpio_key_register_device(test_gpio_key_dev *char_handle)
{
	int ret;
	if((ret = alloc_chrdev_region(&char_handle->devno, 0, 1, DEV_NAME)) != 0)
	{
		return ret;
	}

	cdev_init(&char_handle->char_dev, &test_gpio_key_fops);

	char_handle->char_dev.owner = THIS_MODULE;
	ret = cdev_add(&char_handle->char_dev, char_handle->devno, 1);
	if (ret < 0)
		goto fail_cdev;

	char_handle->class = class_create(THIS_MODULE, DEV_NAME);
	if (IS_ERR(char_handle->class))
	{
		ret = PTR_ERR(char_handle->class);
		goto fail_class;
	}

	char_handle->dev = device_create(char_handle->class, NULL, char_handle->devno, NULL, DEV_NAME);
	if (IS_ERR(char_handle->dev)) {
		ret = PTR_ERR(char_handle->dev);
		goto fail_device;
	}

	return 0;

fail_device:
	class_destroy(char_handle->class);
fail_class:
	cdev_del(&char_handle->char_dev);
fail_cdev:
	unregister_chrdev_region(char_handle->devno, 1);
	return ret;
}

static int test_gpio_key_probe(struct platform_device *pdev)
{
    struct device_node *node = pdev->dev.of_node;
    test_gpio_key_dev *test_dev = NULL;

    test_dev = devm_kzalloc(&pdev->dev, sizeof(test_gpio_key_dev), GFP_KERNEL);
    if (!test_dev) {
        dev_err(&pdev->dev, "Failed to alloc key dev\n");
        return -ENOMEM;
    }

	if (of_property_read_u8(node, "init_val", &test_dev->write_val) != 0)
	{
		test_dev->write_val = 0xaa;
	}
    test_dev->dev = &pdev->dev;
    platform_set_drvdata(pdev, test_dev);

	//申请irq和input

	test_gpio_key_register_device(test_dev);

    dev_info(&pdev->dev, "register char test dev val = %x\n", test_dev->write_val);
    return 0;
}


static int test_gpio_key_remove(struct platform_device *pdev)
{
    test_gpio_key_dev *test_dev = platform_get_drvdata(pdev);
	dev_warn(&pdev->dev, "remove val = %x\n", test_dev->write_val);
	if (test_dev) {
		device_destroy(test_dev->class, test_dev->devno);
		cdev_del(&test_dev->char_dev);
		unregister_chrdev_region(test_dev->devno, 1);
	}
    return 0;
}

static const struct of_device_id test_gpio_dt_ids[] = {
	{ .compatible = "test,test-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, test_gpio_dt_ids);

static struct platform_driver test_gpio_key_driver= {
	.probe = test_gpio_key_probe,

	.driver = {
		.name = DEV_NAME,
		.of_match_table = test_gpio_dt_ids,
        .owner = THIS_MODULE,
	},
};
module_platform_driver(test_gpio_key_driver);

MODULE_AUTHOR("test");
MODULE_DESCRIPTION("GPIO key driver");
MODULE_LICENSE("GPL v2");
