#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 5000
#define MAX_EVENTS 64
#define BUF_SIZE 256
#define LOG_PATH "/tmp/server.log"

// 추가 기능: HTTP 로그 뷰어] 정의
#define HTTP_PORT 8080
#define HTTP_LINES 200

static int log_fd = -1;
static int foreground = 0;          // -f: 포그라운드 모드 
static char led_state[16] = "off";  // LED 현재 상태
static void *device_handle = NULL;  // libdevice.so 핸들
static void (*led_fptr)(char *) = NULL;
static void (*buzzer_fptr)(char *) = NULL;
static void (*seg_fptr)(int) = NULL;
static volatile int stop_seg = 0;  // 카운트다운 중단 플래그

typedef struct {
    int cfd;
    int num;
} seg_arg_t;

// 조도 임계값 초기값 (0~255 중간) 
static int threshold = 128;
static void (*cds_fptr)(int *) = NULL;
static int (*cds_get_fptr)(void) = NULL;
static pthread_t cds_tid;
// CDS 쓰레드 실행 여부
static int cds_running = 0;         

// 추가 기능: HTTP 로그 뷰어
// main에서 실행 디렉토리 기반으로 설정
static char http_html_path[512] = "";   


// 로그
static void log_write(const char *msg)
{
    char buf[BUF_SIZE + 32];
    char date[32];
    time_t now;
    struct tm *tm_info;

    time(&now);
    tm_info = localtime(&now);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", tm_info);
    snprintf(buf, sizeof(buf), "[%s] %s", date, msg);

    if (foreground) {
        write(STDOUT_FILENO, buf, strlen(buf));
        write(STDOUT_FILENO, "\n", 1);
    } else {
        if (log_fd < 0) return;
        write(log_fd, buf, strlen(buf));
        write(log_fd, "\n", 1);
    }
}

// 데몬
// 부모 삭제. (*매우 중요*) 
static void daemonize(void)
{
    pid_t pid;
    int fd;

    // fork: 부모 종료 / 쉘이 프롬프트 돌려받음 
    pid = fork();
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    // 새 세션 생성 / 제어 터미널 분리
    setsid();

    chdir("/");
    umask(0);

    // 표준 입출력을 /dev/null로 변경
    // 아직 이해가 안되는게 왜?
    // # 확인 이후 주석 삭제 
    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > 2) close(fd);
    }
}


static void *cds_thread(void *arg)
{
    pthread_setname_np(pthread_self(), "cds_monitor");
    (void)arg;
    // cds_function(&threshold) — 내부에서 무한 루프
    cds_fptr(&threshold);
    return NULL;
}

// 장치 라이브러리 초기화
static int device_load(void)
{
    device_handle = dlopen("./libdevice.so", RTLD_LAZY);
    if(!device_handle){ 
        log_write(dlerror());
        return -1; 
    }
    dlerror();

    //led
    led_fptr = dlsym(device_handle, "led_function");
    if(!led_fptr){
        log_write(dlerror()); 
        return -1; 
    }

    //부저
    buzzer_fptr = dlsym(device_handle, "buzzer_function");
    if(!buzzer_fptr){ 
        log_write(dlerror()); 
        return -1; 
    }

    //세그먼트
    seg_fptr = dlsym(device_handle, "seg_function");
    if(!seg_fptr){ 
        log_write(dlerror()); 
        return -1; 
    }

    //조도센서
    cds_fptr = dlsym(device_handle, "cds_function");
    if(!cds_fptr){ 
        log_write(dlerror()); 
        return -1; 
    }

    //조도센서 값
    cds_get_fptr = dlsym(device_handle, "cds_get_value");
    if(!cds_get_fptr){ 
        log_write(dlerror()); 
        return -1; 
    }

    return 0;
}

// LED 쓰레드
static void *led_thread(void *arg)
{
    pthread_setname_np(pthread_self(), "led_ctrl");
    char logbuf[BUF_SIZE];

    // 자기 자신을 detach - 종료 시 자원 자동 해제
    pthread_detach(pthread_self());

    snprintf(logbuf, sizeof(logbuf), "[LED] -> %s", (char *)arg);
    log_write(logbuf);

    // led_function(arg) 호출
    led_fptr((char *)arg);  

    free(arg);
    return NULL;
}

