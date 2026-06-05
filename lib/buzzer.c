#include <stdio.h>
#include <string.h>
#include <wiringPi.h>
#include <softTone.h>

#define SPKR  6     /* wiringPi 핀 6 = GPIO 25 */
#define TOTAL 14

static int notes[] = {
    659, 587, 370, 415,
    554, 494, 294, 330,
    494, 440, 277, 330,
    440
};



static int duration[] = {
    80,  80, 160, 160,
    80,  80, 160, 160,
    80,  80, 160, 160,
    250
};

static int initialized = 0;
static volatile int stop_flag = 0;  /* 1이면 재생 루프 중단 */

static void music_play(void)
{
    int i;
    stop_flag = 0;
    for (i = 0; i < TOTAL; i++) {
        if (stop_flag) break;
        softToneWrite(SPKR, notes[i]);
        delay(duration[i]);     /* 음 지속 시간 */
        softToneWrite(SPKR, 0); /* 음 사이 구분 */
        delay(60);
    }
    softToneWrite(SPKR, 0);
}

void buzzer_function(char *arg)
{
    if (!initialized) {
        wiringPiSetup();
        softToneCreate(SPKR);
        initialized = 1;
    }

    if (strcmp(arg, "on") == 0) {
        printf("[BUZZER] music play\n");
        music_play();
    } else if (strcmp(arg, "off") == 0) {
        stop_flag = 1;                  /* 재생 루프 중단 플래그 */
        softToneWrite(SPKR, 0);
        printf("[BUZZER] off\n");
    } else {
        printf("[BUZZER] wrong input: %s\n", arg);
    }
}
