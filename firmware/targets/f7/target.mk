TOOLCHAIN = arm

BOOT_ADDRESS	= 0x08000000
FW_ADDRESS		= 0x08008000
OS_OFFSET		= 0x00008000
FLASH_ADDRESS	= 0x08008000

NO_BOOTLOADER ?= 0
ifeq ($(NO_BOOTLOADER), 1)
BOOT_ADDRESS	= 0x08000000
FW_ADDRESS		= 0x08000000
OS_OFFSET		= 0x00000000
FLASH_ADDRESS	= 0x08000000
CFLAGS			+= -DNO_BOOTLOADER
endif

DEBUG_RTOS_THREADS ?= 1
ifeq ($(DEBUG_RTOS_THREADS), 1)
OPENOCD_OPTS	= -f interface/stlink.cfg -c "transport select hla_swd" -f ../debug/stm32wbx.cfg -c "stm32wbx.cpu configure -rtos auto" -c "init"
else
OPENOCD_OPTS	= -f interface/stlink.cfg -c "transport select hla_swd" -f ../debug/stm32wbx.cfg -c "init"
endif

BOOT_CFLAGS		= -DBOOT_ADDRESS=$(BOOT_ADDRESS) -DFW_ADDRESS=$(FW_ADDRESS) -DOS_OFFSET=$(OS_OFFSET)
MCU_FLAGS		= -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CFLAGS			+= $(MCU_FLAGS) $(BOOT_CFLAGS) -DSTM32WB55xx -Wall -fdata-sections -ffunction-sections
LDFLAGS			+= $(MCU_FLAGS) -specs=nosys.specs -specs=nano.specs -u _printf_float

CPPFLAGS		+= -fno-rtti -fno-use-cxa-atexit -fno-exceptions
LDFLAGS			+= -Wl,--start-group -lstdc++ -lsupc++ -Wl,--end-group

HARDWARE_TARGET = 7

MXPROJECT_DIR = $(TARGET_DIR)

# Entry Point
ASM_SOURCES += $(MXPROJECT_DIR)/startup_stm32wb55xx_cm4.s

# STM32WB HAL
CUBE_DIR = ../lib/STM32CubeWB
CFLAGS += \
	-DUSE_FULL_LL_DRIVER \
	-DHAVE_FREERTOS
CFLAGS += \
	-I$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Inc \
	-I$(CUBE_DIR)/Drivers/CMSIS/Device/ST \
	-I$(CUBE_DIR)/Drivers/CMSIS/Device/ST/STM32WBxx/Include \
	-I$(CUBE_DIR)/Drivers/CMSIS/Include
C_SOURCES += \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_adc.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_comp.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_dma.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_gpio.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_i2c.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_lptim.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_rcc.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_rtc.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_spi.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_tim.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_usart.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_lpuart.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_utils.c \
	$(CUBE_DIR)/Drivers/STM32WBxx_HAL_Driver/Src/stm32wbxx_ll_rng.c

# FreeRTOS
CFLAGS += \
	-I$(LIB_DIR)/FreeRTOS-Kernel/include \
	-I$(LIB_DIR)/FreeRTOS-Kernel/portable/GCC/ARM_CM4F \
	-I$(LIB_DIR)/FreeRTOS-glue/

C_SOURCES += \
	$(LIB_DIR)/FreeRTOS-Kernel/event_groups.c \
	$(LIB_DIR)/FreeRTOS-Kernel/list.c \
	$(LIB_DIR)/FreeRTOS-Kernel/queue.c \
	$(LIB_DIR)/FreeRTOS-Kernel/stream_buffer.c \
	$(LIB_DIR)/FreeRTOS-Kernel/tasks.c \
	$(LIB_DIR)/FreeRTOS-Kernel/timers.c \
	$(LIB_DIR)/FreeRTOS-Kernel/portable/GCC/ARM_CM4F/port.c \
	$(LIB_DIR)/FreeRTOS-glue/cmsis_os2.c \

# BLE glue 
CFLAGS += \
	-I$(TARGET_DIR)/ble_glue \
	-I$(CUBE_DIR)/Middlewares/ST/STM32_WPAN \
	-I$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble \
	-I$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble/core \
	-I$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble/core/template \
	-I$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/utilities \
	-I$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/interface/patterns/ble_thread \
	-I$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/interface/patterns/ble_thread/tl \
	-I$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/interface/patterns/ble_thread/shci
C_SOURCES += \
	$(wildcard $(TARGET_DIR)/ble_glue/*.c) \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/utilities/otp.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/utilities/stm_list.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/utilities/dbg_trace.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble/svc/Src/svc_ctl.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble/core/template/osal.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble/core/auto/ble_hci_le.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble/core/auto/ble_gap_aci.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble/core/auto/ble_gatt_aci.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble/core/auto/ble_hal_aci.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/ble/core/auto/ble_l2cap_aci.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/interface/patterns/ble_thread/tl/tl_mbox.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/interface/patterns/ble_thread/tl/hci_tl.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/interface/patterns/ble_thread/tl/hci_tl_if.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/interface/patterns/ble_thread/tl/shci_tl.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/interface/patterns/ble_thread/tl/shci_tl_if.c \
	$(CUBE_DIR)/Middlewares/ST/STM32_WPAN/interface/patterns/ble_thread/shci/shci.c

# USB stack
CFLAGS += \
	-DSTM32WB \
	-DUSB_PMASIZE=0x400

# Furi HAL
FURI_HAL_OS_DEBUG ?= 0
ifeq ($(FURI_HAL_OS_DEBUG), 1)
CFLAGS += -DFURI_HAL_OS_DEBUG
endif

FURI_HAL_USB_VCP_DEBUG ?= 0
ifeq ($(FURI_HAL_USB_VCP_DEBUG), 1)
CFLAGS += -DFURI_HAL_USB_VCP_DEBUG
endif

FURI_HAL_SUBGHZ_TX_GPIO ?= 0
ifneq ($(FURI_HAL_SUBGHZ_TX_GPIO), 0)
CFLAGS += -DFURI_HAL_SUBGHZ_TX_GPIO=$(FURI_HAL_SUBGHZ_TX_GPIO)
endif

ifeq ($(INVERT_RFID_IN), 1)
CFLAGS += -DINVERT_RFID_IN
endif

FURI_HAL_DIR = $(TARGET_DIR)/furi_hal
CFLAGS += -I$(FURI_HAL_DIR)
C_SOURCES += $(wildcard $(FURI_HAL_DIR)/*.c)

# Other
CFLAGS += \
	-I$(MXPROJECT_DIR)/Inc \
	-I$(MXPROJECT_DIR)/fatfs
C_SOURCES += \
	$(wildcard $(MXPROJECT_DIR)/Src/*.c) \
	$(wildcard $(MXPROJECT_DIR)/fatfs/*.c)

# Linker options
ifeq ($(NO_BOOTLOADER), 1)
LDFLAGS += -T$(MXPROJECT_DIR)/stm32wb55xx_flash_cm4_no_bootloader.ld
else
LDFLAGS += -T$(MXPROJECT_DIR)/stm32wb55xx_flash_cm4_with_bootloader.ld
endif

SVD_FILE = ../debug/STM32WB55_CM4.svd
