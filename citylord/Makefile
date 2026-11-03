# Linux build against a raylib 5.5 checkout: make RAYLIB=/path/to/raylib/src
RAYLIB ?= ../raylib/src
CFLAGS ?= -O2 -Wall -Wextra
SRC = src/main.c src/citylord_data.c src/cl_assets.c src/cl_render.c src/cl_physics.c src/cl_aim.c

citylord: $(SRC) src/*.h
	$(CC) $(CFLAGS) -I$(RAYLIB) $(SRC) $(RAYLIB)/libraylib.a -lGL -lm -lpthread -ldl -lrt -lX11 -o $@

clean:
	rm -f citylord