typedef struct { int cfd; char arg[16]; } buz_arg_t;
static void *buzzer_thread(void *arg);

// 7세그먼트 쓰레드
static void *seg_thread(void *arg)
{
    pthread_setname_np(pthread_self(), "seg_countdown");
    seg_arg_t *a = (seg_arg_t *)arg;
    int cfd = a->cfd;
    int n = a->num;
    int i;
    char logbuf[BUF_SIZE];

    free(arg);
    pthread_detach(pthread_self());

    //시작 전 세그먼트 꺼짐
    seg_fptr(-1);  

    for (i = n; i >= 0; i--) {
        // 새 명령 수신 시 중단
        if (stop_seg) break;        
        seg_fptr(i);
        snprintf(logbuf, sizeof(logbuf), "[7SEG] %d", i);
        log_write(logbuf);

        if (i == 0) {
            // 세그먼트 카운트 끝날 때 부저 3초 울리고 끄기
            pthread_t btid;
            pthread_create(&btid, NULL, buzzer_thread, strdup("on"));
            sleep(3);
            pthread_create(&btid, NULL, buzzer_thread, strdup("off"));
            // off 처리 대기
            sleep(1);  
        } else {
            sleep(1);
        }
    }

    // 종료 후 꺼짐
    seg_fptr(-1);  

    // 카운트다운 완료 후 클라이언트에 OK 전송
    write(cfd, "OK\n", 3);
    log_write("[7SEG] countdown done");

    return NULL;
}

// 부저 쓰레드
static void *buzzer_thread(void *arg)
{
    pthread_setname_np(pthread_self(), "buzzer_ctrl");
    char logbuf[BUF_SIZE];
    pthread_detach(pthread_self());

    snprintf(logbuf, sizeof(logbuf), "[BUZZER] -> %s", (char *)arg);
    log_write(logbuf);

    buzzer_fptr((char *)arg);

    free(arg);
    return NULL;
}

// SET buzzer on 전용: 재생 완료 후 cfd로 OK 전송
static void *buzzer_thread_cfd(void *arg)
{
    pthread_setname_np(pthread_self(), "buzzer_cfd");
    buz_arg_t *a = (buz_arg_t *)arg;
    char logbuf[BUF_SIZE];
    pthread_detach(pthread_self());

    snprintf(logbuf, sizeof(logbuf), "[BUZZER] -> %s", a->arg);
    log_write(logbuf);

    buzzer_fptr(a->arg);
    write(a->cfd, "OK\n", 3);
    log_write("[BUZZER] music done");

    free(a);
    return NULL;
}

