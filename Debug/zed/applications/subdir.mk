################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../zed/applications/app_arm_link.c \
../zed/applications/app_servo_test.c \
../zed/applications/app_uart_test.c \
../zed/applications/emg_runtime.c \
../zed/applications/imu_calibration.c \
../zed/applications/imu_math.c \
../zed/applications/imu_protocol.c \
../zed/applications/imu_runtime.c 

C_DEPS += \
./zed/applications/app_arm_link.d \
./zed/applications/app_servo_test.d \
./zed/applications/app_uart_test.d \
./zed/applications/emg_runtime.d \
./zed/applications/imu_calibration.d \
./zed/applications/imu_math.d \
./zed/applications/imu_protocol.d \
./zed/applications/imu_runtime.d 

OBJS += \
./zed/applications/app_arm_link.o \
./zed/applications/app_servo_test.o \
./zed/applications/app_uart_test.o \
./zed/applications/emg_runtime.o \
./zed/applications/imu_calibration.o \
./zed/applications/imu_math.o \
./zed/applications/imu_protocol.o \
./zed/applications/imu_runtime.o 

SREC += \
IMU_V9.srec 

MAP += \
IMU_V9.map 


# Each subdirectory must supply rules for building sources it contributes
zed/applications/%.o: ../zed/applications/%.c
	$(file > $@.in,-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -O2 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -Wunused -Wuninitialized -Wall -Wextra -Wmissing-declarations -Wconversion -Wpointer-arith -Wshadow -Wlogical-op -Waggregate-return -Wfloat-equal  -g -gdwarf-4 -D_RENESAS_RA_ -D_RA_CORE=CM33 -I"C:/Users/user/e2_studio/workspace/IMU_V9/src" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra/fsp/inc" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra/fsp/inc/api" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra/fsp/inc/instances" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra/arm/CMSIS_5/CMSIS/Core/Include" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra_gen" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra_cfg/fsp_cfg/bsp" -I"C:/Users/user/e2_studio/workspace/IMU_V9/ra_cfg/fsp_cfg" -I"C:/Users/user/e2_studio/workspace/IMU_V9/zed/applications" -I"C:/Users/user/e2_studio/workspace/IMU_V9/zed/drivers" -std=c99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" -x c "$<")
	@echo Building file: $< && arm-none-eabi-gcc @"$@.in"

