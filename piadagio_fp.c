////////////////////////////////////////////////////////////////////
//
// piadagio_fp
// Implements an i2c driver for the Adagio front panel.
//
// This was developed with help from:
//	http://lightsurge2.blogspot.com/2014/05/writing-linux-kernel-device-driver-for.html
//	http://cs.smith.edu/~nhowe/262/labs/kmodule.html
//      https://github.com/vpcola/chip_i2c
//
// Todo:
//	Convert from register_chrdev to cdev
//	Dealloc client data!
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

static bool fp_require_fsync = true;
module_param(fp_require_fsync, bool, 0660);
MODULE_PARM_DESC(fp_require_fsync, "Controls whether a fsync is required to update the front panel, after writing to screen buffer.\n");

////////////////////////////////////////////////////////////////////
// Global variables
////////////////////////////////////////////////////////////////////
// Variables need by the character driver
static struct i2c_client * piadagio_fp_i2c_client = NULL;

// We define a mutex so that only one process at a time can access our driver
// at /dev. Any other process attempting to open this driver will return -EBUSY.
static DEFINE_MUTEX(piadagio_fp_mutex);

// Work queue variables
static struct workqueue_struct *piadagio_fp_wq;
static struct delayed_work piadagio_fp_wq_task_lcd;
static DECLARE_DELAYED_WORK(piadagio_fp_wq_task_lcd, piadagio_fp_task_lcd_update);
static struct delayed_work piadagio_fp_wq_task_led;
static DECLARE_DELAYED_WORK(piadagio_fp_wq_task_led, piadagio_fp_task_led_update);
static int piadagio_fp_wq_kill = 0;

// Module variables
static struct class * piadagio_fp_class = NULL;
static struct device * piadagio_fp_device = NULL;
static int piadagio_fp_major;

// Actual data storage
static struct piadagio_fp_char_buffer piadagio_fp_buffer_lcd_screen;	// Buffer for the LCD screen
static struct piadagio_fp_glyphs piadagio_fp_buffer_lcd_ugram;		// Buffer for the LCD UGRAM
static char *piadagio_fp_buffer_index_ptr = 0;
static unsigned char piadagio_fp_buffer_i2c_rw[I2C_BUFFER_LEN];		// Structure to r/w i2c data
static unsigned char *piadagio_fp_glyph_index_ptr = 0;
static unsigned char piadagio_fp_write_to_buffer = BUFFER_WRITE_CHAR;	// Which buffer to write to
static unsigned int piadagio_fp_buffer_command = 0;			// Command read from the FP
static unsigned long piadagio_fp_i2c_update_lcd_counter = 0;		// Update counter (LCD)
static unsigned long piadagio_fp_i2c_update_glyph_counter = 0;		// Update counter (Glyph)
static unsigned long piadagio_fp_i2c_update_led_counter = 0;		// Update counter (LED)
static unsigned long piadagio_fp_i2c_update_errors_counter = 0;		// Update errors counter
static unsigned long piadagio_fp_i2c_update_retries_counter = 0;	// Update retry counter
static unsigned short piadagio_fp_i2c_update_do = 1;			// Controls whether an update actually happens (they're still scheduled)
static unsigned short piadagio_fp_i2c_update_do_screen = 1;		// Controls whether a screen update actually happens
static unsigned short piadagio_fp_led_online = 0;			// Online LED status
static unsigned short piadagio_fp_led_power = 1;			// Power LED status
static bool piadagio_fp_glyph_updated[8];				// Stores whether a LCD UGRAM glyph has been updated
static bool piadagio_fp_i2c_update_screen_other_half = false;		// Used to store which half of the screen to update next

////////////////////////////////////////////////////////////////////
// General routines
////////////////////////////////////////////////////////////////////
// Clears the character buffer with ASCII spaces
void piadagio_fp_buffer_lcd_clear() {
	unsigned int i;
	char *tmp_index;

	printd("%s\n", __FUNCTION__);

	tmp_index = piadagio_fp_buffer_lcd_screen.line1;
	for (i = 0; i < (4 * LCD_LINE_LEN); i++) {
		*tmp_index = ' ';
		tmp_index++;
	}
}

// Initialise LCD UGRAM buffer
void piadagio_fp_buffer_ugram_init() {
	unsigned int i,j;

	printd("%s\n", __FUNCTION__);

	for (i = 0; i < 8; i++) {
		for (j = 0; j < 8; j++) {
//			piadagio_fp_buffer_lcd_ugram.glyph[i].pixel_line[j] = 0xff;
			piadagio_fp_buffer_lcd_ugram.glyph[i].pixel_line[j] = j * 2;
		}

		piadagio_fp_glyph_updated[i] = false;
	}
}

// Reads the current status and command from the FP
// A double read from the FP produces:
//	1. FP status byte
//	2. FP command byte
int piadagio_fp_i2c_get_status() {
	struct piadagio_fp_data *data = i2c_get_clientdata(piadagio_fp_i2c_client);
	int bytes_recvd;

	//printd("%s\n", __FUNCTION__);

	mutex_lock(&data->update_lock);
	bytes_recvd = i2c_master_recv(piadagio_fp_i2c_client, &piadagio_fp_buffer_i2c_rw[0], 2);
	mutex_unlock(&data->update_lock);
	if (bytes_recvd == 2) {
		piadagio_fp_buffer_command = piadagio_fp_buffer_i2c_rw[1];
		return piadagio_fp_buffer_i2c_rw[0];
	}

	printe("%s: Failed to read FP status. Read %d bytes.\n", __FUNCTION__, bytes_recvd);
	return -1;
}

// Update half of the screen
// Because there is not enough space to receive an entire screen in
// the microcontroller, 2 updates are required. To further complicate
// things, because of the memory layout of the LCD the lines are
// written out in the order of 1 & 3, then 2 & 4.
int piadagio_fp_i2c_update_screen() {
	struct piadagio_fp_data *data = i2c_get_clientdata(piadagio_fp_i2c_client);
	unsigned int bytes_2_send;

	//printd("%s\n", __FUNCTION__);

	if (!piadagio_fp_i2c_update_screen_other_half) {
		bytes_2_send = snprintf(&piadagio_fp_buffer_i2c_rw[0], I2C_BUFFER_LEN,
					"%c%c%c%.*s%.*s",					// Format: length + cmd + position + lines
					I2C_MSG_LEN_UPDATE_LCD - 1,				// Message length doesn't include this byte
					I2C_MSG_TYPE_CHAR,					// Screen write cmd
					0x0,							// Screen write position
					LCD_LINE_LEN,piadagio_fp_buffer_lcd_screen.line1,
					LCD_LINE_LEN,piadagio_fp_buffer_lcd_screen.line3);
	} else {
		bytes_2_send = snprintf(&piadagio_fp_buffer_i2c_rw[0], I2C_BUFFER_LEN,
					"%c%c%c%.*s%.*s",					// Format: length + cmd + position + lines
					I2C_MSG_LEN_UPDATE_LCD - 1,				// Message length doesn't include this byte
					I2C_MSG_TYPE_CHAR,					// Screen write cmd
					0x1,							// Screen write position
					LCD_LINE_LEN,piadagio_fp_buffer_lcd_screen.line2,
					LCD_LINE_LEN,piadagio_fp_buffer_lcd_screen.line4);
	}
	if (bytes_2_send == I2C_MSG_LEN_UPDATE_LCD) {
		mutex_lock(&data->update_lock);
		bytes_2_send = i2c_master_send(piadagio_fp_i2c_client, &piadagio_fp_buffer_i2c_rw[0], I2C_MSG_LEN_UPDATE_LCD);
		mutex_unlock(&data->update_lock);
		if (bytes_2_send == I2C_MSG_LEN_UPDATE_LCD) {
			//printd("%s: Updated screen.\n", __FUNCTION__);
			if (piadagio_fp_i2c_update_screen_other_half) {
				piadagio_fp_i2c_update_screen_other_half = false;
			} else {
				piadagio_fp_i2c_update_screen_other_half = true;
			}
			return 0;
		} else {
			printe("%s: Failed to write screen update.\n", __FUNCTION__);
			return -1;
		}
	}

	printe("%s: Failed to prepare write buffer.\n", __FUNCTION__);
	return -1;
}

