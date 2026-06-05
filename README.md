# TCP 기반 임베디드 시스템 원격 장치 제어

라즈베리파이4(서버)에 연결된 LED, 부저, 조도센서, 7세그먼트를 우분투 리눅스 PC(클라이언트)에서 TCP 소켓으로 원격 제어하는 시스템.

---

## 빌드 환경

| 항목 | 요구사항 |
|------|----------|
| 호스트 OS | 우분투 리눅스 (x86_64) |
| 크로스 컴파일러 | `aarch64-linux-gnu-gcc` |
| 타겟 | 라즈베리파이4 (AArch64) |
| 타겟 라이브러리 | wiringPi (라즈베리파이에 설치) |

**크로스 컴파일러 설치 (우분투):**
```bash
sudo apt install gcc-aarch64-linux-gnu
```

**라즈베리파이 SSH 키 등록 (최초 1회):**
```bash
ssh-copy-id 사용자@<라즈베리파이 IP>
```

---

## 빌드 및 배포

### Makefile 라즈베리파이 접속 정보

`Makefile` 상단의 변수를 환경에 맞게 수정한다:

```makefile
PI_IP   = # 라즈베리파이 IP 주소
PI_USER = # 사용자명
PI_DIR  = ~/project     # 라즈베리파이 배포 경로 / project 디렉토리가 존재하지 않으면 생성
```

---

### 기본 빌드

> **[우분투]** 소스 디렉토리에서 실행한다.

```bash
# [우분투] 소스 디렉토리
# ex) vboxuser@Ubuntu:~/project$ make 
cd ~/project
make
```

`make` 한 번으로 아래 작업이 순서대로 수행된다:

| 단계 | 산출물 | 컴파일러 | 자동 전송 |
|------|--------|----------|-----------|
| 1 | `client` | gcc (우분투 네이티브) | — (우분투 로컬 실행) |
| 2 | `lib/libdevice.so` | aarch64-linux-gnu-gcc | scp → 라즈베리파이 `~/project/` |
| 3 | `server_r` | aarch64-linux-gnu-gcc | scp → 라즈베리파이 `~/project/` |
| 4 | `index.html` | (정적 파일) | scp → 라즈베리파이 `~/project/` |

소스가 변경된 파일만 재빌드·재전송한다 (`.deployed_server` / `.deployed_device` / `.deployed_html` 타임스탬프 비교).

### 빌드 산출물 삭제

```bash
# [우분투] 소스 디렉토리
# ex) vboxuser@Ubuntu:~/project$ make clean
make clean
```

오브젝트 파일, 바이너리, 배포 타임스탬프 파일(`.deployed_*`, `.dir_ready`)을 모두 삭제한다.

---

## 실행

### 1단계 — 서버 실행 (라즈베리파이)

`make` 완료 후 라즈베리파이에 SSH 접속해 배포 디렉토리에서 실행한다.

```bash
# [우분투] SSH 접속
ssh 사용자@<라즈베리파이 IP>

# [라즈베리파이] 배포 디렉토리로 이동
cd ~/project

# [라즈베리파이] 데몬 모드로 실행 (백그라운드, 로그 → /tmp/server.log)
# ex) kwon@MyRaspberry:~/project $ ./server_r 
./server_r

# [라즈베리파이] 또는 포그라운드 모드 (터미널 직접 출력, 디버깅용 )
./server_r -f

# [라즈베리파이] 서버 종료 (데몬 모드)
# ex) kwon@MyRaspberry:~/project $ kill $(pgrep -x server_r) 
kill $(pgrep -x server_r)

# [라즈베리파이] 쓰레드별 이름 확인
ps -T -p $(pgrep server_r)
```

> `server_r`, `libdevice.so`, `index.html` 세 파일이 같은 디렉토리(`~/project/`)에 있어야 한다. `make`가 자동 전송하므로 별도 복사 불필요.

### 2단계 — 클라이언트 실행 (우분투)

서버가 실행 중인 상태에서 우분투의 소스 디렉토리에서 실행한다.

