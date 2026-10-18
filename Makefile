CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -MMD -MP $(shell pkg-config --cflags libheif)
LDFLAGS = -lpthread $(shell pkg-config --libs libheif)

all: rfiledrive

rfiledrive: main.o mainfunctions.o
	$(CXX) main.o mainfunctions.o -o rfiledrive $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(wildcard *.d)

clean:
	rm -f *.o *.d rfiledrive fileserver server