// Updates a CGRAM glyph
int piadagio_fp_i2c_update_glyph(unsigned char glyph_index) {
	struct piadagio_fp_data *data = i2c_get_clientdata(piadagio_fp_i2c_client);
	unsigned int bytes_2_send;
	unsigned char tmp_i2c_buffer[I2C_MSG_LEN_UPDATE_CGRAM + 1];

	//printd("%s\n", __FUNCTION__);

	bytes_2_send = snprintf(&tmp_i2c_buffer[0], (I2C_MSG_LEN_UPDATE_CGRAM + 1),	// Buffer size includes null character from sprintf
				"%c%c%c%c%c%c%c%c%c%c%c",				// Format: length + cmd + glyph index + glyph bytes[8]
				I2C_MSG_LEN_UPDATE_CGRAM - 1,				// Message length, not including this byte
				I2C_MSG_TYPE_GLYPH,					// Glyph update cmd
				glyph_index,
				piadagio_fp_buffer_lcd_ugram.glyph[glyph_index].pixel_line[0],
				piadagio_fp_buffer_lcd_ugram.glyph[glyph_index].pixel_line[1],
				piadagio_fp_buffer_lcd_ugram.glyph[glyph_index].pixel_line[2],
				piadagio_fp_buffer_lcd_ugram.glyph[glyph_index].pixel_line[3],
				piadagio_fp_buffer_lcd_ugram.glyph[glyph_index].pixel_line[4],
				piadagio_fp_buffer_lcd_ugram.glyph[glyph_index].pixel_line[5],
				piadagio_fp_buffer_lcd_ugram.glyph[glyph_index].pixel_line[6],
				piadagio_fp_buffer_lcd_ugram.glyph[glyph_index].pixel_line[7]);
	if (bytes_2_send == I2C_MSG_LEN_UPDATE_CGRAM) {
		mutex_lock(&data->update_lock);
		bytes_2_send = i2c_master_send(piadagio_fp_i2c_client, &tmp_i2c_buffer[0], I2C_MSG_LEN_UPDATE_CGRAM);
		mutex_unlock(&data->update_lock);
		if (bytes_2_send == I2C_MSG_LEN_UPDATE_CGRAM) {
			//printd("%s: Updated glyph.\n", __FUNCTION__);
			return 0;
		} else {
			printe("%s: Failed to write glyph update.\n", __FUNCTION__);
			return -1;
		}
	}

	printe("%s: Failed to prepare write buffer.\n", __FUNCTION__);
	return -1;
}

// Updates the state of the FP LEDs
int piadagio_fp_i2c_update_leds() {
	struct piadagio_fp_data *data = i2c_get_clientdata(piadagio_fp_i2c_client);
	unsigned int bytes_2_send;
	char tmp_i2c_buffer[I2C_MSG_LEN_UPDATE_LED + 1], tmp_led_status = 0;

	//printd("%s\n", __FUNCTION__);

	if (piadagio_fp_led_online > 0) {
		tmp_led_status = tmp_led_status | 2;
	}
	if (piadagio_fp_led_power > 0) {
		tmp_led_status = tmp_led_status | 1;
	}

	bytes_2_send = snprintf(&tmp_i2c_buffer[0], (I2C_MSG_LEN_UPDATE_LED + 1),	// Buffer size includes null character from sprintf
				"%c%c%c",						// Format: length + cmd + led status
				I2C_MSG_LEN_UPDATE_LED - 1,				// Message length, not including this byte
				I2C_MSG_TYPE_LED,					// LED update cmd
				tmp_led_status);					// LED status bits
	if (bytes_2_send == I2C_MSG_LEN_UPDATE_LED) {
		mutex_lock(&data->update_lock);
		bytes_2_send = i2c_master_send(piadagio_fp_i2c_client, &tmp_i2c_buffer[0], I2C_MSG_LEN_UPDATE_LED);
		mutex_unlock(&data->update_lock);
		if (bytes_2_send == I2C_MSG_LEN_UPDATE_LED) {
			//printd("%s: Updated LEDs.\n", __FUNCTION__);
			return 0;
		} else {
			printe("%s: Failed to write LED update.\n", __FUNCTION__);
			return -1;
		}
	}

	printe("%s: Failed to prepare write buffer.\n", __FUNCTION__);
	return -1;
}

/////////////////////////////////////////////////////////////////////
// Workqueue routines
/////////////////////////////////////////////////////////////////////
// Task to periodically update the lcd screen from the buffer
static void piadagio_fp_task_lcd_update(struct work_struct *work) {
	bool update_screen = true;
	int fp_status, i;
	short task_delay = 10;

	//printd("%s\n", __FUNCTION__);

	if (piadagio_fp_i2c_update_do > 0) {						// Check whether to run an update
		fp_status = piadagio_fp_i2c_get_status();				// Check the FP status,

		if (fp_status >= 0) {
			if (fp_status < 2) {						// Is it ready for another command?
				for (i = 0; i < 8; i++) {				// Check if the glyphs need updating
					if (piadagio_fp_glyph_updated[i]) {
						fp_status = piadagio_fp_i2c_update_glyph(i);
						if (fp_status == 0) {			// Did the write succeed?
							piadagio_fp_glyph_updated[i] = false;
							piadagio_fp_i2c_update_glyph_counter++;
						} else {
							piadagio_fp_i2c_update_errors_counter++;
							task_delay = 1;
						}
						update_screen = false;			// Stop screen update as glyph update has to be processed
						break;					// Don't bother checking any others
					}
				}

				// Can we update the screen? Waiting for fsync?
				if (update_screen && (piadagio_fp_i2c_update_do_screen > 0)) {
					piadagio_fp_i2c_update_lcd_counter++;		// Debug helper, to know if this rountine is being executed

					fp_status = piadagio_fp_i2c_update_screen();
					if (fp_status == 0) {				// Did the write succeed?
						// Do we writing need to write the second half of the screen?
						if (!piadagio_fp_i2c_update_screen_other_half) {
							task_delay = 10;		// No, so wait (giving a rough refresh of 10Hz)
						} else {
							task_delay = 1;			// Yes, so keep the delay short
						}
					} else {					// Failed write to screen, so reschedule
						piadagio_fp_i2c_update_errors_counter++;
						task_delay = 1;
					}
				} else {
					task_delay = 1;					// Waiting for buffer to be updated, so reschedule
				}
			} else {							// FP processing existing command so reschedule
				piadagio_fp_i2c_update_retries_counter++;
				task_delay = 1;
			}
		} else {								// Error reading, schedule another check
			piadagio_fp_i2c_update_errors_counter++;
			task_delay = 1;
		}
	}

	if (piadagio_fp_wq_kill == 0) {
		queue_delayed_work(piadagio_fp_wq, &piadagio_fp_wq_task_lcd, task_delay);
	}
}

