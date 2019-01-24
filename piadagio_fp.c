////////////////////////////////////////////////////////////////////
// piadagio_fp
//
// Implements an i2c driver for the Adagio front panel.
//
// This was developed with help from:
//	http://lightsurge2.blogspot.com/2014/05/writing-linux-kernel-device-driver-for.html
//	http://cs.smith.edu/~nhowe/262/labs/kmodule.html
//	https://github.com/vpcola/chip_i2c
//
// Todo:
//	Convert from register_chrdev to cdev
//
////////////////////////////////////////////////////////////////////

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/fs.h>
//#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include "piadagio_fp.h"

////////////////////////////////////////////////////////////////////
// Global variables
////////////////////////////////////////////////////////////////////
// Variables need by the character driver
static struct i2c_client * piadagio_fp_client = NULL;

// We define a mutex so that only one process at a time can access our driver
// at /dev. Any other process attempting to open this driver will return -EBUSY.
static DEFINE_MUTEX(piadagio_fp_mutex);

// Work queue variables
// Module variables
static struct class * piadagio_fp_class = NULL;
static struct device * piadagio_fp_device = NULL;
static int piadagio_fp_major;

// Actual data storage
static int piadagio_fp_buffer_command = 0;				// Command read from the FP
static struct piadagio_fp_char_buffer piadagio_fp_buffer_lcd_screen;	// Buffer for the LCD screen
static char *piadagio_fp_buffer_index = 0;

////////////////////////////////////////////////////////////////////
// General routines
////////////////////////////////////////////////////////////////////
// Clears the character buffer with ASCII spaces
void piadagio_fp_buffer_lcd_clear() {
	unsigned int i;
	char *tmp_index;
	tmp_index = piadagio_fp_buffer_lcd_screen.line1;
	for (i = 0; i < (4 * LCD_LINE_LEN); i++) {
		*tmp_index = ' ';
		tmp_index++;
	}
}

////////////////////////////////////////////////////////////////////
// Character driver
////////////////////////////////////////////////////////////////////
// Called when device is first opened
static int piadagio_fp_open(struct inode * inode, struct file *fp) {
	printd("%s: Attempt to open our device\n", __FUNCTION__);

	// Ensure only one device accesses at a time
	if (!mutex_trylock(&piadagio_fp_mutex)) {
		printd("%s: Device currently in use!\n", __FUNCTION__);
		return -EBUSY;
	}

	// Ensure that the i2c client is available
	if (piadagio_fp_client == NULL)
		return -ENODEV;

	piadagio_fp_buffer_index = piadagio_fp_buffer_lcd_screen.line1;	// Reset screen buffer pointer
	return 0;
}

// Called when the device file pointer is closed
static int piadagio_fp_release(struct inode * inode, struct file * fp) {
	printd("%s: Freeing /dev resource\n", __FUNCTION__);

	mutex_unlock(&piadagio_fp_mutex);
	return 0;
}

// Read from the device
static ssize_t piadagio_fp_read(struct file *filp,   /* see include/linux/fs.h   */
				char *buffer,        /* buffer to fill with data */
				size_t length,       /* length of the buffer     */
				loff_t * offset) {
	// We're just interested in any commands read from the FP
	if (copy_to_user(&piadagio_fp_buffer_command, buffer, 1) > 0) {
		return 1;
	}

	return -EFAULT;
}

// Write to the lcd screen buffer
static ssize_t piadagio_fp_write(struct file * fp, const char __user * buffer, size_t count, loff_t * offset) {
	int num_write = 0;

	printd("%s: Write operation with [%d] bytes, from offset [%lld]\n", __FUNCTION__, count, ((long long int) *offset));

	// Iterate through the user space buffer
	while (count) {
		if (copy_from_user(piadagio_fp_buffer_index, (buffer + num_write), 1)) {
			return -EFAULT;
		}
		num_write++;
		count--;
		piadagio_fp_buffer_index++;
		// Reset the screen buffer pointer, if we have overrun the end
		if (piadagio_fp_buffer_index >= (piadagio_fp_buffer_lcd_screen.line1 + SCREEN_BUFFER_LEN)) {
			piadagio_fp_buffer_index = piadagio_fp_buffer_lcd_screen.line1;
		}
	}

	return num_write;
}

