#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>  // 클라이언트가 gethostbyname으로 실행
#include <pthread.h>
#include <sys/select.h>

#define PORT 5000
#define BUF_SIZE 256

// 장치별 제어 항목
typedef struct {
    const char *label;
    const char *cmd; // NULL이면 fn 사용  
    void (*fn)(void); // 특수 처리 함수
} sub_item_t;

/* 장치 메뉴 */
typedef struct {
    const char *name;
    sub_item_t *items;
    int count;
} device_menu_t;

// led 제어 항목
static sub_item_t led_items[] = {
    {"LED ON",           "SET led on\n",   NULL},
    {"LED OFF",          "SET led off\n",  NULL},
    {"밝기 최대 (high)", "SET led high\n", NULL},
    {"밝기 중간 (mid)",  "SET led mid\n",  NULL},
    {"밝기 최저 (low)",  "SET led low\n",  NULL},
    {"현재 상태 조회",   "GET led\n",      NULL},
};

// cds, segment, buzzer 제어 항목
static void cds_monitor(void);
static void seg_start(void);
static void buzzer_on_fn(void);
static int send_command(const char *cmd, char *resp, int resp_sz);

static sub_item_t cds_items[] = {
    {"조도 모니터링 (임계값 설정 가능)", NULL, cds_monitor},
};

static sub_item_t seg_items[] = {
    {"카운트다운 시작 (0~9 입력)", NULL, seg_start},
};

static sub_item_t buzzer_items[] = {
    {"부저 ON (음악 재생)", NULL,              buzzer_on_fn},
    {"부저 OFF",            "SET buzzer off\n", NULL},
};


// 장치 메뉴 목록
static device_menu_t device_menu[] = {
    {"LED",        led_items,    6},
    {"부저",        buzzer_items, 2},
    {"조도 센서",   cds_items,    1},
    {"7세그먼트",   seg_items,    1},
};
#define DEVICE_COUNT (int)(sizeof(device_menu) / sizeof(device_menu[0]))

// 전역 변수 
static int sock_fd = -1;
static int client_threshold = 128;  // 클라이언트 측 임계값 (Bright/Dark 판단용)

static void sigint_handler(int sig)
{
    (void)sig;
    if (sock_fd >= 0) close(sock_fd);
    _exit(0);
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = sigint_handler;
    sigaction(SIGINT,  &sa, NULL);

    // sigint 이외 무시
    sa.sa_handler = SIG_IGN;
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
}

//부저 ON: 재생 중 2 입력 시 즉시 OFF
static void buzzer_on_fn(void)
{
    char resp[BUF_SIZE];
    char line[16];
    fd_set fds;
    ssize_t n;

    if (send(sock_fd, "SET buzzer on\n", 14, 0) < 0) {
        printf("서버 연결 오류\n");
        return;
    }

    printf("부저 재생 중... (2: OFF)\n");
    fflush(stdout);

    for (;;) {
        FD_ZERO(&fds);
        FD_SET(sock_fd, &fds);
        FD_SET(STDIN_FILENO, &fds);

        if (select(sock_fd + 1, &fds, NULL, NULL, NULL) < 0) break;

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            if (!fgets(line, sizeof(line), stdin)) break;
            if (atoi(line) == 2) {
                send(sock_fd, "SET buzzer off\n", 15, 0);
                // OK 2개 소비:
                // 1) handle_command가 buzzer off에 즉시 응답
                // 2) buzzer_thread_cfd가 stop_flag 감지 후 응답
                recv(sock_fd, resp, sizeof(resp) - 1, 0);
                recv(sock_fd, resp, sizeof(resp) - 1, 0);
                break;
            }
            // 2 외 입력 무시, 계속 대기
        }

        if (FD_ISSET(sock_fd, &fds)) {
            // 음악 자연 종료 
            n = recv(sock_fd, resp, sizeof(resp) - 1, 0);
            if (n > 0) printf("재생 완료\n");
            break;
        }
    }

}


// CDS 모니터링 쓰레드
static void *monitor_thread(void *arg)
{
    char resp[BUF_SIZE];
    int cnt = 0;
    int a2dVal;
    (void)arg;

    while (1) {
        if (send_command("GET cds\n", resp, sizeof(resp)) < 0) break;
        resp[strcspn(resp, "\n")] = '\0';

        if (sscanf(resp, "VALUE %d", &a2dVal) == 1) {
            printf("[%3d] a2dVal=%-4d threshold=%-4d %s\n", cnt++, a2dVal, client_threshold, a2dVal < client_threshold ? "Bright" : "Dark");
        } else {
            printf("[%3d] %s\n", cnt++, resp);
        }
        fflush(stdout);
        // pthread_cancel 취소 지점
        sleep(1);
    }
    return NULL;
}

// 조도 모니터링: 서버 cds_thread 시작 / 값 출력 / 종료 시 서버 cds_thread도 종료
static void cds_monitor(void)
{
    pthread_t mon_t;
    char line[32];
    char cmd[BUF_SIZE];
    char resp[BUF_SIZE];
    int val;

    // 서버에 CDS 모니터링 시작 요청
    if (send_command("SET cds start\n", resp, sizeof(resp)) < 0) {
        printf("서버 연결 오류\n");
        return;
    }

    printf("\n조도 모니터링 시작 (초기 임계값: %d)\n", client_threshold);
    printf("숫자 입력: 임계값 변경 (모니터링 유지) / -1: 종료\n\n");

    pthread_create(&mon_t, NULL, monitor_thread, NULL);

    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "%d", &val) != 1) 
            continue;

        // -1: 모니터링 종료
        if (val == -1) {
            pthread_cancel(mon_t);
            pthread_join(mon_t, NULL);
            send_command("SET cds stop\n", resp, sizeof(resp));
            printf("조도 모니터링 종료\n");
            break;
        }

        //THRESHOLD 변경, 모니터링 우지
        if (val >= 0 && val <= 255) {
            
            client_threshold = val;
            snprintf(cmd, sizeof(cmd), "SET cds %d\n", val);
            if (send_command(cmd, resp, sizeof(resp)) == 0)
                printf(">>> 임계값 변경: %d\n", val);
            } 
            else {
                printf("유효 범위: 0~255  (-1: 종료)\n");
            }
    }
}