// Task to periodically update the panel leds
static void piadagio_fp_task_led_update(struct work_struct *work) {
	int fp_status;
	short task_delay = 50;

	//printd("%s\n", __FUNCTION__);

	if (piadagio_fp_i2c_update_do > 0) {					// Check whether to run an update
		piadagio_fp_i2c_update_led_counter++;				// Debug helper, to know if this rountine is being executed
		fp_status = piadagio_fp_i2c_get_status();			// Check the FP status,

		if (fp_status >= 0) {
			if (fp_status < 2) {					// Is it ready for another command?
				fp_status = piadagio_fp_i2c_update_leds();
				if (fp_status == 0) {				// Did the write succeed?
					task_delay = 50;
				} else {					// Failed write to LEDs, so reschedule
					piadagio_fp_i2c_update_errors_counter++;
					task_delay = 1;
				}
			} else {						// FP processing existing command so reschedule
				piadagio_fp_i2c_update_retries_counter++;
				task_delay = 1;
			}
		} else {							// Error reading, schedule another check
			piadagio_fp_i2c_update_errors_counter++;
			task_delay = 1;
		}
	}

	if (piadagio_fp_wq_kill == 0) {
		queue_delayed_work(piadagio_fp_wq, &piadagio_fp_wq_task_led, task_delay);
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
	if (piadagio_fp_i2c_client == NULL) {
		return -ENODEV;
	}

	piadagio_fp_buffer_index_ptr = piadagio_fp_buffer_lcd_screen.line1;		// Reset screen buffer pointer
	piadagio_fp_glyph_index_ptr = piadagio_fp_buffer_lcd_ugram.glyph[0].pixel_line;	// Reset UGRAM buffer pointer
	piadagio_fp_write_to_buffer = BUFFER_WRITE_CHAR;				// Reset to writing character buffer
	return 0;
}

// Called when the device file pointer is closed
static int piadagio_fp_release(struct inode * inode, struct file * fp) {
	printd("%s: Freeing /dev resource\n", __FUNCTION__);

	mutex_unlock(&piadagio_fp_mutex);
	return 0;
}

// Read from the device
static ssize_t piadagio_fp_read(struct file *filp,			/* see include/linux/fs.h   */
				char __user *buffer,			/* buffer to fill with data */
				size_t length,				/* length of the buffer     */
				loff_t * offset) {
	printd("%s\n", __FUNCTION__);

	// We're just interested in any commands read from the FP
	if (copy_to_user(buffer, &piadagio_fp_buffer_command, 1) == 0) {
		return 1;
	}

	return -EFAULT;
}

// Write to the lcd screen/glyph buffer
static ssize_t piadagio_fp_write(struct file * fp, const char __user * buffer, size_t count, loff_t * offset) {
	int num_write = 0, tmp_glyph_index = 0;

	printd("%s: Write operation with [%d] bytes, from offset [%lld]\n", __FUNCTION__, count, ((long long int) *offset));

	if (piadagio_fp_write_to_buffer == BUFFER_WRITE_CHAR) {
		// Iterate through the user space buffer
		while (count) {
			if (copy_from_user(piadagio_fp_buffer_index_ptr, (buffer + num_write), 1)) {
				return -EFAULT;
			}

			num_write++;
			count--;
			piadagio_fp_buffer_index_ptr++;
			// Reset the screen buffer pointer, if we have overrun the end
			if (piadagio_fp_buffer_index_ptr >= (piadagio_fp_buffer_lcd_screen.line1 + SCREEN_BUFFER_LEN)) {
				piadagio_fp_buffer_index_ptr = piadagio_fp_buffer_lcd_screen.line1;
			}
		}

		if (fp_require_fsync) {
			piadagio_fp_i2c_update_do_screen = 0;
		}
	} else if (piadagio_fp_write_to_buffer == BUFFER_WRITE_GLYPH) {
		// Iterate through the user space buffer
		while (count) {
			if (copy_from_user(piadagio_fp_glyph_index_ptr, (buffer + num_write), 1)) {
				return -EFAULT;
			}

			tmp_glyph_index = (piadagio_fp_glyph_index_ptr - piadagio_fp_buffer_lcd_ugram.glyph[0].pixel_line) / 8;
			piadagio_fp_glyph_updated[tmp_glyph_index] = true;

			num_write++;
			count--;
			piadagio_fp_glyph_index_ptr++;
			// Reset the glyph buffer pointer, if we have overrun the end
			if (piadagio_fp_glyph_index_ptr >= (piadagio_fp_buffer_lcd_ugram.glyph[0].pixel_line + GLYPH_BUFFER_LEN)) {
				piadagio_fp_glyph_index_ptr = piadagio_fp_buffer_lcd_ugram.glyph[0].pixel_line;
			}
		}
	}

	return num_write;
}

// Basic implmentation of llseek, only seeks from the start
static loff_t piadagio_fp_llseek(struct file *file, loff_t offset, int origin) {
	printd("%s: llseek to offset [%llu]\n", __FUNCTION__, ((long long int) offset));

	if (origin != SEEK_SET) {
		printd("%s: llseek by origin [%u], not allowed.\n", __FUNCTION__, origin);
		return -EFAULT;
	}

	if (offset < SCREEN_BUFFER_LEN) {
		piadagio_fp_buffer_index_ptr = piadagio_fp_buffer_lcd_screen.line1;
		piadagio_fp_buffer_index_ptr += offset;

		piadagio_fp_write_to_buffer = BUFFER_WRITE_CHAR;
	} else if ((offset >= 128) && (offset <= (128 + (GLYPH_BUFFER_LEN - 1)))) {
		piadagio_fp_glyph_index_ptr = piadagio_fp_buffer_lcd_ugram.glyph[0].pixel_line;
		piadagio_fp_glyph_index_ptr += (offset - 128);

		piadagio_fp_write_to_buffer = BUFFER_WRITE_GLYPH;
	} else {
		return -EFAULT;
	}

	return offset;
}

// This allows the screen buffer to be flushed to the FP
static int piadagio_fp_fsync(struct file *file, loff_t start, loff_t end, int datasync) {
	piadagio_fp_i2c_update_do_screen = 1;
	return 0;
}

static struct file_operations piadagio_fp_fops = {
	.owner = THIS_MODULE,
	.read = piadagio_fp_read,
	.write = piadagio_fp_write,
	.llseek = piadagio_fp_llseek,
	.fsync = piadagio_fp_fsync,
	.open = piadagio_fp_open,
	.release = piadagio_fp_release
};

////////////////////////////////////////////////////////////////////
// SysFS
////////////////////////////////////////////////////////////////////
// SysFS object to display current command
static ssize_t piadagio_fp_get_command(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "FP Command: 0x%x (None)\n", piadagio_fp_buffer_command);
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

// SysFS object to display update counter
static ssize_t piadagio_fp_get_stats(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Update counter (LCD): %lu\nUpdate counter (Glyph): %lu\nUpdate counter (LED): %lu\nUpdate retries counter: %lu\nUpdate error counter: %lu\n",
			piadagio_fp_i2c_update_lcd_counter,
			piadagio_fp_i2c_update_glyph_counter,
			piadagio_fp_i2c_update_led_counter,
			piadagio_fp_i2c_update_retries_counter,
			piadagio_fp_i2c_update_errors_counter);
}

// SysFS object to display whether the update is enabled
static ssize_t piadagio_fp_get_do_update(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Update enabled: %u\n", piadagio_fp_i2c_update_do);
}

