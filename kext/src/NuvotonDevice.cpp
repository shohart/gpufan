//
//  NuvotonDevice.cpp
//
//  Sensors implementation for Nuvoton SuperIO device
//
//  Based on https://github.com/kozlek/HWSensors/blob/master/SuperIOSensors/NCT677xSensors.cpp
//  @author joedm
//

#include "NuvotonDevice.hpp"
#include "SMCSuperIO.hpp"
#include <Headers/kern_time.hpp>
#include <Headers/kern_iokit.hpp>

namespace Nuvoton {
	
	uint8_t NuvotonDevice::readByte(uint16_t reg) {
		uint8_t bank = reg >> 8;
		uint8_t regi = reg & 0xFF;
		uint16_t address = getDeviceAddress();
		
		::outb(address + NUVOTON_ADDRESS_REGISTER_OFFSET, NUVOTON_BANK_SELECT_REGISTER);
		::outb(address + NUVOTON_DATA_REGISTER_OFFSET, bank);
		::outb(address + NUVOTON_ADDRESS_REGISTER_OFFSET, regi);
		
		return ::inb(address + NUVOTON_DATA_REGISTER_OFFSET);
	}

	uint8_t NuvotonDevice::readByte6683(uint16_t reg) {
		uint8_t bank = reg >> 8;
		uint8_t regi = reg & 0xFF;
		uint16_t address = getDeviceAddress();

		::outb(address + NUVOTON_6683_PAGE_REGISTER_OFFSET, 0xFF);
		::outb(address + NUVOTON_6683_PAGE_REGISTER_OFFSET, bank);
		::outb(address + NUVOTON_6683_INDEX_REGISTER_OFFSET, regi);

		return ::inb(address + NUVOTON_6683_DATA_REGISTER_OFFSET);
	}
	
	void NuvotonDevice::writeByte(uint16_t reg, uint8_t value) {
		uint8_t bank = reg >> 8;
		uint8_t regi = reg & 0xFF;
		uint16_t address = getDeviceAddress();
		
		::outb(address + NUVOTON_ADDRESS_REGISTER_OFFSET, NUVOTON_BANK_SELECT_REGISTER);
		::outb(address + NUVOTON_DATA_REGISTER_OFFSET, bank);
		::outb(address + NUVOTON_ADDRESS_REGISTER_OFFSET, regi);
		::outb(address + NUVOTON_DATA_REGISTER_OFFSET, value);
	}

	void NuvotonDevice::writeByte6683(uint16_t reg, uint8_t value) {
		uint8_t bank = reg >> 8;
		uint8_t regi = reg & 0xFF;
		uint16_t address = getDeviceAddress();

		::outb(address + NUVOTON_6683_PAGE_REGISTER_OFFSET, 0xFF);
		::outb(address + NUVOTON_6683_PAGE_REGISTER_OFFSET, bank);
		::outb(address + NUVOTON_6683_INDEX_REGISTER_OFFSET, regi);
		::outb(address + NUVOTON_6683_DATA_REGISTER_OFFSET, value);
	}
	
	static constexpr SMC_KEY KeyFS__ = SMC_MAKE_IDENTIFIER('F','S','!',' ');

	bool NuvotonDevice::hasFanControl() {
		if (!fanControlChecked) {
			const char *model = getModelName();
			fanControlAvailable = model != nullptr &&
				(strncmp(model, "Nuvoton NCT679", 14) == 0 || strcmp(model, "Nuvoton NCT6779D") == 0) &&
				getTachometerCount() <= NUVOTON_MAX_TACHOMETER_COUNT;
			fanControlChecked = true;
		}
		return fanControlAvailable;
	}

