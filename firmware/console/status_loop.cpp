/**
 * @file	status_loop.cpp
 * @brief Human-readable protocol status messages
 *
 * http://rusefi.com/forum/viewtopic.php?t=263 rusEfi console overview
 * http://rusefi.com/forum/viewtopic.php?t=210 Commands overview
 *
 *
 * @date Mar 15, 2013
 * @author Andrey Belomutskiy, (c) 2012-2020
 *
 * This file is part of rusEfi - see http://rusefi.com
 *
 * rusEfi is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * rusEfi is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "global.h"
#include "status_loop.h"
#include "hip9011_logic.h"
#include "engine_controller.h"

#include "adc_inputs.h"
#if EFI_LOGIC_ANALYZER
#include "logic_analyzer.h"
#endif /* EFI_LOGIC_ANALYZER */

#include "trigger_central.h"
#include "allsensors.h"
#include "sensor_reader.h"
#include "io_pins.h"
#include "efi_gpio.h"
#include "mmc_card.h"
#include "console_io.h"
#include "malfunction_central.h"
#include "speed_density.h"

#include "advance_map.h"
#include "tunerstudio.h"
#include "fuel_math.h"
#include "main_trigger_callback.h"
#include "engine_math.h"
#include "spark_logic.h"
#include "idle_thread.h"
#include "engine_configuration.h"
#include "os_util.h"
#include "svnversion.h"
#include "engine.h"
#include "lcd_controller.h"
#include "settings.h"
#include "can_hw.h"
#include "periodic_thread_controller.h"
#include "cdm_ion_sense.h"
#include "binary_logging.h"
#include "buffered_writer.h"
#include "dynoview.h"

extern bool main_loop_started;

#if EFI_PROD_CODE
// todo: move this logic to algo folder!
#include "rtc_helper.h"
#include "lcd_HD44780.h"
#include "rusefi.h"
#include "pin_repository.h"
#include "flash_main.h"
#include "max31855.h"
#include "vehicle_speed.h"
#include "single_timer_executor.h"
#include "periodic_task.h"
extern int icuRisingCallbackCounter;
extern int icuFallingCallbackCounter;
#endif /* EFI_PROD_CODE */

#if EFI_CJ125
#include "cj125.h"
#endif /* EFI_CJ125 */

#if EFI_MAP_AVERAGING
#include "map_averaging.h"
#endif

#if EFI_FSIO
#include "fsio_impl.h"
#endif /* EFI_FSIO */

#if (BOARD_TLE8888_COUNT > 0)
#include "tle8888.h"
#endif /* BOARD_TLE8888_COUNT */

#if EFI_ENGINE_SNIFFER
#include "engine_sniffer.h"
extern WaveChart waveChart;
#endif /* EFI_ENGINE_SNIFFER */

extern pin_output_mode_e DEFAULT_OUTPUT;
extern pin_output_mode_e INVERTED_OUTPUT;

#ifndef LED_WARNING_BRAIN_PIN_MODE
#define LED_WARNING_BRAIN_PIN_MODE	DEFAULT_OUTPUT
#endif
#ifndef LED_RUNING_BRAIN_PIN_MODE
#define LED_RUNING_BRAIN_PIN_MODE	DEFAULT_OUTPUT
#endif
#ifndef LED_COMMUNICATION_BRAIN_PIN_MODE
#define LED_COMMUNICATION_BRAIN_PIN_MODE	DEFAULT_OUTPUT
#endif

int warningEnabled = true;

extern int maxTriggerReentraint;
extern uint32_t maxLockedDuration;


#if !defined(STATUS_LOGGING_BUFFER_SIZE)
#define STATUS_LOGGING_BUFFER_SIZE 1800
#endif /* STATUS_LOGGING_BUFFER_SIZE */

static char LOGGING_BUFFER[STATUS_LOGGING_BUFFER_SIZE] CCM_OPTIONAL;
static Logging logger("status loop", LOGGING_BUFFER, sizeof(LOGGING_BUFFER));

static void setWarningEnabled(int value) {
	warningEnabled = value;
}

#if EFI_FILE_LOGGING
// this one needs to be in main ram so that SD card SPI DMA works fine
static NO_CACHE char sdLogBuffer[100];
static uint64_t binaryLogCount = 0;

#endif /* EFI_FILE_LOGGING */

EXTERN_ENGINE;

/**
 * This is useful if we are changing engine mode dynamically
 * For example http://rusefi.com/forum/viewtopic.php?f=5&t=1085
 */
static int packEngineMode(DECLARE_ENGINE_PARAMETER_SIGNATURE) {
	return (engineConfiguration->fuelAlgorithm << 4) +
			(engineConfiguration->injectionMode << 2) +
			engineConfiguration->ignitionMode;
}

static float getAirFlowGauge(DECLARE_ENGINE_PARAMETER_SIGNATURE) {
	return hasMafSensor() ? getRealMaf(PASS_ENGINE_PARAMETER_SIGNATURE) : engine->engineState.airFlow;
}

void writeLogLine(Writer& buffer) {
#if EFI_FILE_LOGGING
	if (!main_loop_started)
		return;

	if (binaryLogCount == 0) {
		writeHeader(buffer);
	} else {
		updateTunerStudioState(&tsOutputChannels);
		size_t length = writeBlock(sdLogBuffer);
		efiAssertVoid(OBD_PCM_Processor_Fault, length <= efi::size(sdLogBuffer), "SD log buffer overflow");
		buffer.write(sdLogBuffer, length);
	}

	binaryLogCount++;
#else
	(void)buffer;
#endif /* EFI_FILE_LOGGING */
}