// SysFS object to set whether the update is enabled
static ssize_t piadagio_fp_set_do_update(struct device *dev, struct device_attribute * devattr, const char * buf, size_t count) {
	int value, err;
	printd("%s\n", __FUNCTION__);
	err = kstrtoint(buf, 10, &value);
	if (err < 0) {
		return err;
	} else {
		piadagio_fp_i2c_update_do = value;
	}
	return count;
}

// SysFS object to display whether the screen update is enabled
static ssize_t piadagio_fp_get_do_update_screen(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Update enabled: %u\n", piadagio_fp_i2c_update_do_screen);
}

// SysFS object to set whether the screen update is enabled
static ssize_t piadagio_fp_set_do_update_screen(struct device *dev, struct device_attribute * devattr, const char * buf, size_t count) {
	int value, err;
	printd("%s\n", __FUNCTION__);
	err = kstrtoint(buf, 10, &value);
	if (err < 0) {
		return err;
	} else {
		piadagio_fp_i2c_update_do_screen = value;
	}
	return count;
}

// SysFS object to display the i2c buffer
static ssize_t piadagio_fp_get_i2c_buffer(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	int i, tmp_index;
	printd("%s\n", __FUNCTION__);
	// Convert the i2c buffer to hex
	tmp_index = 0;
	for (i = 0; i < I2C_MSG_LEN_UPDATE_LCD; i++) {
		if (i % 16 == 0) {
			if (i > 0) {
				tmp_index += sprintf((buf + tmp_index), "\n");
			}
			tmp_index += sprintf((buf + tmp_index), "0x%u0: ", (i / 16));
		}
		tmp_index += sprintf((buf + tmp_index), "0x%02x ", piadagio_fp_buffer_i2c_rw[i]);
	}
	return tmp_index;
}

// SysFS object to display UGRAM glyph 0
static ssize_t piadagio_fp_get_ugram_glyph0(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	struct piadagio_fp_glyph tmp_glyph = piadagio_fp_buffer_lcd_ugram.glyph[0];

	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Glyph 0:\nUpdated: %u\n" GLYPH_PRINT, piadagio_fp_glyph_updated[0],
				((tmp_glyph.pixel_line[0] & 16) > 0), ((tmp_glyph.pixel_line[0] & 8) > 0), ((tmp_glyph.pixel_line[0] & 4) > 0), ((tmp_glyph.pixel_line[0] & 2) > 0), ((tmp_glyph.pixel_line[0] & 1) > 0), tmp_glyph.pixel_line[0],
				((tmp_glyph.pixel_line[1] & 16) > 0), ((tmp_glyph.pixel_line[1] & 8) > 0), ((tmp_glyph.pixel_line[1] & 4) > 0), ((tmp_glyph.pixel_line[1] & 2) > 0), ((tmp_glyph.pixel_line[1] & 1) > 0), tmp_glyph.pixel_line[1],
				((tmp_glyph.pixel_line[2] & 16) > 0), ((tmp_glyph.pixel_line[2] & 8) > 0), ((tmp_glyph.pixel_line[2] & 4) > 0), ((tmp_glyph.pixel_line[2] & 2) > 0), ((tmp_glyph.pixel_line[2] & 1) > 0), tmp_glyph.pixel_line[2],
				((tmp_glyph.pixel_line[3] & 16) > 0), ((tmp_glyph.pixel_line[3] & 8) > 0), ((tmp_glyph.pixel_line[3] & 4) > 0), ((tmp_glyph.pixel_line[3] & 2) > 0), ((tmp_glyph.pixel_line[3] & 1) > 0), tmp_glyph.pixel_line[3],
				((tmp_glyph.pixel_line[4] & 16) > 0), ((tmp_glyph.pixel_line[4] & 8) > 0), ((tmp_glyph.pixel_line[4] & 4) > 0), ((tmp_glyph.pixel_line[4] & 2) > 0), ((tmp_glyph.pixel_line[4] & 1) > 0), tmp_glyph.pixel_line[4],
				((tmp_glyph.pixel_line[5] & 16) > 0), ((tmp_glyph.pixel_line[5] & 8) > 0), ((tmp_glyph.pixel_line[5] & 4) > 0), ((tmp_glyph.pixel_line[5] & 2) > 0), ((tmp_glyph.pixel_line[5] & 1) > 0), tmp_glyph.pixel_line[5],
				((tmp_glyph.pixel_line[6] & 16) > 0), ((tmp_glyph.pixel_line[6] & 8) > 0), ((tmp_glyph.pixel_line[6] & 4) > 0), ((tmp_glyph.pixel_line[6] & 2) > 0), ((tmp_glyph.pixel_line[6] & 1) > 0), tmp_glyph.pixel_line[6],
				((tmp_glyph.pixel_line[7] & 16) > 0), ((tmp_glyph.pixel_line[7] & 8) > 0), ((tmp_glyph.pixel_line[7] & 4) > 0), ((tmp_glyph.pixel_line[7] & 2) > 0), ((tmp_glyph.pixel_line[7] & 1) > 0), tmp_glyph.pixel_line[7]);
}

// SysFS object to display UGRAM glyph 1
static ssize_t piadagio_fp_get_ugram_glyph1(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	struct piadagio_fp_glyph tmp_glyph = piadagio_fp_buffer_lcd_ugram.glyph[1];

	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Glyph 1:\nUpdated: %u\n" GLYPH_PRINT, piadagio_fp_glyph_updated[1],
				((tmp_glyph.pixel_line[0] & 16) > 0), ((tmp_glyph.pixel_line[0] & 8) > 0), ((tmp_glyph.pixel_line[0] & 4) > 0), ((tmp_glyph.pixel_line[0] & 2) > 0), ((tmp_glyph.pixel_line[0] & 1) > 0), tmp_glyph.pixel_line[0],
				((tmp_glyph.pixel_line[1] & 16) > 0), ((tmp_glyph.pixel_line[1] & 8) > 0), ((tmp_glyph.pixel_line[1] & 4) > 0), ((tmp_glyph.pixel_line[1] & 2) > 0), ((tmp_glyph.pixel_line[1] & 1) > 0), tmp_glyph.pixel_line[1],
				((tmp_glyph.pixel_line[2] & 16) > 0), ((tmp_glyph.pixel_line[2] & 8) > 0), ((tmp_glyph.pixel_line[2] & 4) > 0), ((tmp_glyph.pixel_line[2] & 2) > 0), ((tmp_glyph.pixel_line[2] & 1) > 0), tmp_glyph.pixel_line[2],
				((tmp_glyph.pixel_line[3] & 16) > 0), ((tmp_glyph.pixel_line[3] & 8) > 0), ((tmp_glyph.pixel_line[3] & 4) > 0), ((tmp_glyph.pixel_line[3] & 2) > 0), ((tmp_glyph.pixel_line[3] & 1) > 0), tmp_glyph.pixel_line[3],
				((tmp_glyph.pixel_line[4] & 16) > 0), ((tmp_glyph.pixel_line[4] & 8) > 0), ((tmp_glyph.pixel_line[4] & 4) > 0), ((tmp_glyph.pixel_line[4] & 2) > 0), ((tmp_glyph.pixel_line[4] & 1) > 0), tmp_glyph.pixel_line[4],
				((tmp_glyph.pixel_line[5] & 16) > 0), ((tmp_glyph.pixel_line[5] & 8) > 0), ((tmp_glyph.pixel_line[5] & 4) > 0), ((tmp_glyph.pixel_line[5] & 2) > 0), ((tmp_glyph.pixel_line[5] & 1) > 0), tmp_glyph.pixel_line[5],
				((tmp_glyph.pixel_line[6] & 16) > 0), ((tmp_glyph.pixel_line[6] & 8) > 0), ((tmp_glyph.pixel_line[6] & 4) > 0), ((tmp_glyph.pixel_line[6] & 2) > 0), ((tmp_glyph.pixel_line[6] & 1) > 0), tmp_glyph.pixel_line[6],
				((tmp_glyph.pixel_line[7] & 16) > 0), ((tmp_glyph.pixel_line[7] & 8) > 0), ((tmp_glyph.pixel_line[7] & 4) > 0), ((tmp_glyph.pixel_line[7] & 2) > 0), ((tmp_glyph.pixel_line[7] & 1) > 0), tmp_glyph.pixel_line[7]);
}

