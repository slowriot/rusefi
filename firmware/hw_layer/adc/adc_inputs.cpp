/**
 * @file	adc_inputs.cpp
 * @brief	Low level ADC code
 *
 * rusEfi uses two ADC devices on the same 16 pins at the moment. Two ADC devices are used in orde to distinguish between
 * fast and slow devices. The idea is that but only having few channels in 'fast' mode we can sample those faster?
 *
 * At the moment rusEfi does not allow to have more than 16 ADC channels combined. At the moment there is no flexibility to use
 * any ADC pins, only the hardcoded choice of 16 pins.
 *
 * Slow ADC group is used for IAT, CLT, AFR, VBATT etc - this one is currently sampled at 500Hz
 *
 * Fast ADC group is used for MAP, MAF HIP - this one is currently sampled at 10KHz
 *  We need frequent MAP for map_averaging.cpp
 *
 * 10KHz equals one measurement every 3.6 degrees at 6000 RPM
 *
 * @date Jan 14, 2013
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#include "global.h"

#if HAL_USE_ADC
#include "os_access.h"

#include "engine.h"
#include "adc_inputs.h"
#include "adc_subscription.h"
#include "AdcConfiguration.h"
#include "mpu_util.h"
#include "periodic_thread_controller.h"

#include "pin_repository.h"
#include "engine_math.h"
#include "engine_controller.h"
#include "maf.h"
#include "perf_trace.h"
#include "thread_priority.h"

/* Depth of the conversion buffer, channels are sampled X times each.*/
#ifndef ADC_BUF_DEPTH_FAST
#define ADC_BUF_DEPTH_FAST      4
#endif

static NO_CACHE adcsample_t slowAdcSamples[ADC_MAX_CHANNELS_COUNT];
static NO_CACHE adcsample_t fastAdcSampleBuf[ADC_BUF_DEPTH_FAST * ADC_MAX_CHANNELS_COUNT];

static adc_channel_mode_e adcHwChannelEnabled[HW_MAX_ADC_INDEX];

EXTERN_ENGINE;

// Board voltage, with divider coefficient accounted for
float getVoltageDivided(const char *msg, adc_channel_e hwChannel DECLARE_ENGINE_PARAMETER_SUFFIX) {
	return getVoltage(msg, hwChannel PASS_ENGINE_PARAMETER_SUFFIX) * engineConfiguration->analogInputDividerCoefficient;
}

// voltage in MCU universe, from zero to VDD
float getVoltage(const char *msg, adc_channel_e hwChannel DECLARE_ENGINE_PARAMETER_SUFFIX) {
	return adcToVolts(getAdcValue(msg, hwChannel));
}

#if EFI_USE_FAST_ADC
AdcDevice::AdcDevice(ADCConversionGroup* hwConfig, adcsample_t *buf, size_t buf_len) {
	this->hwConfig = hwConfig;
	this->samples = buf;
	this->buf_len = buf_len;

	hwConfig->sqr1 = 0;
	hwConfig->sqr2 = 0;
	hwConfig->sqr3 = 0;
#if ADC_MAX_CHANNELS_COUNT > 16
	hwConfig->sqr4 = 0;
	hwConfig->sqr5 = 0;
#endif /* ADC_MAX_CHANNELS_COUNT */
	memset(hardwareIndexByIndernalAdcIndex, EFI_ADC_NONE, sizeof(hardwareIndexByIndernalAdcIndex));
	memset(internalAdcIndexByHardwareIndex, 0xFFFFFFFF, sizeof(internalAdcIndexByHardwareIndex));
}

#if !defined(GPT_FREQ_FAST) || !defined(GPT_PERIOD_FAST)
/**
 * 8000 RPM is 133Hz
 * If we want to sample MAP once per 5 degrees we need 133Hz * (360 / 5) = 9576Hz of fast ADC
 */
