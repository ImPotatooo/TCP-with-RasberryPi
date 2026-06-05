#include <stdio.h>
#include <wiringPi.h>

/* A, B, C, D 핀 (wiringPi 번호) */
static int gpiopins[4] = {4, 1, 16, 15};

/* BCD 인코딩 0~9 */
static int number[10][4] = {
    {0,0,0,0}, /* 0 */
    {0,0,0,1}, /* 1 */
    {0,0,1,0}, /* 2 */
    {0,0,1,1}, /* 3 */
    {0,1,0,0}, /* 4 */
    {0,1,0,1}, /* 5 */
    {0,1,1,0}, /* 6 */
    {0,1,1,1}, /* 7 */
    {1,0,0,0}, /* 8 */
    {1,0,0,1}  /* 9 */
};

static int initialized = 0;

void seg_function(int num)
{
    int i;

    if (!initialized) {
        wiringPiSetup();
        for (i = 0; i < 4; i++)
            pinMode(gpiopins[i], OUTPUT);
            initialized = 1;
    }

    if (num < 0 || num > 9) {
        // 범위 벗어나면 모두 HIGH (꺼짐)
        for (i = 0; i < 4; i++)
            digitalWrite(gpiopins[i], HIGH);
        return;
    }

    for (i = 0; i < 4; i++)
        digitalWrite(gpiopins[i], number[num][i] ? HIGH : LOW);

    printf("[7SEG] display %d\n", num);
}
