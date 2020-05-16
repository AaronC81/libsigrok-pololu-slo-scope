/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2010-2012 Håvard Espeland <gus@ping.uio.no>,
 * Copyright (C) 2010 Martin Stensgård <mastensg@ping.uio.no>
 * Copyright (C) 2010 Carl Henrik Lunde <chlunde@ping.uio.no>
 * Copyright (C) 2020 Gerhard Sittig <gerhard.sittig@gmx.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * ASIX SIGMA/SIGMA2 logic analyzer driver
 */

#include <config.h>
#include "protocol.h"

/*
 * The ASIX SIGMA hardware supports fixed 200MHz and 100MHz sample rates
 * (by means of separate firmware images). As well as 50MHz divided by
 * an integer divider in the 1..256 range (by the "typical" firmware).
 * Which translates to a strict lower boundary of around 195kHz.
 *
 * This driver "suggests" a subset of the available rates by listing a
 * few discrete values, while setter routines accept any user specified
 * rate that is supported by the hardware.
 */
static const uint64_t samplerates[] = {
	/* 50MHz and integer divider. 1/2/5 steps (where possible). */
	SR_KHZ(200), SR_KHZ(500),
	SR_MHZ(1), SR_MHZ(2), SR_MHZ(5),
	SR_MHZ(10), SR_MHZ(25), SR_MHZ(50),
	/* 100MHz/200MHz, fixed rates in special firmware. */
	SR_MHZ(100), SR_MHZ(200),
};

SR_PRIV GVariant *sigma_get_samplerates_list(void)
{
	return std_gvar_samplerates(samplerates, ARRAY_SIZE(samplerates));
}

static const char *firmware_files[] = {
	[SIGMA_FW_50MHZ] = "asix-sigma-50.fw", /* 50MHz, 8bit divider. */
	[SIGMA_FW_100MHZ] = "asix-sigma-100.fw", /* 100MHz, fixed. */
	[SIGMA_FW_200MHZ] = "asix-sigma-200.fw", /* 200MHz, fixed. */
	[SIGMA_FW_SYNC] = "asix-sigma-50sync.fw", /* Sync from external pin. */
	[SIGMA_FW_FREQ] = "asix-sigma-phasor.fw", /* Frequency counter. */
};

#define SIGMA_FIRMWARE_SIZE_LIMIT (256 * 1024)

static int sigma_ftdi_open(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int vid, pid;
	const char *serno;
	int ret;

	devc = sdi->priv;
	if (!devc)
		return SR_ERR_ARG;

	if (devc->ftdi.is_open)
		return SR_OK;

	vid = devc->id.vid;
	pid = devc->id.pid;
	serno = sdi->serial_num;
	if (!vid || !pid || !serno || !*serno)
		return SR_ERR_ARG;

	ret = ftdi_init(&devc->ftdi.ctx);
	if (ret < 0) {
		sr_err("Cannot initialize FTDI context (%d): %s.",
			ret, ftdi_get_error_string(&devc->ftdi.ctx));
		return SR_ERR_IO;
	}
	ret = ftdi_usb_open_desc_index(&devc->ftdi.ctx,
		vid, pid, NULL, serno, 0);
	if (ret < 0) {
		sr_err("Cannot open device (%d): %s.",
			ret, ftdi_get_error_string(&devc->ftdi.ctx));
		return SR_ERR_IO;
	}
	devc->ftdi.is_open = TRUE;

	return SR_OK;
}

static int sigma_ftdi_close(struct dev_context *devc)
{
	int ret;

	ret = ftdi_usb_close(&devc->ftdi.ctx);
	devc->ftdi.is_open = FALSE;
	devc->ftdi.must_close = FALSE;
	ftdi_deinit(&devc->ftdi.ctx);

	return ret == 0 ? SR_OK : SR_ERR_IO;
}

SR_PRIV int sigma_check_open(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;

	if (!sdi)
		return SR_ERR_ARG;
	devc = sdi->priv;
	if (!devc)
		return SR_ERR_ARG;

	if (devc->ftdi.is_open)
		return SR_OK;

	ret = sigma_ftdi_open(sdi);
	if (ret != SR_OK)
		return ret;
	devc->ftdi.must_close = TRUE;

	return ret;
}

SR_PRIV int sigma_check_close(struct dev_context *devc)
{
	int ret;

	if (!devc)
		return SR_ERR_ARG;

	if (devc->ftdi.must_close) {
		ret = sigma_ftdi_close(devc);
		if (ret != SR_OK)
			return ret;
		devc->ftdi.must_close = FALSE;
	}

	return SR_OK;
}

SR_PRIV int sigma_force_open(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;

	if (!sdi)
		return SR_ERR_ARG;
	devc = sdi->priv;
	if (!devc)
		return SR_ERR_ARG;

	ret = sigma_ftdi_open(sdi);
	if (ret != SR_OK)
		return ret;
	devc->ftdi.must_close = FALSE;

	return SR_OK;
}

SR_PRIV int sigma_force_close(struct dev_context *devc)
{
	return sigma_ftdi_close(devc);
}

/*
 * BEWARE! Error propagation is important, as are kinds of return values.
 *
 * - Raw USB tranport communicates the number of sent or received bytes,
 *   or negative error codes in the external library's(!) range of codes.
 * - Internal routines at the "sigrok driver level" communicate success
 *   or failure in terms of SR_OK et al error codes.
 * - Main loop style receive callbacks communicate booleans which arrange
 *   for repeated calls to drive progress during acquisition.
 *
 * Careful consideration by maintainers is essential, because all of the
 * above kinds of values are assignment compatbile from the compiler's
 * point of view. Implementation errors will go unnoticed at build time.
 */

static int sigma_read_raw(struct dev_context *devc, void *buf, size_t size)
{
	int ret;

	ret = ftdi_read_data(&devc->ftdi.ctx, (unsigned char *)buf, size);
	if (ret < 0) {
		sr_err("USB data read failed: %s",
			ftdi_get_error_string(&devc->ftdi.ctx));
	}

	return ret;
}

static int sigma_write_raw(struct dev_context *devc, const void *buf, size_t size)
{
	int ret;

	ret = ftdi_write_data(&devc->ftdi.ctx, buf, size);
	if (ret < 0) {
		sr_err("USB data write failed: %s",
			ftdi_get_error_string(&devc->ftdi.ctx));
	} else if ((size_t)ret != size) {
		sr_err("USB data write length mismatch.");
	}

	return ret;
}

static int sigma_read_sr(struct dev_context *devc, void *buf, size_t size)
{
	int ret;

	ret = sigma_read_raw(devc, buf, size);
	if (ret < 0 || (size_t)ret != size)
		return SR_ERR_IO;

	return SR_OK;
}

static int sigma_write_sr(struct dev_context *devc, const void *buf, size_t size)
{
	int ret;

	ret = sigma_write_raw(devc, buf, size);
	if (ret < 0 || (size_t)ret != size)
		return SR_ERR_IO;

	return SR_OK;
}

/*
 * Implementor's note: The local write buffer's size shall suffice for
 * any know FPGA register transaction that is involved in the supported
 * feature set of this sigrok device driver. If the length check trips,
 * that's a programmer's error and needs adjustment in the complete call
 * stack of the respective code path.
 */
#define SIGMA_MAX_REG_DEPTH	32

/*
 * Implementor's note: The FPGA command set supports register access
 * with automatic address adjustment. This operation is documented to
 * wrap within a 16-address range, it cannot cross boundaries where the
 * register address' nibble overflows. An internal helper assumes that
 * callers remain within this auto-adjustment range, and thus multi
 * register access requests can never exceed that count.
 */
#define SIGMA_MAX_REG_COUNT	16

SR_PRIV int sigma_write_register(struct dev_context *devc,
	uint8_t reg, uint8_t *data, size_t len)
{
	uint8_t buf[2 + SIGMA_MAX_REG_DEPTH * 2], *wrptr;
	size_t idx;

	if (len > SIGMA_MAX_REG_DEPTH) {
		sr_err("Short write buffer for %zu bytes to reg %u.", len, reg);
		return SR_ERR_BUG;
	}

	wrptr = buf;
	write_u8_inc(&wrptr, REG_ADDR_LOW | LO4(reg));
	write_u8_inc(&wrptr, REG_ADDR_HIGH | HI4(reg));
	for (idx = 0; idx < len; idx++) {
		write_u8_inc(&wrptr, REG_DATA_LOW | LO4(data[idx]));
		write_u8_inc(&wrptr, REG_DATA_HIGH_WRITE | HI4(data[idx]));
	}

	return sigma_write_sr(devc, buf, wrptr - buf);
}

