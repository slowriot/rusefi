/*
 * @file	trigger_central.h
 *
 * @date Feb 23, 2014
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#pragma once

#include "rusefi_enums.h"
#include "listener_array.h"
#include "trigger_decoder.h"
#include "trigger_central_generated.h"
#include "timer.h"
#include "pin_repository.h"

class Engine;
typedef void (*ShaftPositionListener)(trigger_event_e signal, uint32_t index, efitick_t edgeTimestamp DECLARE_ENGINE_PARAMETER_SUFFIX);

#define HAVE_CAM_INPUT() (isBrainPinValid(engineConfiguration->camInputs[0]))

class TriggerNoiseFilter {
public:
	void resetAccumSignalData();
	bool noiseFilter(efitick_t nowNt,
			TriggerState * triggerState,
			trigger_event_e signal DECLARE_ENGINE_PARAMETER_SUFFIX);

	efitick_t lastSignalTimes[HW_EVENT_TYPES];
	efitick_t accumSignalPeriods[HW_EVENT_TYPES];
	efitick_t accumSignalPrevPeriods[HW_EVENT_TYPES];
};

/**
 * Maybe merge TriggerCentral and TriggerState classes into one class?
 * Probably not: we have an instance of TriggerState which is used for trigger initialization,
 * also composition probably better than inheritance here
 */
class TriggerCentral final : public trigger_central_s {
public:
	TriggerCentral();
	void init(DECLARE_ENGINE_PARAMETER_SIGNATURE);
	void handleShaftSignal(trigger_event_e signal, efitick_t timestamp DECLARE_ENGINE_PARAMETER_SUFFIX);
	int getHwEventCounter(int index) const;
	void resetCounters();
	void validateCamVvtCounters();

	float getTimeSinceTriggerEvent(efitick_t nowNt) const {
		return m_lastEventTimer.getElapsedSeconds(nowNt);
	}

	bool engineMovedRecently() const {
		// Trigger event some time in the past second = engine moving
		return getTimeSinceTriggerEvent(getTimeNowNt()) < 1.0f;
	}

	TriggerNoiseFilter noiseFilter;

	trigger_type_e vvtTriggerType[CAMS_PER_BANK];
	angle_t getVVTPosition();

#if EFI_UNIT_TEST
	// latest VVT event position (could be not synchronization event)
	angle_t currentVVTEventPosition[BANKS_COUNT][CAMS_PER_BANK];
#endif // EFI_UNIT_TEST

	// synchronization event position
	angle_t vvtPosition[BANKS_COUNT][CAMS_PER_BANK];

	Timer virtualZeroTimer;

	efitick_t vvtSyncTimeNt[BANKS_COUNT][CAMS_PER_BANK];

	TriggerStateWithRunningStatistics triggerState;
	TriggerWaveform triggerShape;

	TriggerState vvtState[BANKS_COUNT][CAMS_PER_BANK];
	TriggerWaveform vvtShape[CAMS_PER_BANK];

	TriggerFormDetails triggerFormDetails;

	// Keep track of the last time we got a valid trigger event
	Timer m_lastEventTimer;
};

void triggerInfo(void);
void hwHandleShaftSignal(trigger_event_e signal, efitick_t timestamp);
void hwHandleVvtCamSignal(trigger_value_e front, efitick_t timestamp, int index DECLARE_ENGINE_PARAMETER_SUFFIX);

void initTriggerCentral(Logging *sharedLogger);

int isSignalDecoderError(void);

void onConfigurationChangeTriggerCallback(DECLARE_ENGINE_PARAMETER_SIGNATURE);
bool checkIfTriggerConfigChanged(DECLARE_ENGINE_PARAMETER_SIGNATURE);
bool isTriggerConfigChanged(DECLARE_ENGINE_PARAMETER_SIGNATURE);

bool isTriggerDecoderError(DECLARE_ENGINE_PARAMETER_SIGNATURE);

#define SYMMETRICAL_CRANK_SENSOR_DIVIDER 4
