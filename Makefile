all:
	cc -g xdmv.c -lX11 -lXext -lXrandr -lm -lfftw3 -ljack -o xdmv

