/**
 * @file    hardware.cpp
 * @brief   Hardware package entry point
 *
 * @date May 27, 2013
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#include "global.h"


#if EFI_PROD_CODE
#include "os_access.h"
#include "trigger_input.h"
#include "servo.h"
#include "adc_inputs.h"
#include "can_hw.h"
#include "hardware.h"
#include "rtc_helper.h"
#include "os_util.h"
#include "bench_test.h"
#include "vehicle_speed.h"
#include "yaw_rate_sensor.h"
#include "pin_repository.h"
#include "max31855.h"
#include "logic_analyzer.h"
#include "smart_gpio.h"
#include "accelerometer.h"
#include "eficonsole.h"
#include "console_io.h"
#include "sensor_chart.h"
#include "serial_hw.h"

#include "mpu_util.h"
//#include "usb_msd.h"

#include "AdcConfiguration.h"
#include "idle_hardware.h"
#include "mcp3208.h"
#include "hip9011.h"
#include "histogram.h"
#include "neo6m.h"
#include "lcd_HD44780.h"
#include "settings.h"
#include "joystick.h"
#include "cdm_ion_sense.h"
#include "trigger_central.h"
#include "svnversion.h"
#include "engine_configuration.h"
#include "vvt_pid.h"
#include "perf_trace.h"
#include "trigger_emulator_algo.h"
#include "boost_control.h"
#include "software_knock.h"
#if EFI_MC33816
#include "mc33816.h"
#endif /* EFI_MC33816 */

#if EFI_MAP_AVERAGING
#include "map_averaging.h"
#endif

#if EFI_INTERNAL_FLASH
#include "flash_main.h"
#endif

#if EFI_CAN_SUPPORT
#include "can_vss.h"
#endif

EXTERN_ENGINE;

/**
 * #311 we want to test RTC before engine start so that we do not test it while engine is running
 */
bool rtcWorks = true;
#if HAL_USE_SPI
extern bool isSpiInitialized[5];

/**
 * Only one consumer can use SPI bus at a given time
 */
void lockSpi(spi_device_e device) {
	efiAssertVoid(CUSTOM_STACK_SPI, getCurrentRemainingStack() > 128, "lockSpi");
	spiAcquireBus(getSpiDevice(device));
}

void unlockSpi(spi_device_e device) {
	spiReleaseBus(getSpiDevice(device));
}

static void initSpiModules(engine_configuration_s *engineConfiguration) {
	UNUSED(engineConfiguration);
	if (CONFIG(is_enabled_spi_1)) {
		 turnOnSpi(SPI_DEVICE_1);
	}
	if (CONFIG(is_enabled_spi_2)) {
		turnOnSpi(SPI_DEVICE_2);
	}
	if (CONFIG(is_enabled_spi_3)) {
		turnOnSpi(SPI_DEVICE_3);
	}
	if (CONFIG(is_enabled_spi_4)) {
		turnOnSpi(SPI_DEVICE_4);
	}
}

/**
 * @return NULL if SPI device not specified
 */
SPIDriver * getSpiDevice(spi_device_e spiDevice) {
	if (spiDevice == SPI_NONE) {
		return NULL;
	}
#if STM32_SPI_USE_SPI1
	if (spiDevice == SPI_DEVICE_1) {
		return &SPID1;
	}
#endif
#if STM32_SPI_USE_SPI2
	if (spiDevice == SPI_DEVICE_2) {
		return &SPID2;
	}
#endif
#if STM32_SPI_USE_SPI3
	if (spiDevice == SPI_DEVICE_3) {
		return &SPID3;
	}
#endif
#if STM32_SPI_USE_SPI4
	if (spiDevice == SPI_DEVICE_4) {
		return &SPID4;
	}
#endif
	firmwareError(CUSTOM_ERR_UNEXPECTED_SPI, "Unexpected SPI device: %d", spiDevice);
	return NULL;
}
#endif

static Logging *sharedLogger;

#if EFI_PROD_CODE

#define TPS_IS_SLOW -1

static int fastMapSampleIndex;
static int hipSampleIndex;
static int tpsSampleIndex;

#if HAL_TRIGGER_USE_ADC
static int triggerSampleIndex;
#endif

#if HAL_USE_ADC
extern AdcDevice fastAdc;

#if EFI_FASTER_UNIFORM_ADC
static int adcCallbackCounter = 0;
static volatile int averagedSamples[ADC_MAX_CHANNELS_COUNT];
static adcsample_t avgBuf[ADC_MAX_CHANNELS_COUNT];

void adc_callback_fast_internal(ADCDriver *adcp);