// todo: migrate to continuous ADC mode? probably not - we cannot afford the callback in
// todo: continuous mode. todo: look into our options
#define GPT_FREQ_FAST 100000   /* PWM clock frequency. I wonder what does this setting mean?  */
#define GPT_PERIOD_FAST 10  /* PWM period (in PWM ticks).    */
#endif /* GPT_FREQ_FAST GPT_PERIOD_FAST */

#endif // EFI_USE_FAST_ADC

// is there a reason to have this configurable at runtime?
#ifndef ADC_FAST_DEVICE
#define ADC_FAST_DEVICE ADCD2
#endif /* ADC_FAST_DEVICE */

static uint32_t slowAdcCounter = 0;
static LoggingWithStorage logger("ADC");

// todo: move this flag to Engine god object
static int adcDebugReporting = false;

EXTERN_ENGINE;

static adcsample_t getAvgAdcValue(int index, adcsample_t *samples, int bufDepth, int numChannels) {
	uint32_t result = 0;
	for (int i = 0; i < bufDepth; i++) {
		result += samples[index];
		index += numChannels;
	}

	// this truncation is guaranteed to not be lossy - the average can't be larger than adcsample_t
	return static_cast<adcsample_t>(result / bufDepth);
}


// See https://github.com/rusefi/rusefi/issues/976 for discussion on these values
#define ADC_SAMPLING_SLOW ADC_SAMPLE_56
#define ADC_SAMPLING_FAST ADC_SAMPLE_28

#if EFI_USE_FAST_ADC
void adc_callback_fast(ADCDriver *adcp);

static ADCConversionGroup adcgrpcfgFast = {
	.circular			= FALSE,
	.num_channels		= 0,
	.end_cb				= adc_callback_fast,
	.error_cb			= nullptr,
	/* HW dependent part.*/
	.cr1				= 0,
	.cr2				= ADC_CR2_SWSTART,
		/**
		 * here we configure all possible channels for fast mode. Some channels would not actually
         * be used hopefully that's fine to configure all possible channels.
		 *
		 */
	// sample times for channels 10...18
	.smpr1 =
		ADC_SMPR1_SMP_AN10(ADC_SAMPLING_FAST) |
		ADC_SMPR1_SMP_AN11(ADC_SAMPLING_FAST) |
		ADC_SMPR1_SMP_AN12(ADC_SAMPLING_FAST) |
		ADC_SMPR1_SMP_AN13(ADC_SAMPLING_FAST) |
		ADC_SMPR1_SMP_AN14(ADC_SAMPLING_FAST) |
		ADC_SMPR1_SMP_AN15(ADC_SAMPLING_FAST),
	// In this field must be specified the sample times for channels 0...9
	.smpr2 =
		ADC_SMPR2_SMP_AN0(ADC_SAMPLING_FAST) |
		ADC_SMPR2_SMP_AN1(ADC_SAMPLING_FAST) |
		ADC_SMPR2_SMP_AN2(ADC_SAMPLING_FAST) |
		ADC_SMPR2_SMP_AN3(ADC_SAMPLING_FAST) |
		ADC_SMPR2_SMP_AN4(ADC_SAMPLING_FAST) |
		ADC_SMPR2_SMP_AN5(ADC_SAMPLING_FAST) |
		ADC_SMPR2_SMP_AN6(ADC_SAMPLING_FAST) |
		ADC_SMPR2_SMP_AN7(ADC_SAMPLING_FAST) |
		ADC_SMPR2_SMP_AN8(ADC_SAMPLING_FAST) |
		ADC_SMPR2_SMP_AN9(ADC_SAMPLING_FAST),
	.htr				= 0,
	.ltr				= 0,
	.sqr1				= 0, // Conversion group sequence 13...16 + sequence length
	.sqr2				= 0, // Conversion group sequence 7...12
	.sqr3				= 0, // Conversion group sequence 1...6
#if ADC_MAX_CHANNELS_COUNT > 16
	.sqr4				= 0, // Conversion group sequence 19...24
	.sqr5				= 0  // Conversion group sequence 25...30
#endif /* ADC_MAX_CHANNELS_COUNT */
};

