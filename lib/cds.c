#include <stdio.h>
#include <wiringPi.h>
#include <wiringPiI2C.h>
#include <softPwm.h>

#define LED 5
#define ADC_ADDR 0x48
#define ADC_CHANNEL 0x00
#define PWM_MAX 100

static int current_val  = 0;
static int initialized  = 0;

void cds_function(int *threshold)
{
    int fd;
    int cnt = 0;
    int prev, a2dVal;

    if (!initialized) {
        wiringPiSetup();
        softPwmCreate(LED, 0, PWM_MAX);
        initialized = 1;
    }

    if ((fd = wiringPiI2CSetupInterface("/dev/i2c-1", ADC_ADDR)) < 0) {
        printf("[CDS] wiringPiI2C SetupInterface Failed\n");
        return;
    }

    printf("[CDS] monitoring start, threshold=%d\n", *threshold);

    while (1) {
        wiringPiI2CWrite(fd, ADC_CHANNEL);
        prev = wiringPiI2CRead(fd);
        a2dVal = wiringPiI2CRead(fd);

        current_val = a2dVal;

        if (a2dVal < *threshold) {
            softPwmWrite(LED, 0);
            printf("[%d] a2dVal=%d Bright -> LED OFF\n", cnt, a2dVal);
        } else {
            softPwmWrite(LED, PWM_MAX);
            printf("[%d] a2dVal=%d Dark  -> LED ON\n", cnt, a2dVal);
        }

        delay(1000);
        cnt++;
    }
}

// 서버에서 GET cds 시 현재 ADC 값 반환
int cds_get_value(void)
{
    return current_val;
}
