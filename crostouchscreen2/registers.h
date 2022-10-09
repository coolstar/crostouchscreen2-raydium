#include "stdint.h"

/* Slave I2C mode */
#define RM_BOOT_BLDR		0x02
#define RM_BOOT_MAIN		0x03

/* I2C bootoloader commands */
#define RM_CMD_BOOT_PAGE_WRT	0x0B		/* send bl page write */
#define RM_CMD_BOOT_WRT		0x11		/* send bl write */
#define RM_CMD_BOOT_ACK		0x22		/* send ack*/
#define RM_CMD_BOOT_CHK		0x33		/* send data check */
#define RM_CMD_BOOT_READ	0x44		/* send wait bl data ready*/

#define RM_BOOT_RDY		0xFF		/* bl data ready */
#define RM_BOOT_CMD_READHWID	0x0E		/* read hwid */

/* I2C main commands */
#define RM_CMD_QUERY_BANK	0x2B
#define RM_CMD_DATA_BANK	0x4D
#define RM_CMD_ENTER_SLEEP	0x4E
#define RM_CMD_BANK_SWITCH	0xAA

#define RM_RESET_MSG_ADDR	0x40000004

#define RM_MAX_READ_SIZE	56
#define RM_PACKET_CRC_SIZE	2

/* Touch relative info */
#define RM_MAX_RETRIES		3
#define RM_RETRY_DELAY_MS	20
#define RM_MAX_TOUCH_NUM	10
#define RM_BOOT_DELAY_MS	100

/* Offsets in contact data */
#define RM_CONTACT_STATE_POS	0
#define RM_CONTACT_X_POS	1
#define RM_CONTACT_Y_POS	3
#define RM_CONTACT_PRESSURE_POS	5
#define RM_CONTACT_WIDTH_X_POS	6
#define RM_CONTACT_WIDTH_Y_POS	7

/* Bootloader relative info */
#define RM_BL_WRT_CMD_SIZE	3	/* bl flash wrt cmd size */
#define RM_BL_WRT_PKG_SIZE	32	/* bl wrt pkg size */
#define RM_BL_WRT_LEN		(RM_BL_WRT_PKG_SIZE + RM_BL_WRT_CMD_SIZE)
#define RM_FW_PAGE_SIZE		128
#define RM_MAX_FW_RETRIES	30
#define RM_MAX_FW_SIZE		0xD000

#define RM_POWERON_DELAY_USEC	500
#define RM_RESET_DELAY_MSEC	50

enum raydium_bl_cmd {
	BL_HEADER = 0,
	BL_PAGE_STR,
	BL_PKG_IDX,
	BL_DATA_STR,
};

enum raydium_bl_ack {
	RAYDIUM_ACK_NULL = 0,
	RAYDIUM_WAIT_READY,
	RAYDIUM_PATH_READY,
};

enum raydium_boot_mode {
	RAYDIUM_TS_MAIN = 0,
	RAYDIUM_TS_BLDR,
};

/* Response to RM_CMD_DATA_BANK request */
struct raydium_data_info {
	UINT32 data_bank_addr;
	UINT8 pkg_size;
	UINT8 tp_info_size;
};

struct raydium_info {
	UINT32 hw_ver;		/*device version */
	UINT8 main_ver;
	UINT8 sub_ver;
	UINT16 ft_ver;		/* test version */
	UINT8 x_num;
	UINT8 y_num;
	UINT16 x_max;
	UINT16 y_max;
	UINT8 x_res;		/* units/mm */
	UINT8 y_res;		/* units/mm */
};