AdcDevice fastAdc(&adcgrpcfgFast, fastAdcSampleBuf, ARRAY_SIZE(fastAdcSampleBuf));

static void fast_adc_callback(GPTDriver*) {
#if EFI_INTERNAL_ADC
	/*
	 * Starts an asynchronous ADC conversion operation, the conversion
	 * will be executed in parallel to the current PWM cycle and will
	 * terminate before the next PWM cycle.
	 */
	chSysLockFromISR()
	;
	if (ADC_FAST_DEVICE.state != ADC_READY &&
	ADC_FAST_DEVICE.state != ADC_COMPLETE &&
	ADC_FAST_DEVICE.state != ADC_ERROR) {
		fastAdc.errorsCount++;
		// todo: when? why? firmwareError(OBD_PCM_Processor_Fault, "ADC fast not ready?");
		chSysUnlockFromISR()
		;
		return;
	}

	adcStartConversionI(&ADC_FAST_DEVICE, &adcgrpcfgFast, fastAdc.samples, ADC_BUF_DEPTH_FAST);
	chSysUnlockFromISR()
	;
	fastAdc.conversionCount++;
#endif /* EFI_INTERNAL_ADC */
}
#endif // EFI_USE_FAST_ADC

static float mcuTemperature;

float getMCUInternalTemperature() {
	return mcuTemperature;
}

int getInternalAdcValue(const char *msg, adc_channel_e hwChannel) {
	if (!isAdcChannelValid(hwChannel)) {
		warning(CUSTOM_OBD_ANALOG_INPUT_NOT_CONFIGURED, "ADC: %s input is not configured", msg);
		return -1;
	}
#if EFI_ENABLE_MOCK_ADC
	if (engine->engineState.mockAdcState.hasMockAdc[hwChannel])
		return engine->engineState.mockAdcState.getMockAdcValue(hwChannel);

#endif /* EFI_ENABLE_MOCK_ADC */

#if EFI_USE_FAST_ADC
	if (adcHwChannelEnabled[hwChannel] == ADC_FAST) {
		int internalIndex = fastAdc.internalAdcIndexByHardwareIndex[hwChannel];
// todo if ADC_BUF_DEPTH_FAST EQ 1
//		return fastAdc.samples[internalIndex];
		int value = getAvgAdcValue(internalIndex, fastAdc.samples, ADC_BUF_DEPTH_FAST, fastAdc.size());
		return value;
	}
#endif // EFI_USE_FAST_ADC

	return slowAdcSamples[hwChannel - 1];
}

#if EFI_USE_FAST_ADC
static GPTConfig fast_adc_config = {
	GPT_FREQ_FAST,
	fast_adc_callback,
	0, 0
};
#endif /* EFI_USE_FAST_ADC */

adc_channel_mode_e getAdcMode(adc_channel_e hwChannel) {
#if EFI_USE_FAST_ADC
	if (fastAdc.isHwUsed(hwChannel)) {
		return ADC_FAST;
	}
#endif // EFI_USE_FAST_ADC

	return ADC_SLOW;
}

#if EFI_USE_FAST_ADC

int AdcDevice::size() const {
	return channelCount;
}

int AdcDevice::getAdcValueByHwChannel(adc_channel_e hwChannel) const {
	int internalIndex = internalAdcIndexByHardwareIndex[hwChannel];
	return values.adc_data[internalIndex];
}

int AdcDevice::getAdcValueByIndex(int internalIndex) const {
	return values.adc_data[internalIndex];
}

void AdcDevice::init(void) {
	hwConfig->num_channels = size();
	/* driver does this internally */
	//hwConfig->sqr1 += ADC_SQR1_NUM_CH(size());
}

bool AdcDevice::isHwUsed(adc_channel_e hwChannelIndex) const {
	for (size_t i = 0; i < channelCount; i++) {
		if (hardwareIndexByIndernalAdcIndex[i] == hwChannelIndex) {
			return true;
		}
	}
	return false;
}

