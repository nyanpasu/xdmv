all:
	cc -g xdmv.c -D_REENTRANT -lX11 -lXext -lXrandr -lm -lfftw3 -ljack -lpulse-simple -lpulse -lpthread -o xdmv