static loff_t piadagio_fp_llseek(struct file *file, loff_t offset, int origin) {
	printd("%s: llseek to offset [%lld]\n", __FUNCTION__, ((long long int) offset));

	if (offset < SCREEN_BUFFER_LEN) {
		piadagio_fp_buffer_index += offset;
	} else {
		return -EFAULT;
	}

	return offset;
}

static struct file_operations piadagio_fp_fops = {
	.owner = THIS_MODULE,
	.read = piadagio_fp_read,
	.write = piadagio_fp_write,
	.llseek = piadagio_fp_llseek,
	.open = piadagio_fp_open,
	.release = piadagio_fp_release
};

////////////////////////////////////////////////////////////////////
// SysFS
////////////////////////////////////////////////////////////////////
// SysFS object to display current command
static ssize_t piadagio_fp_get_command(struct device *dev, struct device_attribute *dev_attr, char * buf) {
//	struct i2c_client * client = to_i2c_client(dev);
	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
//	return sprintf(buf, "%d\n", value);
	return sprintf(buf, "FP Command: 0x00 (None)\n");
}

// SysFS object to display the lcd buffer
static ssize_t piadagio_fp_get_lcd_buffer(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "%.*s\n%.*s\n%.*s\n%.*s\n",
				LCD_LINE_LEN,piadagio_fp_buffer_lcd_screen.line1,
				LCD_LINE_LEN,piadagio_fp_buffer_lcd_screen.line2,
				LCD_LINE_LEN,piadagio_fp_buffer_lcd_screen.line3,
				LCD_LINE_LEN,piadagio_fp_buffer_lcd_screen.line4);
}

// Front panel command is read only
static DEVICE_ATTR(fp_command, S_IRUGO, piadagio_fp_get_command, NULL);
static DEVICE_ATTR(fp_lcd, S_IRUGO, piadagio_fp_get_lcd_buffer, NULL);

////////////////////////////////////////////////////////////////////
// I2C methods
////////////////////////////////////////////////////////////////////
// Device detection routine
static int piadagio_fp_detect(struct i2c_client * client, struct i2c_board_info * info) {
	struct i2c_adapter *adapter = client->adapter;
	int address = client->addr;
	const char * name = NULL;
	int tmp_data;

	printd("%s\n", __FUNCTION__);

	// Check adapter functionality
	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_I2C_BLOCK)) {
		printe("Adapter does not support required functionality.\n");
		return -ENODEV;
	}

	if (address == PIADAGIOFP_I2C_ADDR) {
		// Test read, any negative number is an error
		tmp_data = i2c_smbus_read_byte(client);
		if (tmp_data >= 0) {
			name = PIADAGIOFP_I2C_DEVNAME;
			dev_info(&adapter->dev, "Chip device found at 0x%02x\n", address);
		}
	} else {
		return -ENODEV;
	}

	// Upon successful detection, we copy the name of the driver to the info struct.
	strlcpy(info->type, name, I2C_NAME_SIZE);

	return 0;
}

