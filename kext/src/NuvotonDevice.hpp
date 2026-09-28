//
//  NuvotonDevice.hpp
//
//  Sensors implementation for Nuvoton SuperIO device
//
//  Based on https://github.com/kozlek/HWSensors/blob/master/SuperIOSensors/NCT677xSensors.cpp
//  @author joedm
//

#ifndef _NUVOTONDEVICE_HPP
#define _NUVOTONDEVICE_HPP

#include "SuperIODevice.hpp"
#include "WinbondFamilyDevice.hpp"

namespace Nuvoton {
	static constexpr uint8_t NUVOTON_ADDRESS_REGISTER_OFFSET     = 0x05;
	static constexpr uint8_t NUVOTON_DATA_REGISTER_OFFSET        = 0x06;
	static constexpr uint8_t NUVOTON_BANK_SELECT_REGISTER        = 0x4E;
	static constexpr uint8_t NUVOTON_REG_ENABLE                  = 0x30;
	static constexpr uint8_t NUVOTON_HWMON_IO_SPACE_LOCK         = 0x28;
	static constexpr uint16_t NUVOTON_VENDOR_ID                  = 0x5CA3;
	static constexpr uint8_t NUVOTON_MAX_TACHOMETER_COUNT		 = 7;
	static constexpr uint16_t NUVOTON_FAN_6776_REGS[] = { 0x656, 0x658, 0x65A, 0x65C, 0x65E };
	static constexpr uint16_t NUVOTON_FAN_REGS[] = { 0x4C0, 0x4C2, 0x4C4, 0x4C6, 0x4C8, 0x4CA, 0x4CE };

	// Fan control of NCT6779D / NCT679xD (register map as in Linux nct6775 driver).
	// Index order matches NUVOTON_FAN_REGS: SYSFAN, CPUFAN, AUXFAN0, AUXFAN1, AUXFAN2, AUXFAN3, AUXFAN4.
	static constexpr uint16_t NUVOTON_FAN_MODE_REGS[] = { 0x102, 0x202, 0x302, 0x802, 0x902, 0xA02, 0xB02 }; // REG_FAN_MODE
	static constexpr uint16_t NUVOTON_PWM_REGS[]      = { 0x109, 0x209, 0x309, 0x809, 0x909, 0xA09, 0xB09 }; // REG_PWM[0], written in manual mode
	static constexpr uint16_t NUVOTON_PWM_READ_REGS[] = { 0x001, 0x003, 0x011, 0x013, 0x015, 0xA09, 0xB09 }; // REG_PWM_READ, current output
	static constexpr uint8_t NUVOTON_FAN_MODE_MASK = 0xF0; // upper nibble: 0 = manual, otherwise SmartFan modes

	/**
	 * F<n>Md values.
	 */
	static constexpr uint8_t FanModeAuto   = 0; // BIOS SmartFan
	static constexpr uint8_t FanModeRpm    = 1; // F<n>Tg is target RPM (Apple semantics, closed loop)
	static constexpr uint8_t FanModeDuty   = 2; // F<n>Tg is duty cycle in percent 0..100 (open loop)

	/**
	 * Default fan range when not provided by DeviceProperties (fan<n>-min / fan<n>-max).
	 */
	static constexpr uint16_t NUVOTON_FAN_DEFAULT_MAX = 2000;
	static constexpr uint8_t NUVOTON_MAX_VOLTAGE_COUNT		 	= 16;
	static constexpr uint16_t NUVOTON_VOLTAGE_6775_REGS[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x550, 0x551, 0x552	};
	static constexpr uint16_t NUVOTON_VOLTAGE_REGS[] = { 0x480, 0x481, 0x482, 0x483, 0x484, 0x485, 0x486, 0x487, 0x488, 0x489, 0x48A, 0x48B, 0x48C, 0x48D, 0x48E, 0x48F};
	static constexpr uint16_t NUVOTON_VBAT_REG 					= 0x488;
	static constexpr uint16_t NUVOTON_VBAT_6775_REG				= 0x551;
	static constexpr uint16_t NUVOTON_VBAT_CONTROL_REG			= 0x5D;
	// NCT6683 specific data
	static constexpr uint8_t NUVOTON_6683_PAGE_REGISTER_OFFSET	= 0x04;
	static constexpr uint8_t NUVOTON_6683_INDEX_REGISTER_OFFSET	= 0x05;
	static constexpr uint8_t NUVOTON_6683_DATA_REGISTER_OFFSET	= 0x06;
	static constexpr uint8_t NUVOTON_6683_EVENT_REGISTER_OFFSET	= 0x07;
	static constexpr uint8_t NUVOTON_6683_MON_NUMS				= 32;
	static constexpr uint16_t NUVOTON_6683_MON_REGISTER_OFFSET	= 0x100;
	static constexpr uint16_t NUVOTON_6683_MON_CFG_OFFSET		= 0x1A0;
	static constexpr uint16_t NUVOTON_6683_MON_VOLTAGE_START	= 0x60;
	static constexpr uint8_t NUVOTON_6683_FAN_NUMS				= 16;
	static constexpr uint16_t NUVOTON_6683_FAN_REGS[] = { 0x142, 0x140, 0x144, 0x146, 0x148, 0x14A, 0x14C, 0x14E, 0x150, 0x152, 0x154, 0x156, 0x158, 0x15A, 0x15C, 0x15E };
	static constexpr uint8_t NUVOTON_6683_VOLTAGE_NUMS			= 23;

