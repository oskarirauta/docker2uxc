# Include this from a consumer (e.g. uxcd) to link the docker2uxc converter as a
# library and call docker2uxc::convert()/check_updates()/resolve_digest()
# directly (after fork()) instead of exec'ing the tool.
#
#   DOCKER2UXC_DIR := docker2uxcd
#   include docker2uxcd/Makefile.inc
#   ... add $(DOCKER2UXC_OBJS) to your link and -lcurl -lz -lzstd -llzma to LIBS
#
# Dependencies the CONSUMER must already provide (uxcd does): json_cpp (here via
# ubus_cpp/json), logger_cpp, common_cpp. usage_cpp is NOT needed (CLI-only).
# These are the converter's UNIQUE objects only (no main.o - that is the CLI).

DOCKER2UXC_DIR ?= ./docker2uxcd
INCLUDES += -I$(DOCKER2UXC_DIR)/src

DOCKER2UXC_OBJS := \
	objs/d2u_ref.o \
	objs/d2u_http.o \
	objs/d2u_sha256.o \
	objs/d2u_registry.o \
	objs/d2u_manifest.o \
	objs/d2u_work.o \
	objs/d2u_archive.o \
	objs/d2u_extract.o \
	objs/d2u_bundle.o \
	objs/d2u_reg.o \
	objs/d2u_dockerfile.o \
	objs/d2u_emit.o \
	objs/d2u_space.o \
	objs/d2u_convert.o

objs/d2u_ref.o: $(DOCKER2UXC_DIR)/src/ref.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_http.o: $(DOCKER2UXC_DIR)/src/http.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_sha256.o: $(DOCKER2UXC_DIR)/src/sha256.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_registry.o: $(DOCKER2UXC_DIR)/src/registry.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_manifest.o: $(DOCKER2UXC_DIR)/src/manifest.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_work.o: $(DOCKER2UXC_DIR)/src/work.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_archive.o: $(DOCKER2UXC_DIR)/src/archive.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_extract.o: $(DOCKER2UXC_DIR)/src/extract.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_bundle.o: $(DOCKER2UXC_DIR)/src/bundle.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_reg.o: $(DOCKER2UXC_DIR)/src/reg.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_dockerfile.o: $(DOCKER2UXC_DIR)/src/dockerfile.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_emit.o: $(DOCKER2UXC_DIR)/src/emit.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_space.o: $(DOCKER2UXC_DIR)/src/space.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;

objs/d2u_convert.o: $(DOCKER2UXC_DIR)/src/convert.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<;
