################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../zed/drivers/drv_MG996.c \
../zed/drivers/drv_SG996.c \
../zed/drivers/drv_adc0.c \
../zed/drivers/drv_uart.c \
../zed/drivers/icm42688.c 

C_DEPS += \
./zed/drivers/drv_MG996.d \
./zed/drivers/drv_SG996.d \
./zed/drivers/drv_adc0.d \
./zed/drivers/drv_uart.d \
./zed/drivers/icm42688.d 

OBJS += \
./zed/drivers/drv_MG996.o \
./zed/drivers/drv_SG996.o \
./zed/drivers/drv_adc0.o \
./zed/drivers/drv_uart.o \
./zed/drivers/icm42688.o 

SREC += \
IMU.srec 

MAP += \
IMU.map 


# Each subdirectory must supply rules for building sources it contributes
zed/drivers/%.o: ../zed/drivers/%.c
	$(file > $@.in,-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -O2 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -Wunused -Wuninitialized -Wall -Wextra -Wmissing-declarations -Wconversion -Wpointer-arith -Wshadow -Wlogical-op -Waggregate-return -Wfloat-equal  -g -gdwarf-4 -D_RENESAS_RA_ -D_RA_CORE=CM33 -I"D:/SOFYWARE/renesas/IMU/src" -I"D:/SOFYWARE/renesas/IMU/ra/fsp/inc" -I"D:/SOFYWARE/renesas/IMU/ra/fsp/inc/api" -I"D:/SOFYWARE/renesas/IMU/ra/fsp/inc/instances" -I"D:/SOFYWARE/renesas/IMU/ra/arm/CMSIS_5/CMSIS/Core/Include" -I"D:/SOFYWARE/renesas/IMU/ra_gen" -I"D:/SOFYWARE/renesas/IMU/ra_cfg/fsp_cfg/bsp" -I"D:/SOFYWARE/renesas/IMU/ra_cfg/fsp_cfg" -I"D:/SOFYWARE/renesas/IMU/zed/Middlewares" -I"D:/SOFYWARE/renesas/IMU/zed/applications" -I"D:/SOFYWARE/renesas/IMU/zed/drivers" -std=c99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" -x c "$<")
	@echo Building file: $< && arm-none-eabi-gcc @"$@.in"

