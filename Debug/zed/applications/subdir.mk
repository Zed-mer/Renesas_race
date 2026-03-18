################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../zed/applications/app_adc_test.c \
../zed/applications/app_servo_test.c \
../zed/applications/app_uart_test.c \
../zed/applications/imu_calibration.c \
../zed/applications/imu_math.c \
../zed/applications/imu_protocol.c \
../zed/applications/imu_runtime.c 

C_DEPS += \
./zed/applications/app_adc_test.d \
./zed/applications/app_servo_test.d \
./zed/applications/app_uart_test.d \
./zed/applications/imu_calibration.d \
./zed/applications/imu_math.d \
./zed/applications/imu_protocol.d \
./zed/applications/imu_runtime.d 

OBJS += \
./zed/applications/app_adc_test.o \
./zed/applications/app_servo_test.o \
./zed/applications/app_uart_test.o \
./zed/applications/imu_calibration.o \
./zed/applications/imu_math.o \
./zed/applications/imu_protocol.o \
./zed/applications/imu_runtime.o 

SREC += \
IMU.srec 

MAP += \
IMU.map 


# Each subdirectory must supply rules for building sources it contributes
zed/applications/%.o: ../zed/applications/%.c
	$(file > $@.in,-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -O2 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -Wunused -Wuninitialized -Wall -Wextra -Wmissing-declarations -Wconversion -Wpointer-arith -Wshadow -Wlogical-op -Waggregate-return -Wfloat-equal  -g -gdwarf-4 -D_RENESAS_RA_ -D_RA_CORE=CM33 -I"D:/SOFYWARE/renesas/IMU/src" -I"D:/SOFYWARE/renesas/IMU/ra/fsp/inc" -I"D:/SOFYWARE/renesas/IMU/ra/fsp/inc/api" -I"D:/SOFYWARE/renesas/IMU/ra/fsp/inc/instances" -I"D:/SOFYWARE/renesas/IMU/ra/arm/CMSIS_5/CMSIS/Core/Include" -I"D:/SOFYWARE/renesas/IMU/ra_gen" -I"D:/SOFYWARE/renesas/IMU/ra_cfg/fsp_cfg/bsp" -I"D:/SOFYWARE/renesas/IMU/ra_cfg/fsp_cfg" -I"D:/SOFYWARE/renesas/IMU/zed/applications" -I"D:/SOFYWARE/renesas/IMU/zed/drivers" -std=c99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" -x c "$<")
	@echo Building file: $< && arm-none-eabi-gcc @"$@.in"