SR_PRIV int sigma_set_register(struct dev_context *devc,
	uint8_t reg, uint8_t value)
{
	return sigma_write_register(devc, reg, &value, sizeof(value));
}

static int sigma_read_register(struct dev_context *devc,
	uint8_t reg, uint8_t *data, size_t len)
{
	uint8_t buf[3], *wrptr;
	int ret;

	wrptr = buf;
	write_u8_inc(&wrptr, REG_ADDR_LOW | LO4(reg));
	write_u8_inc(&wrptr, REG_ADDR_HIGH | HI4(reg));
	write_u8_inc(&wrptr, REG_READ_ADDR);
	ret = sigma_write_sr(devc, buf, wrptr - buf);
	if (ret != SR_OK)
		return ret;

	return sigma_read_sr(devc, data, len);
}

static int sigma_get_register(struct dev_context *devc,
	uint8_t reg, uint8_t *data)
{
	return sigma_read_register(devc, reg, data, sizeof(*data));
}

static int sigma_get_registers(struct dev_context *devc,
	uint8_t reg, uint8_t *data, size_t count)
{
	uint8_t buf[2 + SIGMA_MAX_REG_COUNT], *wrptr;
	size_t idx;
	int ret;

	if (count > SIGMA_MAX_REG_COUNT) {
		sr_err("Short command buffer for %zu reg reads at %u.", count, reg);
		return SR_ERR_BUG;
	}

	wrptr = buf;
	write_u8_inc(&wrptr, REG_ADDR_LOW | LO4(reg));
	write_u8_inc(&wrptr, REG_ADDR_HIGH | HI4(reg));
	for (idx = 0; idx < count; idx++)
		write_u8_inc(&wrptr, REG_READ_ADDR | REG_ADDR_INC);
	ret = sigma_write_sr(devc, buf, wrptr - buf);
	if (ret != SR_OK)
		return ret;

	return sigma_read_sr(devc, data, count);
}

static int sigma_read_pos(struct dev_context *devc,
	uint32_t *stoppos, uint32_t *triggerpos, uint8_t *mode)
{
	uint8_t result[7];
	const uint8_t *rdptr;
	uint32_t v32;
	uint8_t v8;
	int ret;

	/*
	 * Read 7 registers starting at trigger position LSB.
	 * Which yields two 24bit counter values, and mode flags.
	 */
	ret = sigma_get_registers(devc, READ_TRIGGER_POS_LOW,
		result, sizeof(result));
	if (ret != SR_OK)
		return ret;

	rdptr = &result[0];
	v32 = read_u24le_inc(&rdptr);
	if (triggerpos)
		*triggerpos = v32;
	v32 = read_u24le_inc(&rdptr);
	if (stoppos)
		*stoppos = v32;
	v8 = read_u8_inc(&rdptr);
	if (mode)
		*mode = v8;

	/*
	 * These positions consist of "the memory row" in the MSB fields,
	 * and "an event index" within the row in the LSB fields. Part
	 * of the memory row's content is sample data, another part is
	 * timestamps.
	 *
	 * The retrieved register values point to after the captured
	 * position. So they need to get decremented, and adjusted to
	 * cater for the timestamps when the decrement carries over to
	 * a different memory row.
	 */
	if (stoppos && (--*stoppos & ROW_MASK) == ROW_MASK)
		*stoppos -= CLUSTERS_PER_ROW;
	if (triggerpos && (--*triggerpos & ROW_MASK) == ROW_MASK)
		*triggerpos -= CLUSTERS_PER_ROW;

	return SR_OK;
}

static int sigma_read_dram(struct dev_context *devc,
	uint16_t startchunk, size_t numchunks, uint8_t *data)
{
	uint8_t buf[128], *wrptr, regval;
	size_t chunk;
	int sel, ret;
	gboolean is_last;

	if (2 + 3 * numchunks > ARRAY_SIZE(buf)) {
		sr_err("Short write buffer for %zu DRAM row reads.", numchunks);
		return SR_ERR_BUG;
	}

	/* Communicate DRAM start address (memory row, aka samples line). */
	wrptr = buf;
	write_u16be_inc(&wrptr, startchunk);
	ret = sigma_write_register(devc, WRITE_MEMROW, buf, wrptr - buf);
	if (ret != SR_OK)
		return ret;

	/*
	 * Access DRAM content. Fetch from DRAM to FPGA's internal RAM,
	 * then transfer via USB. Interleave the FPGA's DRAM access and
	 * USB transfer, use alternating buffers (0/1) in the process.
	 */
	wrptr = buf;
	write_u8_inc(&wrptr, REG_DRAM_BLOCK);
	write_u8_inc(&wrptr, REG_DRAM_WAIT_ACK);
	for (chunk = 0; chunk < numchunks; chunk++) {
		sel = chunk % 2;
		is_last = chunk == numchunks - 1;
		if (!is_last) {
			regval = REG_DRAM_BLOCK | REG_DRAM_SEL_BOOL(!sel);
			write_u8_inc(&wrptr, regval);
		}
		regval = REG_DRAM_BLOCK_DATA | REG_DRAM_SEL_BOOL(sel);
		write_u8_inc(&wrptr, regval);
		if (!is_last)
			write_u8_inc(&wrptr, REG_DRAM_WAIT_ACK);
	}
	ret = sigma_write_sr(devc, buf, wrptr - buf);
	if (ret != SR_OK)
		return ret;

	return sigma_read_sr(devc, data, numchunks * ROW_LENGTH_BYTES);
}

/* Upload trigger look-up tables to Sigma. */
SR_PRIV int sigma_write_trigger_lut(struct dev_context *devc,
	struct triggerlut *lut)
{
	int lut_addr;
	uint16_t bit;
	uint8_t m3d, m2d, m1d, m0d;
	uint8_t buf[6], *wrptr, regval;
	int ret;

	/*
	 * Translate the LUT part of the trigger configuration from the
	 * application's perspective to the hardware register's bitfield
	 * layout. Send the LUT to the device. This configures the logic
	 * which combines pin levels or edges.
	 */
	for (lut_addr = 0; lut_addr < 16; lut_addr++) {
		bit = 1 << lut_addr;

		/* - M4 M3S M3Q */
		m3d = 0;
		if (lut->m4 & bit)
			m3d |= 1 << 2;
		if (lut->m3s & bit)
			m3d |= 1 << 1;
		if (lut->m3 & bit)
			m3d |= 1 << 0;

		/* M2D3 M2D2 M2D1 M2D0 */
		m2d = 0;
		if (lut->m2d[3] & bit)
			m2d |= 1 << 3;
		if (lut->m2d[2] & bit)
			m2d |= 1 << 2;
		if (lut->m2d[1] & bit)
			m2d |= 1 << 1;
		if (lut->m2d[0] & bit)
			m2d |= 1 << 0;

		/* M1D3 M1D2 M1D1 M1D0 */
		m1d = 0;
		if (lut->m1d[3] & bit)
			m1d |= 1 << 3;
		if (lut->m1d[2] & bit)
			m1d |= 1 << 2;
		if (lut->m1d[1] & bit)
			m1d |= 1 << 1;
		if (lut->m1d[0] & bit)
			m1d |= 1 << 0;

		/* M0D3 M0D2 M0D1 M0D0 */
		m0d = 0;
		if (lut->m0d[3] & bit)
			m0d |= 1 << 3;
		if (lut->m0d[2] & bit)
			m0d |= 1 << 2;
		if (lut->m0d[1] & bit)
			m0d |= 1 << 1;
		if (lut->m0d[0] & bit)
			m0d |= 1 << 0;

		/*
		 * Send 16bits with M3D/M2D and M1D/M0D bit masks to the
		 * TriggerSelect register, then strobe the LUT write by
		 * passing A3-A0 to TriggerSelect2. Hold RESET during LUT
		 * programming.
		 */
		wrptr = buf;
		write_u8_inc(&wrptr, (m3d << 4) | (m2d << 0));
		write_u8_inc(&wrptr, (m1d << 4) | (m0d << 0));
		ret = sigma_write_register(devc, WRITE_TRIGGER_SELECT,
			buf, wrptr - buf);
		if (ret != SR_OK)
			return ret;
		ret = sigma_set_register(devc, WRITE_TRIGGER_SELECT2,
			TRGSEL2_RESET | TRGSEL2_LUT_WRITE |
			(lut_addr & TRGSEL2_LUT_ADDR_MASK));
		if (ret != SR_OK)
			return ret;
	}