// SysFS object to display UGRAM glyph 2
static ssize_t piadagio_fp_get_ugram_glyph2(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	struct piadagio_fp_glyph tmp_glyph = piadagio_fp_buffer_lcd_ugram.glyph[2];

	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Glyph 2:\nUpdated: %u\n" GLYPH_PRINT, piadagio_fp_glyph_updated[2],
				((tmp_glyph.pixel_line[0] & 16) > 0), ((tmp_glyph.pixel_line[0] & 8) > 0), ((tmp_glyph.pixel_line[0] & 4) > 0), ((tmp_glyph.pixel_line[0] & 2) > 0), ((tmp_glyph.pixel_line[0] & 1) > 0), tmp_glyph.pixel_line[0],
				((tmp_glyph.pixel_line[1] & 16) > 0), ((tmp_glyph.pixel_line[1] & 8) > 0), ((tmp_glyph.pixel_line[1] & 4) > 0), ((tmp_glyph.pixel_line[1] & 2) > 0), ((tmp_glyph.pixel_line[1] & 1) > 0), tmp_glyph.pixel_line[1],
				((tmp_glyph.pixel_line[2] & 16) > 0), ((tmp_glyph.pixel_line[2] & 8) > 0), ((tmp_glyph.pixel_line[2] & 4) > 0), ((tmp_glyph.pixel_line[2] & 2) > 0), ((tmp_glyph.pixel_line[2] & 1) > 0), tmp_glyph.pixel_line[2],
				((tmp_glyph.pixel_line[3] & 16) > 0), ((tmp_glyph.pixel_line[3] & 8) > 0), ((tmp_glyph.pixel_line[3] & 4) > 0), ((tmp_glyph.pixel_line[3] & 2) > 0), ((tmp_glyph.pixel_line[3] & 1) > 0), tmp_glyph.pixel_line[3],
				((tmp_glyph.pixel_line[4] & 16) > 0), ((tmp_glyph.pixel_line[4] & 8) > 0), ((tmp_glyph.pixel_line[4] & 4) > 0), ((tmp_glyph.pixel_line[4] & 2) > 0), ((tmp_glyph.pixel_line[4] & 1) > 0), tmp_glyph.pixel_line[4],
				((tmp_glyph.pixel_line[5] & 16) > 0), ((tmp_glyph.pixel_line[5] & 8) > 0), ((tmp_glyph.pixel_line[5] & 4) > 0), ((tmp_glyph.pixel_line[5] & 2) > 0), ((tmp_glyph.pixel_line[5] & 1) > 0), tmp_glyph.pixel_line[5],
				((tmp_glyph.pixel_line[6] & 16) > 0), ((tmp_glyph.pixel_line[6] & 8) > 0), ((tmp_glyph.pixel_line[6] & 4) > 0), ((tmp_glyph.pixel_line[6] & 2) > 0), ((tmp_glyph.pixel_line[6] & 1) > 0), tmp_glyph.pixel_line[6],
				((tmp_glyph.pixel_line[7] & 16) > 0), ((tmp_glyph.pixel_line[7] & 8) > 0), ((tmp_glyph.pixel_line[7] & 4) > 0), ((tmp_glyph.pixel_line[7] & 2) > 0), ((tmp_glyph.pixel_line[7] & 1) > 0), tmp_glyph.pixel_line[7]);
}

// SysFS object to display UGRAM glyph 3
static ssize_t piadagio_fp_get_ugram_glyph3(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	struct piadagio_fp_glyph tmp_glyph = piadagio_fp_buffer_lcd_ugram.glyph[3];

	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Glyph 3:\nUpdated: %u\n" GLYPH_PRINT, piadagio_fp_glyph_updated[3],
				((tmp_glyph.pixel_line[0] & 16) > 0), ((tmp_glyph.pixel_line[0] & 8) > 0), ((tmp_glyph.pixel_line[0] & 4) > 0), ((tmp_glyph.pixel_line[0] & 2) > 0), ((tmp_glyph.pixel_line[0] & 1) > 0), tmp_glyph.pixel_line[0],
				((tmp_glyph.pixel_line[1] & 16) > 0), ((tmp_glyph.pixel_line[1] & 8) > 0), ((tmp_glyph.pixel_line[1] & 4) > 0), ((tmp_glyph.pixel_line[1] & 2) > 0), ((tmp_glyph.pixel_line[1] & 1) > 0), tmp_glyph.pixel_line[1],
				((tmp_glyph.pixel_line[2] & 16) > 0), ((tmp_glyph.pixel_line[2] & 8) > 0), ((tmp_glyph.pixel_line[2] & 4) > 0), ((tmp_glyph.pixel_line[2] & 2) > 0), ((tmp_glyph.pixel_line[2] & 1) > 0), tmp_glyph.pixel_line[2],
				((tmp_glyph.pixel_line[3] & 16) > 0), ((tmp_glyph.pixel_line[3] & 8) > 0), ((tmp_glyph.pixel_line[3] & 4) > 0), ((tmp_glyph.pixel_line[3] & 2) > 0), ((tmp_glyph.pixel_line[3] & 1) > 0), tmp_glyph.pixel_line[3],
				((tmp_glyph.pixel_line[4] & 16) > 0), ((tmp_glyph.pixel_line[4] & 8) > 0), ((tmp_glyph.pixel_line[4] & 4) > 0), ((tmp_glyph.pixel_line[4] & 2) > 0), ((tmp_glyph.pixel_line[4] & 1) > 0), tmp_glyph.pixel_line[4],
				((tmp_glyph.pixel_line[5] & 16) > 0), ((tmp_glyph.pixel_line[5] & 8) > 0), ((tmp_glyph.pixel_line[5] & 4) > 0), ((tmp_glyph.pixel_line[5] & 2) > 0), ((tmp_glyph.pixel_line[5] & 1) > 0), tmp_glyph.pixel_line[5],
				((tmp_glyph.pixel_line[6] & 16) > 0), ((tmp_glyph.pixel_line[6] & 8) > 0), ((tmp_glyph.pixel_line[6] & 4) > 0), ((tmp_glyph.pixel_line[6] & 2) > 0), ((tmp_glyph.pixel_line[6] & 1) > 0), tmp_glyph.pixel_line[6],
				((tmp_glyph.pixel_line[7] & 16) > 0), ((tmp_glyph.pixel_line[7] & 8) > 0), ((tmp_glyph.pixel_line[7] & 4) > 0), ((tmp_glyph.pixel_line[7] & 2) > 0), ((tmp_glyph.pixel_line[7] & 1) > 0), tmp_glyph.pixel_line[7]);
}

