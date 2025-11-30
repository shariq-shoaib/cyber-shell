cat > Makefile << 'EOF'
CC = gcc
CFLAGS = -Wall -Wextra
TARGET = mysh
SOURCE = src/mysh.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE)

test:
	./scripts/test_shell.sh

clean:
	rm -f $(TARGET)

.PHONY: all test clean
EOF