	class NuvotonDevice : public WindbondFamilyDevice {

	protected:
		/**
		 * Mapped voltage register for NCT6683
		 */
		uint16_t nuvoton6683VoltageRegs[NUVOTON_6683_VOLTAGE_NUMS] = {0};

		/**
		 * Fan control state.
		 * Hardware is accessed from the timer (update()) context only. SMC key writes
		 * just store the request, so bank-select sequences never interleave.
		 */
		struct FanControl {
			bool modeSaved {false};        // initialMode holds BIOS FAN_MODE register value
			uint8_t initialMode {0};
			uint8_t pwm {0};               // last PWM we wrote in manual mode
			uint8_t stallPwm {0};          // learned minimum PWM at which the fan spins
			uint8_t stallTicks {0};
			bool maxExplicit {false};      // Mx set by DeviceProperties or SMC write
			float integral {0};
		};
		FanControl fanControl[NUVOTON_6683_FAN_NUMS] {};
		_Atomic(uint8_t) pwmOutput[NUVOTON_6683_FAN_NUMS] {};
		char fanNames[NUVOTON_6683_FAN_NUMS][12] {};
		bool fanControlChecked {false};
		bool fanControlAvailable {false};
		uint64_t lastControlNs {0};
		uint32_t watchdogSec {0};              // ssiofanwd=<sec> boot-arg, 0 = off
		_Atomic(uint64_t) lastRequestNs = 0;

		/**
		 * Fan control helpers. Invoked from update() only.
		 */
		void applyFanControl();
		void fanSetManual(uint8_t index, uint8_t pwm);
		void fanRestoreAuto(uint8_t index);
		uint8_t fanRpmControl(uint8_t index, float dt);

		/**
		 * Hook into periodic update: apply fan control, then read tachometers.
		 */
		void updateTachometers() override;

		/**
		 * On power-on init for 679XX devices.
		 */
		void onPowerOn679xx();
		/**
		 * Reads tachometers data. Invoked from update() only.
		 */
		uint16_t tachometerRead(uint8_t);
		uint16_t tachometerRead6776(uint8_t);
		uint16_t tachometerRead6683(uint8_t);

		/**
		 * Reads voltage data. Invoked from update() only.
		 */
		float voltageRead(uint8_t);
		float voltageRead6775(uint8_t);
		float voltageRead6683(uint8_t);

		/**
		 * Mapping monitor regs to voltage regs for 6683 devices.
		 */
		void voltageMapping6683();

	public:
		/**
		 * Reads a byte from device's register
		 */
		uint8_t readByte(uint16_t reg);
		uint8_t readByte6683(uint16_t reg);

		/**
		 * Writes a byte into device's register
		 */
		void writeByte(uint16_t reg, uint8_t value);
		void writeByte6683(uint16_t reg, uint8_t value);

		/**
		 *  Overrides
		 */
		void setupKeys(VirtualSMCAPI::Plugin &vsmcPlugin) override;
		void updateTargets() override;

		/**
		 * Whether manual fan control is supported (NCT6779D and NCT679xD).
		 */
		bool hasFanControl();

		/**
		 * Accessors for key implementations.
		 */
		uint8_t getPwmOutput(uint8_t index) {
			return index < NUVOTON_6683_FAN_NUMS ? atomic_load_explicit(&pwmOutput[index], memory_order_relaxed) : 0;
		}
		void setFanRange(uint8_t index, bool isMax, uint16_t value);
		uint16_t getForceBits();
		void setForceBits(uint16_t bits);

		/**
		 *  Ctor
		 */
		NuvotonDevice() = default;
	};

	/**
	 * Writable F<n>Mn / F<n>Mx.
	 */
	class FanRangeKey : public VirtualSMCValue {
	protected:
		const SMCSuperIO *sio;
		uint8_t index;
		NuvotonDevice *device;
		bool isMax;
		SMC_RESULT readAccess() override;
	public:
		SMC_RESULT update(const SMC_DATA *src) override;
		FanRangeKey(const SMCSuperIO *sio, NuvotonDevice *device, uint8_t index, bool isMax) : sio(sio), index(index), device(device), isMax(isMax) {}
	};

	/**
	 * F<n>Pw: current PWM output 0..255 (read from hardware in any mode).
	 */
	class PwmKey : public VirtualSMCValue {
	protected:
		const SMCSuperIO *sio;
		uint8_t index;
		NuvotonDevice *device;
		SMC_RESULT readAccess() override;
	public:
		PwmKey(const SMCSuperIO *sio, NuvotonDevice *device, uint8_t index) : sio(sio), index(index), device(device) {}
	};

	/**
	 * 'FS! ': legacy forced-fan bitmask used by older fan control utilities.
	 */
	class ForceKey : public VirtualSMCValue {
	protected:
		const SMCSuperIO *sio;
		NuvotonDevice *device;
		SMC_RESULT readAccess() override;
	public:
		SMC_RESULT update(const SMC_DATA *src) override;
		ForceKey(const SMCSuperIO *sio, NuvotonDevice *device) : sio(sio), device(device) {}
	};
}

#endif // _NUVOTONDEVICE_HPP