```bash
# [우분투] 소스 디렉토리
cd ~/project

# IP 주소로 접속
./client <라즈베리파이 IP>

# hostname 설정이 되어있을 때 (추천방식) ex) rpi
# ex) vboxuser@Ubuntu:~/project$ ./client rpi
./client <hostname> 
```

접속 후 메뉴에서 장치를 선택해 제어한다. 클라이언트 종료는 메인 메뉴에서 `0 입력` 또는 `Ctrl+C`.

### HTTP 로그 뷰어 (추가 기능)

서버를 **데몬 모드**(`./server_r`)로 실행했을 때만 활성화된다.  
우분투 브라우저에서 접속:

#### 시크릿 모드 사용 권장
```
http://<라즈베리파이 IP>:8080
```

포그라운드 모드(`./server_r -f`)에서는 비활성화된다.

---

## 디렉토리 구조

**우분투 (소스 / 빌드)**
```
~/project/
├── server.c          # 서버 소스 (데몬, epoll, 명령 처리, 쓰레드)
├── client.c          # 클라이언트 소스 (메뉴 UI, 시그널 처리)
├── client            # 클라이언트 바이너리 (우분투 실행용)
├── lib/
│   ├── led.c         # LED 제어 (softPwm, wiringPi 핀 5)
│   ├── buzzer.c      # 부저 음악 재생 (softTone, wiringPi 핀 6)
│   ├── cds.c         # 조도센서 I2C 읽기 + LED 자동 제어
│   └── 7seg.c        # 7세그먼트 BCD 출력 (wiringPi 핀 4, 1, 16, 15)
├── index.html        # HTTP 로그 뷰어
├── Makefile
└── README.md
```

**라즈베리파이 (배포 / 실행)** — `make` 가 자동으로 전송
```
~/project/
├── server_r          # 서버 실행(크로스컴파일 결과물)
├── libdevice.so      # 장치 라이브러리 (크로스컴파일 결과물)
└── index.html        # HTTP 로그 뷰어 프론트엔드
```

로그 파일은 서버 실행 시 자동 생성된다(라즈베리 파이):
```
/tmp/server.log       # 타임스탬프 포함 동작 로그
```

---

## 동작을 위한 하드웨어 연결

코드에 고정된 wiringPi 핀 번호 기준. 배선이 다를 경우 각 소스 파일의 `#define`을 수정 후 `make` 재실행.

| 장치 | wiringPi 핀 | BCM 핀 | 비고 |
|------|------------|--------|------|
| LED | 5 | BCM 24 | softPwm (밝기 제어) |
| 부저 | 6 | BCM 25 | softTone (음악 재생) |
| 7세그먼트 A | 4 | BCM 23 | BCD 입력 A |
| 7세그먼트 B | 1 | BCM 18 | BCD 입력 B |
| 7세그먼트 C | 16 | BCM 15 | BCD 입력 C |
| 7세그먼트 D | 15 | BCM 14 | BCD 입력 D |
| 조도센서 (CDS) | — | I2C (SDA/SCL) | ADC 주소 `0x48`, 채널 `0x00`, `/dev/i2c-1` |

**조도센서 사용 전 I2C 활성화 필요 (라즈베리파이):**
```bash
sudo raspi-config
# Interface Options → I2C → Enable → 재부팅

# ADC 장치 인식 확인 (0x48 이 보여야 함)
i2cdetect -y 1
```


## TCP 명령 프로토콜

서버 포트 `5000`. 텍스트 기반, 한 줄(`\n`) = 한 명령.

```
SET led on|off|high|mid|low   → OK
GET led                        → VALUE <state>
SET buzzer on                  → OK  (음악 재생 완료 후 응답)
SET buzzer off                 → OK
SET 7seg <0-9>                 → OK  (카운트다운 완료 후 응답)
SET cds start|stop             → OK
SET cds <0-255>                → OK  (임계값 변경)
GET cds                        → VALUE <n>
```
