/****************************************************************************
 *
 *   Copyright (c) 2019 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mag.cpp
 *
 * Driver for the ak09916 magnetometer within the Invensense icm20948
 *
 * @author Robert Dickenson
 *
 */

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/log.h>
#include <px4_platform_common/time.h>
#include <lib/perf/perf_counter.h>
#include <drivers/drv_hrt.h>

#include "ICM20948_mag.hpp"
#include "ICM20948.hpp"

// If interface is non-null, then it will used for interacting with the device.
// Otherwise, it will passthrough the parent ICM20948
ICM20948_mag::ICM20948_mag(ICM20948 *parent, enum Rotation rotation) :
	_px4_mag(parent->_interface->get_device_id(), (parent->_interface->external() ? ORB_PRIO_MAX : ORB_PRIO_HIGH), rotation),
	_parent(parent),
	_mag_overruns(perf_alloc(PC_COUNT, MODULE_NAME": mag_overruns")),
	_mag_overflows(perf_alloc(PC_COUNT, MODULE_NAME": mag_overflows")),
	_mag_errors(perf_alloc(PC_COUNT, MODULE_NAME": mag_errors"))
{
	_px4_mag.set_device_type(DRV_MAG_DEVTYPE_AK09916);
	_px4_mag.set_scale(ICM20948_MAG_RANGE_GA);
}

ICM20948_mag::~ICM20948_mag()
{
	perf_free(_mag_overruns);
	perf_free(_mag_overflows);
	perf_free(_mag_errors);
}

void
ICM20948_mag::_measure(hrt_abstime timestamp_sample, ak09916_regs data)
{
	/* Check if data ready is set.
	 * This is not described to be set in continuous mode according to the
	 * ICM20948 datasheet. However, the datasheet of the AK09916 recommends to
	 * check data ready before doing the read and before triggering the
	 * next measurement by reading ST2.
	 *
	 * If _measure is used in passthrough mode, all the data is already
	 * fetched, however, we should still not use the data if the data ready
	 * is not set. This has lead to intermittent spikes when the data was
	 * being updated while getting read.
	 */
	if (!(data.st1 & AK09916_ST1_DRDY)) {
		return;
	}

	_px4_mag.set_external(_parent->is_external());
	_px4_mag.set_temperature(_parent->_last_temperature);

	/*
	 * Align axes - Keeping consistent with the accel and gyro axes of the ICM20948 here, just aligning the magnetometer to them.
	 */
	_px4_mag.update(timestamp_sample, data.y, data.x, -data.z);
}

void
ICM20948_mag::set_passthrough(uint8_t reg, uint8_t size, uint8_t *out)
{
	uint8_t addr;

	_parent->write_reg(ICMREG_20948_I2C_SLV0_CTRL, 0); // ensure slave r/w is disabled before changing the registers

	if (out) {
		_parent->write_reg(ICMREG_20948_I2C_SLV0_DO, *out);
		addr = AK09916_I2C_ADDR;

	} else {
		addr = AK09916_I2C_ADDR | BIT_I2C_READ_FLAG;
	}

	_parent->write_reg(ICMREG_20948_I2C_SLV0_ADDR, addr);
	_parent->write_reg(ICMREG_20948_I2C_SLV0_REG,  reg);
	_parent->write_reg(ICMREG_20948_I2C_SLV0_CTRL, size | BIT_I2C_SLV0_EN);
}

uint8_t
ICM20948_mag::read_reg(unsigned int reg)
{
	uint8_t buf{};

	set_passthrough(reg, 1);
	px4_usleep(25 + 25 * 1); // wait for the value to be read from slave

	_parent->_interface->read(ICMREG_20948_EXT_SLV_SENS_DATA_00, buf, 1);

	_parent->write_reg(ICMREG_20948_I2C_SLV0_CTRL, 0); // disable new reads

	return buf;
}

bool
ICM20948_mag::ak09916_check_id(uint8_t &deviceid)
{
	deviceid = read_reg(AK09916REG_WIA);

	return (AK09916_DEVICE_ID == deviceid);
}

void
ICM20948_mag::write_reg(unsigned reg, uint8_t value)
{
	// general register transfer at low clock speed
	set_passthrough(reg, 1, &value);
	px4_usleep(50); // wait for the value to be written to slave
	_parent->write_reg(ICMREG_20948_I2C_SLV0_CTRL, 0); // disable new writes
}

int
ICM20948_mag::ak09916_reset()
{
	// First initialize it to use the bus
	int rv = ak09916_setup();

	if (rv == OK) {
		// Now reset the mag
		write_reg(AK09916REG_CNTL3, AK09916_RESET);

		// Then re-initialize the bus/mag
		rv = ak09916_setup();
	}

	return rv;
}

bool
ICM20948_mag::ak09916_read_adjustments()
{
	uint8_t response[3];
	float ak09916_ASA[3];

	write_reg(AK09916REG_CNTL1, AK09916_FUZE_MODE | AK09916_16BIT_ADC);
	px4_usleep(50);

	_interface->read(AK09916REG_ASAX, response, 3);

	write_reg(AK09916REG_CNTL1, AK09916_POWERDOWN_MODE);

	for (int i = 0; i < 3; i++) {
		if (0 != response[i] && 0xff != response[i]) {
			ak09916_ASA[i] = ((float)(response[i] - 128) / 256.0f) + 1.0f;

		} else {
			return false;
		}
	}

	_px4_mag.set_sensitivity(ak09916_ASA[0], ak09916_ASA[1], ak09916_ASA[2]);

	return true;
}

int
ICM20948_mag::ak09916_setup_master_i2c()
{
	// ICM20948 -> AK09916
	_parent->modify_checked_reg(ICMREG_20948_USER_CTRL, 0, BIT_I2C_MST_EN);

	// WAIT_FOR_ES does not exist for ICM20948. Not sure how to replace this (or if that is needed)
	_parent->write_reg(ICMREG_20948_I2C_MST_CTRL, BIT_I2C_MST_P_NSR | ICM_BITS_I2C_MST_CLOCK_400HZ);

	return OK;
}
int
ICM20948_mag::ak09916_setup()
{
	int retries = 20;

	do {

		ak09916_setup_master_i2c();
		write_reg(AK09916REG_CNTL3, AK09916_RESET);

		uint8_t id = 0;

		if (ak09916_check_id(id)) {
			break;
		}

		retries--;
		PX4_WARN("AK09916: bad id %d retries %d", id, retries);
		_parent->modify_reg(ICMREG_20948_USER_CTRL, 0, BIT_I2C_MST_RST);
		px4_usleep(200);
	} while (retries > 0);

	if (retries == 0) {
		PX4_ERR("AK09916: failed to initialize, disabled!");
		_parent->modify_checked_reg(ICMREG_20948_USER_CTRL, BIT_I2C_MST_EN, 0);
		_parent->write_reg(ICMREG_20948_I2C_MST_CTRL, 0);
		return -EIO;
	}

	write_reg(AK09916REG_CNTL2, AK09916_CNTL2_CONTINOUS_MODE_100HZ);


	// Configure mpu' I2c Master interface to read ak09916 data into to fifo
	set_passthrough(AK09916REG_ST1, sizeof(ak09916_regs));

	return OK;
}