//7 세그먼트 카운트다운 쓰레드
static void *seg_display_thread(void *arg)
{
    int n = *(int *)arg;
    int i;
    free(arg);

    for (i = n; i >= 0; i--) {
        printf("\r  카운트다운: %d   ", i);
        fflush(stdout);
        // pthread_cancel 취소 지점
        sleep(1);   
    }
    return NULL;
}

// 7세그먼트 카운트다운 시작
static void seg_start(void)
{
    char line[16];
    char cmd[BUF_SIZE];
    char resp[BUF_SIZE];
    int n;
    ssize_t r;
    pthread_t disp_t;
    int *disp_n;

    printf("카운트다운 시작 숫자 입력 (0~9): ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin)) return;
    if (sscanf(line, "%d", &n) != 1 || n < 0 || n > 9) {
        printf("유효 범위: 0~9\n");
        return;
    }

    snprintf(cmd, sizeof(cmd), "SET 7seg %d\n", n);

    // 서버에 전송
    send(sock_fd, cmd, strlen(cmd), 0);

    // 클라이언트 카운트다운 출력 쓰레드 시작
    disp_n = malloc(sizeof(int));
    *disp_n = n;
    pthread_create(&disp_t, NULL, seg_display_thread, disp_n);

    // 서버 OK 대기 (카운트다운 완료까지 블로킹)
    r = recv(sock_fd, resp, sizeof(resp) - 1, 0);
    if (r > 0) {
        resp[r] = '\0';
        resp[strcspn(resp, "\n")] = '\0';
    }

    pthread_cancel(disp_t);
    pthread_join(disp_t, NULL);
    printf("\n완료: %s\n", resp);
}

// 메뉴 출력
static void print_main_menu(void)
{
    printf("\n================================\n");
    printf("      장치 제어 클라이언트\n");
    printf("================================\n");
    printf("   %2d. %s\n", 1, "LED");
    printf("   %2d. %s\n", 2, "부저");
    printf("   %2d. %s\n", 3, "조도 센서");
    printf("   %2d. %s\n", 4, "7세그먼트");
    printf("--------------------------------\n");
    printf("    0. 종료\n");
    printf("================================\n");
    printf("선택 > ");
    fflush(stdout);
}

static void print_sub_menu(device_menu_t *dev)
{
    printf("\n================================\n");
    printf("      %s 제어\n", dev->name);
    printf("================================\n");
    for (int i = 0; i < dev->count; i++)
        printf("   %2d. %s\n", i + 1, dev->items[i].label);
    printf("--------------------------------\n");
    printf("    0. 뒤로\n");
    printf("================================\n");
    printf("선택 > ");
    fflush(stdout);
}

// 명령 전송
static int send_command(const char *cmd, char *resp, int resp_sz)
{
    if (send(sock_fd, cmd, strlen(cmd), 0) < 0) 
        return -1;

    ssize_t n = recv(sock_fd, resp, resp_sz - 1, 0);

    if (n <= 0) 
        return -1;

    resp[n] = '\0';
    return 0;
}

// main
int main(int argc, char *argv[])
{
    struct sockaddr_in addr;
    struct hostent *he;
    char input[16];
    char resp[BUF_SIZE];
    int choice;
    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";

    setup_signals();

    // 호스트명 또는 IP 주소를 서버 주소 변환
    he = gethostbyname(ip);
    if (he == NULL) {
        perror("gethostbyname");
        return 1;
    }

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0){ 
        perror("socket"); 
        return 1; 
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr = *((struct in_addr *)he->h_addr_list[0]);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return 1;
    }

    printf("서버 접속: %s:%d\n", ip, PORT);

    while (1) {
        // 제어할 장치 선택
        print_main_menu();
        if (!fgets(input, sizeof(input), stdin)) 
            break;
        if (sscanf(input, "%d", &choice) != 1) 
            continue;
        if (choice == 0) 
            break;
        if (choice < 1 || choice > DEVICE_COUNT) {
            printf("없는 메뉴입니다.\n");
            continue;
        }

        device_menu_t *dev = &device_menu[choice - 1];

        // 장치 제어
        while (1) {
            print_sub_menu(dev);
            if (!fgets(input, sizeof(input), stdin)) 
                goto done;
            if (sscanf(input, "%d", &choice) != 1) 
                continue;
            if (choice == 0) 
                break;
            if (choice < 1 || choice > dev->count) {
                printf("없는 메뉴입니다.\n");
                continue;
            }

            if (dev->items[choice - 1].fn) {
                // 특수 처리 함수 (CDS 모니터링)
                dev->items[choice - 1].fn();
            } else {
                if (send_command(dev->items[choice - 1].cmd, resp, sizeof(resp)) < 0) {
                    printf("서버 연결이 끊어졌습니다.\n");
                    goto done;
                }
                resp[strcspn(resp, "\n")] = '\0';
                printf("응답: %s\n", resp);

            }
        }
    }

done:
    printf("\n종료합니다.\n");
    close(sock_fd);
    return 0;
}