// SysFS object to display UGRAM glyph 4
static ssize_t piadagio_fp_get_ugram_glyph4(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	struct piadagio_fp_glyph tmp_glyph = piadagio_fp_buffer_lcd_ugram.glyph[4];

	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Glyph 4:\nUpdated: %u\n" GLYPH_PRINT, piadagio_fp_glyph_updated[4],
				((tmp_glyph.pixel_line[0] & 16) > 0), ((tmp_glyph.pixel_line[0] & 8) > 0), ((tmp_glyph.pixel_line[0] & 4) > 0), ((tmp_glyph.pixel_line[0] & 2) > 0), ((tmp_glyph.pixel_line[0] & 1) > 0), tmp_glyph.pixel_line[0],
				((tmp_glyph.pixel_line[1] & 16) > 0), ((tmp_glyph.pixel_line[1] & 8) > 0), ((tmp_glyph.pixel_line[1] & 4) > 0), ((tmp_glyph.pixel_line[1] & 2) > 0), ((tmp_glyph.pixel_line[1] & 1) > 0), tmp_glyph.pixel_line[1],
				((tmp_glyph.pixel_line[2] & 16) > 0), ((tmp_glyph.pixel_line[2] & 8) > 0), ((tmp_glyph.pixel_line[2] & 4) > 0), ((tmp_glyph.pixel_line[2] & 2) > 0), ((tmp_glyph.pixel_line[2] & 1) > 0), tmp_glyph.pixel_line[2],
				((tmp_glyph.pixel_line[3] & 16) > 0), ((tmp_glyph.pixel_line[3] & 8) > 0), ((tmp_glyph.pixel_line[3] & 4) > 0), ((tmp_glyph.pixel_line[3] & 2) > 0), ((tmp_glyph.pixel_line[3] & 1) > 0), tmp_glyph.pixel_line[3],
				((tmp_glyph.pixel_line[4] & 16) > 0), ((tmp_glyph.pixel_line[4] & 8) > 0), ((tmp_glyph.pixel_line[4] & 4) > 0), ((tmp_glyph.pixel_line[4] & 2) > 0), ((tmp_glyph.pixel_line[4] & 1) > 0), tmp_glyph.pixel_line[4],
				((tmp_glyph.pixel_line[5] & 16) > 0), ((tmp_glyph.pixel_line[5] & 8) > 0), ((tmp_glyph.pixel_line[5] & 4) > 0), ((tmp_glyph.pixel_line[5] & 2) > 0), ((tmp_glyph.pixel_line[5] & 1) > 0), tmp_glyph.pixel_line[5],
				((tmp_glyph.pixel_line[6] & 16) > 0), ((tmp_glyph.pixel_line[6] & 8) > 0), ((tmp_glyph.pixel_line[6] & 4) > 0), ((tmp_glyph.pixel_line[6] & 2) > 0), ((tmp_glyph.pixel_line[6] & 1) > 0), tmp_glyph.pixel_line[6],
				((tmp_glyph.pixel_line[7] & 16) > 0), ((tmp_glyph.pixel_line[7] & 8) > 0), ((tmp_glyph.pixel_line[7] & 4) > 0), ((tmp_glyph.pixel_line[7] & 2) > 0), ((tmp_glyph.pixel_line[7] & 1) > 0), tmp_glyph.pixel_line[7]);
}

// SysFS object to display UGRAM glyph 5
static ssize_t piadagio_fp_get_ugram_glyph5(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	struct piadagio_fp_glyph tmp_glyph = piadagio_fp_buffer_lcd_ugram.glyph[5];

	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Glyph 5:\nUpdated: %u\n" GLYPH_PRINT, piadagio_fp_glyph_updated[5],
				((tmp_glyph.pixel_line[0] & 16) > 0), ((tmp_glyph.pixel_line[0] & 8) > 0), ((tmp_glyph.pixel_line[0] & 4) > 0), ((tmp_glyph.pixel_line[0] & 2) > 0), ((tmp_glyph.pixel_line[0] & 1) > 0), tmp_glyph.pixel_line[0],
				((tmp_glyph.pixel_line[1] & 16) > 0), ((tmp_glyph.pixel_line[1] & 8) > 0), ((tmp_glyph.pixel_line[1] & 4) > 0), ((tmp_glyph.pixel_line[1] & 2) > 0), ((tmp_glyph.pixel_line[1] & 1) > 0), tmp_glyph.pixel_line[1],
				((tmp_glyph.pixel_line[2] & 16) > 0), ((tmp_glyph.pixel_line[2] & 8) > 0), ((tmp_glyph.pixel_line[2] & 4) > 0), ((tmp_glyph.pixel_line[2] & 2) > 0), ((tmp_glyph.pixel_line[2] & 1) > 0), tmp_glyph.pixel_line[2],
				((tmp_glyph.pixel_line[3] & 16) > 0), ((tmp_glyph.pixel_line[3] & 8) > 0), ((tmp_glyph.pixel_line[3] & 4) > 0), ((tmp_glyph.pixel_line[3] & 2) > 0), ((tmp_glyph.pixel_line[3] & 1) > 0), tmp_glyph.pixel_line[3],
				((tmp_glyph.pixel_line[4] & 16) > 0), ((tmp_glyph.pixel_line[4] & 8) > 0), ((tmp_glyph.pixel_line[4] & 4) > 0), ((tmp_glyph.pixel_line[4] & 2) > 0), ((tmp_glyph.pixel_line[4] & 1) > 0), tmp_glyph.pixel_line[4],
				((tmp_glyph.pixel_line[5] & 16) > 0), ((tmp_glyph.pixel_line[5] & 8) > 0), ((tmp_glyph.pixel_line[5] & 4) > 0), ((tmp_glyph.pixel_line[5] & 2) > 0), ((tmp_glyph.pixel_line[5] & 1) > 0), tmp_glyph.pixel_line[5],
				((tmp_glyph.pixel_line[6] & 16) > 0), ((tmp_glyph.pixel_line[6] & 8) > 0), ((tmp_glyph.pixel_line[6] & 4) > 0), ((tmp_glyph.pixel_line[6] & 2) > 0), ((tmp_glyph.pixel_line[6] & 1) > 0), tmp_glyph.pixel_line[6],
				((tmp_glyph.pixel_line[7] & 16) > 0), ((tmp_glyph.pixel_line[7] & 8) > 0), ((tmp_glyph.pixel_line[7] & 4) > 0), ((tmp_glyph.pixel_line[7] & 2) > 0), ((tmp_glyph.pixel_line[7] & 1) > 0), tmp_glyph.pixel_line[7]);
}

