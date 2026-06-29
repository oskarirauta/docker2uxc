all: docker2uxc

CXX?=g++
CXXFLAGS?=--std=c++17 -Wall -fPIC
LDFLAGS?=-L/lib -L/usr/lib

OBJS:= \
	objs/ref.o \
	objs/http.o \
	objs/sha256.o \
	objs/registry.o \
	objs/manifest.o \
	objs/work.o \
	objs/archive.o \
	objs/extract.o \
	objs/bundle.o \
	objs/reg.o \
	objs/dockerfile.o \
	objs/emit.o \
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

objs/http.o: src/http.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/sha256.o: src/sha256.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/registry.o: src/registry.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/manifest.o: src/manifest.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/work.o: src/work.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/archive.o: src/archive.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/extract.o: src/extract.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/bundle.o: src/bundle.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/reg.o: src/reg.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/dockerfile.o: src/dockerfile.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/emit.o: src/emit.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

# libraries AFTER the objects (--as-needed toolchains)
docker2uxc: $(LIBOBJS) $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@;

clean:
	rm -f objs/*.o docker2uxc

.PHONY: all clean