	/*
	 * Send the parameters. This covers counters and durations.
	 */
	wrptr = buf;
	regval = 0;
	regval |= (lut->params.selc & TRGSEL_SELC_MASK) << TRGSEL_SELC_SHIFT;
	regval |= (lut->params.selpresc & TRGSEL_SELPRESC_MASK) << TRGSEL_SELPRESC_SHIFT;
	write_u8_inc(&wrptr, regval);
	regval = 0;
	regval |= (lut->params.selinc & TRGSEL_SELINC_MASK) << TRGSEL_SELINC_SHIFT;
	regval |= (lut->params.selres & TRGSEL_SELRES_MASK) << TRGSEL_SELRES_SHIFT;
	regval |= (lut->params.sela & TRGSEL_SELA_MASK) << TRGSEL_SELA_SHIFT;
	regval |= (lut->params.selb & TRGSEL_SELB_MASK) << TRGSEL_SELB_SHIFT;
	write_u8_inc(&wrptr, regval);
	write_u16be_inc(&wrptr, lut->params.cmpb);
	write_u16be_inc(&wrptr, lut->params.cmpa);
	ret = sigma_write_register(devc, WRITE_TRIGGER_SELECT, buf, wrptr - buf);
	if (ret != SR_OK)
		return ret;

	return SR_OK;
}

/*
 * See Xilinx UG332 for Spartan-3 FPGA configuration. The SIGMA device
 * uses FTDI bitbang mode for netlist download in slave serial mode.
 * (LATER: The OMEGA device's cable contains a more capable FTDI chip
 * and uses MPSSE mode for bitbang. -- Can we also use FT232H in FT245
 * compatible bitbang mode? For maximum code re-use and reduced libftdi
 * dependency? See section 3.5.5 of FT232H: D0 clk, D1 data (out), D2
 * data (in), D3 select, D4-7 GPIOL. See section 3.5.7 for MCU FIFO.)
 *
 * 750kbps rate (four times the speed of sigmalogan) works well for
 * netlist download. All pins except INIT_B are output pins during
 * configuration download.
 *
 * Some pins are inverted as a byproduct of level shifting circuitry.
 * That's why high CCLK level (from the cable's point of view) is idle
 * from the FPGA's perspective.
 *
 * The vendor's literature discusses a "suicide sequence" which ends
 * regular FPGA execution and should be sent before entering bitbang
 * mode and sending configuration data. Set D7 and toggle D2, D3, D4
 * a few times.
 */
#define BB_PIN_CCLK (1 << 0) /* D0, CCLK */
#define BB_PIN_PROG (1 << 1) /* D1, PROG */
#define BB_PIN_D2   (1 << 2) /* D2, (part of) SUICIDE */
#define BB_PIN_D3   (1 << 3) /* D3, (part of) SUICIDE */
#define BB_PIN_D4   (1 << 4) /* D4, (part of) SUICIDE (unused?) */
#define BB_PIN_INIT (1 << 5) /* D5, INIT, input pin */
#define BB_PIN_DIN  (1 << 6) /* D6, DIN */
#define BB_PIN_D7   (1 << 7) /* D7, (part of) SUICIDE */

#define BB_BITRATE (750 * 1000)
#define BB_PINMASK (0xff & ~BB_PIN_INIT)

/*
 * Initiate slave serial mode for configuration download. Which is done
 * by pulsing PROG_B and sensing INIT_B. Make sure CCLK is idle before
 * initiating the configuration download.
 *
 * Run a "suicide sequence" first to terminate the regular FPGA operation
 * before reconfiguration. The FTDI cable is single channel, and shares
 * pins which are used for data communication in FIFO mode with pins that
 * are used for FPGA configuration in bitbang mode. Hardware defaults for
 * unconfigured hardware, and runtime conditions after FPGA configuration
 * need to cooperate such that re-configuration of the FPGA can start.
 */
static int sigma_fpga_init_bitbang_once(struct dev_context *devc)
{
	const uint8_t suicide[] = {
		BB_PIN_D7 | BB_PIN_D2,
		BB_PIN_D7 | BB_PIN_D2,
		BB_PIN_D7 |           BB_PIN_D3,
		BB_PIN_D7 | BB_PIN_D2,
		BB_PIN_D7 |           BB_PIN_D3,
		BB_PIN_D7 | BB_PIN_D2,
		BB_PIN_D7 |           BB_PIN_D3,
		BB_PIN_D7 | BB_PIN_D2,
	};
	const uint8_t init_array[] = {
		BB_PIN_CCLK,
		BB_PIN_CCLK | BB_PIN_PROG,
		BB_PIN_CCLK | BB_PIN_PROG,
		BB_PIN_CCLK,
		BB_PIN_CCLK,
		BB_PIN_CCLK,
		BB_PIN_CCLK,
		BB_PIN_CCLK,
		BB_PIN_CCLK,
		BB_PIN_CCLK,
	};
	int retries, ret;
	uint8_t data;

	/* Section 2. part 1), do the FPGA suicide. */
	ret = SR_OK;
	ret |= sigma_write_sr(devc, suicide, sizeof(suicide));
	ret |= sigma_write_sr(devc, suicide, sizeof(suicide));
	ret |= sigma_write_sr(devc, suicide, sizeof(suicide));
	ret |= sigma_write_sr(devc, suicide, sizeof(suicide));
	if (ret != SR_OK)
		return SR_ERR_IO;
	g_usleep(10 * 1000);

	/* Section 2. part 2), pulse PROG. */
	ret = sigma_write_sr(devc, init_array, sizeof(init_array));
	if (ret != SR_OK)
		return ret;
	g_usleep(10 * 1000);
	ftdi_usb_purge_buffers(&devc->ftdi.ctx);

	/*
	 * Wait until the FPGA asserts INIT_B. Check in a maximum number
	 * of bursts with a given delay between them. Read as many pin
	 * capture results as the combination of FTDI chip and FTID lib
	 * may provide. Cope with absence of pin capture data in a cycle.
	 * This approach shall result in fast reponse in case of success,
	 * low cost of execution during wait, reliable error handling in
	 * the transport layer, and robust response to failure or absence
	 * of result data (hardware inactivity after stimulus).
	 */
	retries = 10;
	while (retries--) {
		do {
			ret = sigma_read_raw(devc, &data, sizeof(data));
			if (ret < 0)
				return SR_ERR_IO;
			if (ret == sizeof(data) && (data & BB_PIN_INIT))
				return SR_OK;
		} while (ret == sizeof(data));
		if (retries)
			g_usleep(10 * 1000);
	}

	return SR_ERR_TIMEOUT;
}

/*
 * This is belt and braces. Re-run the bitbang initiation sequence a few
 * times should first attempts fail. Failure is rare but can happen (was
 * observed during driver development).
 */
static int sigma_fpga_init_bitbang(struct dev_context *devc)
{
	size_t retries;
	int ret;

	retries = 10;
	while (retries--) {
		ret = sigma_fpga_init_bitbang_once(devc);
		if (ret == SR_OK)
			return ret;
		if (ret != SR_ERR_TIMEOUT)
			return ret;
	}
	return ret;
}

/*
 * Configure the FPGA for logic-analyzer mode.
 */
