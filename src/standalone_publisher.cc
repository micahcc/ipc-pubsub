#include <fcntl.h>     // For O_* constants
#include <sys/mman.h>  // for shm_open
#include <sys/socket.h>
#include <sys/stat.h>  // For mode constants
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cassert>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

struct OnReturn {
    OnReturn(std::function<void()> cb) : mCb(cb) {}
    ~OnReturn() { mCb(); }
    std::function<void()> mCb;
};

static std::string RandomString(size_t len) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    const size_t seed1 = std::chrono::system_clock::now().time_since_epoch().count();
    std::minstd_rand0 g1(seed1);

    std::string tmp(len, ' ');
    for (size_t i = 0; i < len; ++i) {
        tmp[i] = alphanum[g1() % (sizeof(alphanum) - 1)];
    }
    return tmp;
}

class Publisher {
   public:
    Publisher();
    bool Send(const std::string& meta, size_t len, const uint8_t* data);

    int mFd;
    std::mutex mMtx;
    std::vector<int> mClients;
    std::thread mAcceptThread;
};

Publisher::Publisher() {
    struct sockaddr_un addr;
    const std::string socket_path = "\0socket";
    assert(socket_path.size() + 1 < sizeof(addr.sun_path));

    if ((mFd = socket(AF_UNIX, SOCK_SEQPACKET, 0)) == -1) {
        perror("socket error");
        exit(-1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::copy(socket_path.c_str(), socket_path.c_str() + socket_path.size(), addr.sun_path);
    if (bind(mFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
        perror("bind error");
        exit(-1);
    }

    if (listen(mFd, 5) == -1) {
        perror("listen error");
        exit(-1);
    }

    mAcceptThread = std::thread([this]() {
        while (1) {
            int cl;
            if ((cl = accept(mFd, nullptr, nullptr)) == -1) {
                perror("accept error");
            } else {
                std::cerr << "Accepted" << std::endl;
                std::lock_guard<std::mutex> lk(mMtx);
                mClients.push_back(cl);
            }
        }
    });
}

bool Publisher::Send(const std::string& meta, size_t len, const uint8_t* data) {
    const std::string name = RandomString(8);

    // open and write data
    int shmFdWrite = shm_open(name.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (shmFdWrite == -1) {
        perror("Failed to create shm");
        return false;
    }

    // unlink when we leave this function, since it will have been shared by
    // then or failed
    OnReturn onRet1([&name, shmFdWrite]() {
        shm_unlink(name.c_str());
        close(shmFdWrite);
    });

    if (ssize_t written = write(shmFdWrite, data, len); written != ssize_t(len)) {
        perror("Failed to write to shm");
        return false;
    }

    // send read only version to other processes, close after we finish sending
    int shmFdSend = shm_open(name.c_str(), O_RDONLY, S_IRUSR);
    OnReturn onRet2([shmFdSend]() { close(shmFdSend); });

    // Construct header
    struct msghdr msgh;

    constexpr size_t BUFFLEN = 1 << 12;
    char metadata[BUFFLEN];
    if (meta.size() + 1 >= BUFFLEN) {
        std::cerr << "Metadata is too large" << std::endl;
        return false;
    }
    std::copy(meta.c_str(), meta.c_str() + 1 + meta.size(), metadata);

    // Vector of data to send, NOTE: we must always send at least one byte
    // This is the data that will be sent in the actual socket stream
    struct iovec iov;
    iov.iov_base = metadata;
    iov.iov_len = meta.size() + 1;

    // Don't need destination because we are using a connection
    msgh.msg_name = nullptr;
    msgh.msg_namelen = 0;
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;  // sending one message

    // Allocate a char array of suitable size to hold the ancillary data.
    // However, since this buffer is in reality a 'struct cmsghdr', use a
    // union to ensure that it is aligned as required for that structure.
    union {
        char buf[CMSG_SPACE(sizeof(shmFdSend))];
        /* Space large enough to hold an 'int' */
        struct cmsghdr align;
    } controlMsg;
    msgh.msg_control = controlMsg.buf;
    msgh.msg_controllen = sizeof(controlMsg.buf);

    // The control message buffer must be zero-initialized in order
    // for the CMSG_NXTHDR() macro to work correctly.
    memset(controlMsg.buf, 0, sizeof(controlMsg.buf));

    struct cmsghdr* cmsgp = CMSG_FIRSTHDR(&msgh);
    cmsgp->cmsg_len = CMSG_LEN(sizeof(shmFdSend));
    cmsgp->cmsg_level = SOL_SOCKET;
    cmsgp->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsgp), &shmFdSend, sizeof(shmFdSend));

    // Send
    for (const int client : mClients) {
        ssize_t ns = sendmsg(client, &msgh, 0);
        if (ns == -1) {
            std::cerr << "Failed to send to " << client << std::endl;
        }
    }
    return true;
}

int main(int argc, char** argv) {
    Publisher publisher;
    std::string msg = "hello";
    std::string meta = "txt";
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        publisher.Send(meta, msg.size() + 1, reinterpret_cast<const uint8_t*>(msg.c_str()));
    }
}
