# Build test link modules: link/*.cpp -> link/*.so, link/base/*.cpp -> link/base.so
CXX ?= g++
CFLAGS = -std=c++17 -Wall -O2 -march=native -I../../lib/alxlib/include/alxscpt -I../../lib/alxlib/include/alxbase -I../../lib/alxlib/include/alxcore -I../../lib/alxlib/include/alxcomm -MMD -MP
LDFLAGS = -L../../bin/alxlib -lalxcore -lalxbase -Wl,-rpath,'$$ORIGIN/../../../bin/alxlib'
BINDIR = link

LINK_SRCS = $(wildcard $(BINDIR)/*.cpp)
LINK_SOS  = $(patsubst $(BINDIR)/%.cpp,$(BINDIR)/%.so,$(LINK_SRCS))
BASE_SO   = $(BINDIR)/base.so
BASE_SRCS = $(wildcard $(BINDIR)/base/*.cpp)

all: $(LINK_SOS) $(BASE_SO)

# -MMD -MP emits .d dep files: header changes (e.g. libalexis bump) rebuild the .so
$(BINDIR)/%.so: $(BINDIR)/%.cpp
	$(CXX) $(CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ $<

$(BASE_SO): $(BASE_SRCS)
	$(CXX) $(CFLAGS) -fPIC -shared $(LDFLAGS) -o $@ $(BASE_SRCS)

# pull generated dependency files (if any)
-include $(wildcard $(BINDIR)/*.d $(BINDIR)/base/*.d)

clean:
	rm -f $(BINDIR)/*.so $(BINDIR)/*.d $(BINDIR)/base/*.d