// Device instantiation
static int piadagio_fp_probe(struct i2c_client *client, const struct i2c_device_id *id) {
	int retval = 0;
	struct device * dev = &client->dev;
	struct piadagio_fp_data *data = NULL;

	printd("%s\n", __FUNCTION__);

	/* Allocate the client's data here */
	data = devm_kzalloc(&client->dev, sizeof(struct piadagio_fp_data), GFP_KERNEL);
	if(!data)
		return -ENOMEM;

	// Initialize client's data to default
	i2c_set_clientdata(client, data);

	// Initialize the mutex
	mutex_init(&data->update_lock);

	// Clear the lcd buffer
	piadagio_fp_buffer_lcd_clear();

	/* If our driver requires additional data initialization
	 * we do it here. For our intents and purposes, we only
	 * set the data->kind which is taken from the i2c_device_id.
	 */
	data->kind = id->driver_data;

	// Store for character driver operations
	piadagio_fp_client = client;

	// We now create our character device driver
	piadagio_fp_major = register_chrdev(0, PIADAGIOFP_I2C_DEVNAME, &piadagio_fp_fops);
	if (piadagio_fp_major < 0) {
		retval = piadagio_fp_major;
		printe("%s: Failed to register char device!\n", __FUNCTION__);
		goto out;
	}

	piadagio_fp_class = class_create(THIS_MODULE, PIADAGIOFP_I2C_DEVNAME);
	if (IS_ERR(piadagio_fp_class)) {
		retval = PTR_ERR(piadagio_fp_class);
		printe("%s: Failed to create class!\n", __FUNCTION__);
		goto unreg_chrdev;
	}
	piadagio_fp_device = device_create(piadagio_fp_class, NULL,
						MKDEV(piadagio_fp_major, 0),
						NULL,
						PIADAGIOFP_I2C_DEVNAME "_lcd");
	if (IS_ERR(piadagio_fp_device)) {
		retval = PTR_ERR(piadagio_fp_device);
		printe("%s: Failed to create device!\n", __FUNCTION__);
		goto unreg_class;
	}

	// Initialize the mutex for /dev fops clients
	mutex_init(&piadagio_fp_mutex);

	// We now register our sysfs attributs.
	device_create_file(dev, &dev_attr_fp_command);
	device_create_file(dev, &dev_attr_fp_lcd);

	return 0;

// Cleanup on failed operations
unreg_class:
	class_unregister(piadagio_fp_class);
	class_destroy(piadagio_fp_class);
unreg_chrdev:
	unregister_chrdev(piadagio_fp_major, PIADAGIOFP_I2C_DEVNAME);
	printe("%s: Driver initialization failed!\n", __FUNCTION__);
out:
	return retval;
}

// Device removal
static int piadagio_fp_remove(struct i2c_client * client) {
	struct device * dev = &client->dev;

	printd("%s\n", __FUNCTION__);

	piadagio_fp_client = NULL;

	device_remove_file(dev, &dev_attr_fp_command);
	device_remove_file(dev, &dev_attr_fp_lcd);
	device_destroy(piadagio_fp_class, MKDEV(piadagio_fp_major, 0));

	class_unregister(piadagio_fp_class);
	class_destroy(piadagio_fp_class);

	unregister_chrdev(piadagio_fp_major, PIADAGIOFP_I2C_DEVNAME);

	return 0;
}

// Devices supported by this driver
static const struct i2c_device_id piadagio_fp_id[] = {
	{ "piadagio_fp", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, piadagio_fp_id);

// Define the addresses to scan.
static const unsigned short scan_i2c_addrs[] = { PIADAGIOFP_I2C_ADDR, I2C_CLIENT_END };

////////////////////////////////////////////////////////////////////
// Module methods
////////////////////////////////////////////////////////////////////
static struct i2c_driver piadagio_fp_driver = {
	.driver = {
		.name	= PIADAGIOFP_I2C_DEVNAME,
	},
	.id_table	= piadagio_fp_id,
	.probe		= piadagio_fp_probe,
	.remove		= piadagio_fp_remove,
	.class		= I2C_CLASS_HWMON,
// Maybe it would be better to define as a memory device
//	.class	= I2C_CLASS_SPD,	// Memory device
	.detect		= piadagio_fp_detect,
	.address_list	= scan_i2c_addrs,
};
module_i2c_driver(piadagio_fp_driver);

//static int __init piadagio_fp_init(void) {
//	return i2c_add_driver(&piadagio_fp_driver);
//module_init(piadagio_fp_init);

//static void __exit piadagio_fp_remove(void) {
//	return i2c_del_driver(&piadagio_fp_driver);
//module_exit(piadagio_fp_remove);

MODULE_AUTHOR("Charles Burgoyne");
MODULE_DESCRIPTION("Adagio front panel driver");
MODULE_LICENSE("GPL");
