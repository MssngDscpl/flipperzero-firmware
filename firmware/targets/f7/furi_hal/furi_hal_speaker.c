#include <furi_hal_speaker.h>
#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>

#include <stm32wbxx_ll_tim.h>

#define FURI_HAL_SPEAKER_TIMER TIM16
#define FURI_HAL_SPEAKER_CHANNEL LL_TIM_CHANNEL_CH1
#define FURI_HAL_SPEAKER_PRESCALER 500
#define FURI_HAL_SPEAKER_MAX_VOLUME 60

// #define FURI_HAL_SPEAKER_NEW_VOLUME

void furi_hal_speaker_init() {
    FURI_CRITICAL_ENTER();
    LL_TIM_DeInit(FURI_HAL_SPEAKER_TIMER);
    FURI_CRITICAL_EXIT();

    furi_hal_gpio_init_ex(
        &gpio_speaker, GpioModeAltFunctionPushPull, GpioPullNo, GpioSpeedLow, GpioAltFn14TIM16);
}

void furi_hal_speaker_start(float frequency, float volume) {
    if(volume < 0) volume = 0;
    if(volume > 1) volume = 1;
    volume = volume * volume * volume;

    uint32_t autoreload = (SystemCoreClock / FURI_HAL_SPEAKER_PRESCALER / frequency) - 1;
    if(autoreload < 2) {
        autoreload = 2;
    } else if(autoreload > UINT16_MAX) {
        autoreload = UINT16_MAX;
    }

    LL_TIM_InitTypeDef TIM_InitStruct = {0};
    TIM_InitStruct.Prescaler = FURI_HAL_SPEAKER_PRESCALER - 1;
    TIM_InitStruct.Autoreload = autoreload;
    LL_TIM_Init(FURI_HAL_SPEAKER_TIMER, &TIM_InitStruct);

#ifdef FURI_HAL_SPEAKER_NEW_VOLUME
    uint32_t compare_value = volume * FURI_HAL_SPEAKER_MAX_VOLUME;
    uint32_t clip_value = volume * TIM_InitStruct.Autoreload / 2;
    if(compare_value > clip_value) {
        compare_value = clip_value;
    }
#else
    uint32_t compare_value = volume * autoreload / 2;
#endif

    if(compare_value == 0) {
        compare_value = 1;
    }

    LL_TIM_OC_InitTypeDef TIM_OC_InitStruct = {0};
    TIM_OC_InitStruct.OCMode = LL_TIM_OCMODE_PWM1;
    TIM_OC_InitStruct.OCState = LL_TIM_OCSTATE_ENABLE;
    TIM_OC_InitStruct.CompareValue = compare_value;
    LL_TIM_OC_Init(FURI_HAL_SPEAKER_TIMER, FURI_HAL_SPEAKER_CHANNEL, &TIM_OC_InitStruct);

    LL_TIM_EnableAllOutputs(FURI_HAL_SPEAKER_TIMER);
    LL_TIM_EnableCounter(FURI_HAL_SPEAKER_TIMER);
}

void furi_hal_speaker_stop() {
    LL_TIM_DisableAllOutputs(FURI_HAL_SPEAKER_TIMER);
    LL_TIM_DisableCounter(FURI_HAL_SPEAKER_TIMER);
}
