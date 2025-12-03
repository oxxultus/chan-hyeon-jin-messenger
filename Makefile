# Makefile - 메신저 프로젝트용 (Server/Client)

# --- 변수 정의 ---
CC = gcc
# GTK 3 라이브러리와 POSIX 스레드(-pthread)를 CFLAGS와 LDFLAGS에 모두 포함
# CFLAGS: 컴파일 플래그
CFLAGS = -Wall -Wextra -Wno-unused-parameter -c $(shell pkg-config --cflags gtk+-3.0)
# LDFLAGS: 링크 플래그
LDFLAGS = -pthread $(shell pkg-config --libs gtk+-3.0)

# 실행 파일 이름 정의
SERVER_TARGET = bin/server
CLIENT_TARGET = bin/client
TARGETS = $(SERVER_TARGET) $(CLIENT_TARGET)

# 빌드 디렉토리 정의
BUILD_DIR = build
DIR_CHECK = $(BUILD_DIR) # 디렉토리 생성 타겟 이름


# --- 소스 파일 목록 ---
# src/server.c 와 src/client.c 만 컴파일한다고 가정합니다.
SERVER_SRCS = src/server.c
CLIENT_SRCS = src/client.c

# 오브젝트 파일 목록 (build/ 디렉토리에 저장)
SERVER_OBJS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SERVER_SRCS))
CLIENT_OBJS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(CLIENT_SRCS))
OBJS = $(SERVER_OBJS) $(CLIENT_OBJS)


# --- 기본 타겟: 컴파일 및 링크 ---
.PHONY: all
all: $(TARGETS)

# 1. 서버 실행 파일 생성
$(SERVER_TARGET): $(SERVER_OBJS)
	@mkdir -p bin
	@$(CC) $(SERVER_OBJS) -o $@ $(LDFLAGS)
	@echo "Server 빌드 완료: $@"

# 2. 클라이언트 실행 파일 생성
$(CLIENT_TARGET): $(CLIENT_OBJS)
	@mkdir -p bin
	@$(CC) $(CLIENT_OBJS) -o $@ $(LDFLAGS)
	@echo "Client 빌드 완료: $@"


# 3. 오브젝트 파일 생성 규칙 (컴파일)
# build/ 디렉토리를 prerequisite으로 추가
$(BUILD_DIR)/%.o: src/%.c | $(DIR_CHECK)
	@$(CC) $(CFLAGS) $< -o $@

# 디렉토리 생성 규칙
$(DIR_CHECK):
	@mkdir -p $(BUILD_DIR)


# --- 유틸리티 타겟 ---

# clean 타겟: 생성된 실행 파일 및 build 디렉토리 전체 제거
.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) 
	rm -rf bin
	@echo "빌드 파일과 실행 파일이 모두 제거되었습니다."

# run-server 타겟: 서버 빌드 후 실행
.PHONY: run-server
run-server: $(SERVER_TARGET)
	@echo "==================================="
	@echo "서버 실행 (Ctrl+C로 종료)"
	@echo "==================================="
	./$(SERVER_TARGET)

# run-client 타겟: 클라이언트 빌드 후 실행
.PHONY: run-client
run-client: $(CLIENT_TARGET)
	./$(CLIENT_TARGET)

# rebuild 타겟: clean 후 all 실행
.PHONY: rebuild
rebuild: clean all