static int prevCkpEventCounter = -1;

static LoggingWithStorage logger2("main event handler");

/**
 * Time when the firmware version was reported last time, in seconds
 * TODO: implement a request/response instead of just constantly sending this out
 */
static systime_t timeOfPreviousPrintVersion = 0;

#if EFI_PROD_CODE
static void printOutPin(const char *pinName, brain_pin_e hwPin) {
	if (isBrainPinValid(hwPin)) {
		logger.appendPrintf("%s%s%s@%s%s", PROTOCOL_OUTPIN, DELIMETER, pinName, hwPortname(hwPin), DELIMETER);
	}
}
#endif /* EFI_PROD_CODE */

void printOverallStatus(efitimesec_t nowSeconds) {
#if EFI_ENGINE_SNIFFER
	waveChart.publishIfFull();
#endif /* EFI_ENGINE_SNIFFER */

	/**
	 * we report the version every 4 seconds - this way the console does not need to
	 * request it and we will display it pretty soon
	 */
	if (overflowDiff(nowSeconds, timeOfPreviousPrintVersion) < 4) {
		return;
	}
	timeOfPreviousPrintVersion = nowSeconds;
	int seconds = getTimeNowSeconds();
	printCurrentState(&logger, seconds, getConfigurationName(engineConfiguration->engineType), FIRMWARE_ID);
#if EFI_PROD_CODE
	printOutPin(PROTOCOL_CRANK1, CONFIG(triggerInputPins)[0]);
	printOutPin(PROTOCOL_CRANK2, CONFIG(triggerInputPins)[1]);
	printOutPin(PROTOCOL_VVT_NAME, engineConfiguration->camInputs[0]);
	printOutPin(PROTOCOL_HIP_NAME, CONFIG(hip9011IntHoldPin));
	printOutPin(PROTOCOL_TACH_NAME, CONFIG(tachOutputPin));
#if EFI_LOGIC_ANALYZER
	printOutPin(PROTOCOL_WA_CHANNEL_1, CONFIG(logicAnalyzerPins)[0]);
	printOutPin(PROTOCOL_WA_CHANNEL_2, CONFIG(logicAnalyzerPins)[1]);
#endif /* EFI_LOGIC_ANALYZER */

	for (int i = 0; i < engineConfiguration->specs.cylindersCount; i++) {
		printOutPin(enginePins.coils[i].getShortName(), CONFIG(ignitionPins)[i]);

		printOutPin(enginePins.injectors[i].getShortName(), CONFIG(injectionPins)[i]);
	}
	for (int i = 0; i < AUX_DIGITAL_VALVE_COUNT;i++) {
		printOutPin(enginePins.auxValve[i].getShortName(), engineConfiguration->auxValves[i]);
	}

#endif /* EFI_PROD_CODE */
	scheduleLogging(&logger);
}

static systime_t timeOfPreviousReport = (systime_t) -1;

/**
 * @brief Sends all pending data to rusEfi console
 *
 * This method is periodically invoked by the main loop
 * todo: is this mostly dead code?
 */
void updateDevConsoleState(void) {
	// todo: make SWO work
//	char *msg = "hello\r\n";
//	for(int i=0;i<strlen(msg);i++) {
//		ITM_SendChar(msg[i]);
//	}



	if (!isCommandLineConsoleReady()) {
		return;
	}

#if EFI_PROD_CODE
	// todo: unify with simulator!
	if (hasFirmwareError()) {
		scheduleMsg(&logger, "%s error: %s", CRITICAL_PREFIX, getFirmwareError());
		warningEnabled = false;
		scheduleLogging(&logger);
		return;
	}
#endif /* EFI_PROD_CODE */

#if HAL_USE_ADC
	printFullAdcReportIfNeeded(&logger);
#endif /* HAL_USE_ADC */

	systime_t nowSeconds = getTimeNowSeconds();

#if EFI_ENGINE_CONTROL && EFI_SHAFT_POSITION_INPUT
	int currentCkpEventCounter = engine->triggerCentral.triggerState.getTotalEventCounter();
	if (prevCkpEventCounter == currentCkpEventCounter && timeOfPreviousReport == nowSeconds) {
		return;
	}
	timeOfPreviousReport = nowSeconds;

	prevCkpEventCounter = currentCkpEventCounter;
#else
	chThdSleepMilliseconds(200);
#endif

#if EFI_LOGIC_ANALYZER
	printWave(&logger);
#endif /* EFI_LOGIC_ANALYZER */

	scheduleLogging(&logger);
}

/*
 * command example:
 * sfm 3500 400
 * that would be 'show fuel for rpm 3500 maf 4.0'
 */

