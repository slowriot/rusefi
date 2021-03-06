/*
 * @file audi_avp.cpp
 *
 * AUDI_AVP
 * set engine_type 107
 *
 * Similar configurations should apply to any 40v 4.2 Audi V8, and possibly also 32v.
 *
 * @date Mar 5, 2021
 * @author Eugene Hopkinson (SlowRiot), 2021
 */

#include "audi_avp.h"
#include "engine_math.h"
#include "custom_engine.h"

EXTERN_CONFIG;

void setAudiAVP(DECLARE_CONFIG_PARAMETER_SIGNATURE) {
  engineConfiguration->specs.displacement = 4.2;
  engineConfiguration->specs.cylindersCount = 8;
  strcpy(CONFIG(engineMake), ENGINE_MAKE_VAG);
  strcpy(CONFIG(engineCode), "AVP");

  engineConfiguration->specs.firingOrder = FO_1_5_4_8_6_3_7_2;
  engineConfiguration->triggerSimulatorFrequency = 600;

  setAlgorithm(LM_ALPHA_N PASS_CONFIG_PARAMETER_SUFFIX);
  setOperationMode(engineConfiguration, FOUR_STROKE_CRANK_SENSOR);
  engineConfiguration->trigger.type = TT_TOOTHED_WHEEL_60_2;
  engineConfiguration->cranking.rpm = 100;
  engineConfiguration->injectionMode = IM_SEQUENTIAL;
  engineConfiguration->crankingInjectionMode = IM_SEQUENTIAL;

  engineConfiguration->mainRelayPin      = GPIOB_9;   // "main relay"   # pin 10/black35
  
  CONFIG(fanPin)                         = GPIOE_1;   // "Fuel Pump"    # pin 23/black35
  CONFIG(fuelPumpPin)                    = GPIOE_2;   // "radiator fan" # pin 12/black35

  // allocating Proteus injection pins from https://rusefi.com/docs/pinouts/proteus/ in descending order
  engineConfiguration->injectionPins[0]  = GPIOB_8;   // "Lowside 12"   # pin 21/black35
  engineConfiguration->injectionPins[1]  = GPIOB_6;   // "Lowside 10"   # pin 20/black35
  engineConfiguration->injectionPins[2]  = GPIOB_4;   // "Lowside 8"    # pin 19/black35
  engineConfiguration->injectionPins[3]  = GPIOG_11;  // "Lowside 4"    # pin 16/black35
  engineConfiguration->injectionPins[4]  = GPIOG_9;   // "Lowside 2"    # pin 15/black35
  engineConfiguration->injectionPins[5]  = GPIOB_7;   // "Lowside 11"   # pin 9/black35
  engineConfiguration->injectionPins[6]  = GPIOB_5;   // "Lowside 9"    # pin 8/black35
  engineConfiguration->injectionPins[7]  = GPIOG_14;  // "Lowside 7"    # pin 7/black35

  // matching Proteus default pinouts from https://rusefi.com/docs/pinouts/proteus/
  engineConfiguration->ignitionPins[0]   = GPIOD_4;   // "Ign 1"        # pin 35/black35
  engineConfiguration->ignitionPins[1]   = GPIOD_3;   // "Ign 2"        # pin 34/black35
  engineConfiguration->ignitionPins[2]   = GPIOC_9;   // "Ign 3"        # pin 22/black35
  engineConfiguration->ignitionPins[3]   = GPIOC_8;   // "Ign 4"        # pin 33/black35
  engineConfiguration->ignitionPins[4]   = GPIOC_7;   // "Ign 5"        # pin 32/black35
  engineConfiguration->ignitionPins[5]   = GPIOG_8;   // "Ign 6"        # pin 31/black35
  engineConfiguration->ignitionPins[6]   = GPIOG_7;   // "Ign 7"        # pin 30/black35
  engineConfiguration->ignitionPins[7]   = GPIOG_6;   // "Ign 8"        # pin 29/black35
}


