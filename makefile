CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2
	CXXFLAGS += -std=c++11
	CXXFLAGS += -DNDEBUG

endif

server: main.cpp  ./timer/lst_timer.cpp ./http/HttpConnection.cpp ./log/log.cpp ./mysql/mysqlConnectPool.cpp  webserver.cpp config.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient 

clean:
	rm  -r server
