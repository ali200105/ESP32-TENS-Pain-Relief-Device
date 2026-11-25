#ifndef I2C_LCD_H
#define I2C_LCD_H

#define LCD_ROWS 4
#define LCD_COLS 20

// I2C address of the LCD (change according to your setup)
#define SLAVE_ADDRESS_LCD 0x27    

// GPIO number used for I2C master clock
#define I2C_MASTER_SCL_IO           GPIO_NUM_27      

// GPIO number used for I2C master data
#define I2C_MASTER_SDA_IO           GPIO_NUM_26      

// I2C master port number
#define I2C_MASTER_NUM              0                

// I2C master clock frequency
#define I2C_MASTER_FREQ_HZ          400000           

// I2C master doesn't need buffer for transmission
#define I2C_MASTER_TX_BUF_DISABLE   0                

// I2C master doesn't need buffer for reception
#define I2C_MASTER_RX_BUF_DISABLE   0                

// Timeout for I2C master in milliseconds
#define I2C_MASTER_TIMEOUT_MS       1000

// Define the I2C number to be used
#define I2C_NUM                     I2C_NUM_0

// LCD command definitions
#define LCD_CMD_CLEAR_DISPLAY 0x01
#define LCD_CMD_RETURN_HOME 0x02
#define LCD_CMD_ENTRY_MODE_SET 0x06
#define LCD_CMD_DISPLAY_ON 0x0C
#define LCD_CMD_DISPLAY_OFF 0x08
#define LCD_CMD_FUNCTION_SET 0x28
#define LCD_CMD_SET_CURSOR 0x80
#define LCD_CMD_INIT_8_BIT_MODE 0x30
#define LCD_CMD_INIT_4_BIT_MODE 0x20

// ==================== تعريفات التوابع ====================

/**
 * @brief Initializes the LCD
 */
void lcd_init(void);   

/**
 * @brief Sends a command to the LCD
 */
void lcd_send_cmd(char cmd);  

/**
 * @brief Sends data to the LCD
 */
void lcd_send_data(char data);  

/**
 * @brief Sends a string to the LCD
 */
void lcd_send_string(char *str);  

/**
 * @brief Sets the cursor position on the LCD
 */
void lcd_put_cursor(int row, int col); 

/**
 * @brief Clears the LCD screen
 */
void lcd_clear(void);

// ====== التوابع الجديدة المضافة ======

/**
 * @brief Clears a specific row on the LCD
 */
void lcd_clear_row(int row);

/**
 * @brief Centers text on a specific row
 */
void lcd_center_text(int row, char *str);

/**
 * @brief Sends a number to the LCD
 */
void lcd_send_number(int num);

/**
 * @brief Fast clear of all LCD rows without delay
 */
void lcd_clear_fast(void);

#endif /* I2C_LCD_H */