void AdcDevice::enableChannel(adc_channel_e hwChannel) {
	if (channelCount >= efi::size(values.adc_data)) {
		firmwareError(OBD_PCM_Processor_Fault, "Too many ADC channels configured");
		return;
	}

	int logicChannel = channelCount++;

	size_t channelAdcIndex = hwChannel - 1;

	internalAdcIndexByHardwareIndex[hwChannel] = logicChannel;
	hardwareIndexByIndernalAdcIndex[logicChannel] = hwChannel;
	if (logicChannel < 6) {
		hwConfig->sqr3 |= channelAdcIndex << (5 * logicChannel);
	} else if (logicChannel < 12) {
		hwConfig->sqr2 |= channelAdcIndex << (5 * (logicChannel - 6));
	} else if (logicChannel < 18) {
		hwConfig->sqr1 |= channelAdcIndex << (5 * (logicChannel - 12));
	}
#if ADC_MAX_CHANNELS_COUNT > 16
	else if (logicChannel < 24) {
		hwConfig->sqr4 |= channelAdcIndex << (5 * (logicChannel - 18));
	}
	else if (logicChannel < 30) {
		hwConfig->sqr5 |= channelAdcIndex << (5 * (logicChannel - 24));
	}
#endif /* ADC_MAX_CHANNELS_COUNT */
}

void AdcDevice::enableChannelAndPin(const char *msg, adc_channel_e hwChannel) {
	enableChannel(hwChannel);

	brain_pin_e pin = getAdcChannelBrainPin(msg, hwChannel);
	efiSetPadMode(msg, pin, PAL_MODE_INPUT_ANALOG);
}

adc_channel_e AdcDevice::getAdcHardwareIndexByInternalIndex(int index) const {
	return hardwareIndexByIndernalAdcIndex[index];
}

#endif // EFI_USE_FAST_ADC

static void printAdcValue(int channel) {
	int value = getAdcValue("print", (adc_channel_e)channel);
	float volts = adcToVoltsDivided(value);
	scheduleMsg(&logger, "adc voltage : %.2f", volts);
}

static uint32_t slowAdcConversionCount = 0;
static uint32_t slowAdcErrorsCount = 0;

static void printFullAdcReport(Logging *logger) {
#if EFI_USE_FAST_ADC
	scheduleMsg(logger, "fast %d slow %d", fastAdc.conversionCount, slowAdcConversionCount);

	for (int index = 0; index < fastAdc.size(); index++) {
		appendMsgPrefix(logger);

		adc_channel_e hwIndex = fastAdc.getAdcHardwareIndexByInternalIndex(index);

		if (isAdcChannelValid(hwIndex)) {
			ioportid_t port = getAdcChannelPort("print", hwIndex);
			int pin = getAdcChannelPin(hwIndex);

			int adcValue = getAvgAdcValue(hwIndex, fastAdc.samples, ADC_BUF_DEPTH_FAST, fastAdc.size());
			logger->appendPrintf(" F ch%d %s%d", index, portname(port), pin);
			logger->appendPrintf(" ADC%d 12bit=%d", hwIndex, adcValue);
			float volts = adcToVolts(adcValue);
			logger->appendPrintf(" v=%.2f", volts);

			appendMsgPostfix(logger);
			scheduleLogging(logger);
		}
	}
#endif // EFI_USE_FAST_ADC

	for (int index = 0; index < ADC_MAX_CHANNELS_COUNT; index++) {
		appendMsgPrefix(logger);

		adc_channel_e hwIndex = static_cast<adc_channel_e>(index + EFI_ADC_0);

		if (isAdcChannelValid(hwIndex)) {
			ioportid_t port = getAdcChannelPort("print", hwIndex);
			int pin = getAdcChannelPin(hwIndex);

			int adcValue = slowAdcSamples[index];
			logger->appendPrintf(" S ch%d %s%d", index, portname(port), pin);
			logger->appendPrintf(" ADC%d 12bit=%d", hwIndex, adcValue);
			float volts = adcToVolts(adcValue);
			logger->appendPrintf(" v=%.2f", volts);

			appendMsgPostfix(logger);
			scheduleLogging(logger);
		}
	}
}

