#include "ipc_pubsub/IPCNode.h"

#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace ips {
struct IPCHandlers {
    IPCNode::RawCallback rawCb;
};

struct IPCNeighbor {
    std::string name;
    uint64_t id;
    std::string path;

    std::unordered_set<std::string> subscriptions;

    struct sockaddr_un address;
};

void IPCNode::Publish(const std::string& topic, int64_t len, const uint8_t* data) {
    static thread_local MetadataMessage msg;
    msg.mutable_topic()->assign(topic.begin(), topic.end());
    msg.mutable_inlined()->assign(reinterpret_cast<const char*>(data), len);
    static thread_local std::string blob;
    msg.SerializeToString(&blob);

    std::scoped_lock<std::mutex> lk(mMtx);

    for (const auto& pair : mNodeById) {
        const IPCNeighbor& node = *pair.second;
        if (node.subscriptions.count(topic) == 0) {
            SPDLOG_INFO("Node {} has no subscriptions", node.id);
            continue;
        }
        if (node.path.empty()) {
            SPDLOG_INFO("Node ({}) without reception address is subscribed to {}", node.id, topic);
            continue;
        }

        // NOTE: even though we tell it sizeof(sockaddr_un) it will stop at the
        // first \0 that isn't the first character (it will obey c_str() ending)
        ssize_t sent = sendto(mOutFd, reinterpret_cast<const char*>(blob.data()), blob.size(), 0,
                              reinterpret_cast<const struct sockaddr*>(&node.address),
                              sizeof(struct sockaddr_un));
        if (sent < 0) {
            perror("sending datagram message");
        }
    }
}

void IPCNode::Unsubscribe(const std::string& topic) {
    // publish that we want the messages
    mTopologyManager->Unsubscribe(topic);
}

void IPCNode::Subscribe(const std::string& topic, RawCallback cb) {
    // publish that we want the messages
    mTopologyManager->Subscribe(topic);

    std::lock_guard<std::mutex> lk(mMtx);
    auto [it, inserted] = mSubscriptions.emplace(topic, nullptr);
    if (inserted) {
        it->second = std::make_unique<IPCHandlers>();
    }
    it->second->rawCb = cb;
}

void IPCNode::Announce(const std::string& topic, const std::string& mime) {
    mTopologyManager->Announce(topic, mime);
}

void IPCNode::Retract(const std::string& topic) { mTopologyManager->Retract(topic); }

void IPCNode::OnJoin(const ips::NodeChange& msg) {
    SPDLOG_ERROR("{}", msg.DebugString());

    std::lock_guard<std::mutex> lk(mMtx);

    auto [it, inserted] = mNodeById.emplace(msg.id(), nullptr);
    if (!inserted) {
        // TODO sansity check?
        SPDLOG_INFO("Updating node: {}", msg.id());
    } else {
        it->second = std::make_unique<IPCNeighbor>();
    }
    it->second->name = msg.name();
    it->second->id = msg.id();
    it->second->path = msg.address();

    memset(&it->second->address, 0, sizeof(struct sockaddr_un));
    it->second->address.sun_family = AF_UNIX;
    std::copy(msg.address().begin(), msg.address().end(), it->second->address.sun_path);
}

void IPCNode::OnLeave(const ips::NodeChange& msg) {
    SPDLOG_ERROR("{}", msg.DebugString());
    std::lock_guard<std::mutex> lk(mMtx);
    mNodeById.erase(msg.id());
}

void IPCNode::OnAnnounce(const ips::TopicChange& msg) {
    // doesn't really affect us
    SPDLOG_ERROR("{}", msg.DebugString());
}

void IPCNode::OnRetract(const ips::TopicChange& msg) {
    SPDLOG_ERROR("{}", msg.DebugString());
    // Doesn't really affect us
}

void IPCNode::OnSubscribe(const ips::TopicChange& msg) {
    SPDLOG_ERROR("{}", msg.DebugString());
    std::lock_guard<std::mutex> lk(mMtx);
    auto [it, inserted] = mNodeById.emplace(msg.node_id(), nullptr);
    if (inserted) {
        SPDLOG_ERROR("Node: {} subscribed, but hasn't joined the topology");

        // go ahead and add it, but we won't be able to send anything to it
        it->second = std::make_unique<IPCNeighbor>();
        it->second->id = msg.node_id();
    }

    // need to record so we know where to send datagrams to
    it->second->subscriptions.emplace(msg.name());
}

void IPCNode::OnUnsubscribe(const ips::TopicChange& msg) {
    std::lock_guard<std::mutex> lk(mMtx);
    SPDLOG_ERROR("{}", msg.DebugString());
    auto it = mNodeById.find(msg.node_id());
    if (it == mNodeById.end()) {
        // thats ok, just was going to remove anyway...
        return;
    }

    it->second->subscriptions.erase(msg.name());
}