static int sigma_fpga_init_la(struct dev_context *devc)
{
	uint8_t buf[20], *wrptr;
	uint8_t data_55, data_aa, mode;
	uint8_t result[3];
	const uint8_t *rdptr;
	int ret;

	wrptr = buf;

	/* Read ID register. */
	write_u8_inc(&wrptr, REG_ADDR_LOW | LO4(READ_ID));
	write_u8_inc(&wrptr, REG_ADDR_HIGH | HI4(READ_ID));
	write_u8_inc(&wrptr, REG_READ_ADDR);

	/* Write 0x55 to scratch register, read back. */
	data_55 = 0x55;
	write_u8_inc(&wrptr, REG_ADDR_LOW | LO4(WRITE_TEST));
	write_u8_inc(&wrptr, REG_ADDR_HIGH | HI4(WRITE_TEST));
	write_u8_inc(&wrptr, REG_DATA_LOW | LO4(data_55));
	write_u8_inc(&wrptr, REG_DATA_HIGH_WRITE | HI4(data_55));
	write_u8_inc(&wrptr, REG_READ_ADDR);

	/* Write 0xaa to scratch register, read back. */
	data_aa = 0xaa;
	write_u8_inc(&wrptr, REG_ADDR_LOW | LO4(WRITE_TEST));
	write_u8_inc(&wrptr, REG_ADDR_HIGH | HI4(WRITE_TEST));
	write_u8_inc(&wrptr, REG_DATA_LOW | LO4(data_aa));
	write_u8_inc(&wrptr, REG_DATA_HIGH_WRITE | HI4(data_aa));
	write_u8_inc(&wrptr, REG_READ_ADDR);

	/* Initiate SDRAM initialization in mode register. */
	mode = WMR_SDRAMINIT;
	write_u8_inc(&wrptr, REG_ADDR_LOW | LO4(WRITE_MODE));
	write_u8_inc(&wrptr, REG_ADDR_HIGH | HI4(WRITE_MODE));
	write_u8_inc(&wrptr, REG_DATA_LOW | LO4(mode));
	write_u8_inc(&wrptr, REG_DATA_HIGH_WRITE | HI4(mode));

	/*
	 * Send the command sequence which contains 3 READ requests.
	 * Expect to see the corresponding 3 response bytes.
	 */
	ret = sigma_write_sr(devc, buf, wrptr - buf);
	if (ret != SR_OK) {
		sr_err("Could not request LA start response.");
		return ret;
	}
	ret = sigma_read_sr(devc, result, ARRAY_SIZE(result));
	if (ret != SR_OK) {
		sr_err("Could not receive LA start response.");
		return SR_ERR_IO;
	}
	rdptr = result;
	if (read_u8_inc(&rdptr) != 0xa6) {
		sr_err("Unexpected ID response.");
		return SR_ERR_DATA;
	}
	if (read_u8_inc(&rdptr) != data_55) {
		sr_err("Unexpected scratch read-back (55).");
		return SR_ERR_DATA;
	}
	if (read_u8_inc(&rdptr) != data_aa) {
		sr_err("Unexpected scratch read-back (aa).");
		return SR_ERR_DATA;
	}

	return SR_OK;
}

/*
 * Read the firmware from a file and transform it into a series of bitbang
 * pulses used to program the FPGA. Note that the *bb_cmd must be free()'d
 * by the caller of this function.
 */
static int sigma_fw_2_bitbang(struct sr_context *ctx, const char *name,
	uint8_t **bb_cmd, gsize *bb_cmd_size)
{
	uint8_t *firmware;
	size_t file_size;
	uint8_t *p;
	size_t l;
	uint32_t imm;
	size_t bb_size;
	uint8_t *bb_stream, *bbs, byte, mask, v;

	/* Retrieve the on-disk firmware file content. */
	firmware = sr_resource_load(ctx, SR_RESOURCE_FIRMWARE, name,
		&file_size, SIGMA_FIRMWARE_SIZE_LIMIT);
	if (!firmware)
		return SR_ERR_IO;

	/* Unscramble the file content (XOR with "random" sequence). */
	p = firmware;
	l = file_size;
	imm = 0x3f6df2ab;
	while (l--) {
		imm = (imm + 0xa853753) % 177 + (imm * 0x8034052);
		*p++ ^= imm & 0xff;
	}

	/*
	 * Generate a sequence of bitbang samples. With two samples per
	 * FPGA configuration bit, providing the level for the DIN signal
	 * as well as two edges for CCLK. See Xilinx UG332 for details
	 * ("slave serial" mode).
	 *
	 * Note that CCLK is inverted in hardware. That's why the
	 * respective bit is first set and then cleared in the bitbang
	 * sample sets. So that the DIN level will be stable when the
	 * data gets sampled at the rising CCLK edge, and the signals'
	 * setup time constraint will be met.
	 *
	 * The caller will put the FPGA into download mode, will send
	 * the bitbang samples, and release the allocated memory.
	 */
	bb_size = file_size * 8 * 2;
	bb_stream = g_try_malloc(bb_size);
	if (!bb_stream) {
		sr_err("Memory allocation failed during firmware upload.");
		g_free(firmware);
		return SR_ERR_MALLOC;
	}
	bbs = bb_stream;
	p = firmware;
	l = file_size;
	while (l--) {
		byte = *p++;
		mask = 0x80;
		while (mask) {
			v = (byte & mask) ? BB_PIN_DIN : 0;
			mask >>= 1;
			*bbs++ = v | BB_PIN_CCLK;
			*bbs++ = v;
		}
	}
	g_free(firmware);

	/* The transformation completed successfully, return the result. */
	*bb_cmd = bb_stream;
	*bb_cmd_size = bb_size;

	return SR_OK;
}

static int upload_firmware(struct sr_context *ctx, struct dev_context *devc,
	enum sigma_firmware_idx firmware_idx)
{
	int ret;
	uint8_t *buf;
	uint8_t pins;
	size_t buf_size;
	const char *firmware;

	/* Check for valid firmware file selection. */
	if (firmware_idx >= ARRAY_SIZE(firmware_files))
		return SR_ERR_ARG;
	firmware = firmware_files[firmware_idx];
	if (!firmware || !*firmware)
		return SR_ERR_ARG;

	/* Avoid downloading the same firmware multiple times. */
	if (devc->firmware_idx == firmware_idx) {
		sr_info("Not uploading firmware file '%s' again.", firmware);
		return SR_OK;
	}

	devc->state.state = SIGMA_CONFIG;

	/* Set the cable to bitbang mode. */
	ret = ftdi_set_bitmode(&devc->ftdi.ctx, BB_PINMASK, BITMODE_BITBANG);
	if (ret < 0) {
		sr_err("Could not setup cable mode for upload: %s",
			ftdi_get_error_string(&devc->ftdi.ctx));
		return SR_ERR;
	}
	ret = ftdi_set_baudrate(&devc->ftdi.ctx, BB_BITRATE);
	if (ret < 0) {
		sr_err("Could not setup bitrate for upload: %s",
			ftdi_get_error_string(&devc->ftdi.ctx));
		return SR_ERR;
	}

	/* Initiate FPGA configuration mode. */
	ret = sigma_fpga_init_bitbang(devc);
	if (ret) {
		sr_err("Could not initiate firmware upload to hardware");
		return ret;
	}

	/* Prepare wire format of the firmware image. */
	ret = sigma_fw_2_bitbang(ctx, firmware, &buf, &buf_size);
	if (ret != SR_OK) {
		sr_err("Could not prepare file %s for upload.", firmware);
		return ret;
	}

	/* Write the FPGA netlist to the cable. */
	sr_info("Uploading firmware file '%s'.", firmware);
	ret = sigma_write_sr(devc, buf, buf_size);
	g_free(buf);
	if (ret != SR_OK) {
		sr_err("Could not upload firmware file '%s'.", firmware);
		return ret;
	}

	/* Leave bitbang mode and discard pending input data. */
	ret = ftdi_set_bitmode(&devc->ftdi.ctx, 0, BITMODE_RESET);
	if (ret < 0) {
		sr_err("Could not setup cable mode after upload: %s",
			ftdi_get_error_string(&devc->ftdi.ctx));
		return SR_ERR;
	}
	ftdi_usb_purge_buffers(&devc->ftdi.ctx);
	while (sigma_read_raw(devc, &pins, sizeof(pins)) > 0)
		;

	/* Initialize the FPGA for logic-analyzer mode. */
	ret = sigma_fpga_init_la(devc);
	if (ret != SR_OK) {
		sr_err("Hardware response after firmware upload failed.");
		return ret;
	}

	/* Keep track of successful firmware download completion. */
	devc->state.state = SIGMA_IDLE;
	devc->firmware_idx = firmware_idx;
	sr_info("Firmware uploaded.");

	return SR_OK;
}