static void setAdcDebugReporting(int value) {
	adcDebugReporting = value;
	scheduleMsg(&logger, "adcDebug=%d", adcDebugReporting);
}

void waitForSlowAdc(int lastAdcCounter) {
	// we use slowAdcCounter instead of slowAdc.conversionCount because we need ADC_COMPLETE state
	// todo: use sync.objects?
	while (slowAdcCounter <= lastAdcCounter) {
		chThdSleepMilliseconds(1);
	}
}

int getSlowAdcCounter() {
	return slowAdcCounter;
}


class SlowAdcController : public PeriodicController<256> {
public:
	SlowAdcController() 
		: PeriodicController("ADC", PRIO_ADC, SLOW_ADC_RATE)
	{
	}

	void PeriodicTask(efitick_t nowNt) override {
		{
			ScopePerf perf(PE::AdcConversionSlow);

			slowAdcConversionCount++;
			if (!readSlowAnalogInputs(slowAdcSamples)) {
				slowAdcErrorsCount++;
				return;
			}

#ifdef USE_ADC3_VBATT_HACK
			void proteusAdcHack();
			proteusAdcHack();
#endif

			// Ask the port to sample the MCU temperature
			mcuTemperature = getMcuTemperature();
		}

		{
			ScopePerf perf(PE::AdcProcessSlow);

			slowAdcCounter++;

			AdcSubscription::UpdateSubscribers(nowNt);
		}
	}
};

void addChannel(const char *name, adc_channel_e setting, adc_channel_mode_e mode) {
	if (!isAdcChannelValid(setting)) {
		return;
	}
	if (/*type-limited (int)setting < 0 || */(int)setting>=HW_MAX_ADC_INDEX) {
		firmwareError(CUSTOM_INVALID_ADC, "Invalid ADC setting %s", name);
		return;
	}

	adcHwChannelEnabled[setting] = mode;

#if EFI_USE_FAST_ADC
	if (mode == ADC_FAST) {
		fastAdc.enableChannelAndPin(name, setting);
		return;
	}
#endif

	// Slow ADC always samples all channels, simply set the input mode
	brain_pin_e pin = getAdcChannelBrainPin(name, setting);
	efiSetPadMode(name, pin, PAL_MODE_INPUT_ANALOG);
}

void removeChannel(const char *name, adc_channel_e setting) {
	(void)name;
	if (!isAdcChannelValid(setting)) {
		return;
	}
	adcHwChannelEnabled[setting] = ADC_OFF;
}

// Weak link a stub so that every board doesn't have to implement this function
__attribute__((weak)) void setAdcChannelOverrides() { }

