CXX = g++
CUSTOM_SRC   = $(wildcard hardcodedcode.cpp)
CUSTOM_OBJ   = $(CUSTOM_SRC:.cpp=.o)
CUSTOM_FLAGS = $(if $(CUSTOM_SRC),-DHAS_HARDCODEDCODE,)

CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -MMD -MP $(CUSTOM_FLAGS) $(shell pkg-config --cflags libheif)
LDFLAGS = -lpthread -lsqlite3 $(shell pkg-config --libs libheif)

all: rfiledrive

rfiledrive: main.o mainfunctions.o liveedit.o sqlite_db.o $(CUSTOM_OBJ)
	$(CXX) main.o mainfunctions.o liveedit.o sqlite_db.o $(CUSTOM_OBJ) -o rfiledrive $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(wildcard *.d)

clean:
	rm -f *.o *.d rfiledrive fileserver server
