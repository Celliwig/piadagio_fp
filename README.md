# piadagio_fp

# Overview
Raspberry Pi kernel module to drive the front panel from Adagio Sound Server. The module creates a character device which is backed by a buffer which is used to write to the LCD display on the front panel. Writing to the device, writes to the buffer. While communicating with the front panel it buffers the value of the current button command which is returned when reading from the character device. It supports lseek for tranversing the buffer memory, and fsync which is used to signal that the driver can write the buffer to the LCD (This allows several writes to be made before the results are flushed to the LCD e.g. buffer clear, then write). Additional buffer space is allocated to support user generated glyphs.

# SYSFS objects
 - fp_lcd_buffer - RO - Returns the contents of the LCD buffer.
 - fp_i2c_buffer - RO - Returns the contents of the i2c comms buffer.
 - fp_glyph<b>[n]</b> - RO - Returns an ASCII representation of the glyph buffer.
 - fp_command - RO - Returns the currently depressed button.
 - fp_do_update - RW - Get/set whether updates are allowed (this includes reading button commands).
 - fp_do_update_screen - RW - Get/set whether screen updates are allowed.
 - fp_led_online - RW - Get/set the 'online' led state.
 - fp_led_power - RW - Get/set the power led state.
 - fp_stats - RO - Returns stats about the module e.g. number of writes done, errors, etc.
 - fp_version - RO - Returns the current module version.

# Memory Map

|          | address |
| --------:| -------:|
|  line 1  |     0   |
|  line 2  |    20   |
|  line 3  |    40   |
|  line 4  |    60   |
|  glyph 1 |   128   |
|  glyph 2 |   136   |
|  glyph 3 |   144   |
|  glyph 4 |   152   |
|  glyph 5 |   160   |
|  glyph 6 |   168   |
|  glyph 7 |   176   |
|  glyph 8 |   184   |
