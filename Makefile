all: docker2uxc

CXX?=g++
CXXFLAGS?=--std=c++17 -Wall -fPIC
LDFLAGS?=-L/lib -L/usr/lib

OBJS:= \
	objs/ref.o \
	objs/main.o

# dependency trees: json_cpp; logger_cpp -> common_cpp -> rva/tsl; usage_cpp
JSON_DIR:=json_cpp
COMMON_DIR:=common_cpp
LOGGER_DIR:=logger_cpp
USAGECPP_DIR:=usage_cpp

include json_cpp/Makefile.inc
include common_cpp/Makefile.inc
include logger_cpp/Makefile.inc
include usage_cpp/Makefile.inc

INCLUDES += -Isrc

LIBOBJS:= $(JSON_OBJS) $(COMMON_OBJS) $(LOGGER_OBJS) $(USAGE_OBJS)
LIBS:= -lcurl -lz -lzstd -llzma

$(shell mkdir -p objs)

objs/main.o: src/main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/ref.o: src/ref.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

# libraries AFTER the objects (--as-needed toolchains)
docker2uxc: $(LIBOBJS) $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@;

clean:
	rm -f objs/*.o docker2uxc

.PHONY: all clean