/*
 * The driver supports user specified time or sample count limits. The
 * device's hardware supports neither, and hardware compression prevents
 * reliable detection of "fill levels" (currently reached sample counts)
 * from register values during acquisition. That's why the driver needs
 * to apply some heuristics:
 *
 * - The (optional) sample count limit and the (normalized) samplerate
 *   get mapped to an estimated duration for these samples' acquisition.
 * - The (optional) time limit gets checked as well. The lesser of the
 *   two limits will terminate the data acquisition phase. The exact
 *   sample count limit gets enforced in session feed submission paths.
 * - Some slack needs to be given to account for hardware pipelines as
 *   well as late storage of last chunks after compression thresholds
 *   are tripped. The resulting data set will span at least the caller
 *   specified period of time, which shall be perfectly acceptable.
 *
 * With RLE compression active, up to 64K sample periods can pass before
 * a cluster accumulates. Which translates to 327ms at 200kHz. Add two
 * times that period for good measure, one is not enough to flush the
 * hardware pipeline (observation from an earlier experiment).
 */
SR_PRIV int sigma_set_acquire_timeout(struct dev_context *devc)
{
	int ret;
	GVariant *data;
	uint64_t user_count, user_msecs;
	uint64_t worst_cluster_time_ms;
	uint64_t count_msecs, acquire_msecs;

	sr_sw_limits_init(&devc->acq_limits);

	/* Get sample count limit, convert to msecs. */
	ret = sr_sw_limits_config_get(&devc->cfg_limits,
		SR_CONF_LIMIT_SAMPLES, &data);
	if (ret != SR_OK)
		return ret;
	user_count = g_variant_get_uint64(data);
	g_variant_unref(data);
	count_msecs = 0;
	if (user_count)
		count_msecs = 1000 * user_count / devc->samplerate + 1;

	/* Get time limit, which is in msecs. */
	ret = sr_sw_limits_config_get(&devc->cfg_limits,
		SR_CONF_LIMIT_MSEC, &data);
	if (ret != SR_OK)
		return ret;
	user_msecs = g_variant_get_uint64(data);
	g_variant_unref(data);

	/* Get the lesser of them, with both being optional. */
	acquire_msecs = ~0ull;
	if (user_count && count_msecs < acquire_msecs)
		acquire_msecs = count_msecs;
	if (user_msecs && user_msecs < acquire_msecs)
		acquire_msecs = user_msecs;
	if (acquire_msecs == ~0ull)
		return SR_OK;

	/* Add some slack, and use that timeout for acquisition. */
	worst_cluster_time_ms = 1000 * 65536 / devc->samplerate;
	acquire_msecs += 2 * worst_cluster_time_ms;
	data = g_variant_new_uint64(acquire_msecs);
	ret = sr_sw_limits_config_set(&devc->acq_limits,
		SR_CONF_LIMIT_MSEC, data);
	g_variant_unref(data);
	if (ret != SR_OK)
		return ret;

	sr_sw_limits_acquisition_start(&devc->acq_limits);
	return SR_OK;
}

/*
 * Check whether a caller specified samplerate matches the device's
 * hardware constraints (can be used for acquisition). Optionally yield
 * a value that approximates the original spec.
 *
 * This routine assumes that input specs are in the 200kHz to 200MHz
 * range of supported rates, and callers typically want to normalize a
 * given value to the hardware capabilities. Values in the 50MHz range
 * get rounded up by default, to avoid a more expensive check for the
 * closest match, while higher sampling rate is always desirable during
 * measurement. Input specs which exactly match hardware capabilities
 * remain unaffected. Because 100/200MHz rates also limit the number of
 * available channels, they are not suggested by this routine, instead
 * callers need to pick them consciously.
 */
SR_PRIV int sigma_normalize_samplerate(uint64_t want_rate, uint64_t *have_rate)
{
	uint64_t div, rate;

	/* Accept exact matches for 100/200MHz. */
	if (want_rate == SR_MHZ(200) || want_rate == SR_MHZ(100)) {
		if (have_rate)
			*have_rate = want_rate;
		return SR_OK;
	}

	/* Accept 200kHz to 50MHz range, and map to near value. */
	if (want_rate >= SR_KHZ(200) && want_rate <= SR_MHZ(50)) {
		div = SR_MHZ(50) / want_rate;
		rate = SR_MHZ(50) / div;
		if (have_rate)
			*have_rate = rate;
		return SR_OK;
	}

	return SR_ERR_ARG;
}

SR_PRIV uint64_t sigma_get_samplerate(const struct sr_dev_inst *sdi)
{
	/* TODO Retrieve value from hardware. */
	(void)sdi;
	return samplerates[0];
}

SR_PRIV int sigma_set_samplerate(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct drv_context *drvc;
	uint64_t samplerate;
	int ret;
	int num_channels;

	devc = sdi->priv;
	drvc = sdi->driver->context;

	/* Accept any caller specified rate which the hardware supports. */
	ret = sigma_normalize_samplerate(devc->samplerate, &samplerate);
	if (ret != SR_OK)
		return ret;

	/*
	 * Depending on the samplerates of 200/100/50- MHz, specific
	 * firmware is required and higher rates might limit the set
	 * of available channels.
	 */
	num_channels = devc->num_channels;
	if (samplerate <= SR_MHZ(50)) {
		ret = upload_firmware(drvc->sr_ctx, devc, SIGMA_FW_50MHZ);
		num_channels = 16;
	} else if (samplerate == SR_MHZ(100)) {
		ret = upload_firmware(drvc->sr_ctx, devc, SIGMA_FW_100MHZ);
		num_channels = 8;
	} else if (samplerate == SR_MHZ(200)) {
		ret = upload_firmware(drvc->sr_ctx, devc, SIGMA_FW_200MHZ);
		num_channels = 4;
	}

	/*
	 * The samplerate affects the number of available logic channels
	 * as well as a sample memory layout detail (the number of samples
	 * which the device will communicate within an "event").
	 */
	if (ret == SR_OK) {
		devc->num_channels = num_channels;
		devc->samples_per_event = 16 / devc->num_channels;
	}

	return ret;
}

/*
 * Arrange for a session feed submit buffer. A queue where a number of
 * samples gets accumulated to reduce the number of send calls. Which
 * also enforces an optional sample count limit for data acquisition.
 *
 * The buffer holds up to CHUNK_SIZE bytes. The unit size is fixed (the
 * driver provides a fixed channel layout regardless of samplerate).
 */

#define CHUNK_SIZE	(4 * 1024 * 1024)

struct submit_buffer {
	size_t unit_size;
	size_t max_samples, curr_samples;
	uint8_t *sample_data;
	uint8_t *write_pointer;
	struct sr_dev_inst *sdi;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_logic logic;
};

static int alloc_submit_buffer(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct submit_buffer *buffer;
	size_t size;

	devc = sdi->priv;

	buffer = g_malloc0(sizeof(*buffer));
	devc->buffer = buffer;

	buffer->unit_size = sizeof(uint16_t);
	size = CHUNK_SIZE;
	size /= buffer->unit_size;
	buffer->max_samples = size;
	size *= buffer->unit_size;
	buffer->sample_data = g_try_malloc0(size);
	if (!buffer->sample_data)
		return SR_ERR_MALLOC;
	buffer->write_pointer = buffer->sample_data;
	sr_sw_limits_init(&devc->feed_limits);

	buffer->sdi = sdi;
	memset(&buffer->logic, 0, sizeof(buffer->logic));
	buffer->logic.unitsize = buffer->unit_size;
	buffer->logic.data = buffer->sample_data;
	memset(&buffer->packet, 0, sizeof(buffer->packet));
	buffer->packet.type = SR_DF_LOGIC;
	buffer->packet.payload = &buffer->logic;

	return SR_OK;
}

static int setup_submit_limit(struct dev_context *devc)
{
	struct sr_sw_limits *limits;
	int ret;
	GVariant *data;
	uint64_t total;

	limits = &devc->feed_limits;

	ret = sr_sw_limits_config_get(&devc->cfg_limits,
		SR_CONF_LIMIT_SAMPLES, &data);
	if (ret != SR_OK)
		return ret;
	total = g_variant_get_uint64(data);
	g_variant_unref(data);

	sr_sw_limits_init(limits);
	if (total) {
		data = g_variant_new_uint64(total);
		ret = sr_sw_limits_config_set(limits,
			SR_CONF_LIMIT_SAMPLES, data);
		g_variant_unref(data);
		if (ret != SR_OK)
			return ret;
	}

	sr_sw_limits_acquisition_start(limits);

	return SR_OK;
}

