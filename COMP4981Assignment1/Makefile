CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Iinclude

TARGET = http_server
SRC = src/main.c src/server.c src/http.c src/path.c src/util.c

.PHONY: all clean run test debug

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET) 8080 ./www

test: $(TARGET)
	bash tests/test.sh

debug: clean
	$(MAKE) CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -g -O0' all

clean:
	rm -f $(TARGET)