void adc_callback_fast(ADCDriver *adcp) {
	adcsample_t *buffer = adcp->samples;
	//size_t n = adcp->depth;

	if (adcp->state == ADC_COMPLETE) {
#if HAL_TRIGGER_USE_ADC
		// we need to call this ASAP, because trigger processing is time-critical
		if (triggerSampleIndex >= 0)
			triggerAdcCallback(buffer[triggerSampleIndex]);
#endif /* HAL_TRIGGER_USE_ADC */

		// store the values for averaging
		for (int i = fastAdc.size() - 1; i >= 0; i--) {
			averagedSamples[i] += fastAdc.samples[i];
		}

		// if it's time to process the data
		if (++adcCallbackCounter >= ADC_BUF_NUM_AVG) {
			// get an average
			for (int i = fastAdc.size() - 1; i >= 0; i--) {
				avgBuf[i] = (adcsample_t)(averagedSamples[i] / ADC_BUF_NUM_AVG);	// todo: rounding?
			}

			// call the real callback (see below)
			adc_callback_fast_internal(adcp);

			// reset the avg buffer & counter
			for (int i = fastAdc.size() - 1; i >= 0; i--) {
				averagedSamples[i] = 0;
			}
			adcCallbackCounter = 0;
		}
	}
}

#endif /* EFI_FASTER_UNIFORM_ADC */

/**
 * This method is not in the adc* lower-level file because it is more business logic then hardware.
 */