std::shared_ptr<IPCNode> IPCNode::Create(const std::string& groupName,
                                         const std::string& nodeName) {
    std::random_device rd;
    std::mt19937_64 e2(rd());
    const uint64_t nodeId = e2();

    // create event reader for shutdown
    int shutdownFd = eventfd(0, EFD_SEMAPHORE);

    // Create socket to listen on
    std::ostringstream oss;
    oss << '\0' << std::hex << std::setw(16) << std::setfill('0') << e2();
    std::string dataPath = oss.str();

    // create socket to read from
    struct sockaddr_un name;

    /* Create Socket For Sending */
    int outSock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (outSock < 0) {
        perror("opening datagram socket");
        return nullptr;
    }

    /* Create socket from which to read. */
    int listenSock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (listenSock < 0) {
        perror("opening datagram socket");
        return nullptr;
    }

    /* Create name. */
    name.sun_family = AF_UNIX;
    std::copy(dataPath.begin(), dataPath.end(), name.sun_path);
    name.sun_path[dataPath.size()] = 0;

    /* Bind the UNIX domain address to the created socket */
    if (bind(listenSock, reinterpret_cast<struct sockaddr*>(&name), sizeof(struct sockaddr_un))) {
        perror("binding name to datagram socket");
        return nullptr;
    }

    return std::make_shared<IPCNode>(shutdownFd, listenSock, outSock, groupName, nodeName, nodeId,
                                     dataPath);
}

IPCNode::IPCNode(int shutdownFd, int listenSock, int outSock, const std::string& groupName,
                 const std::string& nodeName, uint64_t nodeId, const std::string& dataPath)
    : mShutdownFd(shutdownFd),
      mInputFd(listenSock),
      mOutFd(outSock),
      mGroupName(groupName),
      mNodeName(nodeName),
      mDataPath(dataPath) {
    auto onJoin = [this](const NodeChange& msg) { OnJoin(msg); };
    auto onLeave = [this](const NodeChange& msg) { OnLeave(msg); };
    auto onAnnounce = [this](const TopicChange& msg) { OnAnnounce(msg); };
    auto onRetract = [this](const TopicChange& msg) { OnRetract(msg); };
    auto onSubscribe = [this](const TopicChange& msg) { OnSubscribe(msg); };
    auto onUnsubscribe = [this](const TopicChange& msg) { OnUnsubscribe(msg); };
    mTopologyManager =
        std::make_shared<TopologyManager>(groupName, nodeName, nodeId, dataPath, onJoin, onLeave,
                                          onAnnounce, onRetract, onSubscribe, onUnsubscribe);

    mMainThread = std::thread([this]() { MainLoop(); });
}

void IPCNode::OnData(int64_t len, uint8_t* data) {
    static thread_local MetadataMessage msg;
    if (!msg.ParseFromArray(data, int(len))) {
        SPDLOG_ERROR("Failed to parse data of size {}", len);
        return;
    }

    IPCHandlers handlers;
    {
        std::scoped_lock<std::mutex> lk(mMtx);
        auto it = mSubscriptions.find(msg.topic());
        if (it == mSubscriptions.end()) return;
        handlers = *it->second;
    }

    if (!msg.inlined().empty()) {
        handlers.rawCb(msg.inlined().size(),
                       reinterpret_cast<const uint8_t*>(msg.inlined().data()));
    }

    // RawCallback rawCb;
    // ProtoCallback protoCb;
    //{
    //    std::scoped_lock<std::mutex> lk(mMtx);
    //    auto it = mTopics->find(msg.topic);
    //    if (it == mTopics.end()) return;
    //}
}

int IPCNode::MainLoop() {
    // Read from data loop
    struct pollfd fds[2];
    fds[0].fd = mShutdownFd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    fds[1].fd = mInputFd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    // now that we are connected field events from leader OR shutdown event
    // wait for it to close or shutdown event
    while (1) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            SPDLOG_ERROR("Failed to Poll: {}", strerror(errno));
            return -1;
        }

        if (fds[0].revents != 0) {
            SPDLOG_INFO("Polled shutdown");
            // shutdown event received, exit
            return 0;
        }

        if (fds[1].revents != 0) {
            if (fds[1].revents & POLLERR) {
                SPDLOG_ERROR("poll error");
                return -1;
            } else if (fds[1].revents & POLLNVAL) {
                SPDLOG_INFO("File descriptor not open");
                return -1;
            } else if (fds[1].revents & POLLIN) {
                // socket has data, read it
                uint8_t buffer[UINT16_MAX];
                int64_t nBytes = read(mInputFd, buffer, UINT16_MAX);
                if (nBytes < 0) {
                    SPDLOG_ERROR("Error reading: {}", strerror(errno));
                } else {
                    OnData(nBytes, buffer);
                }
            }
        }
    }
}

IPCNode::~IPCNode() {
    size_t payload = UINT32_MAX;
    write(mShutdownFd, &payload, sizeof(payload));
    mMainThread.join();

    close(mInputFd);
    close(mOutFd);
    close(mShutdownFd);
}
}  // namespace ips
