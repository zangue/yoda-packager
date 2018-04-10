CC           =clang
#CFLAGS       =
FFMPEG_FLAGS =-lavutil -lavformat -lavcodec -lavutil -lswscale -lswresample
SRC          =main.c third_party/argtable3.c muxer.c mpd.c utils.c
BIN          =segmenter

.PHONY: all
all: main.c \
    third_party/argtable3.c third_party/argtable3.h \
    utils.c utils.h \
    muxer.c muxer.h \
    mpd.c mpd.h \
    common.h
	mkdir -p bin
	$(CC) $(FFMPEG_FLAGS) $(SRC) -o bin/$(BIN) -g

.PHONY: clean
clean:
	rm -f bin/$(BIN)
	rm -rf bin/$(BIN).dSYM
	rm -rf bin
