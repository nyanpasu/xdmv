all:
	cc xdmv.c -lX11 -lXext -lXrandr -lm -lfftw3 -o xdmv

