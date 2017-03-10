CXX=g++
CXXFLAGS=-O2 -Wall
LIBS=-lboost_context


CORONET_DIR=coronet
CORONET=$(wildcard ${CORONET_DIR}/*hpp)
CORONET_EXAMPLES=echo stupid_http

all: coronet_examples

coronet_examples: $(patsubst %,build/%,${CORONET_EXAMPLES})


build/%: examples/%.cpp
	${CXX} ${CFLAGS} -I${CORONET_DIR} ${LIBS} $< -o $@
