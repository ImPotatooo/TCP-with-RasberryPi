#include <stdio.h>
#include <string.h>
#include <wiringPi.h>
#include <softPwm.h>

#define LED 5 // GPIO 34 - 5번 
#define PWM_MAX 100
#define PWM_MID 50
#define PWM_LOW 20

// dlopen 후 핸들을 닫지 않으므로 static 변수가 유지됨
static int initialized = 0;

void led_function(char *arg)
{
    // 최초 한번만 초기화
    if (!initialized) {
        wiringPiSetup();
        pinMode(LED, OUTPUT);
        softPwmCreate(LED, 0, PWM_MAX);
        initialized = 1;
    }

    if (strcmp(arg, "on") == 0) 
        softPwmWrite(LED, PWM_MAX);
    else if (strcmp(arg, "off") == 0) 
        softPwmWrite(LED, 0);
    else if (strcmp(arg, "high") == 0) 
        softPwmWrite(LED, PWM_MAX);
    else if (strcmp(arg, "mid") == 0) 
        softPwmWrite(LED, PWM_MID);
    else if (strcmp(arg, "low") == 0) 
        softPwmWrite(LED, PWM_LOW);
    else 
        printf("[LED] wrong input: %s\n", arg);

    printf("[LED] %s\n", arg);
}