// 명령어 처리
static void handle_command(int cfd, char *line)
{
    char cmd[32] = {0}, dev[32] = {0}, val[64] = {0};
    char resp[BUF_SIZE];
    char logbuf[BUF_SIZE];

    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0') return;

    sscanf(line, "%31s %31s %63s", cmd, dev, val);

    if (strcmp(cmd, "SET") == 0 && strcmp(dev, "led") == 0 && val[0]) {
        // 상태 저장 후 쓰레드 생성
        strncpy(led_state, val, sizeof(led_state) - 1);
        char *arg = strdup(val);
        pthread_t tid;
        pthread_create(&tid, NULL, led_thread, arg);
        snprintf(resp, sizeof(resp), "OK\n");

    } else if (strcmp(cmd, "GET") == 0 && strcmp(dev, "led") == 0) {
        snprintf(resp, sizeof(resp), "VALUE %s\n", led_state);

    } else if (strcmp(cmd, "SET") == 0 && strcmp(dev, "7seg") == 0 && val[0]) {
        int n = atoi(val);
        if (n < 0 || n > 9) {
            snprintf(resp, sizeof(resp), "ERROR range 0-9\n");
            write(cfd, resp, strlen(resp));
        } else {
            // 카운트 정료 후 새로 시작
            stop_seg = 1;
            usleep(50000);
            stop_seg = 0;
            seg_arg_t *sa = malloc(sizeof(seg_arg_t));
            sa->cfd = cfd;
            sa->num = n;
            pthread_t tid;
            pthread_create(&tid, NULL, seg_thread, sa);
            snprintf(logbuf, sizeof(logbuf), "[TCP] SET 7seg %d -> countdown start", n);
            log_write(logbuf);
        }
        // OK는 seg_thread 완료 후 전송, 여기서 return
        return;  

    } else if (strcmp(cmd, "SET") == 0 && strcmp(dev, "buzzer") == 0 && val[0]) {
        if (strcmp(val, "on") == 0) {
            buz_arg_t *ba = malloc(sizeof(buz_arg_t));
            ba->cfd = cfd;
            strncpy(ba->arg, val, sizeof(ba->arg) - 1);
            ba->arg[sizeof(ba->arg) - 1] = '\0';
            pthread_t tid;
            pthread_create(&tid, NULL, buzzer_thread_cfd, ba);
            log_write("[BUZZER] playing...");
            // OK는 buzzer_thread_cfd 완료 후 전송 
            return;  
        }
        char *arg = strdup(val);
        pthread_t tid;
        pthread_create(&tid, NULL, buzzer_thread, arg);
        snprintf(resp, sizeof(resp), "OK\n");

    } else if (strcmp(cmd, "SET") == 0 && strcmp(dev, "cds") == 0 && val[0]) {
        if (strcmp(val, "start") == 0) {
            // CDS 모니터링 시작
            if (!cds_running) {
                pthread_create(&cds_tid, NULL, cds_thread, NULL);
                cds_running = 1;
                log_write("[CDS] monitoring started");
            }
        } else if (strcmp(val, "stop") == 0) {
            // CDS 모니터링 종료
            if (cds_running) {
                pthread_cancel(cds_tid);
                pthread_join(cds_tid, NULL);
                cds_running = 0;
                log_write("[CDS] monitoring stopped");
            }
        } else {
            // 임계값(THRESHOLD) 변경
            char logbuf2[BUF_SIZE];
            threshold = atoi(val);
            snprintf(logbuf2, sizeof(logbuf2), "[CDS] threshold -> %d", threshold);
            log_write(logbuf2);
        }
        snprintf(resp, sizeof(resp), "OK\n");

    } else if (strcmp(cmd, "GET") == 0 && strcmp(dev, "cds") == 0) {
        snprintf(resp, sizeof(resp), "VALUE %d\n", cds_get_fptr());

    } else {
        snprintf(resp, sizeof(resp), "ERROR unknown command\n");
    }

    write(cfd, resp, strlen(resp));

    snprintf(logbuf, sizeof(logbuf), "[TCP] %s -> %.*s",
             line, (int)strlen(resp) - 1, resp);
    log_write(logbuf);
}

/* HTTP 로그 뷰어
 *   accept - malloc(csock) - pthread_create(clnt_connection) - pthread_detach
 *   clnt_connection: fdopen - fgets/fputs (FILE 스트림)
 * 라우팅:
 *   GET /     - index.html 파일 서빙 (sendData 방식)
 *   GET /log  - 로그 파일 최근 200줄을 HTML div로 반환 (JS fetch 대상) */

static void html_escape(char *dst, int dst_sz, const char *src)
{
    int d = 0, s = 0;
    while (src[s] && d < dst_sz - 6) {
        if(src[s] == '<'){ 
            memcpy(dst+d, "&lt;",  4);
            d += 4; 
        }
        else if(src[s] == '>'){ 
            memcpy(dst+d, "&gt;",  4); 
            d += 4; 
        }
        else if(src[s] == '&'){
            memcpy(dst+d, "&amp;", 5); 
            d += 5; 
        }
        else{ 
            dst[d++] = src[s]; 
        }
        s++;
    }
    dst[d] = '\0';
}

static const char *log_dev(const char *line)
{
    if (strstr(line, "[LED]"))
        return "LED";
    if (strstr(line, "[BUZZER]"))
        return "BUZZER";
    if (strstr(line, "[CDS]"))
        return "CDS";
    if (strstr(line, "[7SEG]"))
        return "7SEG";
    if (strstr(line, "[TCP]") || strstr(line, "[CONNECT]") ||strstr(line, "[DISCONNECT]"))
        return "TCP";
    return "SYS";
}

static const char *log_class(const char *dev)
{
    if (strcmp(dev, "LED") == 0) 
        return "led";
    if (strcmp(dev, "BUZZER") == 0) 
        return "buz";
    if (strcmp(dev, "CDS") == 0) 
        return "cds";
    if (strcmp(dev, "7SEG") == 0) 
        return "seg";
    if (strcmp(dev, "TCP") == 0) 
        return "tcp";
    return "sys";
}