static void configureInputs(void) {
	memset(adcHwChannelEnabled, 0, sizeof(adcHwChannelEnabled));

	/**
	 * order of analog channels here is totally random and has no meaning
	 * we also have some weird implementation with internal indices - that all has no meaning, it's just a random implementation
	 * which does not mean anything.
	 */

	addChannel("MAP", engineConfiguration->map.sensor.hwChannel, ADC_FAST);
	addChannel("MAF", engineConfiguration->mafAdcChannel, ADC_SLOW);

	addChannel("HIP9011", engineConfiguration->hipOutputChannel, ADC_FAST);

	addChannel("Baro Press", engineConfiguration->baroSensor.hwChannel, ADC_SLOW);

	addChannel("TPS 1 Primary", engineConfiguration->tps1_1AdcChannel, ADC_SLOW);
	addChannel("TPS 1 Secondary", engineConfiguration->tps1_2AdcChannel, ADC_SLOW);
	addChannel("TPS 2 Primary", engineConfiguration->tps2_1AdcChannel, ADC_SLOW);
	addChannel("TPS 2 Secondary", engineConfiguration->tps2_2AdcChannel, ADC_SLOW);

	addChannel("Wastegate Position", engineConfiguration->wastegatePositionSensor, ADC_SLOW);
	addChannel("Idle Position Sensor", engineConfiguration->idlePositionSensor, ADC_SLOW);

	addChannel("Fuel Level", engineConfiguration->fuelLevelSensor, ADC_SLOW);
	addChannel("Acc Pedal1", engineConfiguration->throttlePedalPositionAdcChannel, ADC_SLOW);
	addChannel("Acc Pedal2", engineConfiguration->throttlePedalPositionSecondAdcChannel, ADC_SLOW);
	addChannel("VBatt", engineConfiguration->vbattAdcChannel, ADC_SLOW);
	// not currently used	addChannel("Vref", engineConfiguration->vRefAdcChannel, ADC_SLOW);
	addChannel("CLT", engineConfiguration->clt.adcChannel, ADC_SLOW);
	addChannel("IAT", engineConfiguration->iat.adcChannel, ADC_SLOW);
	addChannel("AUX Temp 1", engineConfiguration->auxTempSensor1.adcChannel, ADC_SLOW);
	addChannel("AUX Temp 2", engineConfiguration->auxTempSensor2.adcChannel, ADC_SLOW);

	addChannel("AUXF#1", engineConfiguration->auxFastSensor1_adcChannel, ADC_FAST);

	addChannel("AFR", engineConfiguration->afr.hwChannel, ADC_SLOW);
	addChannel("Oil Pressure", engineConfiguration->oilPressure.hwChannel, ADC_SLOW);

	addChannel("LFP", engineConfiguration->lowPressureFuel.hwChannel, ADC_SLOW);
	addChannel("HFP", engineConfiguration->highPressureFuel.hwChannel, ADC_SLOW);


	if (CONFIG(isCJ125Enabled)) {
		addChannel("CJ125 UR", engineConfiguration->cj125ur, ADC_SLOW);
		addChannel("CJ125 UA", engineConfiguration->cj125ua, ADC_SLOW);
	}

	for (int i = 0; i < FSIO_ANALOG_INPUT_COUNT ; i++) {
		addChannel("FSIOadc", engineConfiguration->fsioAdc[i], ADC_SLOW);
	}

	setAdcChannelOverrides();
}

static SlowAdcController slowAdcController;

void initAdcInputs() {
	scheduleMsg(&logger, "initAdcInputs()");

	configureInputs();

	// migrate to 'enable adcdebug'
	addConsoleActionI("adcdebug", &setAdcDebugReporting);

#if EFI_INTERNAL_ADC
	portInitAdc();

	// Start the slow ADC thread
	slowAdcController.Start();

#if EFI_USE_FAST_ADC
	fastAdc.init();

	gptStart(EFI_INTERNAL_FAST_ADC_GPT, &fast_adc_config);
	gptStartContinuous(EFI_INTERNAL_FAST_ADC_GPT, GPT_PERIOD_FAST);
#endif // EFI_USE_FAST_ADC

	addConsoleActionI("adc", (VoidInt) printAdcValue);
#else
	scheduleMsg(&logger, "ADC disabled");
#endif
}

void printFullAdcReportIfNeeded(Logging *logger) {
	if (!adcDebugReporting)
		return;
	printFullAdcReport(logger);
}

#else /* not HAL_USE_ADC */

__attribute__((weak)) float getVoltageDivided(const char*, adc_channel_e DECLARE_ENGINE_PARAMETER_SUFFIX) {
	return 0;
}

// voltage in MCU universe, from zero to VDD
__attribute__((weak)) float getVoltage(const char*, adc_channel_e DECLARE_ENGINE_PARAMETER_SUFFIX) {
	return 0;
}

#endif