static void showFuelInfo2(float rpm, float engineLoad) {
	scheduleMsg(&logger, "inj flow %.2fcc/min displacement %.2fL", engineConfiguration->injector.flow,
			engineConfiguration->specs.displacement);

	scheduleMsg(&logger2, "algo=%s/pump=%s", getEngine_load_mode_e(engineConfiguration->fuelAlgorithm),
			boolToString(enginePins.fuelPumpRelay.getLogicValue()));

	scheduleMsg(&logger2, "injection phase=%.2f/global fuel correction=%.2f", getInjectionOffset(rpm, getFuelingLoad()), engineConfiguration->globalFuelCorrection);

	scheduleMsg(&logger2, "baro correction=%.2f", engine->engineState.baroCorrection);

#if EFI_ENGINE_CONTROL
	scheduleMsg(&logger, "base cranking fuel %.2f", engineConfiguration->cranking.baseFuel);
	scheduleMsg(&logger2, "cranking fuel: %.2f", ENGINE(engineState.cranking.fuel));

	if (!engine->rpmCalculator.isStopped()) {
		float iatCorrection = engine->engineState.running.intakeTemperatureCoefficient;
		float cltCorrection = engine->engineState.running.coolantTemperatureCoefficient;
		floatms_t injectorLag = engine->engineState.running.injectorLag;
		scheduleMsg(&logger2, "rpm=%.2f engineLoad=%.2f", rpm, engineLoad);

		scheduleMsg(&logger2, "iatCorrection=%.2f cltCorrection=%.2f injectorLag=%.2f", iatCorrection, cltCorrection,
				injectorLag);
	}
#endif
}

#if EFI_ENGINE_CONTROL
static void showFuelInfo(void) {
	showFuelInfo2((float) GET_RPM(), getFuelingLoad(PASS_ENGINE_PARAMETER_SIGNATURE));
}
#endif

static OutputPin *leds[] = { &enginePins.warningLedPin, &enginePins.runningLedPin,
		&enginePins.errorLedPin, &enginePins.communicationLedPin, &enginePins.checkEnginePin };

static void initStatusLeds(void) {
	enginePins.communicationLedPin.initPin("led: comm status", engineConfiguration->communicationLedPin, &LED_COMMUNICATION_BRAIN_PIN_MODE);
	// checkEnginePin is already initialized by the time we get here

	enginePins.warningLedPin.initPin("led: warning status", engineConfiguration->warningLedPin, &LED_WARNING_BRAIN_PIN_MODE);
	enginePins.runningLedPin.initPin("led: running status", engineConfiguration->runningLedPin, &LED_RUNING_BRAIN_PIN_MODE);
}

#if EFI_PROD_CODE

static bool isTriggerErrorNow() {
#if EFI_ENGINE_CONTROL && EFI_SHAFT_POSITION_INPUT
	bool justHadError = (getTimeNowNt() - engine->triggerCentral.triggerState.lastDecodingErrorTime) < MS2NT(200);
	return justHadError || isTriggerDecoderError();
#else
	return false;
#endif /* EFI_ENGINE_CONTROL && EFI_SHAFT_POSITION_INPUT */
}

extern bool consoleByteArrived;

class CommunicationBlinkingTask : public PeriodicTimerController {

	int getPeriodMs() override {
		return counter % 2 == 0 ? onTimeMs : offTimeMs;
	}

	void setAllLeds(int value) {
		// make sure we do not turn the critical LED off if already have
		// critical error by now
		for (uint32_t i = 0; !hasFirmwareError() && i < sizeof(leds) / sizeof(leds[0]); i++) {
			leds[i]->setValue(value);
		}
	}

	void PeriodicTask() override {
		counter++;

		bool lowVBatt = Sensor::get(SensorType::BatteryVoltage).value_or(0) < LOW_VBATT;

		if (counter == 1) {
			// first invocation of BlinkingTask
			setAllLeds(1);
		} else if (counter == 2) {
			// second invocation of BlinkingTask
			setAllLeds(0);
		} else if (counter % 2 == 0) {
			enginePins.communicationLedPin.setValue(0);
#if HW_CHECK_MODE
			// we have to do anything possible to help users notice FACTORY MODE
			enginePins.errorLedPin.setValue(1);
			enginePins.runningLedPin.setValue(1);
#endif // HW_CHECK_MODE
			if (!lowVBatt) {
				enginePins.warningLedPin.setValue(0);
			}
		} else {
#define BLINKING_PERIOD_MS 33

			if (hasFirmwareError()) {
				// special behavior in case of critical error - not equal on/off time
				// this special behaviour helps to notice that something is not right, also
				// differentiates software firmware error from critical interrupt error with CPU halt.
				offTimeMs = 50;
				onTimeMs = 450;
			} else if (consoleByteArrived) {
				consoleByteArrived = false;
				offTimeMs = 100;
				onTimeMs = 33;
#if EFI_INTERNAL_FLASH
			} else if (getNeedToWriteConfiguration()) {
				offTimeMs = onTimeMs = 500;
#endif // EFI_INTERNAL_FLASH
			} else {
				onTimeMs =
#if EFI_USB_SERIAL
				is_usb_serial_ready() ? 3 * BLINKING_PERIOD_MS :
#endif // EFI_USB_SERIAL
				BLINKING_PERIOD_MS;
				offTimeMs = 0.6 * onTimeMs;
			}

			enginePins.communicationLedPin.setValue(1);
#if HW_CHECK_MODE
			// we have to do anything possible to help users notice FACTORY MODE
			enginePins.errorLedPin.setValue(0);
			enginePins.runningLedPin.setValue(0);
#endif // HW_CHECK_MODE

	#if EFI_ENGINE_CONTROL
			if (lowVBatt || isTriggerErrorNow() || isIgnitionTimingError()) {
				// todo: at the moment warning codes do not affect warning LED?!
				enginePins.warningLedPin.setValue(1);
			}
	#endif /* EFI_ENGINE_CONTROL */
		}
	}

private:
	int counter = 0;
	int onTimeMs = 100;
	int offTimeMs = 100;
};

