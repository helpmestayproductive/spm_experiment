// Copyright 2011 INDILINX Co., Ltd.
//
// This file is part of Jasmine.
//
// Jasmine is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Jasmine is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Jasmine. See the file COPYING.
// If not, see <http://www.gnu.org/licenses/>.


#include "jasmine.h"
// #include "ftl_sgx.h"
//#include "ftl.h"

sata_context_t		g_sata_context;
sata_ncq_t			g_sata_ncq;
volatile UINT32		g_sata_action_flags;

#define HW_EQ_SIZE		128
#define HW_EQ_MARGIN	4

static UINT32 num_ata_flush_cache=0;
static UINT32 num_ata_flush_cache_execute=0;

extern UINT32 g_ftl_write_buf_id;	//page단위의 id. 밑단에서 write끝냐면 +1. WRITE_BUFFER위치.
extern UINT32 g_ftl_read_buf_id;
extern const int input_blocksize;

// EVENT_Q eve_sq[Q_SIZE];
extern EVENT_Q eve_q[Q_SIZE];
extern UINT32 eveq_front;
extern UINT32 eveq_rear;
extern UINT32 eveq_size;

static void HMAC_DELAY(int utime);
static void PV_policy_update(UINT32 const lba, UINT32 const sect_count, UINT32 const cmd_type);
static void PV_write (UINT32 const lba, UINT32 const sect_count, UINT32 const cmd_type);
static UINT32* PV_extract_writebuf (const UINT32 offset, const UINT32 num_page);

static UINT32 queue_isEmpty()
{
	return (eveq_front==eveq_rear);
}

static void queue_pop(UINT32* lba, UINT32* sector_count, UINT32* cmd_type, SGX_PARAM *sgx_param)
{
	if(!queue_isEmpty())
	{
		*lba = eve_q[eveq_front].lba;
		*sector_count = eve_q[eveq_front].sector_count;
		
		if(eve_q[eveq_front].sgx_fd==0)
		{
			//*cmd_type = eve_q[eveq_front].cmd_type;
			*cmd_type = eve_q[eveq_front].cmd_type;
			// sgx_param->fid = 0;	//sgx인지 구별자.
		}	

		eveq_size--;
		eveq_front = (eveq_front+1) % Q_SIZE;

		SETREG(SATA_EQ_STATUS, ((eveq_size) & 0xFF)<<16);
	}
	else{
	}
	return;
}

static UINT32 eventq_get_count(void)
{
	//key eventq sw version
	//return (eveq_rear - eveq_front)
	//r////e/turn !(eveq_front==eveq_rear);
	
	//return eveq_size;
	return (GETREG(SATA_EQ_STATUS) >> 16) & 0xFF;
	
	//return eveq_rear-eveq_front;
}

static void eventq_get(CMD_T* cmd, SGX_PARAM *sgx_param)
{
	disable_fiq();

	SETREG(SATA_EQ_CTRL, 1);	// The next entry from the Event Queue is copied to SATA_EQ_DATA_0 through SATA_EQ_DATA_2.

	while ((GETREG(SATA_EQ_DATA_2) & 8) != 0);

	UINT32 EQReadData0	= GETREG(SATA_EQ_DATA_0);
	UINT32 EQReadData1	= GETREG(SATA_EQ_DATA_1);

//key event q SW version
	//cmd->lba			= EQReadData1 & 0x3FFFFFFF;
	//cmd->sector_count	= EQReadData0 >> 16;
	//cmd->cmd_type		= EQReadData1 >> 31;

	//key event queue pop
	queue_pop( &(cmd->lba), &(cmd->sector_count), &(cmd->cmd_type), sgx_param);

	if(cmd->sector_count == 0)
	{
		//uart_printf("!!!seccoun00000!!!");
		cmd->sector_count = 0x10000;
	}	
	if (g_sata_context.eq_full)
	{
		g_sata_context.eq_full = FALSE;
		if ((GETREG(SATA_PHY_STATUS) & 0xF0F) == 0x103)
		{
			SETREG(SATA_CTRL_2, g_sata_action_flags);
		}
	}
	enable_fiq();
}

__inline ATA_FUNCTION_T search_ata_function(UINT32 command_code)
{
	UINT32 index;
	ATA_FUNCTION_T ata_function;

	index = mem_search_equ(ata_command_code_table, sizeof(UINT8), CMD_TABLE_SIZE, MU_CMD_SEARCH_EQU_SRAM, command_code);

	ata_function = (index == CMD_TABLE_SIZE) ? ata_not_supported : ata_function_table[index];

	if (ata_function == (ATA_FUNCTION_T) INVALID32)
		ata_function = ata_not_supported;

	return ata_function;
}