// SysFS object to display UGRAM glyph 6
static ssize_t piadagio_fp_get_ugram_glyph6(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	struct piadagio_fp_glyph tmp_glyph = piadagio_fp_buffer_lcd_ugram.glyph[6];

	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Glyph 6:\nUpdated: %u\n" GLYPH_PRINT, piadagio_fp_glyph_updated[6],
				((tmp_glyph.pixel_line[0] & 16) > 0), ((tmp_glyph.pixel_line[0] & 8) > 0), ((tmp_glyph.pixel_line[0] & 4) > 0), ((tmp_glyph.pixel_line[0] & 2) > 0), ((tmp_glyph.pixel_line[0] & 1) > 0), tmp_glyph.pixel_line[0],
				((tmp_glyph.pixel_line[1] & 16) > 0), ((tmp_glyph.pixel_line[1] & 8) > 0), ((tmp_glyph.pixel_line[1] & 4) > 0), ((tmp_glyph.pixel_line[1] & 2) > 0), ((tmp_glyph.pixel_line[1] & 1) > 0), tmp_glyph.pixel_line[1],
				((tmp_glyph.pixel_line[2] & 16) > 0), ((tmp_glyph.pixel_line[2] & 8) > 0), ((tmp_glyph.pixel_line[2] & 4) > 0), ((tmp_glyph.pixel_line[2] & 2) > 0), ((tmp_glyph.pixel_line[2] & 1) > 0), tmp_glyph.pixel_line[2],
				((tmp_glyph.pixel_line[3] & 16) > 0), ((tmp_glyph.pixel_line[3] & 8) > 0), ((tmp_glyph.pixel_line[3] & 4) > 0), ((tmp_glyph.pixel_line[3] & 2) > 0), ((tmp_glyph.pixel_line[3] & 1) > 0), tmp_glyph.pixel_line[3],
				((tmp_glyph.pixel_line[4] & 16) > 0), ((tmp_glyph.pixel_line[4] & 8) > 0), ((tmp_glyph.pixel_line[4] & 4) > 0), ((tmp_glyph.pixel_line[4] & 2) > 0), ((tmp_glyph.pixel_line[4] & 1) > 0), tmp_glyph.pixel_line[4],
				((tmp_glyph.pixel_line[5] & 16) > 0), ((tmp_glyph.pixel_line[5] & 8) > 0), ((tmp_glyph.pixel_line[5] & 4) > 0), ((tmp_glyph.pixel_line[5] & 2) > 0), ((tmp_glyph.pixel_line[5] & 1) > 0), tmp_glyph.pixel_line[5],
				((tmp_glyph.pixel_line[6] & 16) > 0), ((tmp_glyph.pixel_line[6] & 8) > 0), ((tmp_glyph.pixel_line[6] & 4) > 0), ((tmp_glyph.pixel_line[6] & 2) > 0), ((tmp_glyph.pixel_line[6] & 1) > 0), tmp_glyph.pixel_line[6],
				((tmp_glyph.pixel_line[7] & 16) > 0), ((tmp_glyph.pixel_line[7] & 8) > 0), ((tmp_glyph.pixel_line[7] & 4) > 0), ((tmp_glyph.pixel_line[7] & 2) > 0), ((tmp_glyph.pixel_line[7] & 1) > 0), tmp_glyph.pixel_line[7]);
}

// SysFS object to display UGRAM glyph 7
static ssize_t piadagio_fp_get_ugram_glyph7(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	struct piadagio_fp_glyph tmp_glyph = piadagio_fp_buffer_lcd_ugram.glyph[7];

	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Glyph 7:\nUpdated: %u\n" GLYPH_PRINT, piadagio_fp_glyph_updated[7],
				((tmp_glyph.pixel_line[0] & 16) > 0), ((tmp_glyph.pixel_line[0] & 8) > 0), ((tmp_glyph.pixel_line[0] & 4) > 0), ((tmp_glyph.pixel_line[0] & 2) > 0), ((tmp_glyph.pixel_line[0] & 1) > 0), tmp_glyph.pixel_line[0],
				((tmp_glyph.pixel_line[1] & 16) > 0), ((tmp_glyph.pixel_line[1] & 8) > 0), ((tmp_glyph.pixel_line[1] & 4) > 0), ((tmp_glyph.pixel_line[1] & 2) > 0), ((tmp_glyph.pixel_line[1] & 1) > 0), tmp_glyph.pixel_line[1],
				((tmp_glyph.pixel_line[2] & 16) > 0), ((tmp_glyph.pixel_line[2] & 8) > 0), ((tmp_glyph.pixel_line[2] & 4) > 0), ((tmp_glyph.pixel_line[2] & 2) > 0), ((tmp_glyph.pixel_line[2] & 1) > 0), tmp_glyph.pixel_line[2],
				((tmp_glyph.pixel_line[3] & 16) > 0), ((tmp_glyph.pixel_line[3] & 8) > 0), ((tmp_glyph.pixel_line[3] & 4) > 0), ((tmp_glyph.pixel_line[3] & 2) > 0), ((tmp_glyph.pixel_line[3] & 1) > 0), tmp_glyph.pixel_line[3],
				((tmp_glyph.pixel_line[4] & 16) > 0), ((tmp_glyph.pixel_line[4] & 8) > 0), ((tmp_glyph.pixel_line[4] & 4) > 0), ((tmp_glyph.pixel_line[4] & 2) > 0), ((tmp_glyph.pixel_line[4] & 1) > 0), tmp_glyph.pixel_line[4],
				((tmp_glyph.pixel_line[5] & 16) > 0), ((tmp_glyph.pixel_line[5] & 8) > 0), ((tmp_glyph.pixel_line[5] & 4) > 0), ((tmp_glyph.pixel_line[5] & 2) > 0), ((tmp_glyph.pixel_line[5] & 1) > 0), tmp_glyph.pixel_line[5],
				((tmp_glyph.pixel_line[6] & 16) > 0), ((tmp_glyph.pixel_line[6] & 8) > 0), ((tmp_glyph.pixel_line[6] & 4) > 0), ((tmp_glyph.pixel_line[6] & 2) > 0), ((tmp_glyph.pixel_line[6] & 1) > 0), tmp_glyph.pixel_line[6],
				((tmp_glyph.pixel_line[7] & 16) > 0), ((tmp_glyph.pixel_line[7] & 8) > 0), ((tmp_glyph.pixel_line[7] & 4) > 0), ((tmp_glyph.pixel_line[7] & 2) > 0), ((tmp_glyph.pixel_line[7] & 1) > 0), tmp_glyph.pixel_line[7]);
}

// SysFS object to display the online LED status
static ssize_t piadagio_fp_get_led_online(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Online LED: %u\n", piadagio_fp_led_online);
}

// SysFS object to set the online LED status
static ssize_t piadagio_fp_set_led_online(struct device *dev, struct device_attribute * devattr, const char * buf, size_t count) {
	int value, err;
	printd("%s\n", __FUNCTION__);
	err = kstrtoint(buf, 10, &value);
	if (err < 0) {
		return err;
	} else {
		piadagio_fp_led_online = value;
	}
	return count;
}

// SysFS object to display the power LED status
static ssize_t piadagio_fp_get_led_power(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	printd("%s\n", __FUNCTION__);
	// Copy the result back to buf
	return sprintf(buf, "Power LED: %u\n", piadagio_fp_led_power);
}

// SysFS object to set the power LED status
static ssize_t piadagio_fp_set_led_power(struct device *dev, struct device_attribute * devattr, const char * buf, size_t count) {
	int value, err;
	printd("%s\n", __FUNCTION__);
	err = kstrtoint(buf, 10, &value);
	if (err < 0) {
		return err;
	} else {
		piadagio_fp_led_power = value;
	}
	return count;
}

// SysFS object to display the module version
static ssize_t piadagio_fp_get_version(struct device *dev, struct device_attribute *dev_attr, char * buf) {
	unsigned char *tmp_version = PIADAGIOFP_VERSION;
	printd("%s\n", __FUNCTION__);
	return sprintf(buf, "%s\n", tmp_version);
}

