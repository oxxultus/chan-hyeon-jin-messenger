# Makefile

# --- 변수 정의 ---
CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -I./include -c
LDFLAGS = 
TARGET = bin/main

# 빌드 디렉토리 정의
BUILD_DIR = build/src
DIR_CHECK = $(BUILD_DIR) # 디렉토리 생성 타겟 이름

# 소스 파일 목록
SRCS = src/main.c \
       src/command_func.c \
       src/signal_func.c \
       src/custom_command.c

# 오브젝트 파일 목록 (build/src/ 디렉토리에 저장)
# 예: build/src/main.o, build/src/command_func.o
OBJS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRCS))


# --- 기본 타겟: 컴파일 및 링크 ---
.PHONY: all
all: $(TARGET)

# 1. 최종 실행 파일 생성: 오브젝트 파일들을 링크하여 TARGET 생성
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

# 2. 오브젝트 파일 생성 규칙 (컴파일)
# build/src/ 디렉토리를 prerequisite으로 추가
$(BUILD_DIR)/%.o: src/%.c | $(DIR_CHECK)
	$(CC) $(CFLAGS) $< -o $@

# 디렉토리 생성 규칙
$(DIR_CHECK):
	@mkdir -p $(BUILD_DIR)

# --- 유틸리티 타겟 ---

# clean 타겟: 생성된 실행 파일 및 build 디렉토리 전체 제거
.PHONY: clean
clean:
	rm -f $(TARGET)
	rm -rf build 

# run 타겟: 빌드 후 바로 실행
.PHONY: run
run: $(TARGET)
	./$(TARGET)

# rebuild 타겟: clean 후 all 실행
.PHONY: rebuild
rebuild: clean all
