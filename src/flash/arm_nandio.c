/*
 * Copyright (C) 2009 by Marvell Semiconductors, Inc.
 * Written by Nicolas Pitre <nico at marvell.com>
 *
 * Copyright (C) 2009 by David Brownell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arm_nandio.h"
#include "armv4_5.h"
#include "algorithm.h"

/**
 * Copies code to a working area.  This will allocate room for the code plus the
 * additional amount requested if the working area pointer is null.
 *
 * @param target Pointer to the target to copy code to
 * @param code Pointer to the code area to be copied
 * @param code_size Size of the code being copied
 * @param additional Size of the additional area to be allocated in addition to
 *                   code
 * @param area Pointer to a pointer to a working area to copy code to
 * @return Success or failure of the operation
 */
int arm_code_to_working_area(struct target *target, const uint32_t *code, unsigned code_size,
		unsigned additional, struct working_area **area)
{
	uint8_t code_buf[code_size];
	unsigned i;
	int retval;
	unsigned size = code_size + additional;

	/* make sure we have a working area */
	if (NULL == *area) {
		retval = target_alloc_working_area(target, size, area);
		if (retval != ERROR_OK) {
			LOG_DEBUG("%s: no %d byte buffer", __FUNCTION__, (int) size);
			return ERROR_NAND_NO_BUFFER;
		}
	}

	/* buffer code in target endianness */
	for (i = 0; i < code_size / 4; i++)
		target_buffer_set_u32(target, code_buf + i * 4, code[i]);

	/* copy code to work area */
	retval = target_write_memory(target, (*area)->address,
			4, code_size / 4, code_buf);

	return retval;
}

/**
 * ARM-specific bulk write from buffer to address of 8-bit wide NAND.
 * For now this only supports ARMv4 and ARMv5 cores.
 *
 * Enhancements to target_run_algorithm() could enable:
 *   - ARMv6 and ARMv7 cores in ARM mode
 *
 * Different code fragments could handle:
 *   - Thumb2 cores like Cortex-M (needs different byteswapping)
 *   - 16-bit wide data (needs different setup too)
 *
 * @param nand Pointer to the arm_nand_data struct that defines the I/O
 * @param data Pointer to the data to be copied to flash
 * @param size Size of the data being copied
 * @return Success or failure of the operation
 */
int arm_nandwrite(struct arm_nand_data *nand, uint8_t *data, int size)
{
	struct target		*target = nand->target;
	struct armv4_5_algorithm	algo;
	struct arm		*armv4_5 = target->arch_info;
	struct reg_param	reg_params[3];
	uint32_t		target_buf;
	uint32_t		exit = 0;
	int			retval;

	/* Inputs:
	 *  r0	NAND data address (byte wide)
	 *  r1	buffer address
	 *  r2	buffer length
	 */
	static const uint32_t code[] = {
		0xe4d13001,	/* s: ldrb  r3, [r1], #1 */
		0xe5c03000,	/*    strb  r3, [r0]     */
		0xe2522001,	/*    subs  r2, r2, #1   */
		0x1afffffb,	/*    bne   s            */

		/* exit: ARMv4 needs hardware breakpoint */
		0xe1200070,	/* e: bkpt  #0           */
	};

	if (!nand->copy_area) {
		retval = arm_code_to_working_area(target, code, sizeof(code),
				nand->chunk_size, &nand->copy_area);
		if (retval != ERROR_OK) {
			return retval;
		}
	}

	/* copy data to work area */
	target_buf = nand->copy_area->address + sizeof(code);
	retval = target_bulk_write_memory(target, target_buf, size / 4, data);
	if (retval == ERROR_OK && (size & 3) != 0)
		retval = target_write_memory(target,
				target_buf + (size & ~3),
				1, size & 3, data + (size & ~3));
	if (retval != ERROR_OK)
		return retval;

	/* set up algorithm and parameters */
	algo.common_magic = ARMV4_5_COMMON_MAGIC;
	algo.core_mode = ARMV4_5_MODE_SVC;
	algo.core_state = ARMV4_5_STATE_ARM;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_IN);
	init_reg_param(&reg_params[2], "r2", 32, PARAM_IN);

	buf_set_u32(reg_params[0].value, 0, 32, nand->data);
	buf_set_u32(reg_params[1].value, 0, 32, target_buf);
	buf_set_u32(reg_params[2].value, 0, 32, size);

	/* armv4 must exit using a hardware breakpoint */
	if (armv4_5->is_armv4)
		exit = nand->copy_area->address + sizeof(code) - 4;

	/* use alg to write data from work area to NAND chip */
	retval = target_run_algorithm(target, 0, NULL, 3, reg_params,
			nand->copy_area->address, exit, 1000, &algo);
	if (retval != ERROR_OK)
		LOG_ERROR("error executing hosted NAND write");

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);

	return retval;
}

/**
 * Uses an on-chip algorithm for an ARM device to read from a NAND device and
 * store the data into the host machine's memory.
 *
 * @param nand Pointer to the arm_nand_data struct that defines the I/O
 * @param data Pointer to the data buffer to store the read data
 * @param size Amount of data to be stored to the buffer.
 * @return Success or failure of the operation
 */
int arm_nandread(struct arm_nand_data *nand, uint8_t *data, uint32_t size)
{
	struct target *target = nand->target;
	struct armv4_5_algorithm algo;
	struct arm *armv4_5 = target->arch_info;
	struct reg_param reg_params[3];
	uint32_t target_buf;
	uint32_t exit = 0;
	int retval;

	/* Inputs:
	 *  r0	buffer address
	 *  r1	NAND data address (byte wide)
	 *  r2	buffer length
	 */
	static const uint32_t code[] = {
		0xe5d13000,	/* s: ldrb  r3, [r1]     */
		0xe4c03001,	/*    strb  r3, [r0], #1 */
		0xe2522001,	/*    subs  r2, r2, #1   */
		0x1afffffb,	/*    bne   s            */

		/* exit: ARMv4 needs hardware breakpoint */
		0xe1200070,	/* e: bkpt  #0           */
	};

	/* create the copy area if not yet available */
	if (!nand->copy_area) {
		retval = arm_code_to_working_area(target, code, sizeof(code),
				nand->chunk_size, &nand->copy_area);
		if (retval != ERROR_OK) {
			return retval;
		}
	}

	target_buf = nand->copy_area->address + sizeof(code);

	/* set up algorithm and parameters */
	algo.common_magic = ARMV4_5_COMMON_MAGIC;
	algo.core_mode = ARMV4_5_MODE_SVC;
	algo.core_state = ARMV4_5_STATE_ARM;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_IN);
	init_reg_param(&reg_params[2], "r2", 32, PARAM_IN);

	buf_set_u32(reg_params[0].value, 0, 32, target_buf);
	buf_set_u32(reg_params[1].value, 0, 32, nand->data);
	buf_set_u32(reg_params[2].value, 0, 32, size);

	/* armv4 must exit using a hardware breakpoint */
	if (armv4_5->is_armv4)
		exit = nand->copy_area->address + sizeof(code) - 4;

	/* use alg to write data from NAND chip to work area */
	retval = target_run_algorithm(target, 0, NULL, 3, reg_params,
			nand->copy_area->address, exit, 1000, &algo);
	if (retval != ERROR_OK)
		LOG_ERROR("error executing hosted NAND write");

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);

	/* read from work area to the host's memory */
	retval = target_read_buffer(target, target_buf, size, data);

	return retval;
}