	void NuvotonDevice::setupKeys(VirtualSMCAPI::Plugin &vsmcPlugin) {
		uint8_t count = getTachometerCount();
		bool control = hasFanControl();
		const IORegistryEntry *lpc = smcSuperIO ? smcSuperIO->getParentEntry(gIOServicePlane) : nullptr;

		if (control)
			lilu_get_boot_args("ssiofanwd", &watchdogSec, sizeof(watchdogSec));

		if (count > NUVOTON_6683_FAN_NUMS)
			count = NUVOTON_6683_FAN_NUMS;

		for (uint8_t index = 0; index < count; ++index) {
			char prop[24];
			uint16_t value;

			// Fan range: DeviceProperties on the LPC device (fan<n>-min / fan<n>-max as 2-byte data).
			setMinValue(index, 0);
			setMaxValue(index, NUVOTON_FAN_DEFAULT_MAX);
			snprintf(prop, sizeof(prop), "fan%u-min", index);
			if (lpc && WIOKit::getOSDataValue(lpc, prop, value))
				setMinValue(index, value);
			snprintf(prop, sizeof(prop), "fan%u-max", index);
			if (lpc && WIOKit::getOSDataValue(lpc, prop, value)) {
				setMaxValue(index, value);
				fanControl[index].maxExplicit = true;
			}

			// Fan name: fan<n>-name (string data), otherwise chip header name (SYSFAN, CPUFAN, ...).
			const char *name = getTachometerName(index);
			snprintf(prop, sizeof(prop), "fan%u-name", index);
			auto nameData = lpc ? OSDynamicCast(OSData, lpc->getProperty(prop)) : nullptr;
			if (nameData && nameData->getLength() > 0) {
				size_t len = nameData->getLength();
				if (len > sizeof(fanNames[index])) len = sizeof(fanNames[index]);
				lilu_os_memcpy(fanNames[index], nameData->getBytesNoCopy(), len);
			} else if (name) {
				lilu_os_strlcpy(fanNames[index], name, sizeof(fanNames[index]));
			}

			VirtualSMCAPI::addKey(KeyF0Ac(index), vsmcPlugin.data,
				VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new TachometerKey(getSmcSuperIO(), this, index)));

			// Apple fan descriptor: {type, zone, location, rsvd, name[12]}.
			struct PACKED { uint8_t type, zone, location, rsvd; char name[12]; } desc {};
			desc.zone = 1;
			lilu_os_memcpy(desc.name, fanNames[index], sizeof(desc.name));
			VirtualSMCAPI::addKey(KeyF0ID(index), vsmcPlugin.data, VirtualSMCAPI::valueWithData(
				reinterpret_cast<const SMC_DATA *>(&desc), sizeof(desc), SmcKeyTypeFds, nullptr, SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));

			if (!control)
				continue;

			VirtualSMCAPI::addKey(KeyF0Md(index), vsmcPlugin.data,
				VirtualSMCAPI::valueWithUint8(0, new ManualKey(getSmcSuperIO(), this, index), SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_READ));
			VirtualSMCAPI::addKey(KeyF0Mn(index), vsmcPlugin.data,
				VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new FanRangeKey(getSmcSuperIO(), this, index, false), SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_READ));
			VirtualSMCAPI::addKey(KeyF0Mx(index), vsmcPlugin.data,
				VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new FanRangeKey(getSmcSuperIO(), this, index, true), SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_READ));
			VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER('F', KeyIndexes[index], 'P', 'w'), vsmcPlugin.data,
				VirtualSMCAPI::valueWithUint8(0, new PwmKey(getSmcSuperIO(), this, index), SMC_KEY_ATTRIBUTE_READ));
			VirtualSMCAPI::addKey(KeyF0Tg(index), vsmcPlugin.data,
				VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new TargetKey(getSmcSuperIO(), this, index), SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_READ));
		}

		VirtualSMCAPI::addKey(KeyFNum, vsmcPlugin.data,
			VirtualSMCAPI::valueWithUint8(count, nullptr, SMC_KEY_ATTRIBUTE_CONST | SMC_KEY_ATTRIBUTE_READ));

		if (control) {
			VirtualSMCAPI::addKey(KeyFS__, vsmcPlugin.data,
				VirtualSMCAPI::valueWithUint16(0, new ForceKey(getSmcSuperIO(), this), SMC_KEY_ATTRIBUTE_WRITE | SMC_KEY_ATTRIBUTE_READ));
			SYSLOG("ssio", "%s: fan control enabled for %u fans (F<n>Md: 0 auto, 1 rpm, 2 duty%%), watchdog %u s",
				   getModelName(), count, watchdogSec);
		}

		// Plugin key storage must be sorted.
		qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(), sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);
	}

	void NuvotonDevice::updateTargets() {
		// Called from SMC key write context. Hardware is touched in the timer only.
		atomic_store_explicit(&lastRequestNs, getCurrentTimeNs(), memory_order_relaxed);
		const_cast<SMCSuperIO *>(getSmcSuperIO())->quickReschedule();
	}

	void NuvotonDevice::setFanRange(uint8_t index, bool isMax, uint16_t value) {
		if (index >= getTachometerCount())
			return;
		if (isMax) {
			setMaxValue(index, value);
			fanControl[index].maxExplicit = true;
		} else {
			setMinValue(index, value);
		}
	}

	uint16_t NuvotonDevice::getForceBits() {
		uint16_t bits = 0;
		for (uint8_t index = 0; index < getTachometerCount(); ++index)
			if (getManualValue(index) != FanModeAuto)
				bits |= 1U << index;
		return bits;
	}

	void NuvotonDevice::setForceBits(uint16_t bits) {
		for (uint8_t index = 0; index < getTachometerCount(); ++index) {
			bool force = (bits >> index) & 1U;
			if (!force)
				setManualValue(index, FanModeAuto);
			else if (getManualValue(index) == FanModeAuto)
				setManualValue(index, FanModeRpm);
		}
		updateTargets();
	}

	void NuvotonDevice::fanSetManual(uint8_t index, uint8_t pwm) {
		auto &fc = fanControl[index];
		uint16_t modeReg = NUVOTON_FAN_MODE_REGS[index];
		uint8_t mode = readByte(modeReg);

		if (!fc.modeSaved) {
			fc.initialMode = mode;
			fc.modeSaved = true;
			DBGLOG("ssio", "fan %u: saved BIOS mode 0x%02X", index, mode);
		}
		// Re-assert manual mode every tick: firmware restores SmartFan after wake.
		if ((mode & NUVOTON_FAN_MODE_MASK) != 0)
			writeByte(modeReg, mode & ~NUVOTON_FAN_MODE_MASK);
		if (readByte(NUVOTON_PWM_REGS[index]) != pwm)
			writeByte(NUVOTON_PWM_REGS[index], pwm);
		fc.pwm = pwm;
	}

	void NuvotonDevice::fanRestoreAuto(uint8_t index) {
		auto &fc = fanControl[index];
		if (fc.modeSaved) {
			writeByte(NUVOTON_FAN_MODE_REGS[index], fc.initialMode);
			fc.modeSaved = false;
			fc.integral = 0;
			DBGLOG("ssio", "fan %u: restored BIOS mode 0x%02X", index, fc.initialMode);
		}
	}

	uint8_t NuvotonDevice::fanRpmControl(uint8_t index, float dt) {
		auto &fc = fanControl[index];
		float target = getTargetValue(index);
		float actual = getTachometerValue(index);
		float mn = getMinValue(index), mx = getMaxValue(index);

		// Apple semantics: F<n>Tg is a target RPM, and zero is "not set yet", not "stop".
		// Utilities set the mode (or FS!) before the target — Macs Fan Control does exactly
		// that — and stopping the fan in that window can leave a CPU with no airflow.
		// Hold the current PWM instead; the first real target takes over on the next tick.
		if (target <= 0) {
			fc.integral = 0;
			return fc.pwm;
		}
		if (target < mn)
			target = mn;
		if (mx > 0 && target >= mx && fc.maxExplicit)
			return 0xFF;

		// Feed-forward (fan RPM is roughly proportional to duty) + PI correction.
		constexpr float Kp = 0.02f, Ki = 0.04f, MaxStep = 24.0f;
		float ff = mx > 0 ? 255.0f * target / mx : 128.0f;
		float err = target - actual;
		float integral = fc.integral + Ki * err * dt;
		if (integral > 255) integral = 255;
		if (integral < -255) integral = -255;
		float out = ff + integral + Kp * err;

		float floor = fc.stallPwm;
		bool saturated = false;
		if (out > 255) { out = 255; saturated = err > 0; }
		if (out < floor) { out = floor; saturated = err < 0; }
		if (!saturated)
			fc.integral = integral; // anti-windup: do not integrate into a saturated output

		float prev = fc.pwm;
		if (out > prev + MaxStep) out = prev + MaxStep;
		if (out < prev - MaxStep) out = prev - MaxStep;

		// Stall learning: output is non-zero, but fan does not spin for 3 control periods.
		if (actual == 0 && prev > 0 && err > 0) {
			if (++fc.stallTicks >= 3 && fc.stallPwm < 0xF0) {
				fc.stallPwm = static_cast<uint8_t>(prev) + 12;
				fc.stallTicks = 0;
				DBGLOG("ssio", "fan %u: stall at pwm %u, new floor %u", index, static_cast<uint8_t>(prev), fc.stallPwm);
			}
		} else {
			fc.stallTicks = 0;
		}

		return static_cast<uint8_t>(out + 0.5f);
	}

	void NuvotonDevice::applyFanControl() {
		if (!hasFanControl())
			return;

		uint8_t count = getTachometerCount();
		uint64_t now = getCurrentTimeNs();

		// Current PWM output for F<n>Pw (valid in any mode).
		for (uint8_t index = 0; index < count; ++index)
			atomic_store_explicit(&pwmOutput[index], readByte(NUVOTON_PWM_READ_REGS[index]), memory_order_relaxed);

		// Learn Mx from observed RPM unless it was set explicitly.
		for (uint8_t index = 0; index < count; ++index) {
			uint16_t rpm = getTachometerValue(index);
			if (!fanControl[index].maxExplicit && rpm > getMaxValue(index) && rpm < 10000)
				setMaxValue(index, rpm);
		}

		// Optional watchdog (ssiofanwd=<sec>): fall back to BIOS if no one writes Md/Tg.
		if (watchdogSec > 0) {
			uint64_t last = atomic_load_explicit(&lastRequestNs, memory_order_relaxed);
			if (getForceBits() != 0 && now > last && now - last > static_cast<uint64_t>(watchdogSec) * 1000000000ULL) {
				SYSLOG("ssio", "fan watchdog: no F<n>Md/F<n>Tg writes for %u s, returning fans to BIOS", watchdogSec);
				for (uint8_t index = 0; index < count; ++index)
					setManualValue(index, FanModeAuto);
			}
		}

		// Closed loop runs at ~1 Hz (tachometer resolution), duty/auto transitions on every tick.
		bool controlTick = lastControlNs == 0 || now - lastControlNs >= 900000000ULL;
		float dt = lastControlNs == 0 ? 1.0f : (now - lastControlNs) / 1e9f;
		if (dt > 3.0f) dt = 3.0f;
		if (controlTick)
			lastControlNs = now;

		for (uint8_t index = 0; index < count; ++index) {
			auto &fc = fanControl[index];
			uint8_t mode = getManualValue(index);
			if (mode == FanModeAuto) {
				fanRestoreAuto(index);
				continue;
			}
			if (!fc.modeSaved)
				fc.pwm = atomic_load_explicit(&pwmOutput[index], memory_order_relaxed); // bumpless start

			uint8_t pwm = fc.pwm;
			if (mode == FanModeDuty) {
				uint32_t duty = getTargetValue(index);
				pwm = static_cast<uint8_t>(duty >= 100 ? 255 : (duty * 255 + 50) / 100);
			} else if (controlTick || !fc.modeSaved) {
				pwm = fanRpmControl(index, dt);
			}
			fanSetManual(index, pwm);
		}
	}

	void NuvotonDevice::updateTachometers() {
		applyFanControl();
		SuperIODevice::updateTachometers();
	}

	SMC_RESULT FanRangeKey::readAccess() {
		double val = isMax ? device->getMaxValue(index) : device->getMinValue(index);
		*reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, val);
		return SmcSuccess;
	}

	SMC_RESULT FanRangeKey::update(const SMC_DATA *src) {
		VirtualSMCValue::update(src);
		auto val = VirtualSMCAPI::decodeIntFp(SmcKeyTypeFpe2, *reinterpret_cast<const uint16_t *>(src));
		device->setFanRange(index, isMax, val);
		return SmcSuccess;
	}

	SMC_RESULT PwmKey::readAccess() {
		*reinterpret_cast<uint8_t *>(data) = device->getPwmOutput(index);
		const_cast<SMCSuperIO *>(sio)->quickReschedule();
		return SmcSuccess;
	}

	SMC_RESULT ForceKey::readAccess() {
		uint16_t bits = device->getForceBits();
		data[0] = bits >> 8;
		data[1] = bits & 0xFF;
		return SmcSuccess;
	}

	SMC_RESULT ForceKey::update(const SMC_DATA *src) {
		VirtualSMCValue::update(src);
		device->setForceBits(static_cast<uint16_t>((src[0] << 8) | src[1]));
		return SmcSuccess;
	}

	uint16_t NuvotonDevice::tachometerRead(uint8_t index) {
		uint8_t high = readByte(NUVOTON_FAN_REGS[index]);
		uint8_t low = readByte(NUVOTON_FAN_REGS[index] + 1);
		return (high << 8) | low;
	}

	uint16_t NuvotonDevice::tachometerRead6776(uint8_t index) {
		uint8_t high = readByte(NUVOTON_FAN_6776_REGS[index]);
		uint8_t low = readByte(NUVOTON_FAN_6776_REGS[index] + 1);
		return (high << 8) | low;
	}

	uint16_t NuvotonDevice::tachometerRead6683(uint8_t index) {
		if (index >= NUVOTON_6683_FAN_NUMS) {
			return 0;
		}
		uint8_t high = readByte6683(NUVOTON_6683_FAN_REGS[index]);
		uint8_t low = readByte6683(NUVOTON_6683_FAN_REGS[index] + 1);
		return (high << 8) | low;
	}

	float NuvotonDevice::voltageRead(uint8_t index) {
		if (NUVOTON_VBAT_REG == NUVOTON_VOLTAGE_REGS[index]) {
			if (!(readByte(NUVOTON_VBAT_CONTROL_REG) & 1)) {
				return 0.0f;
			}
		}
		float value = readByte(NUVOTON_VOLTAGE_REGS[index]) * 0.008f;
		return value > 0 ? value : 0.0f;
	}

	float NuvotonDevice::voltageRead6775(uint8_t index) {
		if (NUVOTON_VBAT_6775_REG == NUVOTON_VOLTAGE_6775_REGS[index]) {
			if (!(readByte(NUVOTON_VBAT_CONTROL_REG) & 1)) {
				return 0.0f;
			}
		}
		float value = readByte(NUVOTON_VOLTAGE_6775_REGS[index]) * 0.008f;
		return value > 0 ? value : 0.0f;
	}

	float NuvotonDevice::voltageRead6683(uint8_t index) {
		if (nuvoton6683VoltageRegs[index] == 0) {
			return 0.0f;
		}
		float value = readByte6683(nuvoton6683VoltageRegs[index]) * 0.016f;
		return value > 0 ? value : 0.0f;
	}

	void NuvotonDevice::voltageMapping6683() {
		uint8_t value = 0;
		uint8_t index = 0;

		for (uint8_t i = 0; i < NUVOTON_6683_MON_NUMS; i++) {
			value = readByte6683(NUVOTON_6683_MON_CFG_OFFSET + i) & 0x7f;
			if (value >= NUVOTON_6683_MON_VOLTAGE_START) {
				index = value % NUVOTON_6683_MON_VOLTAGE_START;
				if (index >= NUVOTON_6683_VOLTAGE_NUMS) {
					continue;
				}
				nuvoton6683VoltageRegs[index] = NUVOTON_6683_MON_REGISTER_OFFSET + i * 2;
			}
		}
	}

	void NuvotonDevice::onPowerOn679xx() {
		i386_ioport_t port = getDevicePort();
		// disable the hardware monitor i/o space lock on NCT679xD chips
		enter(port);
		selectLogicalDevice(port, WinbondHardwareMonitorLDN);
		/* Activate logical device if needed */
		uint8_t options = listenPortByte(port, NUVOTON_REG_ENABLE);
		if (!(options & 0x01)) {
			writePortByte(port, NUVOTON_REG_ENABLE, options | 0x01);
		}
		options = listenPortByte(port, NUVOTON_HWMON_IO_SPACE_LOCK);
		// if the i/o space lock is enabled
		if (options & 0x10) {
			// disable the i/o space lock
			writePortByte(port, NUVOTON_HWMON_IO_SPACE_LOCK, options & ~0x10);
		}
		leave(port);
	}
} // namespace Nuvoton
