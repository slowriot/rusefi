/*
 * @file trigger_vw.cpp
 *
 * @date Aug 25, 2018
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#include "trigger_vw.h"
#include "trigger_universal.h"
#include "error_handling.h"

void setSkodaFavorit(TriggerWaveform *s) {
	s->initialize(FOUR_STROKE_CRANK_SENSOR);

	int m = 2;

	s->addEvent720(m * 46, T_PRIMARY, TV_RISE);
	s->addEvent720(m * 177, T_PRIMARY, TV_FALL);

	s->addEvent720(m * 180, T_PRIMARY, TV_RISE);
	s->addEvent720(m * 183, T_PRIMARY, TV_FALL);

	s->addEvent720(m * 226, T_PRIMARY, TV_RISE);
	s->addEvent720(m * 360, T_PRIMARY, TV_FALL);

	s->tdcPosition = 180 - 46;
	s->setTriggerSynchronizationGap(3.91);
}

void setVwConfiguration(TriggerWaveform *s) {
	s->initialize(FOUR_STROKE_CRANK_SENSOR);

	int totalTeethCount = 60;
	int skippedCount = 2;

	float engineCycle = FOUR_STROKE_ENGINE_CYCLE;
	float toothWidth = 0.5;

	addSkippedToothTriggerEvents(T_PRIMARY, s, 60, 2, toothWidth, 0, engineCycle,
			NO_LEFT_FILTER, 690);

	float angleDown = engineCycle / totalTeethCount * (totalTeethCount - skippedCount - 1 + (1 - toothWidth) );
	s->addEventClamped(0 + angleDown + 12, T_PRIMARY, TV_RISE, NO_LEFT_FILTER, NO_RIGHT_FILTER);
	s->addEventClamped(0 + engineCycle, T_PRIMARY, TV_FALL, NO_LEFT_FILTER, NO_RIGHT_FILTER);

	s->setTriggerSynchronizationGap2(1.6, 4);
}
