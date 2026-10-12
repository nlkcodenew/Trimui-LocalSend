CXX ?= g++
STRIP ?= strip
SYSROOT ?=
VERSION ?= 0.1.0

TARGET := files/bin/trimui-localsend
SOURCES := src/main.cpp src/AppConfig.cpp src/Logger.cpp src/LocalSendManager.cpp src/OtaVerify.cpp
CPPFLAGS := -Isrc
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -DAPP_VERSION=\"$(VERSION)\"
LDLIBS := -lSDL2 -lSDL2_ttf -lcurl -lssl -lcrypto -lpthread -ldl -lm

ifneq ($(strip $(SYSROOT)),)
CPPFLAGS += --sysroot=$(SYSROOT) -I$(SYSROOT)/usr/include/SDL2
LDFLAGS += --sysroot=$(SYSROOT) -L$(SYSROOT)/usr/lib
else
CPPFLAGS += $(shell pkg-config --cflags sdl2 SDL2_ttf 2>/dev/null)
LDLIBS := $(shell pkg-config --libs sdl2 SDL2_ttf 2>/dev/null) -lcurl -lssl -lcrypto -lpthread -ldl -lm
endif

.PHONY: all build clean

all: build

build: $(TARGET)

$(TARGET): $(SOURCES)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)
