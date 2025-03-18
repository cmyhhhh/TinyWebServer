#include "webserver.h"

int main(int argc, char *argv[])
{
    WebServer server;
    server.init("config.ini");
    server.start();

    return 0;
}