static CommunicationBlinkingTask communicationsBlinkingTask;

#endif /* EFI_PROD_CODE */

#if EFI_LCD
class LcdController : public PeriodicController<UTILITY_THREAD_STACK_SIZE> {
public:
	LcdController() : PeriodicController("LCD") { }
private:
	void PeriodicTask(efitick_t nowNt) override	{
		UNUSED(nowNt);
		setPeriod(NOT_TOO_OFTEN(10 /* ms */, 300));
		if (engineConfiguration->useLcdScreen) {
#if EFI_HD44780_LCD
			updateHD44780lcd();
#endif
		}
	}
};

static LcdController lcdInstance;
#endif /* EFI_LCD */

#if EFI_HIP_9011
extern HIP9011 instance;
#endif /* EFI_HIP_9011 */


#if EFI_TUNER_STUDIO

void updateTunerStudioState(TunerStudioOutputChannels *tsOutputChannels DECLARE_ENGINE_PARAMETER_SUFFIX) {
#if EFI_SHAFT_POSITION_INPUT
	int rpm = Sensor::get(SensorType::Rpm).Value;
#else /* EFI_SHAFT_POSITION_INPUT */
	int rpm = 0;
#endif /* EFI_SHAFT_POSITION_INPUT */

#if EFI_PROD_CODE
	executorStatistics();
#endif /* EFI_PROD_CODE */

#if EFI_SIMULATOR
	tsOutputChannels->sd_status = 1 + 4;
#endif

	// header
	tsOutputChannels->tsConfigVersion = TS_FILE_VERSION;

	// offset 0
	tsOutputChannels->rpm = rpm;

	SensorResult clt = Sensor::get(SensorType::Clt);
	tsOutputChannels->coolantTemperature = clt.Value;
	tsOutputChannels->isCltError = !clt.Valid;

	SensorResult iat = Sensor::get(SensorType::Iat);
	tsOutputChannels->intakeAirTemperature = iat.Value;
	tsOutputChannels->isIatError = !iat.Valid;

	SensorResult auxTemp1 = Sensor::get(SensorType::AuxTemp1);
	tsOutputChannels->auxTemp1 = auxTemp1.Value;

	SensorResult auxTemp2 = Sensor::get(SensorType::AuxTemp2);
	tsOutputChannels->auxTemp2 = auxTemp2.Value;

	SensorResult tps1 = Sensor::get(SensorType::Tps1);
	tsOutputChannels->throttlePosition = tps1.Value;
	tsOutputChannels->isTpsError = !tps1.Valid;
	tsOutputChannels->tpsADC = convertVoltageTo10bitADC(Sensor::getRaw(SensorType::Tps1Primary));

	SensorResult tps2 = Sensor::get(SensorType::Tps2);
	tsOutputChannels->throttle2Position = tps2.Value;
	// If we don't have a TPS2 at all, don't turn on the failure light
	tsOutputChannels->isTps2Error = !tps2.Valid && Sensor::hasSensor(SensorType::Tps2Primary);

	SensorResult pedal = Sensor::get(SensorType::AcceleratorPedal);
	tsOutputChannels->pedalPosition = pedal.Value;
	// Only report fail if you have one (many people don't)
	tsOutputChannels->isPedalError = !pedal.Valid && Sensor::hasSensor(SensorType::AcceleratorPedalPrimary);

	// Set raw sensors
	tsOutputChannels->rawTps1Primary = Sensor::getRaw(SensorType::Tps1Primary);
	tsOutputChannels->rawPpsPrimary = Sensor::getRaw(SensorType::AcceleratorPedalPrimary);
	tsOutputChannels->rawPpsSecondary = Sensor::getRaw(SensorType::AcceleratorPedalSecondary);
	tsOutputChannels->rawClt = Sensor::getRaw(SensorType::Clt);
	tsOutputChannels->rawIat = Sensor::getRaw(SensorType::Iat);
	tsOutputChannels->rawOilPressure = Sensor::getRaw(SensorType::OilPressure);
	tsOutputChannels->rawLowFuelPressure = Sensor::getRaw(SensorType::FuelPressureLow);
	tsOutputChannels->rawHighFuelPressure = Sensor::getRaw(SensorType::FuelPressureHigh);

	// offset 16
	tsOutputChannels->massAirFlowVoltage = hasMafSensor() ? getMafVoltage(PASS_ENGINE_PARAMETER_SIGNATURE) : 0;

	float lambdaValue = Sensor::get(SensorType::Lambda1).value_or(0);
	tsOutputChannels->lambda = lambdaValue;
	tsOutputChannels->airFuelRatio = lambdaValue * ENGINE(engineState.stoichiometricRatio);

	float lambda2Value = Sensor::get(SensorType::Lambda2).value_or(0);
	tsOutputChannels->lambda2 = lambda2Value;
	tsOutputChannels->airFuelRatio2 = lambda2Value * ENGINE(engineState.stoichiometricRatio);

	// offset 24
	tsOutputChannels->engineLoad = getEngineLoadT(PASS_ENGINE_PARAMETER_SIGNATURE);

	tsOutputChannels->fuelingLoad = getFuelingLoad(PASS_ENGINE_PARAMETER_SIGNATURE);
	tsOutputChannels->ignitionLoad = getIgnitionLoad(PASS_ENGINE_PARAMETER_SIGNATURE);
	tsOutputChannels->veTableYAxis = ENGINE(engineState.currentVeLoad);
	tsOutputChannels->afrTableYAxis = ENGINE(engineState.currentAfrLoad);

	// offset 28
	tsOutputChannels->vBatt = Sensor::get(SensorType::BatteryVoltage).value_or(0);

	// offset 36
	tsOutputChannels->baroPressure = Sensor::get(SensorType::BarometricPressure).value_or(0);

	// 48
	tsOutputChannels->fuelBase = engine->engineState.baseFuel * 1000;	// Convert grams to mg
	// 64
	tsOutputChannels->actualLastInjection = ENGINE(actualLastInjection);


	// 104
	tsOutputChannels->rpmAcceleration = engine->rpmCalculator.getRpmAcceleration();
	// offset 108
	// For air-interpolated tCharge mode, we calculate a decent massAirFlow approximation, so we can show it to users even without MAF sensor!
	tsOutputChannels->massAirFlow = getAirFlowGauge(PASS_ENGINE_PARAMETER_SIGNATURE);
	// offset 116
	// TPS acceleration
	tsOutputChannels->deltaTps = engine->tpsAccelEnrichment.getMaxDelta();
	// 128
	tsOutputChannels->totalTriggerErrorCounter = engine->triggerCentral.triggerState.totalTriggerErrorCounter;
	// 132
	tsOutputChannels->orderingErrorCounter = engine->triggerCentral.triggerState.orderingErrorCounter;
	// 68
	tsOutputChannels->baroCorrection = engine->engineState.baroCorrection;
	// 140
#if EFI_ENGINE_CONTROL
	tsOutputChannels->injectorDutyCycle = getInjectorDutyCycle(rpm PASS_ENGINE_PARAMETER_SUFFIX);
#endif
	// 148
	tsOutputChannels->fuelTankLevel = engine->sensors.fuelTankLevel;
	// 160
	const auto& wallFuel = ENGINE(injectionEvents.elements[0].wallFuel);
	tsOutputChannels->wallFuelAmount = wallFuel.getWallFuel();
	// 168
	tsOutputChannels->wallFuelCorrection = wallFuel.wallFuelCorrection;

	// 164
	tsOutputChannels->iatCorrection = ENGINE(engineState.running.intakeTemperatureCoefficient);
	// 184
	tsOutputChannels->cltCorrection = ENGINE(engineState.running.coolantTemperatureCoefficient);
	// 188
	tsOutputChannels->fuelRunning = ENGINE(engineState.running.fuel);
	// 196
	tsOutputChannels->injectorLagMs = ENGINE(engineState.running.injectorLag);
	// 224
	efitimesec_t timeSeconds = getTimeNowSeconds();
	tsOutputChannels->timeSeconds = timeSeconds;

#if EFI_SHAFT_POSITION_INPUT
	// 248
	tsOutputChannels->vvtPosition = engine->triggerCentral.getVVTPosition();
#endif

	// 252
	tsOutputChannels->engineMode = packEngineMode(PASS_ENGINE_PARAMETER_SIGNATURE);
	// 120
	tsOutputChannels->firmwareVersion = getRusEfiVersion();
	// 268
	tsOutputChannels->shortTermFuelTrim = 100.0f * (ENGINE(engineState.running.pidCorrection) - 1.0f);
	// 276
	tsOutputChannels->accelerationX = engine->sensors.accelerometer.x;
	// 278
	tsOutputChannels->accelerationY = engine->sensors.accelerometer.y;
	// 280
	tsOutputChannels->oilPressure = Sensor::get(SensorType::OilPressure).Value;

	// Low pressure is directly in kpa
	tsOutputChannels->lowFuelPressure = Sensor::get(SensorType::FuelPressureLow).Value;
	// High pressure is in bar, aka 100 kpa
	tsOutputChannels->highFuelPressure = KPA2BAR(Sensor::get(SensorType::FuelPressureHigh).Value);

	tsOutputChannels->flexPercent = Sensor::get(SensorType::FuelEthanolPercent).Value;

	// 288
	tsOutputChannels->injectionOffset = engine->engineState.injectionOffset;

	// offset 112
	tsOutputChannels->veValue = engine->engineState.currentVe;
	tsOutputChannels->currentTargetAfr = ENGINE(engineState.targetAFR);
	tsOutputChannels->targetLambda = ENGINE(engineState.targetLambda);

	tsOutputChannels->manifoldAirPressure = Sensor::get(SensorType::Map).value_or(0);

#if EFI_DYNO_VIEW
	tsOutputChannels->VssAcceleration = getDynoviewAcceleration(PASS_ENGINE_PARAMETER_SIGNATURE);
#endif

	//tsOutputChannels->knockCount = engine->knockCount;
	//tsOutputChannels->knockLevel = engine->knockVolts;

#if HW_CHECK_MODE
	tsOutputChannels->hasCriticalError = 1;
#else
	tsOutputChannels->hasCriticalError = hasFirmwareError();
#endif // HW_CHECK_MODE

	tsOutputChannels->isWarnNow = engine->engineState.warnings.isWarningNow(timeSeconds, true);
#if EFI_HIP_9011
	tsOutputChannels->isKnockChipOk = (instance.invalidHip9011ResponsesCount == 0);
#endif /* EFI_HIP_9011 */

#if EFI_LAUNCH_CONTROL
	tsOutputChannels->launchTriggered = engine->isLaunchCondition;
#endif

	tsOutputChannels->tpsAccelFuel = engine->engineState.tpsAccelEnrich;
	// engine load acceleration
	if (hasMapSensor(PASS_ENGINE_PARAMETER_SIGNATURE)) {
		tsOutputChannels->engineLoadAccelExtra = engine->engineLoadAccelEnrichment.getEngineLoadEnrichment(PASS_ENGINE_PARAMETER_SIGNATURE) * 100 / Sensor::get(SensorType::Map).value_or(0);
	}
	tsOutputChannels->engineLoadDelta = engine->engineLoadAccelEnrichment.getMaxDelta();

	tsOutputChannels->checkEngine = hasErrorCodes();

#if	HAL_USE_ADC
	tsOutputChannels->internalMcuTemperature = getMCUInternalTemperature();
#endif /* HAL_USE_ADC */

#if EFI_MAX_31855
	for (int i = 0; i < EGT_CHANNEL_COUNT; i++)
		tsOutputChannels->egtValues.values[i] = getEgtValue(i);
#endif /* EFI_MAX_31855 */

#if EFI_IDLE_CONTROL
	tsOutputChannels->idlePosition = getIdlePosition();
#endif

	tsOutputChannels->idlePositionSensor = Sensor::get(SensorType::IdlePosition).value_or(0);
	tsOutputChannels->rawIdlePositionSensor = Sensor::getRaw(SensorType::IdlePosition);

	tsOutputChannels->wastegatePosition = Sensor::get(SensorType::WastegatePosition).value_or(0);
	tsOutputChannels->rawWastegatePositionSensor = Sensor::getRaw(SensorType::WastegatePosition);

#if EFI_PROD_CODE
	tsOutputChannels->isTriggerError = isTriggerErrorNow();

#if EFI_INTERNAL_FLASH
	tsOutputChannels->needBurn = getNeedToWriteConfiguration();
#endif /* EFI_INTERNAL_FLASH */

#if EFI_FILE_LOGGING
	tsOutputChannels->hasSdCard = isSdCardAlive();
#endif /* EFI_FILE_LOGGING */

	tsOutputChannels->isFuelPumpOn = enginePins.fuelPumpRelay.getLogicValue();
	tsOutputChannels->isFanOn = enginePins.fanRelay.getLogicValue();
	tsOutputChannels->isO2HeaterOn = enginePins.o2heater.getLogicValue();
	tsOutputChannels->isIgnitionEnabledIndicator = ENGINE(limpManager).allowIgnition();
	tsOutputChannels->isInjectionEnabledIndicator = ENGINE(limpManager).allowInjection();
	tsOutputChannels->isCylinderCleanupEnabled = engineConfiguration->isCylinderCleanupEnabled;
	tsOutputChannels->isCylinderCleanupActivated = engine->isCylinderCleanupMode;

#if EFI_VEHICLE_SPEED
	float vehicleSpeed = getVehicleSpeed();
	tsOutputChannels->vehicleSpeedKph = vehicleSpeed;
	tsOutputChannels->speedToRpmRatio = vehicleSpeed / rpm;

#endif /* EFI_VEHICLE_SPEED */
#endif /* EFI_PROD_CODE */

	tsOutputChannels->fuelConsumptionPerHour = engine->engineState.fuelConsumption.perSecondConsumption;

	tsOutputChannels->warningCounter = engine->engineState.warnings.warningCounter;
	tsOutputChannels->lastErrorCode = engine->engineState.warnings.lastErrorCode;
	for (int i = 0; i < 8;i++) {
		tsOutputChannels->recentErrorCodes[i] = engine->engineState.warnings.recentWarnings.get(i);
	}

	tsOutputChannels->knockNowIndicator = engine->knockCount > 0;
	tsOutputChannels->knockEverIndicator = engine->knockEver;

	tsOutputChannels->clutchUpState = engine->clutchUpState;
	tsOutputChannels->clutchDownState = engine->clutchDownState;
	tsOutputChannels->brakePedalState = engine->brakePedalState;
	tsOutputChannels->acSwitchState = engine->acSwitchState;

#if EFI_ENGINE_CONTROL
	// tCharge depends on the previous state, so we should use the stored value.
	tsOutputChannels->tCharge = ENGINE(engineState.sd.tCharge);
	float timing = engine->engineState.timingAdvance;
	tsOutputChannels->ignitionAdvance = timing > 360 ? timing - 720 : timing;
	// 60
	tsOutputChannels->sparkDwell = ENGINE(engineState.sparkDwell);
	tsOutputChannels->crankingFuelMass = ENGINE(engineState.cranking.fuel);
	tsOutputChannels->chargeAirMass = engine->engineState.sd.airMassInOneCylinder;

	tsOutputChannels->coilDutyCycle = getCoilDutyCycle(rpm PASS_ENGINE_PARAMETER_SUFFIX);
#endif // EFI_ENGINE_CONTROL

	switch (engineConfiguration->debugMode)	{
	case DBG_START_STOP:
		tsOutputChannels->debugIntField1 = engine->startStopStateToggleCounter;
		tsOutputChannels->debugIntField2 = enginePins.starterControl.getLogicValue();
		tsOutputChannels->debugIntField3 = enginePins.starterRelayDisable.getLogicValue();
		break;
	case DBG_STATUS:
		tsOutputChannels->debugFloatField1 = timeSeconds;
		tsOutputChannels->debugIntField1 = atoi(VCS_VERSION);
		break;
	case DBG_METRICS:
#if EFI_CLOCK_LOCKS
		tsOutputChannels->debugIntField1 = maxLockedDuration;
		tsOutputChannels->debugIntField2 = maxTriggerReentraint;
#endif /* EFI_CLOCK_LOCKS */
		break;
	case DBG_TPS_ACCEL:
		tsOutputChannels->debugIntField1 = engine->tpsAccelEnrichment.cb.getSize();
		break;
	case DBG_SR5_PROTOCOL: {
		const int _10_6 = 100000;
		tsOutputChannels->debugIntField1 = tsState.textCommandCounter * _10_6 +  tsState.totalCounter;
		tsOutputChannels->debugIntField2 = tsState.outputChannelsCommandCounter * _10_6 + tsState.writeValueCommandCounter;
		tsOutputChannels->debugIntField3 = tsState.readPageCommandsCounter * _10_6 + tsState.burnCommandCounter;
		break;
		}
	case DBG_AUX_VALVES:
		tsOutputChannels->debugFloatField1 = engine->engineState.auxValveStart;
		tsOutputChannels->debugFloatField2 = engine->engineState.auxValveEnd;
		break;
	case DBG_TRIGGER_COUNTERS:
		tsOutputChannels->debugIntField1 = engine->triggerCentral.getHwEventCounter((int)SHAFT_PRIMARY_FALLING);
		tsOutputChannels->debugIntField2 = engine->triggerCentral.getHwEventCounter((int)SHAFT_SECONDARY_FALLING);
// no one uses shaft so far		tsOutputChannels->debugIntField3 = engine->triggerCentral.getHwEventCounter((int)SHAFT_3RD_FALLING);
#if EFI_PROD_CODE && HAL_USE_ICU == TRUE
		tsOutputChannels->debugIntField3 = icuRisingCallbackCounter + icuFallingCallbackCounter;
		tsOutputChannels->debugIntField4 = engine->triggerCentral.vvtEventRiseCounter;
		tsOutputChannels->debugIntField5 = engine->triggerCentral.vvtEventFallCounter;
#endif /* EFI_PROD_CODE */

		tsOutputChannels->debugFloatField1 = engine->triggerCentral.getHwEventCounter((int)SHAFT_PRIMARY_RISING);
		tsOutputChannels->debugFloatField2 = engine->triggerCentral.getHwEventCounter((int)SHAFT_SECONDARY_RISING);

		tsOutputChannels->debugIntField4 = engine->triggerCentral.triggerState.currentCycle.eventCount[0];
		tsOutputChannels->debugIntField5 = engine->triggerCentral.triggerState.currentCycle.eventCount[1];

		// debugFloatField6 used
		// no one uses shaft so far		tsOutputChannels->debugFloatField3 = engine->triggerCentral.getHwEventCounter((int)SHAFT_3RD_RISING);
		break;
	case DBG_FSIO_ADC:
		// todo: implement a proper loop
		if (isAdcChannelValid(engineConfiguration->fsioAdc[0])) {
			tsOutputChannels->debugFloatField1 = getVoltage("fsio", engineConfiguration->fsioAdc[0] PASS_ENGINE_PARAMETER_SUFFIX);
		}
		break;
	case DBG_FSIO_EXPRESSION_1_7:
#if EFI_FSIO
		tsOutputChannels->debugFloatField1 = getFsioOutputValue(0 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField2 = getFsioOutputValue(1 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField3 = getFsioOutputValue(2 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField4 = getFsioOutputValue(3 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField5 = getFsioOutputValue(4 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField6 = getFsioOutputValue(5 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField7 = getFsioOutputValue(6 PASS_ENGINE_PARAMETER_SUFFIX);
		break;
	case DBG_FSIO_EXPRESSION_8_14:
		tsOutputChannels->debugFloatField1 = getFsioOutputValue(7 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField2 = getFsioOutputValue(8 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField3 = getFsioOutputValue(9 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField4 = getFsioOutputValue(10 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField5 = getFsioOutputValue(11 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField6 = getFsioOutputValue(12 PASS_ENGINE_PARAMETER_SUFFIX);
		tsOutputChannels->debugFloatField7 = getFsioOutputValue(13 PASS_ENGINE_PARAMETER_SUFFIX);
		break;
	case DBG_FSIO_SPECIAL:
		tsOutputChannels->debugFloatField1 = ENGINE(fsioState.fsioIdleOffset);
		tsOutputChannels->debugFloatField2 = ENGINE(fsioState.fsioIdleMinValue);
		break;
#endif /* EFI_FSIO */
	case DBG_VEHICLE_SPEED_SENSOR:
		tsOutputChannels->debugIntField1 = engine->engineState.vssEventCounter;
		break;
	case DBG_CRANKING_DETAILS:
		tsOutputChannels->debugIntField1 = engine->rpmCalculator.getRevolutionCounterSinceStart();
		break;
#if EFI_HIP_9011
	case DBG_KNOCK:
		// todo: maybe extract hipPostState(tsOutputChannels);
		tsOutputChannels->debugIntField1 = instance.correctResponsesCount;
		tsOutputChannels->debugIntField2 = instance.invalidHip9011ResponsesCount;
		break;
#endif /* EFI_HIP_9011 */
#if EFI_CJ125 && HAL_USE_SPI
	case DBG_CJ125:
		cjPostState(tsOutputChannels);
		break;
#endif /* EFI_CJ125 && HAL_USE_SPI */
#if EFI_MAP_AVERAGING
		case DBG_MAP:
		postMapState(tsOutputChannels);
		break;
#endif /* EFI_MAP_AVERAGING */
#if EFI_CAN_SUPPORT
	case DBG_CAN:
		postCanState(tsOutputChannels);
		break;
#endif /* EFI_CAN_SUPPORT */
	case DBG_ANALOG_INPUTS:
		tsOutputChannels->debugFloatField1 = isAdcChannelValid(engineConfiguration->vbattAdcChannel) ? getVoltageDivided("vbatt", engineConfiguration->vbattAdcChannel PASS_ENGINE_PARAMETER_SUFFIX) : 0.0f;
		tsOutputChannels->debugFloatField2 = Sensor::getRaw(SensorType::Tps1);
		tsOutputChannels->debugFloatField3 = isAdcChannelValid(engineConfiguration->mafAdcChannel) ? getVoltageDivided("maf", engineConfiguration->mafAdcChannel PASS_ENGINE_PARAMETER_SUFFIX) : 0.0f;
		tsOutputChannels->debugFloatField4 = isAdcChannelValid(engineConfiguration->map.sensor.hwChannel) ? getVoltageDivided("map", engineConfiguration->map.sensor.hwChannel PASS_ENGINE_PARAMETER_SUFFIX) : 0.0f;
		tsOutputChannels->debugFloatField5 = isAdcChannelValid(engineConfiguration->clt.adcChannel) ? getVoltageDivided("clt", engineConfiguration->clt.adcChannel PASS_ENGINE_PARAMETER_SUFFIX) : 0.0f;
		tsOutputChannels->debugFloatField6 = isAdcChannelValid(engineConfiguration->iat.adcChannel) ? getVoltageDivided("iat", engineConfiguration->iat.adcChannel PASS_ENGINE_PARAMETER_SUFFIX) : 0.0f;
		tsOutputChannels->debugFloatField7 = isAdcChannelValid(engineConfiguration->afr.hwChannel) ? getVoltageDivided("ego", engineConfiguration->afr.hwChannel PASS_ENGINE_PARAMETER_SUFFIX) : 0.0f;
		break;
	case DBG_ANALOG_INPUTS2:
		// TPS 1 pri/sec split
		tsOutputChannels->debugFloatField1 = Sensor::get(SensorType::Tps1Primary).value_or(0) - Sensor::get(SensorType::Tps1Secondary).value_or(0);
		// TPS 2 pri/sec split
		tsOutputChannels->debugFloatField2 = Sensor::get(SensorType::Tps2Primary).value_or(0) - Sensor::get(SensorType::Tps2Secondary).value_or(0);
		// TPS1 - TPS2 split
		tsOutputChannels->debugFloatField3 = Sensor::get(SensorType::Tps1).value_or(0) - Sensor::get(SensorType::Tps2).value_or(0);
		// Pedal pri/sec split
		tsOutputChannels->debugFloatField4 = Sensor::get(SensorType::AcceleratorPedalPrimary).value_or(0) - Sensor::get(SensorType::AcceleratorPedalSecondary).value_or(0);
		break;
	case DBG_INSTANT_RPM:
		{
			float instantRpm = engine->triggerCentral.triggerState.getInstantRpm();
			tsOutputChannels->debugFloatField1 = instantRpm;
			tsOutputChannels->debugFloatField2 = instantRpm / GET_RPM();
		}
		break;
	case DBG_ION:
#if EFI_CDM_INTEGRATION
		ionPostState(tsOutputChannels);
#endif /* EFI_CDM_INTEGRATION */
		break;
	case DBG_TLE8888:
#if (BOARD_TLE8888_COUNT > 0)
		tle8888PostState(tsOutputChannels->getDebugChannels());
#endif /* BOARD_TLE8888_COUNT */
		break;
	case DBG_LOGIC_ANALYZER: 
#if EFI_LOGIC_ANALYZER	
		reportLogicAnalyzerToTS();
#endif /* EFI_LOGIC_ANALYZER */		
		break;
	default:
		;
	}
}

void prepareTunerStudioOutputs(void) {
	// sensor state for EFI Analytics Tuner Studio
	updateTunerStudioState(&tsOutputChannels PASS_ENGINE_PARAMETER_SUFFIX);
}

#endif /* EFI_TUNER_STUDIO */

void initStatusLoop(void) {
	addConsoleActionI("warn", setWarningEnabled);

#if EFI_ENGINE_CONTROL
	addConsoleActionFF("fuelinfo2", (VoidFloatFloat) showFuelInfo2);
	addConsoleAction("fuelinfo", showFuelInfo);
#endif
}

void startStatusThreads(void) {
	// todo: refactoring needed, this file should probably be split into pieces
#if EFI_PROD_CODE
	initStatusLeds();
	communicationsBlinkingTask.Start();
#endif /* EFI_PROD_CODE */

#if EFI_LCD
	lcdInstance.Start();
#endif /* EFI_LCD */
}