static DEVICE_ATTR(fp_command, S_IRUGO, piadagio_fp_get_command, NULL);
static DEVICE_ATTR(fp_lcd_buffer, S_IRUGO, piadagio_fp_get_lcd_buffer, NULL);
static DEVICE_ATTR(fp_stats, S_IRUGO, piadagio_fp_get_stats, NULL);
static DEVICE_ATTR(fp_do_update, 0644, piadagio_fp_get_do_update, piadagio_fp_set_do_update);
static DEVICE_ATTR(fp_do_update_screen, 0644, piadagio_fp_get_do_update_screen, piadagio_fp_set_do_update_screen);
static DEVICE_ATTR(fp_i2c_buffer, S_IRUGO, piadagio_fp_get_i2c_buffer, NULL);
static DEVICE_ATTR(fp_glyph0, S_IRUGO, piadagio_fp_get_ugram_glyph0, NULL);
static DEVICE_ATTR(fp_glyph1, S_IRUGO, piadagio_fp_get_ugram_glyph1, NULL);
static DEVICE_ATTR(fp_glyph2, S_IRUGO, piadagio_fp_get_ugram_glyph2, NULL);
static DEVICE_ATTR(fp_glyph3, S_IRUGO, piadagio_fp_get_ugram_glyph3, NULL);
static DEVICE_ATTR(fp_glyph4, S_IRUGO, piadagio_fp_get_ugram_glyph4, NULL);
static DEVICE_ATTR(fp_glyph5, S_IRUGO, piadagio_fp_get_ugram_glyph5, NULL);
static DEVICE_ATTR(fp_glyph6, S_IRUGO, piadagio_fp_get_ugram_glyph6, NULL);
static DEVICE_ATTR(fp_glyph7, S_IRUGO, piadagio_fp_get_ugram_glyph7, NULL);
static DEVICE_ATTR(fp_led_online, 0644, piadagio_fp_get_led_online, piadagio_fp_set_led_online);
static DEVICE_ATTR(fp_led_power, 0644, piadagio_fp_get_led_power, piadagio_fp_set_led_power);
static DEVICE_ATTR(fp_version, S_IRUGO, piadagio_fp_get_version, NULL);

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
			dev_info(&adapter->dev, "PiAdagio front panel found at 0x%02x\n", address);
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

	piadagio_fp_wq = alloc_ordered_workqueue(PIADAGIOFP_WQ_NAME, 0);
	if(!piadagio_fp_wq) {
		return -ENOMEM;
	}

	/* Allocate the client's data here */
	data = devm_kzalloc(&client->dev, sizeof(struct piadagio_fp_data), GFP_KERNEL);
	if(!data) {
		retval = -ENOMEM;
		goto unreg_wq;
	}

	// Initialize client's data to default
	i2c_set_clientdata(client, data);
	// Initialize the mutex
	mutex_init(&data->update_lock);

	/* If our driver requires additional data initialization
	 * we do it here. For our intents and purposes, we only
	 * set the data->kind which is taken from the i2c_device_id.
	 */
	data->kind = id->driver_data;

	// Store for character driver operations
	piadagio_fp_i2c_client = client;

	// Clear the lcd buffer
	piadagio_fp_buffer_lcd_clear();

	// Initialise the lcd ugram buffer
	piadagio_fp_buffer_ugram_init();

	// We now create our character device driver
	piadagio_fp_major = register_chrdev(0, PIADAGIOFP_I2C_DEVNAME, &piadagio_fp_fops);
	if (piadagio_fp_major < 0) {
		retval = piadagio_fp_major;
		printe("%s: Failed to register char device!\n", __FUNCTION__);
		goto unreg_wq;
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
						PIADAGIOFP_I2C_DEVNAME);
	if (IS_ERR(piadagio_fp_device)) {
		retval = PTR_ERR(piadagio_fp_device);
		printe("%s: Failed to create device!\n", __FUNCTION__);
		goto unreg_class;
	}

	// Initialize the mutex for /dev fops clients
	mutex_init(&piadagio_fp_mutex);

	// We now register our sysfs attributs.
	device_create_file(dev, &dev_attr_fp_command);
	device_create_file(dev, &dev_attr_fp_lcd_buffer);
	device_create_file(dev, &dev_attr_fp_stats);
	device_create_file(dev, &dev_attr_fp_do_update);
	device_create_file(dev, &dev_attr_fp_do_update_screen);
	device_create_file(dev, &dev_attr_fp_i2c_buffer);
	device_create_file(dev, &dev_attr_fp_glyph0);
	device_create_file(dev, &dev_attr_fp_glyph1);
	device_create_file(dev, &dev_attr_fp_glyph2);
	device_create_file(dev, &dev_attr_fp_glyph3);
	device_create_file(dev, &dev_attr_fp_glyph4);
	device_create_file(dev, &dev_attr_fp_glyph5);
	device_create_file(dev, &dev_attr_fp_glyph6);
	device_create_file(dev, &dev_attr_fp_glyph7);
	device_create_file(dev, &dev_attr_fp_led_online);
	device_create_file(dev, &dev_attr_fp_led_power);
	device_create_file(dev, &dev_attr_fp_version);

	// Create the first workqueue task
	queue_delayed_work(piadagio_fp_wq, &piadagio_fp_wq_task_lcd, 10);
	queue_delayed_work(piadagio_fp_wq, &piadagio_fp_wq_task_led, 500);

	return 0;

// Cleanup on failed operations
unreg_class:
	class_unregister(piadagio_fp_class);
	class_destroy(piadagio_fp_class);
unreg_chrdev:
	unregister_chrdev(piadagio_fp_major, PIADAGIOFP_I2C_DEVNAME);
unreg_wq:
	destroy_workqueue(piadagio_fp_wq);
	printe("%s: Driver initialization failed!\n", __FUNCTION__);
out:
	return retval;
}

// Device removal
static int piadagio_fp_remove(struct i2c_client * client) {
	struct device * dev = &client->dev;

	printd("%s\n", __FUNCTION__);

	piadagio_fp_i2c_client = NULL;

	device_remove_file(dev, &dev_attr_fp_command);
	device_remove_file(dev, &dev_attr_fp_lcd_buffer);
	device_remove_file(dev, &dev_attr_fp_stats);
	device_remove_file(dev, &dev_attr_fp_do_update);
	device_remove_file(dev, &dev_attr_fp_do_update_screen);
	device_remove_file(dev, &dev_attr_fp_i2c_buffer);
	device_remove_file(dev, &dev_attr_fp_glyph0);
	device_remove_file(dev, &dev_attr_fp_glyph1);
	device_remove_file(dev, &dev_attr_fp_glyph2);
	device_remove_file(dev, &dev_attr_fp_glyph3);
	device_remove_file(dev, &dev_attr_fp_glyph4);
	device_remove_file(dev, &dev_attr_fp_glyph5);
	device_remove_file(dev, &dev_attr_fp_glyph6);
	device_remove_file(dev, &dev_attr_fp_glyph7);
	device_remove_file(dev, &dev_attr_fp_led_online);
	device_remove_file(dev, &dev_attr_fp_led_power);
	device_remove_file(dev, &dev_attr_fp_version);
	device_destroy(piadagio_fp_class, MKDEV(piadagio_fp_major, 0));

	class_unregister(piadagio_fp_class);
	class_destroy(piadagio_fp_class);

	unregister_chrdev(piadagio_fp_major, PIADAGIOFP_I2C_DEVNAME);

	piadagio_fp_wq_kill = 1;
	cancel_delayed_work(&piadagio_fp_wq_task_lcd);	// Cancel any new tasks
	cancel_delayed_work(&piadagio_fp_wq_task_led);	// Cancel any new tasks
	flush_workqueue(piadagio_fp_wq);		// And wait until all "old ones" finished
	destroy_workqueue(piadagio_fp_wq);

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
MODULE_VERSION(PIADAGIOFP_VERSION);
