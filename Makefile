CXX=g++
CXXFLAGS=-O2 -Wall -g
LIBS=-lboost_context


CORONET_DIR=coronet
CORONET=$(wildcard ${CORONET_DIR}/*hpp)
CORONET_EXAMPLES=echo stupid_http stupid_http_keepalive

all: coronet_examples

coronet_examples: $(patsubst %,build/%,${CORONET_EXAMPLES})

build/%: examples/%.cpp ${CORONET}
	${CXX} ${CXXFLAGS} -I${CORONET_DIR} ${LIBS} $< -o $@
