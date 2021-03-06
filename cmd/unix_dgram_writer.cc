#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

/*
 * Send a datagram to a receiver whose name is specified in the command
 * line arguments.  The form of the command line is <programname> <pathname>
 */

static int streamversion() {
    const char* socket_path = "socket";
    struct sockaddr_un addr;
    char buf[100];
    int64_t fd, rc;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket error");
        exit(-1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (*socket_path == '\0') {
        *addr.sun_path = '\0';
        strncpy(addr.sun_path + 1, socket_path + 1, sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    }

    if (connect(int(fd), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
        perror("connect error");
        exit(-1);
    }

    while ((rc = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        if (write(int(fd), buf, rc) != rc) {
            if (rc > 0)
                fprintf(stderr, "partial write");
            else {
                perror("write error");
                exit(-1);
            }
        }
    }

    return 0;
}

static int sequence_version() {
    const char* socket_path = "socket";
    struct sockaddr_un addr;
    char buf[100];
    int64_t fd, rc, bts;

    if ((fd = socket(AF_UNIX, SOCK_SEQPACKET, 0)) == -1) {
        perror("socket error");
        exit(-1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (*socket_path == '\0') {
        *addr.sun_path = '\0';
        strncpy(addr.sun_path + 1, socket_path + 1, sizeof(addr.sun_path) - 2);
    } else {
        strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    }

    if (connect(int(fd), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
        perror("connect error");
        exit(-1);
    }

    while ((rc = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        std::cout << "read " << rc << " bytes: ";
        for (int i = 0; i < rc; ++i) std::cout << std::hex << std::setw(2) << buf[i];
        std::cout << std::endl;
        if ((bts = write(int(fd), buf, rc)) != rc) {
            // if ((bts = sendmsg(fd, &msg, 0)) != rc) {
            if (bts < 0) {
                perror("Error");
            }
            if (rc > 0)
                std::cerr << "partial write: " << bts << std::endl;
            else {
                perror("write error");
                exit(-1);
            }
        }
    }

    return 0;
}

static int datagramversion() {
    const std::string NAME = "socket";
    const std::string DATA = "The sea is calm tonight, the tide is full . . .";
    int sock;
    struct sockaddr_un name;

    /* Create socket on which to send. */
    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("opening datagram socket");
        exit(1);
    }
    /* Construct name of socket to send to. */
    name.sun_family = AF_UNIX;
    strcpy(name.sun_path, NAME.c_str());
    /* Send message. */
    for (size_t i = 0; i < 5; ++i) {
        if (sendto(sock, DATA.c_str(), DATA.size() + 1, 0,
                   reinterpret_cast<const struct sockaddr*>(&name),
                   sizeof(struct sockaddr_un)) < 0) {
            perror("sending datagram message");
        }

        sleep(1);
    }
    close(sock);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 1 || strcmp(argv[1], "stream") == 0) {
        return streamversion();
    } else if (strcmp(argv[1], "datagram") == 0) {
        return datagramversion();
    } else if (strcmp(argv[1], "sequence") == 0) {
        return sequence_version();
    }
}
