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
#include <linux/timer.h>
#include <linux/cdev.h>

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
	struct timer_list										debounce_timer;
	bool													last_state;
    spinlock_t												lock;
}test_gpio_ctrl_t;

static int test_gpio_key_open(struct inode *inode, struct file *filp);
static ssize_t test_gpio_key_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos);
static ssize_t test_gpio_key_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos);
static int test_gpio_key_release(struct inode *inode, struct file *filp);

static const struct file_operations test_gpio_key_fops = 
{
	.owner		= THIS_MODULE,
	.open		= test_gpio_key_open,
	.read		= test_gpio_key_read,
	.write		= test_gpio_key_write,
	.release	= test_gpio_key_release,
};

static void test_debounce_timer_callback(struct timer_list *t)
{
    test_gpio_ctrl_t *test_handle = from_timer(test_handle, t, debounce_timer);
    int state;
    unsigned long flags;

    state = gpiod_get_value(test_handle->gpio_desc);
	dev_info(test_handle->dev, "cur = %d, old = %d\n", state, test_handle->last_state);
    spin_lock_irqsave(&test_handle->lock, flags);
    if (state != test_handle->last_state) {
        // 状态确实改变，上报事件
        input_report_key(test_handle->input_dev, KEY_POWER, state);
        input_sync(test_handle->input_dev);
        test_handle->last_state = state;
    }
    // 重新使能中断
    enable_irq(test_handle->irq);
    spin_unlock_irqrestore(&test_handle->lock, flags);
}

static irqreturn_t test_irq_handler(int irq, void *dev_id)
{
    test_gpio_ctrl_t *test_handle = dev_id;
    unsigned long flags;

    spin_lock_irqsave(&test_handle->lock, flags);
    // 禁止中断，防止抖动期间反复进入
    disable_irq_nosync(irq);
    // 修改定时器超时时间并启动（例如 20ms）
    mod_timer(&test_handle->debounce_timer, jiffies + msecs_to_jiffies(20));
    spin_unlock_irqrestore(&test_handle->lock, flags);

    return IRQ_HANDLED;
}

static int test_gpio_key_open(struct inode *inode, struct file *filp)
{
	test_gpio_ctrl_t *char_handle = container_of(inode->i_cdev, test_gpio_ctrl_t, char_dev);
	//if(char_handle->has_open)
	//{
	//	return -EBUSY;
	//}
	//char_handle->has_open = 1;
	//filp->private_data = char_handle;
	dev_info(char_handle->dev, "Open %s device\n", DEV_NAME);
	return 0;
}
static ssize_t test_gpio_key_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
	test_gpio_ctrl_t *char_handle = filp->private_data;
	//if (copy_to_user(buf, &char_handle->write_val, 1))
	//{
	//	return -EFAULT;
	//}
	return 1;
}
static ssize_t test_gpio_key_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
	test_gpio_ctrl_t *char_handle = filp->private_data;
	if(count > 1)
	{
		return -ENOSPC;
	}
	else if(count == 0)
	{
		return 0;
	}
	//if (copy_from_user(&char_handle->write_val, buf, count)) {
	//	return -EFAULT;
	//}
	return 1;
}
static int test_gpio_key_release(struct inode *inode, struct file *filp)
{
	test_gpio_ctrl_t *char_handle = container_of(inode->i_cdev, test_gpio_ctrl_t, char_dev);
	//char_handle->has_open = 0;
	dev_info(char_handle->dev, "Close %s device\n", DEV_NAME);
	return 0;
}

static int test_gpio_key_register_device(test_gpio_ctrl_t *char_handle)
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

	char_handle->class = class_create(DEV_NAME);
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
    //struct device_node *node = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct input_dev *input;
    test_gpio_ctrl_t *test_dev = NULL;
	int ret;

    test_dev = devm_kzalloc(&pdev->dev, sizeof(test_gpio_ctrl_t), GFP_KERNEL);
    if (!test_dev) {
        dev_err(&pdev->dev, "Failed to alloc key dev\n");
        return -ENOMEM;
    }
	spin_lock_init(&test_dev->lock);

	test_dev->gpio_desc = devm_gpiod_get(dev, "test", GPIOD_IN);
	if (IS_ERR(test_dev->gpio_desc))
	{
        dev_err(&pdev->dev, "Failed to Get Gpio:%d\n", PTR_ERR(test_dev->gpio_desc));
		return PTR_ERR(test_dev->gpio_desc);
	}
	dev_err(dev, "gpio num = %d\n", desc_to_gpio(test_dev->gpio_desc));
    test_dev->dev = &pdev->dev;
    platform_set_drvdata(pdev, test_dev);

	//申请irq和input
	test_dev->irq = gpiod_to_irq(test_dev->gpio_desc);
	dev_err(dev, "irq num = %d\n", test_dev->irq);

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	test_dev->input_dev = input;
	//给input传递数据
	input_set_drvdata(input, test_dev->input_dev);

	input->name = "TEST GPIO";
	input->phys = "test-gpio-key/input0";
	input->dev.parent = dev;

	input->id.bustype = BUS_HOST;
	input_set_capability(input, EV_KEY, KEY_POWER);
    ret = input_register_device(input);
	if(ret)
	{
        dev_err(&pdev->dev, "Failed to register input\n");
		return ret;
	}

	//初始化timer
	timer_setup(&test_dev->debounce_timer, test_debounce_timer_callback, 0);

	test_dev->last_state = gpiod_get_value(test_dev->gpio_desc);

    // 请求中断（触发方式：双边沿，因为我们要检测按下和释放）
    ret = devm_request_irq(dev, test_dev->irq, test_irq_handler, IRQF_TRIGGER_FALLING, "test irq", test_dev);
	if(ret)
	{
        dev_err(&pdev->dev, "Failed to register irq:%d\n", test_dev->irq);
		return ret;
	}

	//test_gpio_key_register_device(test_dev);

    //dev_info(&pdev->dev, "register char test dev val = %x\n", test_dev->write_val);
    return 0;
}


static int test_gpio_key_remove(struct platform_device *pdev)
{
    test_gpio_ctrl_t *test_dev = platform_get_drvdata(pdev);
	//dev_warn(&pdev->dev, "remove val = %x\n", test_dev->write_val);
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
