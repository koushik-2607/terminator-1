# ==============================================================================
# Terminator: A Custom Terminal Emulator and Shell
# Computing Lab (CS69201) · Term Project
# Makefile
# ==============================================================================

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I./src
LDFLAGS = -lX11 -lpthread -lrt

TARGET = terminator

SRCS = src/main.cpp \
       src/history.cpp \
       src/config.cpp \
       src/jobs.cpp \
       src/autocomplete.cpp \
       src/parser.cpp \
       src/multiwatch.cpp \
       src/shell.cpp \
       src/tab.cpp \
       src/gui.cpp

OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) test_runner

test: $(OBJS)
	$(CXX) $(CXXFLAGS) tests/comprehensive_tests.cpp $(filter-out src/main.o src/gui.o,$(OBJS)) -o test_runner $(LDFLAGS)
	./test_runner
	rm -f test_runner

.PHONY: all clean test

