/*
 * trigger_misc.cpp
 *
 *  Created on: Oct 30, 2018
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#include "trigger_misc.h"
#include "trigger_universal.h"

// TT_FIAT_IAW_P8
void configureFiatIAQ_P8(TriggerWaveform * s) {
	s->initialize(FOUR_STROKE_CAM_SENSOR);

	int width = 60;
	s->tdcPosition = width;

	s->addEvent720(width, T_PRIMARY, TV_RISE);
	s->addEvent720(180, T_PRIMARY, TV_FALL);

	s->addEvent720(180 + width, T_PRIMARY, TV_RISE);
	s->addEvent720(720, T_PRIMARY, TV_FALL);
	s->setTriggerSynchronizationGap(3);
}

// TT_TRI_TACH
void configureTriTach(TriggerWaveform * s) {
	s->initialize(FOUR_STROKE_CRANK_SENSOR);

	s->isSynchronizationNeeded = false;

	float toothWidth = 0.5;

	float engineCycle = FOUR_STROKE_ENGINE_CYCLE;

	int totalTeethCount = 135;
	float offset = 0;

	float angleDown = engineCycle / totalTeethCount * (0 + (1 - toothWidth));
	float angleUp = engineCycle / totalTeethCount * (0 + 1);
	s->addEventClamped(offset + angleDown, T_PRIMARY, TV_RISE, NO_LEFT_FILTER, NO_RIGHT_FILTER);
	s->addEventClamped(offset + angleDown + 0.1, T_SECONDARY, TV_RISE, NO_LEFT_FILTER, NO_RIGHT_FILTER);
	s->addEventClamped(offset + angleUp, T_PRIMARY, TV_FALL, NO_LEFT_FILTER, NO_RIGHT_FILTER);
	s->addEventClamped(offset + angleUp + 0.1, T_SECONDARY, TV_FALL, NO_LEFT_FILTER, NO_RIGHT_FILTER);


	addSkippedToothTriggerEvents(T_SECONDARY, s, totalTeethCount, /* skipped */ 0, toothWidth, offset, engineCycle,
			1.0 * FOUR_STROKE_ENGINE_CYCLE / 135,
			NO_RIGHT_FILTER);
}

void configureFordST170(TriggerWaveform * s) {
	s->initialize(FOUR_STROKE_CAM_SENSOR);
	int width = 10;

	int total = s->getCycleDuration() / 8;

	s->addEventAngle(1 * total - width, T_PRIMARY, TV_RISE);
	s->addEventAngle(1 * total, T_PRIMARY, TV_FALL);

	s->addEventAngle(2 * total - width, T_PRIMARY, TV_RISE);
	s->addEventAngle(2 * total, T_PRIMARY, TV_FALL);

	s->addEventAngle(4 * total - width, T_PRIMARY, TV_RISE);
	s->addEventAngle(4 * total, T_PRIMARY, TV_FALL);

	s->addEventAngle(6 * total - width, T_PRIMARY, TV_RISE);
	s->addEventAngle(6 * total, T_PRIMARY, TV_FALL);

	s->addEventAngle(8 * total - width, T_PRIMARY, TV_RISE);
	s->addEventAngle(8 * total, T_PRIMARY, TV_FALL);
}

void configureDaihatsu4(TriggerWaveform * s) {
	s->initialize(FOUR_STROKE_CAM_SENSOR);

	int width = 10;

	s->setTriggerSynchronizationGap(0.125);

	s->addEventAngle(30 - width, T_PRIMARY, TV_RISE);
	s->addEventAngle(30, T_PRIMARY, TV_FALL);


	s->addEventAngle(s->getCycleDuration() / 3 - width, T_PRIMARY, TV_RISE);
	s->addEventAngle(s->getCycleDuration() / 3, T_PRIMARY, TV_FALL);

	s->addEventAngle(s->getCycleDuration() / 3 * 2 - width, T_PRIMARY, TV_RISE);
	s->addEventAngle(s->getCycleDuration() / 3 * 2, T_PRIMARY, TV_FALL);

	s->addEventAngle(s->getCycleDuration() - width, T_PRIMARY, TV_RISE);
	s->addEventAngle(s->getCycleDuration(), T_PRIMARY, TV_FALL);

}
