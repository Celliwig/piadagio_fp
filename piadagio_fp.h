#define PIADAGIOFP_LOG_PREFIX "PiAdagio FP: "
#define PIADAGIOFP_DEBUG = 1
#ifdef PIADAGIOFP_DEBUG
#       define printd(...) pr_alert(PIADAGIOFP_LOG_PREFIX __VA_ARGS__)
#else
#       define printd(...) do {} while (0)
#endif
#define printe(...) pr_err(PIADAGIOFP_LOG_PREFIX __VA_ARGS__)
#define printi(...) pr_info(PIADAGIOFP_LOG_PREFIX __VA_ARGS__)
#define printn(...) pr_notice(PIADAGIOFP_LOG_PREFIX __VA_ARGS__)

#define	PIADAGIOFP_I2C_ADDR	0x11
#define PIADAGIOFP_I2C_DEVNAME "piadagio_fp"
#define PIADAGIOFP_BUF_LEN 	80
#define PIADAGIOFP_WQ_NAME 	"piadagio_fp"

#define PIADAGIOFP_MODE_SELECTED_LCD 0x1
#define PIADAGIOFP_MODE_SELECTED_LED 0x2

#define LCD_LINE_LEN		0x14
struct piadagio_fp_char_buffer {
	char line1[LCD_LINE_LEN];
	char line2[LCD_LINE_LEN];
	char line3[LCD_LINE_LEN];
	char line4[LCD_LINE_LEN];
};
#define SCREEN_BUFFER_LEN	(LCD_LINE_LEN * 4)
#define I2C_BUFFER_LEN		((LCD_LINE_LEN * 2) + 3)	// Maximum i2c command size

struct piadagio_fp_data {
	struct mutex update_lock;
	unsigned long lcd_last_updated;		// In jiffies
	unsigned long command_last_read;	// In jiffies
	int kind;
};

// General routines
/////////////////////////////////////////////////////////////////////
void piadagio_fp_buffer_lcd_clear(void);
int piadagio_fp_i2c_get_status(void);
int piadagio_fp_i2c_update_screen(void);

// Workqueue routines
/////////////////////////////////////////////////////////////////////
static void piadagio_fp_i2c_update(struct work_struct *work);

// Character device
/////////////////////////////////////////////////////////////////////
static int piadagio_fp_open(struct inode * inode, struct file *fp);
static int piadagio_fp_release(struct inode * inode, struct file * fp);
static ssize_t piadagio_fp_read(struct file *filp, char *buffer, size_t length, loff_t * offset);
static ssize_t piadagio_fp_write(struct file * fp, const char __user * buf, size_t count, loff_t * offset);

// I2C driver
/////////////////////////////////////////////////////////////////////
static int piadagio_fp_detect(struct i2c_client * client, struct i2c_board_info * info);
static int piadagio_fp_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int piadagio_fp_remove(struct i2c_client * client);

// Module
/////////////////////////////////////////////////////////////////////
// Not used
//static int __init piadagio_fp_init(void);
//static void __exit piadagio_fp_remove(void);