#if EFI_FASTER_UNIFORM_ADC
void adc_callback_fast_internal(ADCDriver *adcp) {
#else
void adc_callback_fast(ADCDriver *adcp) {
#endif
	adcsample_t *buffer = adcp->samples;
	size_t n = adcp->depth;
	(void) buffer;
	(void) n;

	ScopePerf perf(PE::AdcCallbackFast);

	/**
	 * Note, only in the ADC_COMPLETE state because the ADC driver fires an
	 * intermediate callback when the buffer is half full.
	 * */
	if (adcp->state == ADC_COMPLETE) {
		ScopePerf perf(PE::AdcCallbackFastComplete);

		/**
		 * this callback is executed 10 000 times a second, it needs to be as fast as possible
		 */
		efiAssertVoid(CUSTOM_ERR_6676, getCurrentRemainingStack() > 128, "lowstck#9b");

#if EFI_SENSOR_CHART && EFI_SHAFT_POSITION_INPUT
		if (ENGINE(sensorChartMode) == SC_AUX_FAST1) {
			float voltage = getAdcValue("fAux1", engineConfiguration->auxFastSensor1_adcChannel);
			scAddData(getCrankshaftAngleNt(getTimeNowNt() PASS_ENGINE_PARAMETER_SUFFIX), voltage);
		}
#endif /* EFI_SENSOR_CHART */

#if EFI_MAP_AVERAGING
		mapAveragingAdcCallback(buffer[fastMapSampleIndex]);
#endif /* EFI_MAP_AVERAGING */
#if EFI_HIP_9011
		if (CONFIG(isHip9011Enabled)) {
			hipAdcCallback(buffer[hipSampleIndex]);
		}
#endif /* EFI_HIP_9011 */
//		if (tpsSampleIndex != TPS_IS_SLOW) {
//			tpsFastAdc = buffer[tpsSampleIndex];
//		}
	}
}
#endif /* HAL_USE_ADC */

static void calcFastAdcIndexes(void) {
#if HAL_USE_ADC && EFI_USE_FAST_ADC
	fastMapSampleIndex = fastAdc.internalAdcIndexByHardwareIndex[engineConfiguration->map.sensor.hwChannel];
	hipSampleIndex =
			isAdcChannelValid(engineConfiguration->hipOutputChannel) ?
					fastAdc.internalAdcIndexByHardwareIndex[engineConfiguration->hipOutputChannel] : -1;
	tpsSampleIndex =
			isAdcChannelValid(engineConfiguration->tps1_1AdcChannel) ?
					fastAdc.internalAdcIndexByHardwareIndex[engineConfiguration->tps1_1AdcChannel] : TPS_IS_SLOW;
#if HAL_TRIGGER_USE_ADC
	adc_channel_e triggerChannel = getAdcChannelForTrigger();
	triggerSampleIndex = isAdcChannelValid(triggerChannel) ?
		fastAdc.internalAdcIndexByHardwareIndex[triggerChannel] : -1;
#endif /* HAL_TRIGGER_USE_ADC */

#endif/* HAL_USE_ADC */
}

static void adcConfigListener(Engine *engine) {
	UNUSED(engine);
	// todo: something is not right here - looks like should be a callback for each configuration change?
	calcFastAdcIndexes();
}

void turnOnHardware(Logging *sharedLogger) {
#if EFI_FASTER_UNIFORM_ADC
	for (int i = 0; i < ADC_MAX_CHANNELS_COUNT; i++) {
		averagedSamples[i] = 0;
	}
	adcCallbackCounter = 0;
#endif /* EFI_FASTER_UNIFORM_ADC */

#if EFI_SHAFT_POSITION_INPUT
	turnOnTriggerInputPins(sharedLogger);
#endif /* EFI_SHAFT_POSITION_INPUT */
}

void stopSpi(spi_device_e device) {
#if HAL_USE_SPI
	if (!isSpiInitialized[device]) {
		return; // not turned on
	}
	isSpiInitialized[device] = false;
	efiSetPadUnused(getSckPin(device));
	efiSetPadUnused(getMisoPin(device));
	efiSetPadUnused(getMosiPin(device));
#endif /* HAL_USE_SPI */
}

/**
 * this method is NOT currently invoked on ECU start
 * todo: maybe start invoking this method on ECU start so that peripheral start-up initialization and restart are unified?
 */

void applyNewHardwareSettings(void) {
    /**
     * All 'stop' methods need to go before we begin starting pins.
     *
     * We take settings from 'activeConfiguration' not 'engineConfiguration' while stopping hardware.
     * Some hardware is restart unconditionally on change of parameters while for some systems we make extra effort and restart only
     * relevant settings were changes.
     *
     */
	ButtonDebounce::stopConfigurationList();

#if EFI_SHAFT_POSITION_INPUT
	stopTriggerInputPins();
#endif /* EFI_SHAFT_POSITION_INPUT */


#if (HAL_USE_PAL && EFI_JOYSTICK)
	stopJoystickPins();
#endif /* HAL_USE_PAL && EFI_JOYSTICK */

#if EFI_CAN_SUPPORT
	stopCanPins();
#endif /* EFI_CAN_SUPPORT */

#if EFI_AUX_SERIAL
	stopAuxSerialPins();
#endif /* EFI_AUX_SERIAL */

#if EFI_HIP_9011
	stopHip9001_pins();
#endif /* EFI_HIP_9011 */

#if (BOARD_EXT_GPIOCHIPS > 0)
	stopSmartCsPins();
#endif /* (BOARD_EXT_GPIOCHIPS > 0) */

#if EFI_VEHICLE_SPEED
	stopVSSPins();
#endif /* EFI_VEHICLE_SPEED */

#if EFI_LOGIC_ANALYZER
	stopLogicAnalyzerPins();
#endif /* EFI_LOGIC_ANALYZER */

#if EFI_EMULATE_POSITION_SENSORS
	stopTriggerEmulatorPins();
#endif /* EFI_EMULATE_POSITION_SENSORS */

#if EFI_AUX_PID
	stopVvtControlPins();
#endif /* EFI_AUX_PID */

	if (isConfigurationChanged(is_enabled_spi_1)) {
		stopSpi(SPI_DEVICE_1);
	}

	if (isConfigurationChanged(is_enabled_spi_2)) {
		stopSpi(SPI_DEVICE_2);
	}

	if (isConfigurationChanged(is_enabled_spi_3)) {
		stopSpi(SPI_DEVICE_3);
	}

	if (isConfigurationChanged(is_enabled_spi_4)) {
		stopSpi(SPI_DEVICE_4);
	}

#if EFI_HD44780_LCD
	stopHD44780_pins();
#endif /* #if EFI_HD44780_LCD */

	if (isPinOrModeChanged(clutchUpPin, clutchUpPinMode)) {
		efiSetPadUnused(activeConfiguration.clutchUpPin);
	}

	enginePins.unregisterPins();

	ButtonDebounce::startConfigurationList();

	/*******************************************
	 * Start everything back with new settings *
	 ******************************************/

#if EFI_SHAFT_POSITION_INPUT
	startTriggerInputPins();
#endif /* EFI_SHAFT_POSITION_INPUT */

#if (HAL_USE_PAL && EFI_JOYSTICK)
	startJoystickPins();
#endif /* HAL_USE_PAL && EFI_JOYSTICK */

#if EFI_HD44780_LCD
	startHD44780_pins();
#endif /* #if EFI_HD44780_LCD */

#if (BOARD_EXT_GPIOCHIPS > 0)
	/* TODO: properly restart gpio chips...
	 * This is only workaround for "CS pin lost" bug
	 * see: https://github.com/rusefi/rusefi/issues/2107
	 * We should provide better way to gracefully stop all
	 * gpio chips: set outputs to safe state, release all
	 * on-chip resources (gpios, SPIs, etc) and then restart
	 * with updated settings.
	 * Following code just re-inits CS pins for all external
	 * gpio chips, but does not update CS pin definition in
	 * gpio chips private data/settings. So changing CS pin
	 * on-fly does not work */
	startSmartCsPins();
#endif /* (BOARD_EXT_GPIOCHIPS > 0) */

	enginePins.startPins();

#if EFI_CAN_SUPPORT
	startCanPins();
#endif /* EFI_CAN_SUPPORT */

#if EFI_AUX_SERIAL
	startAuxSerialPins();
#endif /* EFI_AUX_SERIAL */

#if EFI_HIP_9011
	startHip9001_pins();
#endif /* EFI_HIP_9011 */


#if EFI_IDLE_CONTROL
	if (isIdleHardwareRestartNeeded()) {
		 initIdleHardware(sharedLogger);
	}
#endif

#if EFI_VEHICLE_SPEED
	startVSSPins();
#endif /* EFI_VEHICLE_SPEED */

#if EFI_BOOST_CONTROL
	startBoostPin();
#endif
#if EFI_EMULATE_POSITION_SENSORS
	startTriggerEmulatorPins();
#endif /* EFI_EMULATE_POSITION_SENSORS */
#if EFI_LOGIC_ANALYZER
	startLogicAnalyzerPins();
#endif /* EFI_LOGIC_ANALYZER */
#if EFI_AUX_PID
	startVvtControlPins();
#endif /* EFI_AUX_PID */

	adcConfigListener(engine);
}

void setBor(int borValue) {
	scheduleMsg(sharedLogger, "setting BOR to %d", borValue);
	BOR_Set((BOR_Level_t)borValue);
	showBor();
}

void showBor(void) {
	scheduleMsg(sharedLogger, "BOR=%d", (int)BOR_Get());
}

void initHardware(Logging *l) {
	efiAssertVoid(CUSTOM_IH_STACK, getCurrentRemainingStack() > EXPECTED_REMAINING_STACK, "init h");
	sharedLogger = l;
	efiAssertVoid(CUSTOM_EC_NULL, engineConfiguration!=NULL, "engineConfiguration");
	

	printMsg(sharedLogger, "initHardware()");
	// todo: enable protection. it's disabled because it takes
	// 10 extra seconds to re-flash the chip
	//flashProtect();

#if EFI_HISTOGRAMS
	/**
	 * histograms is a data structure for CPU monitor, it does not depend on configuration
	 */
	initHistogramsModule();
#endif /* EFI_HISTOGRAMS */

	/**
	 * We need the LED_ERROR pin even before we read configuration
	 */
	initPrimaryPins(sharedLogger);

	if (hasFirmwareError()) {
		return;
	}

#if EFI_INTERNAL_FLASH

#ifdef CONFIG_RESET_SWITCH_PORT
// this pin is not configurable at runtime so that we have a reliable way to reset configuration
#define SHOULD_IGNORE_FLASH() (palReadPad(CONFIG_RESET_SWITCH_PORT, CONFIG_RESET_SWITCH_PIN) == 0)
#else
#define SHOULD_IGNORE_FLASH() (false)
#endif // CONFIG_RESET_SWITCH_PORT

#ifdef CONFIG_RESET_SWITCH_PORT
	palSetPadMode(CONFIG_RESET_SWITCH_PORT, CONFIG_RESET_SWITCH_PIN, PAL_MODE_INPUT_PULLUP);
#endif /* CONFIG_RESET_SWITCH_PORT */

	initFlash(sharedLogger);
	/**
	 * this call reads configuration from flash memory or sets default configuration
	 * if flash state does not look right.
	 *
	 * interesting fact that we have another read from flash before we get here
	 */
	if (SHOULD_IGNORE_FLASH()) {
		engineConfiguration->engineType = DEFAULT_ENGINE_TYPE;
		resetConfigurationExt(sharedLogger, engineConfiguration->engineType PASS_ENGINE_PARAMETER_SUFFIX);
		writeToFlashNow();
	} else {
		readFromFlash();
	}
#else
	engineConfiguration->engineType = DEFAULT_ENGINE_TYPE;
	resetConfigurationExt(sharedLogger, engineConfiguration->engineType PASS_ENGINE_PARAMETER_SUFFIX);
#endif /* EFI_INTERNAL_FLASH */

	// it's important to initialize this pretty early in the game before any scheduling usages
	initSingleTimerExecutorHardware();

#if EFI_HD44780_LCD
	lcd_HD44780_init(sharedLogger);
	if (hasFirmwareError())
		return;

	lcd_HD44780_print_string(VCS_VERSION);

#endif /* EFI_HD44780_LCD */

	if (hasFirmwareError()) {
		return;
	}

#if HAL_USE_ADC
	initAdcInputs();

	// wait for first set of ADC values so that we do not produce invalid sensor data
	waitForSlowAdc(1);
#endif /* HAL_USE_ADC */

#if EFI_SOFTWARE_KNOCK
	initSoftwareKnock();
#endif /* EFI_SOFTWARE_KNOCK */

	initRtc();

#if HAL_USE_SPI
	initSpiModules(engineConfiguration);
#endif /* HAL_USE_SPI */

#if BOARD_EXT_GPIOCHIPS > 0
	// initSmartGpio depends on 'initSpiModules'
	initSmartGpio(PASS_ENGINE_PARAMETER_SIGNATURE);
#endif

	// output pins potentially depend on 'initSmartGpio'
	initOutputPins(PASS_ENGINE_PARAMETER_SIGNATURE);

#if EFI_ENGINE_CONTROL
	enginePins.startPins(PASS_ENGINE_PARAMETER_SIGNATURE);
#endif /* EFI_ENGINE_CONTROL */

#if EFI_MC33816
	initMc33816(sharedLogger);
#endif /* EFI_MC33816 */

#if EFI_MAX_31855
	initMax31855(sharedLogger, CONFIG(max31855spiDevice), CONFIG(max31855_cs));
#endif /* EFI_MAX_31855 */

#if EFI_CAN_SUPPORT
	initCan();
#endif /* EFI_CAN_SUPPORT */

//	init_adc_mcp3208(&adcState, &SPID2);
//	requestAdcValue(&adcState, 0);

#if EFI_SHAFT_POSITION_INPUT
	// todo: figure out better startup logic
	initTriggerCentral(sharedLogger);
#endif /* EFI_SHAFT_POSITION_INPUT */

	turnOnHardware(sharedLogger);

#if EFI_HIP_9011
	initHip9011(sharedLogger);
#endif /* EFI_HIP_9011 */

#if EFI_MEMS
	initAccelerometer(PASS_ENGINE_PARAMETER_SIGNATURE);
#endif
//	initFixedLeds();


#if EFI_BOSCH_YAW
	initBoschYawRateSensor();
#endif /* EFI_BOSCH_YAW */

	//	initBooleanInputs();

#if EFI_UART_GPS
	initGps();
#endif

#if EFI_SERVO
	initServo();
#endif

#if EFI_AUX_SERIAL
	initAuxSerial();
#endif /* EFI_AUX_SERIAL */

#if EFI_VEHICLE_SPEED
	initVehicleSpeed(sharedLogger);
#endif // EFI_VEHICLE_SPEED

#if EFI_CAN_SUPPORT
	initCanVssSupport(sharedLogger);
#endif // EFI_CAN_SUPPORT

#if EFI_CDM_INTEGRATION
	cdmIonInit();
#endif // EFI_CDM_INTEGRATION

#if (HAL_USE_PAL && EFI_JOYSTICK)
	initJoystick(sharedLogger);
#endif /* HAL_USE_PAL && EFI_JOYSTICK */

	calcFastAdcIndexes();

	printMsg(sharedLogger, "initHardware() OK!");
}

#endif /* EFI_PROD_CODE */

#endif  /* EFI_PROD_CODE || EFI_SIMULATOR */

#if HAL_USE_SPI
// this is F4 implementation but we will keep it here for now for simplicity
int getSpiPrescaler(spi_speed_e speed, spi_device_e device) {
	switch (speed) {
	case _5MHz:
		return device == SPI_DEVICE_1 ? SPI_BaudRatePrescaler_16 : SPI_BaudRatePrescaler_8;
	case _2_5MHz:
		return device == SPI_DEVICE_1 ? SPI_BaudRatePrescaler_32 : SPI_BaudRatePrescaler_16;
	case _1_25MHz:
		return device == SPI_DEVICE_1 ? SPI_BaudRatePrescaler_64 : SPI_BaudRatePrescaler_32;

	case _150KHz:
		// SPI1 does not support 150KHz, it would be 300KHz for SPI1
		return SPI_BaudRatePrescaler_256;
	default:
		// unexpected
		return 0;
	}
}

#endif /* HAL_USE_SPI */
