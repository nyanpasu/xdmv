CFLAGS	= -Wall -Werror -D_REENTRANT
LDLIBS	= -lX11 -lXext -lXrandr -lm -lfftw3 -ljack -lpulse-simple -lpulse -lpthread

all: xdmv

clean:
	-rm xdmv