// GET / - index.html 파일 읽어 전송
static void send_file(FILE *fp, const char *path)
{
    FILE *f;
    char buf[BUFSIZ];

    f = fopen(path, "r");
    if (!f) {
        fputs("HTTP/1.1 404 Not Found\r\n"
              "Content-Type: text/html; charset=UTF-8\r\n"
              "Connection: close\r\n\r\n"
              "<h1>404 - index.html not found</h1>"
              "<p>index.html 파일이 서버 실행 디렉토리에 없습니다.</p>", fp);
        fflush(fp);
        return;
    }

    fputs("HTTP/1.1 200 OK\r\n", fp);
    fputs("Content-Type: text/html; charset=UTF-8\r\n", fp);
    fputs("Connection: close\r\n", fp);
    fputs("\r\n", fp);

    while (fgets(buf, BUFSIZ, f))
        fputs(buf, fp);

    fclose(f);
    fflush(fp);
}

// GET /log → 로그 최근 HTTP_LINES줄을 HTML div 조각으로 반환 (JS fetch 대상)
static void send_log(FILE *fp)
{
    char lines[HTTP_LINES][BUF_SIZE + 64];
    int  head = 0, count = 0;
    FILE *f;
    char ts_esc[64], msg_esc[2048];
    int  start, i;

    f = fopen(LOG_PATH, "r");
    if (f) {
        while (fgets(lines[head], sizeof(lines[head]), f)) {
            head = (head + 1) % HTTP_LINES;
            if (count < HTTP_LINES) count++;
        }
        fclose(f);
    }

    fputs("HTTP/1.1 200 OK\r\n", fp);
    fputs("Content-Type: text/html; charset=UTF-8\r\n", fp);
    fputs("Connection: close\r\n", fp);
    fputs("\r\n", fp);

    if (count == 0) {
        fputs("<div class='empty'>로그 없음</div>", fp);
        fflush(fp);
        return;
    }

    start = (count == HTTP_LINES) ? head : 0;
    for (i = count - 1; i >= 0; i--) {  // 최신 항목이 먼저 출력되도록 역순
        int cur = (start + i) % HTTP_LINES;
        const char *dev = log_dev(lines[cur]);
        const char *msg;
        lines[cur][strcspn(lines[cur], "\n")] = '\0';

        // 타임스탬프 "[YYYY-MM-DD HH:MM:SS]" 와 메시지 분리
        msg = lines[cur];
        ts_esc[0] = '\0';
        if (msg[0] == '[') {
            const char *end = strchr(msg + 1, ']');
            if (end) {
                char ts_buf[32];
                int len = (int)(end - msg + 1);
                if (len < (int)sizeof(ts_buf)) {
                    memcpy(ts_buf, msg, len);
                    ts_buf[len] = '\0';
                    html_escape(ts_esc, sizeof(ts_esc), ts_buf);
                }
                msg = end + 1;
                while (*msg == ' ') msg++;
            }
        }
        html_escape(msg_esc, sizeof(msg_esc), msg);

        fprintf(fp,
            "<div class='log-entry %s' data-dev='%s'>"
            "<span class='ts'>%s</span>"
            "<span class='msg'>%s</span>"
            "</div>\n",
            log_class(dev), dev, ts_esc, msg_esc);
    }

    fflush(fp);
}

// 접속마다 생성되는 쓰레드: HTTP 요청 파싱 후 라우팅
static void *clnt_connection(void *arg)
{
    pthread_setname_np(pthread_self(), "http_client");
    int csock = *((int *)arg);
    free(arg);

    FILE *clnt_read  = fdopen(csock, "r");
    FILE *clnt_write = fdopen(dup(csock), "w");

    char req[BUFSIZ];
    int  is_log;
    char tmp[BUFSIZ];

    // 요청 첫 줄 읽기
    if (!fgets(req, BUFSIZ, clnt_read)) goto done;

    is_log = strstr(req, "GET /log") != NULL;

    // 나머지 HTTP 헤더 소비
    do {
        if (!fgets(tmp, BUFSIZ, clnt_read)) 
            break;
    } while (strcmp(tmp, "\r\n") != 0);

    // 로그 데이터 반환
    if (is_log)
        send_log(clnt_write);       
    else
        // index.html 서빙
        send_file(clnt_write, http_html_path);  

done:
    fflush(clnt_write);
    fclose(clnt_read);
    fclose(clnt_write);
    pthread_exit(NULL);
    return NULL;
}

