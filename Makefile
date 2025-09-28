CC		:= g++
CFLAGS 	:= -O3 -flto
LDFLAGS := $(CFLAGS) \
-lcurl -lm
TARGET  := tuimarket
SRCS    := $(wildcard src/*.cpp)
OBJS    := $(patsubst %.cpp,%.o,$(SRCS))
all: $(OBJS)
	$(CC) $(OBJS) $(CFLAGS) $(LDFLAGS) -o $(TARGET) 