static void free_submit_buffer(struct dev_context *devc)
{
	struct submit_buffer *buffer;

	if (!devc)
		return;

	buffer = devc->buffer;
	if (!buffer)
		return;
	devc->buffer = NULL;

	g_free(buffer->sample_data);
	g_free(buffer);
}

static int flush_submit_buffer(struct dev_context *devc)
{
	struct submit_buffer *buffer;
	int ret;

	buffer = devc->buffer;

	/* Is queued sample data available? */
	if (!buffer->curr_samples)
		return SR_OK;

	/* Submit to the session feed. */
	buffer->logic.length = buffer->curr_samples * buffer->unit_size;
	ret = sr_session_send(buffer->sdi, &buffer->packet);
	if (ret != SR_OK)
		return ret;

	/* Rewind queue position. */
	buffer->curr_samples = 0;
	buffer->write_pointer = buffer->sample_data;

	return SR_OK;
}

static int addto_submit_buffer(struct dev_context *devc,
	uint16_t sample, size_t count)
{
	struct submit_buffer *buffer;
	struct sr_sw_limits *limits;
	int ret;

	buffer = devc->buffer;
	limits = &devc->feed_limits;
	if (sr_sw_limits_check(limits))
		count = 0;

	/*
	 * Individually accumulate and check each sample, such that
	 * accumulation between flushes won't exceed local storage, and
	 * enforcement of user specified limits is exact.
	 */
	while (count--) {
		write_u16le_inc(&buffer->write_pointer, sample);
		buffer->curr_samples++;
		if (buffer->curr_samples == buffer->max_samples) {
			ret = flush_submit_buffer(devc);
			if (ret != SR_OK)
				return ret;
		}
		sr_sw_limits_update_samples_read(limits, 1);
		if (sr_sw_limits_check(limits))
			break;
	}

	return SR_OK;
}

/*
 * In 100 and 200 MHz mode, only a single pin rising/falling can be
 * set as trigger. In other modes, two rising/falling triggers can be set,
 * in addition to value/mask trigger for any number of channels.
 *
 * The Sigma supports complex triggers using boolean expressions, but this
 * has not been implemented yet.
 */
SR_PRIV int sigma_convert_trigger(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_trigger *trigger;
	struct sr_trigger_stage *stage;
	struct sr_trigger_match *match;
	const GSList *l, *m;
	int channelbit, trigger_set;

	devc = sdi->priv;
	memset(&devc->trigger, 0, sizeof(devc->trigger));
	trigger = sr_session_trigger_get(sdi->session);
	if (!trigger)
		return SR_OK;

	trigger_set = 0;
	for (l = trigger->stages; l; l = l->next) {
		stage = l->data;
		for (m = stage->matches; m; m = m->next) {
			match = m->data;
			/* Ignore disabled channels with a trigger. */
			if (!match->channel->enabled)
				continue;
			channelbit = 1 << match->channel->index;
			if (devc->samplerate >= SR_MHZ(100)) {
				/* Fast trigger support. */
				if (trigger_set) {
					sr_err("100/200MHz modes limited to single trigger pin.");
					return SR_ERR;
				}
				if (match->match == SR_TRIGGER_FALLING) {
					devc->trigger.fallingmask |= channelbit;
				} else if (match->match == SR_TRIGGER_RISING) {
					devc->trigger.risingmask |= channelbit;
				} else {
					sr_err("100/200MHz modes limited to edge trigger.");
					return SR_ERR;
				}

				trigger_set++;
			} else {
				/* Simple trigger support (event). */
				if (match->match == SR_TRIGGER_ONE) {
					devc->trigger.simplevalue |= channelbit;
					devc->trigger.simplemask |= channelbit;
				} else if (match->match == SR_TRIGGER_ZERO) {
					devc->trigger.simplevalue &= ~channelbit;
					devc->trigger.simplemask |= channelbit;
				} else if (match->match == SR_TRIGGER_FALLING) {
					devc->trigger.fallingmask |= channelbit;
					trigger_set++;
				} else if (match->match == SR_TRIGGER_RISING) {
					devc->trigger.risingmask |= channelbit;
					trigger_set++;
				}

				/*
				 * Actually, Sigma supports 2 rising/falling triggers,
				 * but they are ORed and the current trigger syntax
				 * does not permit ORed triggers.
				 */
				if (trigger_set > 1) {
					sr_err("Limited to 1 edge trigger.");
					return SR_ERR;
				}
			}
		}
	}

	return SR_OK;
}

/* Software trigger to determine exact trigger position. */
static int get_trigger_offset(uint8_t *samples, uint16_t last_sample,
	struct sigma_trigger *t)
{
	const uint8_t *rdptr;
	int i;
	uint16_t sample;

	rdptr = samples;
	sample = 0;
	for (i = 0; i < 8; i++) {
		if (i > 0)
			last_sample = sample;
		sample = read_u16le_inc(&rdptr);

		/* Simple triggers. */
		if ((sample & t->simplemask) != t->simplevalue)
			continue;

		/* Rising edge. */
		if (((last_sample & t->risingmask) != 0) ||
		    ((sample & t->risingmask) != t->risingmask))
			continue;

		/* Falling edge. */
		if ((last_sample & t->fallingmask) != t->fallingmask ||
		    (sample & t->fallingmask) != 0)
			continue;

		break;
	}

	/* If we did not match, return original trigger pos. */
	return i & 0x7;
}

static gboolean sample_matches_trigger(struct dev_context *devc, uint16_t sample)
{
	/* TODO
	 * Check whether the combination of this very sample and the
	 * previous state match the configured trigger condition. This
	 * improves the resolution of the trigger marker's position.
	 * The hardware provided position is coarse, and may point to
	 * a position before the actual match.
	 *
	 * See the previous get_trigger_offset() implementation. This
	 * code needs to get re-used here.
	 */
	(void)devc;
	(void)sample;
	(void)get_trigger_offset;

	return FALSE;
}

static int check_and_submit_sample(struct dev_context *devc,
	uint16_t sample, size_t count, gboolean check_trigger)
{
	gboolean triggered;
	int ret;

	triggered = check_trigger && sample_matches_trigger(devc, sample);
	if (triggered) {
		ret = flush_submit_buffer(devc);
		if (ret != SR_OK)
			return ret;
		ret = std_session_send_df_trigger(devc->buffer->sdi);
		if (ret != SR_OK)
			return ret;
	}

	ret = addto_submit_buffer(devc, sample, count);
	if (ret != SR_OK)
		return ret;

	return SR_OK;
}

/*
 * Return the timestamp of "DRAM cluster".
 */
static uint16_t sigma_dram_cluster_ts(struct sigma_dram_cluster *cluster)
{
	return read_u16le((const uint8_t *)&cluster->timestamp);
}

/*
 * Return one 16bit data entity of a DRAM cluster at the specified index.
 */
static uint16_t sigma_dram_cluster_data(struct sigma_dram_cluster *cl, int idx)
{
	return read_u16le((const uint8_t *)&cl->samples[idx]);
}

/*
 * Deinterlace sample data that was retrieved at 100MHz samplerate.
 * One 16bit item contains two samples of 8bits each. The bits of
 * multiple samples are interleaved.
 */
static uint16_t sigma_deinterlace_100mhz_data(uint16_t indata, int idx)
{
	uint16_t outdata;

	indata >>= idx;
	outdata = 0;
	outdata |= (indata >> (0 * 2 - 0)) & (1 << 0);
	outdata |= (indata >> (1 * 2 - 1)) & (1 << 1);
	outdata |= (indata >> (2 * 2 - 2)) & (1 << 2);
	outdata |= (indata >> (3 * 2 - 3)) & (1 << 3);
	outdata |= (indata >> (4 * 2 - 4)) & (1 << 4);
	outdata |= (indata >> (5 * 2 - 5)) & (1 << 5);
	outdata |= (indata >> (6 * 2 - 6)) & (1 << 6);
	outdata |= (indata >> (7 * 2 - 7)) & (1 << 7);
	return outdata;
}

/*
 * Deinterlace sample data that was retrieved at 200MHz samplerate.
 * One 16bit item contains four samples of 4bits each. The bits of
 * multiple samples are interleaved.
 */
