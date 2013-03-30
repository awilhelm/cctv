main:
main.o: makefile
CPPFLAGS = -Wall -Wextra -Werror
CXXFLAGS = -Wconversion -O3 -g3 -march=athlon64 -m32 -mfpmath=both
LDLIBS = -lavcodec -lavformat -lpng -lboost_thread -lfftw3f