void Main(void)
{
	UINT32 key;
	UINT32 event_cnt;
	SGX_PARAM sgx_param;
	SGX_LBA sgx_lba;
	int i;

	while (1)
	{
		if (event_cnt=eventq_get_count())// && !queue_isEmpty())
		{
			CMD_T cmd;
			eventq_get(&cmd, &sgx_param);

			if (cmd.cmd_type == READ)
			{
				ftl_read(cmd.lba, cmd.sector_count);
			}
			else
			{
				if (PV_is_cmd(cmd.cmd_type))
					PV_policy_update (cmd.lba, cmd.sector_count, cmd.cmd_type);
				else 
					ftl_write(cmd.lba, cmd.sector_count, 0);
					
			}
		}
		else if (g_sata_context.slow_cmd.status == SLOW_CMD_STATUS_PENDING)
		{
			void (*ata_function)(UINT32 lba, UINT32 sector_count);

			slow_cmd_t* slow_cmd = &g_sata_context.slow_cmd;
			slow_cmd->status = SLOW_CMD_STATUS_BUSY;
			ata_function = search_ata_function(slow_cmd->code);
			ata_function(slow_cmd->lba, slow_cmd->sector_count);

			slow_cmd->status = SLOW_CMD_STATUS_NONE;
		}
		else
		{
			// idle time operations
		}
	}
}

void sata_reset(void)
{
	disable_interrupt();

	mem_set_sram(&g_sata_context, 0, sizeof(g_sata_context));

	g_sata_context.write_cache_enabled = TRUE;
	g_sata_context.read_look_ahead_enabled = TRUE;

	SETREG(PMU_ResetCon, RESET_SATA | RESET_SATADWCLK | RESET_SATAHCLK | RESET_PMCLK | RESET_PHYDOMAIN);
	delay(100);

	SETREG(PHY_DEBUG, 0x400A040E);
	while ((GETREG(PHY_DEBUG) & BIT30) == 1);

	SETREG(SATA_BUF_PAGE_SIZE, BYTES_PER_PAGE);
	SETREG(SATA_WBUF_BASE, (WR_BUF_ADDR - DRAM_BASE));
	SETREG(SATA_RBUF_BASE, (RD_BUF_ADDR - DRAM_BASE));
	SETREG(SATA_WBUF_SIZE, NUM_WR_BUFFERS);
	SETREG(SATA_RBUF_SIZE, NUM_RD_BUFFERS);
	SETREG(SATA_WBUF_MARGIN, 16);
	SETREG(SATA_RESET_WBUF_PTR, BIT0);
	SETREG(SATA_RESET_RBUF_PTR, BIT0);

	SETREG(SATA_NCQ_BASE, g_sata_ncq.queue);

	SETREG(SATA_EQ_CFG_1, BIT0 | BIT14 | BIT9 | BIT16 | ((NUM_BANKS / 2) << 24));
	SETREG(SATA_EQ_CFG_2, (EQ_MARGIN & 0xF) << 16);

	SETREG(SATA_CFG_10, BIT0);

	SETREG(SATA_NCQ_CTRL, AUTOINC | FLUSH_NCQ);
	SETREG(SATA_NCQ_CTRL, AUTOINC);
	SETREG(SATA_CFG_5, BIT12 | BIT11*BSO_RX_SSC | (BIT9|BIT10)*BSO_TX_SSC | BIT4*0x05);
	SETREG(SATA_CFG_8, 0);
	SETREG(SATA_CFG_9, BIT20);

	SETREG(SATA_MAX_LBA, MAX_LBA);

	SETREG(APB_INT_STS, INTR_SATA);

	#if OPTION_SLOW_SATA
	SETREG(SATA_PHY_CTRL, 0x00000310);
	#else
	SETREG(SATA_PHY_CTRL, 0x00000300);
	#endif

	SETREG(SATA_ERROR, 0xFFFFFFFF);
	SETREG(SATA_INT_STAT, 0xFFFFFFFF);

	SETREG(SATA_CTRL_1, BIT31);

	while ((GETREG(SATA_INT_STAT) & PHY_ONLINE) == 0);

	SETREG(SATA_CTRL_1, BIT31 | BIT25 | BIT24);

	SETREG(SATA_INT_ENABLE, PHY_ONLINE);

	enable_interrupt();
}

void delay(UINT32 const count)
{
	static volatile UINT32 temp;
	UINT32 i;

	for (i = 0; i < count; i++)
	{
		temp = i;
	}
}