static uint16_t sigma_deinterlace_200mhz_data(uint16_t indata, int idx)
{
	uint16_t outdata;

	indata >>= idx;
	outdata = 0;
	outdata |= (indata >> (0 * 4 - 0)) & (1 << 0);
	outdata |= (indata >> (1 * 4 - 1)) & (1 << 1);
	outdata |= (indata >> (2 * 4 - 2)) & (1 << 2);
	outdata |= (indata >> (3 * 4 - 3)) & (1 << 3);
	return outdata;
}

static void sigma_decode_dram_cluster(struct dev_context *devc,
	struct sigma_dram_cluster *dram_cluster,
	size_t events_in_cluster, gboolean triggered)
{
	struct sigma_state *ss;
	uint16_t tsdiff, ts, sample, item16;
	unsigned int i;

	if (!devc->use_triggers || !ASIX_SIGMA_WITH_TRIGGER)
		triggered = FALSE;

	/*
	 * If this cluster is not adjacent to the previously received
	 * cluster, then send the appropriate number of samples with the
	 * previous values to the sigrok session. This "decodes RLE".
	 *
	 * These samples cannot match the trigger since they just repeat
	 * the previously submitted data pattern. (This assumption holds
	 * for simple level and edge triggers. It would not for timed or
	 * counted conditions, which currently are not supported.)
	 */
	ss = &devc->state;
	ts = sigma_dram_cluster_ts(dram_cluster);
	tsdiff = ts - ss->lastts;
	if (tsdiff > 0) {
		size_t count;
		sample = ss->lastsample;
		count = tsdiff * devc->samples_per_event;
		(void)check_and_submit_sample(devc, sample, count, FALSE);
	}
	ss->lastts = ts + EVENTS_PER_CLUSTER;

	/*
	 * Grab sample data from the current cluster and prepare their
	 * submission to the session feed. Handle samplerate dependent
	 * memory layout of sample data. Accumulation of data chunks
	 * before submission is transparent to this code path, specific
	 * buffer depth is neither assumed nor required here.
	 */
	sample = 0;
	for (i = 0; i < events_in_cluster; i++) {
		item16 = sigma_dram_cluster_data(dram_cluster, i);
		if (devc->samplerate == SR_MHZ(200)) {
			sample = sigma_deinterlace_200mhz_data(item16, 0);
			check_and_submit_sample(devc, sample, 1, triggered);
			sample = sigma_deinterlace_200mhz_data(item16, 1);
			check_and_submit_sample(devc, sample, 1, triggered);
			sample = sigma_deinterlace_200mhz_data(item16, 2);
			check_and_submit_sample(devc, sample, 1, triggered);
			sample = sigma_deinterlace_200mhz_data(item16, 3);
			check_and_submit_sample(devc, sample, 1, triggered);
		} else if (devc->samplerate == SR_MHZ(100)) {
			sample = sigma_deinterlace_100mhz_data(item16, 0);
			check_and_submit_sample(devc, sample, 1, triggered);
			sample = sigma_deinterlace_100mhz_data(item16, 1);
			check_and_submit_sample(devc, sample, 1, triggered);
		} else {
			sample = item16;
			check_and_submit_sample(devc, sample, 1, triggered);
		}
	}
	ss->lastsample = sample;
}

/*
 * Decode chunk of 1024 bytes, 64 clusters, 7 events per cluster.
 * Each event is 20ns apart, and can contain multiple samples.
 *
 * For 200 MHz, events contain 4 samples for each channel, spread 5 ns apart.
 * For 100 MHz, events contain 2 samples for each channel, spread 10 ns apart.
 * For 50 MHz and below, events contain one sample for each channel,
 * spread 20 ns apart.
 */
static int decode_chunk_ts(struct dev_context *devc,
	struct sigma_dram_line *dram_line,
	size_t events_in_line, size_t trigger_event)
{
	struct sigma_dram_cluster *dram_cluster;
	unsigned int clusters_in_line;
	unsigned int events_in_cluster;
	unsigned int i;
	uint32_t trigger_cluster;

	clusters_in_line = events_in_line;
	clusters_in_line += EVENTS_PER_CLUSTER - 1;
	clusters_in_line /= EVENTS_PER_CLUSTER;
	trigger_cluster = ~0;

	/* Check if trigger is in this chunk. */
	if (trigger_event < EVENTS_PER_ROW) {
		if (devc->samplerate <= SR_MHZ(50)) {
			trigger_event -= MIN(EVENTS_PER_CLUSTER - 1,
					     trigger_event);
		}

		/* Find in which cluster the trigger occurred. */
		trigger_cluster = trigger_event / EVENTS_PER_CLUSTER;
	}

	/* For each full DRAM cluster. */
	for (i = 0; i < clusters_in_line; i++) {
		dram_cluster = &dram_line->cluster[i];

		/* The last cluster might not be full. */
		if ((i == clusters_in_line - 1) &&
		    (events_in_line % EVENTS_PER_CLUSTER)) {
			events_in_cluster = events_in_line % EVENTS_PER_CLUSTER;
		} else {
			events_in_cluster = EVENTS_PER_CLUSTER;
		}

		sigma_decode_dram_cluster(devc, dram_cluster,
			events_in_cluster, i == trigger_cluster);
	}

	return SR_OK;
}

static int download_capture(struct sr_dev_inst *sdi)
{
	const uint32_t chunks_per_read = 32;

	struct dev_context *devc;
	struct sigma_dram_line *dram_line;
	uint32_t stoppos, triggerpos;
	uint8_t modestatus;
	uint32_t i;
	uint32_t dl_lines_total, dl_lines_curr, dl_lines_done;
	uint32_t dl_first_line, dl_line;
	uint32_t dl_events_in_line, trigger_event;
	uint32_t trg_line, trg_event;
	int ret;

	devc = sdi->priv;

	sr_info("Downloading sample data.");
	devc->state.state = SIGMA_DOWNLOAD;

	/*
	 * Ask the hardware to stop data acquisition. Reception of the
	 * FORCESTOP request makes the hardware "disable RLE" (store
	 * clusters to DRAM regardless of whether pin state changes) and
	 * raise the POSTTRIGGERED flag.
	 */
	modestatus = WMR_FORCESTOP | WMR_SDRAMWRITEEN;
	ret = sigma_set_register(devc, WRITE_MODE, modestatus);
	if (ret != SR_OK)
		return ret;
	do {
		ret = sigma_get_register(devc, READ_MODE, &modestatus);
		if (ret != SR_OK) {
			sr_err("Could not poll for post-trigger state.");
			return FALSE;
		}
	} while (!(modestatus & RMR_POSTTRIGGERED));

	/* Set SDRAM Read Enable. */
	ret = sigma_set_register(devc, WRITE_MODE, WMR_SDRAMREADEN);
	if (ret != SR_OK)
		return ret;

	/* Get the current position. Check if trigger has fired. */
	ret = sigma_read_pos(devc, &stoppos, &triggerpos, &modestatus);
	if (ret != SR_OK) {
		sr_err("Could not query capture positions/state.");
		return FALSE;
	}
	trg_line = ~0;
	trg_event = ~0;
	if (modestatus & RMR_TRIGGERED) {
		trg_line = triggerpos >> ROW_SHIFT;
		trg_event = triggerpos & ROW_MASK;
	}

	/*
	 * Determine how many "DRAM lines" of 1024 bytes each we need to
	 * retrieve from the Sigma hardware, so that we have a complete
	 * set of samples. Note that the last line need not contain 64
	 * clusters, it might be partially filled only.
	 *
	 * When RMR_ROUND is set, the circular buffer in DRAM has wrapped
	 * around. Since the status of the very next line is uncertain in
	 * that case, we skip it and start reading from the next line.
	 */
	dl_first_line = 0;
	dl_lines_total = (stoppos >> ROW_SHIFT) + 1;
	if (modestatus & RMR_ROUND) {
		dl_first_line = dl_lines_total + 1;
		dl_lines_total = ROW_COUNT - 2;
	}
	dram_line = g_try_malloc0(chunks_per_read * sizeof(*dram_line));
	if (!dram_line)
		return FALSE;
	ret = alloc_submit_buffer(sdi);
	if (ret != SR_OK)
		return FALSE;
	ret = setup_submit_limit(devc);
	if (ret != SR_OK)
		return FALSE;
	dl_lines_done = 0;
	while (dl_lines_total > dl_lines_done) {
		/* We can download only up-to 32 DRAM lines in one go! */
		dl_lines_curr = MIN(chunks_per_read, dl_lines_total - dl_lines_done);

		dl_line = dl_first_line + dl_lines_done;
		dl_line %= ROW_COUNT;
		ret = sigma_read_dram(devc, dl_line, dl_lines_curr,
			(uint8_t *)dram_line);
		if (ret != SR_OK)
			return FALSE;

		/* This is the first DRAM line, so find the initial timestamp. */
		if (dl_lines_done == 0) {
			devc->state.lastts =
				sigma_dram_cluster_ts(&dram_line[0].cluster[0]);
			devc->state.lastsample = 0;
		}

		for (i = 0; i < dl_lines_curr; i++) {
			/* The last "DRAM line" need not span its full length. */
			dl_events_in_line = EVENTS_PER_ROW;
			if (dl_lines_done + i == dl_lines_total - 1)
				dl_events_in_line = stoppos & ROW_MASK;

			/* Test if the trigger happened on this line. */
			trigger_event = ~0;
			if (dl_lines_done + i == trg_line)
				trigger_event = trg_event;

			decode_chunk_ts(devc, dram_line + i,
				dl_events_in_line, trigger_event);
		}

		dl_lines_done += dl_lines_curr;
	}
	flush_submit_buffer(devc);
	free_submit_buffer(devc);
	g_free(dram_line);

	std_session_send_df_end(sdi);

	devc->state.state = SIGMA_IDLE;
	sr_dev_acquisition_stop(sdi);

	return TRUE;
}

