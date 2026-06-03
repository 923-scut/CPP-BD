CXX ?= g++
CXXFLAGS ?= -std=c++14 -O2 -Wall -Wextra
LDFLAGS ?= -mwindows -lws2_32 -lgdi32
OUT_DIR := build
TARGET := $(OUT_DIR)/YingYuBoYi.exe
SRC := CPP-BD/src/main.cpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC)
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	$(CXX) $(CXXFLAGS) "$<" -o "$@" $(LDFLAGS)

run: $(TARGET)
	"$(TARGET)"

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"