/* PV-SSD Command Processing Functions! */
static void PV_policy_update(UINT32 const lba, UINT32 const sect_count, UINT32 const cmd_type)
{
	
	uart_print("PV Policy Update Command!\n");
	
	UINT32 i;

	// UINT8 mac[MAC_SIZE];
	UINT64 offset = (UINT64)lba << 9; // (lba * 512) is byte address
	UINT32 *data;

	struct policy_metadata p_info;
	
	// 1. Get Write buffer Address First
	data = PV_extract_writebuf(offset, 0);

	p_info.pid 			= read_dram_32(&data[0]);
	p_info.ret_time 	= read_dram_32(&data[1]);
	p_info.backup_cycle = read_dram_32(&data[2]);
	p_info.num_version 	= read_dram_32(&data[3]);

	uart_printf("pid %d, ret_time %d\n", p_info.pid, p_info.ret_time);
	uart_printf("backup_cycle %d, num_version %d\n", p_info.backup_cycle, p_info.num_version);
	// MAC Verification Emulating Delay
	// HMAC_DELAY(3); 

	// PV_ftl_policy_update (p_info, cmd_type);

	/* Buffer Pointer Management*/
	while (g_ftl_write_buf_id == GETREG(SATA_WBUF_PTR)); // bm_write_limit should not outpace SATA_WBUF_PTR

	g_ftl_write_buf_id = (g_ftl_write_buf_id + 1) % NUM_WR_BUFFERS; // Circular buffer

	SETREG(BM_STACK_WRSET, g_ftl_write_buf_id);	// change bm_write_limit
	SETREG(BM_STACK_RESET, 0x01);	// change bm_write_limit			

	uart_print("Policy Update Done!");
}

static void PV_write (UINT32 const lba, UINT32 const sect_count, UINT32 const cmd_type)
{
	
	uart_print("PV Write Command!\n");
	
	UINT32 i;
	UINT32 data_end, num_page;
	UINT32 pid, file_path_hash;
	UINT32 *data;

	UINT64 offset = (UINT64)lba << 9; // (lba * 512) is byte address

	data_end = ((offset / BYTES_PER_SECTOR) % SECTORS_PER_PAGE) + (sect_count - 1); //ftl_write할 page 내 write buffer end point (이후 메타데이터포홤)
	num_page = data_end / SECTORS_PER_PAGE;	// 0 or 1 

	// 1. Get Write buffer Address First
	data = PV_extract_writebuf (offset + ((sect_count - 1) * 512), num_page);

	pid = read_dram_32 (&data[0]);
	// UINT32 file_path_hash = read_dram_32 (&data[1]);
	// UINT32 offset = read_dram_32 (&data[2]);
	
	// Write the real data!
	ftl_write (lba, (sect_count-1), pid);

	/* Buffer Pointer Management */
	if(data_end % SECTORS_PER_PAGE == 0) // If the additional sector is across two pages
	{	// Do the same thing with PV_policy_update ()
		flash_finish();
		while (g_ftl_write_buf_id == GETREG(SATA_WBUF_PTR));

		g_ftl_write_buf_id = (g_ftl_write_buf_id + 1) % NUM_WR_BUFFERS;

		SETREG(BM_STACK_WRSET, g_ftl_write_buf_id);
		SETREG(BM_STACK_RESET, 0x01);
	}

	uart_print("PV_SSD Write Done!");
}


static UINT32* PV_extract_writebuf (const UINT32 offset, const UINT32 num_page)
{
	// Get the byte-address where the policy metadata are saved
	UINT32 lba;
	UINT32 *write_buf_addr;

	lba = offset / BYTES_PER_SECTOR;	//현재 offset에 맵핑되는 lba

	write_buf_addr = (UINT32 *) (WR_BUF_PTR((g_ftl_write_buf_id + num_page) % NUM_WR_BUFFERS) + ((lba % SECTORS_PER_PAGE) * BYTES_PER_SECTOR));

	return write_buf_addr;
}

/* PV-SSD Utility Function */

// void HMAC_DELAY (int time)
// {
   
//     start_interval_measurement(TIMER_CH1, TIMER_PRESCALE_0); //timer start
//     UINT32 rtime;
//     char buf[21];

//     do {
//         rtime = 0xFFFFFFFF - GET_TIMER_VALUE(TIMER_CH1);
			
// 			  // Tick to us
//         rtime = (UINT32)((UINT64)rtime * 2 * 1000000 * PRESCALE_TO_DIV(TIMER_PRESCALE_0) / CLOCK_SPEED);
//     }
//     while(rtime<time);

// }