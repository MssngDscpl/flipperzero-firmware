#include <furi_hal_gpio.h>
#include <stddef.h>
#include <assert.h>

#define GET_SYSCFG_EXTI_PORT(gpio)                \
    (((gpio) == (GPIOA)) ? LL_SYSCFG_EXTI_PORTA : \
     ((gpio) == (GPIOB)) ? LL_SYSCFG_EXTI_PORTB : \
     ((gpio) == (GPIOC)) ? LL_SYSCFG_EXTI_PORTC : \
     ((gpio) == (GPIOD)) ? LL_SYSCFG_EXTI_PORTD : \
     ((gpio) == (GPIOE)) ? LL_SYSCFG_EXTI_PORTE : \
                           LL_SYSCFG_EXTI_PORTH)

#define GPIO_PIN_MAP(pin, prefix)               \
    (((pin) == (LL_GPIO_PIN_0))  ? prefix##0 :  \
     ((pin) == (LL_GPIO_PIN_1))  ? prefix##1 :  \
     ((pin) == (LL_GPIO_PIN_2))  ? prefix##2 :  \
     ((pin) == (LL_GPIO_PIN_3))  ? prefix##3 :  \
     ((pin) == (LL_GPIO_PIN_4))  ? prefix##4 :  \
     ((pin) == (LL_GPIO_PIN_5))  ? prefix##5 :  \
     ((pin) == (LL_GPIO_PIN_6))  ? prefix##6 :  \
     ((pin) == (LL_GPIO_PIN_7))  ? prefix##7 :  \
     ((pin) == (LL_GPIO_PIN_8))  ? prefix##8 :  \
     ((pin) == (LL_GPIO_PIN_9))  ? prefix##9 :  \
     ((pin) == (LL_GPIO_PIN_10)) ? prefix##10 : \
     ((pin) == (LL_GPIO_PIN_11)) ? prefix##11 : \
     ((pin) == (LL_GPIO_PIN_12)) ? prefix##12 : \
     ((pin) == (LL_GPIO_PIN_13)) ? prefix##13 : \
     ((pin) == (LL_GPIO_PIN_14)) ? prefix##14 : \
                                   prefix##15)

#define GET_SYSCFG_EXTI_LINE(pin) GPIO_PIN_MAP(pin, LL_SYSCFG_EXTI_LINE)
#define GET_EXTI_LINE(pin) GPIO_PIN_MAP(pin, LL_EXTI_LINE_)

static volatile GpioInterrupt gpio_interrupt[GPIO_NUMBER];

static uint8_t furi_hal_gpio_get_pin_num(const GpioPin* gpio) {
    uint8_t pin_num = 0;
    for(pin_num = 0; pin_num < GPIO_NUMBER; pin_num++) {
        if(gpio->pin & (1 << pin_num)) break;
    }
    return pin_num;
}

void furi_hal_gpio_init_simple(const GpioPin* gpio, const GpioMode mode) {
    furi_hal_gpio_init(gpio, mode, GpioPullNo, GpioSpeedLow);
}

void furi_hal_gpio_init(
    const GpioPin* gpio,
    const GpioMode mode,
    const GpioPull pull,
    const GpioSpeed speed) {
    // we cannot set alternate mode in this function
    assert(mode != GpioModeAltFunctionPushPull);
    assert(mode != GpioModeAltFunctionOpenDrain);

    furi_hal_gpio_init_ex(gpio, mode, pull, speed, GpioAltFnUnused);
}

void furi_hal_gpio_init_ex(
    const GpioPin* gpio,
    const GpioMode mode,
    const GpioPull pull,
    const GpioSpeed speed,
    const GpioAltFn alt_fn) {
    uint32_t sys_exti_port = GET_SYSCFG_EXTI_PORT(gpio->port);
    uint32_t sys_exti_line = GET_SYSCFG_EXTI_LINE(gpio->pin);
    uint32_t exti_line = GET_EXTI_LINE(gpio->pin);

    // Configure gpio with interrupts disabled
    __disable_irq();

    // Set gpio speed
    switch(speed) {
    case GpioSpeedLow:
        LL_GPIO_SetPinSpeed(gpio->port, gpio->pin, LL_GPIO_SPEED_FREQ_LOW);
        break;
    case GpioSpeedMedium:
        LL_GPIO_SetPinSpeed(gpio->port, gpio->pin, LL_GPIO_SPEED_FREQ_MEDIUM);
        break;
    case GpioSpeedHigh:
        LL_GPIO_SetPinSpeed(gpio->port, gpio->pin, LL_GPIO_SPEED_FREQ_HIGH);
        break;
    case GpioSpeedVeryHigh:
        LL_GPIO_SetPinSpeed(gpio->port, gpio->pin, LL_GPIO_SPEED_FREQ_VERY_HIGH);
        break;
    }

    // Set gpio pull mode
    switch(pull) {
    case GpioPullNo:
        LL_GPIO_SetPinPull(gpio->port, gpio->pin, LL_GPIO_PULL_NO);
        break;
    case GpioPullUp:
        LL_GPIO_SetPinPull(gpio->port, gpio->pin, LL_GPIO_PULL_UP);
        break;
    case GpioPullDown:
        LL_GPIO_SetPinPull(gpio->port, gpio->pin, LL_GPIO_PULL_DOWN);
        break;
    }

    // Set gpio mode
    if(mode >= GpioModeInterruptRise) {
        // Set pin in interrupt mode
        LL_GPIO_SetPinMode(gpio->port, gpio->pin, LL_GPIO_MODE_INPUT);
        LL_SYSCFG_SetEXTISource(sys_exti_port, sys_exti_line);
        if(mode == GpioModeInterruptRise || mode == GpioModeInterruptRiseFall) {
            LL_EXTI_EnableIT_0_31(exti_line);
            LL_EXTI_EnableRisingTrig_0_31(exti_line);
        }
        if(mode == GpioModeInterruptFall || mode == GpioModeInterruptRiseFall) {
            LL_EXTI_EnableIT_0_31(exti_line);
            LL_EXTI_EnableFallingTrig_0_31(exti_line);
        }
        if(mode == GpioModeEventRise || mode == GpioModeEventRiseFall) {
            LL_EXTI_EnableEvent_0_31(exti_line);
            LL_EXTI_EnableRisingTrig_0_31(exti_line);
        }
        if(mode == GpioModeEventFall || mode == GpioModeEventRiseFall) {
            LL_EXTI_EnableEvent_0_31(exti_line);
            LL_EXTI_EnableFallingTrig_0_31(exti_line);
        }
    } else {
        // Disable interrupts if set
        if(LL_SYSCFG_GetEXTISource(sys_exti_line) == sys_exti_port &&
           LL_EXTI_IsEnabledIT_0_31(exti_line)) {
            LL_EXTI_DisableIT_0_31(exti_line);
            LL_EXTI_DisableRisingTrig_0_31(exti_line);
            LL_EXTI_DisableFallingTrig_0_31(exti_line);
        }

        // Prepare alternative part if any
        if(mode == GpioModeAltFunctionPushPull || mode == GpioModeAltFunctionOpenDrain) {
            // set alternate function
            if(furi_hal_gpio_get_pin_num(gpio) < 8) {
                LL_GPIO_SetAFPin_0_7(gpio->port, gpio->pin, alt_fn);
            } else {
                LL_GPIO_SetAFPin_8_15(gpio->port, gpio->pin, alt_fn);
            }
        }

        // Set not interrupt pin modes
        switch(mode) {
        case GpioModeInput:
            LL_GPIO_SetPinMode(gpio->port, gpio->pin, LL_GPIO_MODE_INPUT);
            break;
        case GpioModeOutputPushPull:
            LL_GPIO_SetPinOutputType(gpio->port, gpio->pin, LL_GPIO_OUTPUT_PUSHPULL);
            LL_GPIO_SetPinMode(gpio->port, gpio->pin, LL_GPIO_MODE_OUTPUT);
            break;
        case GpioModeAltFunctionPushPull:
            LL_GPIO_SetPinOutputType(gpio->port, gpio->pin, LL_GPIO_OUTPUT_PUSHPULL);
            LL_GPIO_SetPinMode(gpio->port, gpio->pin, LL_GPIO_MODE_ALTERNATE);
            break;
        case GpioModeOutputOpenDrain:
            LL_GPIO_SetPinOutputType(gpio->port, gpio->pin, LL_GPIO_OUTPUT_OPENDRAIN);
            LL_GPIO_SetPinMode(gpio->port, gpio->pin, LL_GPIO_MODE_OUTPUT);
            break;
        case GpioModeAltFunctionOpenDrain:
            LL_GPIO_SetPinOutputType(gpio->port, gpio->pin, LL_GPIO_OUTPUT_OPENDRAIN);
            LL_GPIO_SetPinMode(gpio->port, gpio->pin, LL_GPIO_MODE_ALTERNATE);
            break;
        case GpioModeAnalog:
            LL_GPIO_SetPinMode(gpio->port, gpio->pin, LL_GPIO_MODE_ANALOG);
            break;
        default:
            break;
        }
    }
    __enable_irq();
}

void furi_hal_gpio_add_int_callback(const GpioPin* gpio, GpioExtiCallback cb, void* ctx) {
    assert(gpio);
    assert(cb);

    __disable_irq();
    uint8_t pin_num = furi_hal_gpio_get_pin_num(gpio);
    gpio_interrupt[pin_num].callback = cb;
    gpio_interrupt[pin_num].context = ctx;
    gpio_interrupt[pin_num].ready = true;
    __enable_irq();
}

void furi_hal_gpio_enable_int_callback(const GpioPin* gpio) {
    assert(gpio);

    __disable_irq();
    uint8_t pin_num = furi_hal_gpio_get_pin_num(gpio);
    if(gpio_interrupt[pin_num].callback) {
        gpio_interrupt[pin_num].ready = true;
    }
    __enable_irq();
}

void furi_hal_gpio_disable_int_callback(const GpioPin* gpio) {
    assert(gpio);

    __disable_irq();
    uint8_t pin_num = furi_hal_gpio_get_pin_num(gpio);
    gpio_interrupt[pin_num].ready = false;
    __enable_irq();
}

void furi_hal_gpio_remove_int_callback(const GpioPin* gpio) {
    assert(gpio);

    __disable_irq();
    uint8_t pin_num = furi_hal_gpio_get_pin_num(gpio);
    gpio_interrupt[pin_num].callback = NULL;
    gpio_interrupt[pin_num].context = NULL;
    gpio_interrupt[pin_num].ready = false;
    __enable_irq();
}