// HTTP 서버 쓰레드: 포트 8080 accept 루프
static void *http_thread(void *arg)
{
    pthread_setname_np(pthread_self(), "http_server");
    int ssock, *csock;
    struct sockaddr_in servaddr, cliaddr;
    unsigned int len = sizeof(cliaddr);
    pthread_t thread;
    int opt = 1;

    pthread_detach(pthread_self());
    (void)arg;

    ssock = socket(AF_INET, SOCK_STREAM, 0);
    if (ssock < 0) 
        return NULL;

    setsockopt(ssock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(HTTP_PORT);

    if (bind(ssock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0 || listen(ssock, 10) < 0) {
        close(ssock);
        return NULL;
    }

    log_write("[HTTP] log viewer started on port 8080");

    while (1) {
        csock  = malloc(sizeof(int));
        *csock = accept(ssock, (struct sockaddr *)&cliaddr, &len);
        if (*csock < 0) { 
            free(csock); 
            continue; 
        }
        pthread_create(&thread, NULL, clnt_connection, csock);
        pthread_detach(thread);
    }

    close(ssock);
    return NULL;
}


// 서버 초기화
static int server_init(int *server_fd, int *epfd)
{
    struct sockaddr_in addr;
    struct epoll_event ev;
    int opt = 1;

    *server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (*server_fd < 0) 
        return -1;

    setsockopt(*server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(*server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return -1;
    if (listen(*server_fd, 5) < 0) 
        return -1;

    fcntl(*server_fd, F_SETFL, O_NONBLOCK);

    *epfd = epoll_create1(0);
    if (*epfd < 0)
        return -1;

    ev.events  = EPOLLIN;
    ev.data.fd = *server_fd;
    epoll_ctl(*epfd, EPOLL_CTL_ADD, *server_fd, &ev);

    return 0;
}

// 서버 메인 루프
static void server_run(int server_fd, int epfd)
{
    struct epoll_event ev, events[MAX_EVENTS];

    for (;;) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) 
            break;

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                // 새 클라이언트 접쇽
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int cfd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
                if (cfd < 0) continue;

                fcntl(cfd, F_SETFL, O_NONBLOCK);
                ev.events  = EPOLLIN;
                ev.data.fd = cfd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);

                char logbuf[BUF_SIZE];
                snprintf(logbuf, sizeof(logbuf), "[CONNECT] %s:%d",inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
                log_write(logbuf);
            } else {
                // 클라이언트의 데이터 수신
                char buf[BUF_SIZE];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n <= 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                    log_write("[DISCONNECT] client");
                    // 한번에 클라이언트가 생성한 쓰레드 종료
                    
                } else {
                    buf[n] = '\0';
                    handle_command(fd, buf);
                }
            }
        }
    }
}


int main(int argc, char *argv[])
{
    int server_fd, epfd;

    // 포어그라운드 모드(터미널 출력, 디버깅 할떄만 사용해야지 , 데몬 X )
    // 실행시 -f 옵션으로 실행
    if (argc > 1 && strcmp(argv[1], "-f") == 0)
        foreground = 1;

    signal(SIGPIPE, SIG_IGN);   // HTTP write 중 브라우저 연결 끊김 시 프로세스 종료 방지

    // daemonize 전 실행 디렉토리 저장
    {
        char cwd[500];
        if (getcwd(cwd, sizeof(cwd)))
            snprintf(http_html_path, sizeof(http_html_path), "%s/index.html", cwd);
    }
    // 로그 뷰어

    // device_load는 daemonize 전에 호출
    if (device_load() < 0) {
        fprintf(stderr, "device_load failed\n");
        return 1;
    }

    if (!foreground) {
        daemonize();
        log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }

    if (server_init(&server_fd, &epfd) < 0) {
        log_write("server_init failed");
        return 1;
    }

    // 로그 뷰어
    if (!foreground) {
        pthread_t htid;
        pthread_create(&htid, NULL, http_thread, NULL);
    }

    log_write("server started, listening on port 5000");
    server_run(server_fd, epfd);

    close(server_fd);
    close(epfd);
    if (log_fd >= 0) close(log_fd);
    return 0;
}
