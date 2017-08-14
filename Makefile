CC           =clang
#CFLAGS       =
FFMPEG_FLAGS =-lavutil -lavformat -lavcodec -lavutil -lswscale -lswresample
SRC          =muxer.c
BIN          =segmenter

.PHONY: all
all:
	$(CC) $(FFMPEG_FLAGS) $(SRC) -o $(BIN) -g 

.PHONY: clean
clean:
	rm -f $(BIN)
	rm -rf $(BIN).dSYM
