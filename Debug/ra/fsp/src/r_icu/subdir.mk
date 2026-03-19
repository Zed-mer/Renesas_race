################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../ra/fsp/src/r_icu/r_icu.c 

C_DEPS += \
./ra/fsp/src/r_icu/r_icu.d 

OBJS += \
./ra/fsp/src/r_icu/r_icu.o 

SREC += \
IMU_V7.srec 

MAP += \
IMU_V7.map 


# Each subdirectory must supply rules for building sources it contributes
ra/fsp/src/r_icu/%.o: ../ra/fsp/src/r_icu/%.c
	$(file > $@.in,-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -O2 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -Wunused -Wuninitialized -Wall -Wextra -Wmissing-declarations -Wconversion -Wpointer-arith -Wshadow -Wlogical-op -Waggregate-return -Wfloat-equal  -g -gdwarf-4 -D_RENESAS_RA_ -D_RA_CORE=CM33 -I"D:/Renesas/e2 studio/workplace/IMU_V7/src" -I"D:/Renesas/e2 studio/workplace/IMU_V7/ra/fsp/inc" -I"D:/Renesas/e2 studio/workplace/IMU_V7/ra/fsp/inc/api" -I"D:/Renesas/e2 studio/workplace/IMU_V7/ra/fsp/inc/instances" -I"D:/Renesas/e2 studio/workplace/IMU_V7/ra/arm/CMSIS_5/CMSIS/Core/Include" -I"D:/Renesas/e2 studio/workplace/IMU_V7/ra_gen" -I"D:/Renesas/e2 studio/workplace/IMU_V7/ra_cfg/fsp_cfg/bsp" -I"D:/Renesas/e2 studio/workplace/IMU_V7/ra_cfg/fsp_cfg" -I"D:/Renesas/e2 studio/workplace/IMU_V7/zed/applications" -I"D:/Renesas/e2 studio/workplace/IMU_V7/zed/drivers" -std=c99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" -x c "$<")
	@echo Building file: $< && arm-none-eabi-gcc @"$@.in"

