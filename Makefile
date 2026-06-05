CC       = gcc
CROSS_CC = aarch64-linux-gnu-gcc
CFLAGS   = -Wall -g
LDLIBS   = -lwiringPi -lcrypt
SERVER_LDFLAGS = -lpthread -ldl
CLIENT_LDFLAGS = -lpthread

# 라즈베리파이 접속 정보
PI_IP   = 
PI_USER = 
PI_DIR  = ~/project

DEVICE_OBJS = lib/led.o lib/buzzer.o lib/cds.o lib/7seg.o

.PHONY: all clean check_env

# 'make'만 치면 빌드 + 변경된 파일만 전송
all: client .deployed_server .deployed_device .deployed_html

# 장치 소스는 lib/ 에 존재
$(DEVICE_OBJS): lib/%.o: lib/%.c
	$(CROSS_CC) -c -fPIC -o $@ $<

server_r: server.c
	$(CROSS_CC) $(CFLAGS) -o $@ $< $(SERVER_LDFLAGS)

# 모든 장치 소스를 lib/libdevice.so 하나로 통합
lib/libdevice.so: $(DEVICE_OBJS)
	$(CROSS_CC) -shared -o $@ $(DEVICE_OBJS) $(LDLIBS)

client: client.c
	$(CC) $(CFLAGS) -o $@ $< $(CLIENT_LDFLAGS)

# 환경변수 체크
check_env:
	@if [ -z "$(PI_IP)" ] || [ -z "$(PI_USER)" ] || [ -z "$(PI_DIR)" ]; then \
		echo "ERROR: PI_IP / PI_USER / PI_DIR 를 설정하세요"; exit 1; fi

# Pi 디렉토리 생성 (최초 1회)
.dir_ready: | check_env
	@ssh $(PI_USER)@$(PI_IP) "mkdir -p $(PI_DIR)"
	@touch $@

# 서버 / 장치 라이브러리 개별 전송
.deployed_server: server_r | .dir_ready
	@echo "전송: server_r"
	@scp server_r $(PI_USER)@$(PI_IP):$(PI_DIR)
	@touch $@

.deployed_device: lib/libdevice.so | .dir_ready
	@echo "전송: libdevice.so"
	@scp lib/libdevice.so $(PI_USER)@$(PI_IP):$(PI_DIR)
	@touch $@

# HTTP 로그 뷰어
.deployed_html: index.html | .dir_ready
	@echo "전송: index.html"
	@scp index.html $(PI_USER)@$(PI_IP):$(PI_DIR)
	@touch $@

clean:
	rm -f lib/*.o lib/libdevice.so server_r client
	rm -f .deployed_server .deployed_device .deployed_html .dir_ready
