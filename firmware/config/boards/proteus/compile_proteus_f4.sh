#!/bin/bash

export PROJECT_BOARD=proteus
export PROJECT_CPU=ARCH_STM32F4
export EXTRA_PARAMS=-DSHORT_BOARD_NAME=proteus_f4

bash ../common_make.sh