/*
 * Periodically check the Sigma status when in CAPTURE mode. This routine
 * checks whether the configured sample count or sample time have passed,
 * and will stop acquisition and download the acquired samples.
 */
static int sigma_capture_mode(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;

	devc = sdi->priv;
	if (sr_sw_limits_check(&devc->acq_limits))
		return download_capture(sdi);

	return TRUE;
}

SR_PRIV int sigma_receive_data(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;

	(void)fd;
	(void)revents;

	sdi = cb_data;
	devc = sdi->priv;

	if (devc->state.state == SIGMA_IDLE)
		return TRUE;

	/*
	 * When the application has requested to stop the acquisition,
	 * then immediately start downloading sample data. Otherwise
	 * keep checking configured limits which will terminate the
	 * acquisition and initiate download.
	 */
	if (devc->state.state == SIGMA_STOPPING)
		return download_capture(sdi);
	if (devc->state.state == SIGMA_CAPTURE)
		return sigma_capture_mode(sdi);

	return TRUE;
}

/* Build a LUT entry used by the trigger functions. */
static void build_lut_entry(uint16_t *lut_entry,
	uint16_t spec_value, uint16_t spec_mask)
{
	size_t quad, bitidx, ch;
	uint16_t quadmask, bitmask;
	gboolean spec_value_low, bit_idx_low;

	/*
	 * For each quad-channel-group, for each bit in the LUT (each
	 * bit pattern of the channel signals, aka LUT address), for
	 * each channel in the quad, setup the bit in the LUT entry.
	 *
	 * Start from all-ones in the LUT (true, always matches), then
	 * "pessimize the truthness" for specified conditions.
	 */
	for (quad = 0; quad < 4; quad++) {
		lut_entry[quad] = ~0;
		for (bitidx = 0; bitidx < 16; bitidx++) {
			for (ch = 0; ch < 4; ch++) {
				quadmask = 1 << ch;
				bitmask = quadmask << (quad * 4);
				if (!(spec_mask & bitmask))
					continue;
				/*
				 * This bit is part of the spec. The
				 * condition which gets checked here
				 * (got checked in all implementations
				 * so far) is uncertain. A bit position
				 * in the current index' number(!) is
				 * checked?
				 */
				spec_value_low = !(spec_value & bitmask);
				bit_idx_low = !(bitidx & quadmask);
				if (spec_value_low == bit_idx_low)
					continue;
				lut_entry[quad] &= ~(1 << bitidx);
			}
		}
	}
}

/* Add a logical function to LUT mask. */
static void add_trigger_function(enum triggerop oper, enum triggerfunc func,
	int index, int neg, uint16_t *mask)
{
	int i, j;
	int x[2][2], tmp, a, b, aset, bset, rset;

	memset(x, 0, sizeof(x));

	/* Trigger detect condition. */
	switch (oper) {
	case OP_LEVEL:
		x[0][1] = 1;
		x[1][1] = 1;
		break;
	case OP_NOT:
		x[0][0] = 1;
		x[1][0] = 1;
		break;
	case OP_RISE:
		x[0][1] = 1;
		break;
	case OP_FALL:
		x[1][0] = 1;
		break;
	case OP_RISEFALL:
		x[0][1] = 1;
		x[1][0] = 1;
		break;
	case OP_NOTRISE:
		x[1][1] = 1;
		x[0][0] = 1;
		x[1][0] = 1;
		break;
	case OP_NOTFALL:
		x[1][1] = 1;
		x[0][0] = 1;
		x[0][1] = 1;
		break;
	case OP_NOTRISEFALL:
		x[1][1] = 1;
		x[0][0] = 1;
		break;
	}

	/* Transpose if neg is set. */
	if (neg) {
		for (i = 0; i < 2; i++) {
			for (j = 0; j < 2; j++) {
				tmp = x[i][j];
				x[i][j] = x[1 - i][1 - j];
				x[1 - i][1 - j] = tmp;
			}
		}
	}

	/* Update mask with function. */
	for (i = 0; i < 16; i++) {
		a = (i >> (2 * index + 0)) & 1;
		b = (i >> (2 * index + 1)) & 1;

		aset = (*mask >> i) & 1;
		bset = x[b][a];

		rset = 0;
		if (func == FUNC_AND || func == FUNC_NAND)
			rset = aset & bset;
		else if (func == FUNC_OR || func == FUNC_NOR)
			rset = aset | bset;
		else if (func == FUNC_XOR || func == FUNC_NXOR)
			rset = aset ^ bset;

		if (func == FUNC_NAND || func == FUNC_NOR || func == FUNC_NXOR)
			rset = !rset;

		*mask &= ~(1 << i);

		if (rset)
			*mask |= 1 << i;
	}
}

/*
 * Build trigger LUTs used by 50 MHz and lower sample rates for supporting
 * simple pin change and state triggers. Only two transitions (rise/fall) can be
 * set at any time, but a full mask and value can be set (0/1).
 */
SR_PRIV int sigma_build_basic_trigger(struct dev_context *devc,
	struct triggerlut *lut)
{
	uint16_t masks[2];
	int bitidx, condidx;
	uint16_t value, mask;

	/* Start assuming simple triggers. */
	memset(lut, 0, sizeof(*lut));
	lut->m4 = 0xa000;
	lut->m3 = 0xffff;

	/* Process value/mask triggers. */
	value = devc->trigger.simplevalue;
	mask = devc->trigger.simplemask;
	build_lut_entry(lut->m2d, value, mask);

	/* Scan for and process rise/fall triggers. */
	memset(&masks, 0, sizeof(masks));
	condidx = 0;
	for (bitidx = 0; bitidx < 16; bitidx++) {
		mask = 1 << bitidx;
		value = devc->trigger.risingmask | devc->trigger.fallingmask;
		if (!(value & mask))
			continue;
		if (condidx == 0)
			build_lut_entry(lut->m0d, mask, mask);
		if (condidx == 1)
			build_lut_entry(lut->m1d, mask, mask);
		masks[condidx++] = mask;
		if (condidx == ARRAY_SIZE(masks))
			break;
	}

	/* Add glue logic for rise/fall triggers. */
	if (masks[0] || masks[1]) {
		lut->m3 = 0;
		if (masks[0] & devc->trigger.risingmask)
			add_trigger_function(OP_RISE, FUNC_OR, 0, 0, &lut->m3);
		if (masks[0] & devc->trigger.fallingmask)
			add_trigger_function(OP_FALL, FUNC_OR, 0, 0, &lut->m3);
		if (masks[1] & devc->trigger.risingmask)
			add_trigger_function(OP_RISE, FUNC_OR, 1, 0, &lut->m3);
		if (masks[1] & devc->trigger.fallingmask)
			add_trigger_function(OP_FALL, FUNC_OR, 1, 0, &lut->m3);
	}

	/* Triggertype: event. */
	lut->params.selres = 3;

	return SR_OK;
}
