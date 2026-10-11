// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Anderson S.
// A free TCP port on 127.0.0.1 for a test's debug listener: two runs at once (make test and make asan) must not fight over a fixed number.
#ifndef TESTUTIL_H
#define TESTUTIL_H
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int free_port(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};   // port 0: the kernel picks one
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    socklen_t len = sizeof a;
    int port = -1;
    if (bind(s, (struct sockaddr*)&a, sizeof a) == 0 && getsockname(s, (struct sockaddr*)&a, &len) == 0) port = ntohs(a.sin_port);
    close(s);
    return port;
}
#endif
