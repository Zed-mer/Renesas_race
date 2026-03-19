################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../ra/fsp/src/r_gpt/r_gpt.c 

C_DEPS += \
./ra/fsp/src/r_gpt/r_gpt.d 

OBJS += \
./ra/fsp/src/r_gpt/r_gpt.o 

SREC += \
IMU_V9.srec 

MAP += \
IMU_V9.map 


# Each subdirectory must supply rules for building sources it contributes
ra/fsp/src/r_gpt/%.o: ../ra/fsp/src/r_gpt/%.c
	$(file > $@.in,-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -O2 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -Wunused -Wuninitialized -Wall -Wextra -Wmissing-declarations -Wconversion -Wpointer-arith -Wshadow -Wlogical-op -Waggregate-return -Wfloat-equal  -g -gdwarf-4 -D_RENESAS_RA_ -D_RA_CORE=CM33 -I"C:/Users/user/e2_studio/workspace/IMU_V9/src" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra/fsp/inc" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra/fsp/inc/api" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra/fsp/inc/instances" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra/arm/CMSIS_5/CMSIS/Core/Include" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra_gen" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra_cfg/fsp_cfg/bsp" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra_cfg/fsp_cfg" -I"C:/Users/user/e2_studio/workspace/IMU_V9/zed/applications" -I"C:/Users/user/e2_studio/workspace/IMU_V9/zed/drivers" -std=c99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" -x c "$<")
	@echo Building file: $< && arm-none-eabi-gcc @"$@.in"

