#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <liburing.h>

namespace {

constexpr size_t MAX_LENGTH = 4096;
constexpr int URING_ENTRIES = 64;
constexpr int BACKLOG = 32;

enum class EventType {
    kListen,
    kPollin,
    kRead,
    kWrite,
};

struct QueueData {
    explicit QueueData(int fd, EventType type) : fd_(fd), type_(type), buf_({}) {
    }
    ~QueueData() {
        shutdown(fd_, SHUT_RDWR);
        delete[] static_cast<uint8_t *>(buf_.iov_base);
    }

    int fd_;
    EventType type_;
    iovec buf_;
};

} // namespace

int main(int argc, char *argv[]) {
    constexpr uint16_t DEFAULT_PORT = 42390;

    uint16_t port = DEFAULT_PORT;
    if (argc >= 2) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    printf("Listen %d port\n", port);

    int server_sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_sock == -1) {
        perror("socket");
        return 1;
    }

    const int val = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    int ret = bind(server_sock, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr));
    if (ret == -1) {
        perror("bind");
        return 1;
    }

    ret = listen(server_sock, BACKLOG);
    if (ret == -1) {
        perror("listen");
        return 1;
    }

    io_uring ring{};
    ret = io_uring_queue_init(URING_ENTRIES, &ring, 0);
    if (ret != 0) {
        fprintf(stderr, "Failed io_uring_queue_init: %s\n", strerror(-ret));
        return 1;
    }

    io_uring_sqe *first_sqe = io_uring_get_sqe(&ring);
    io_uring_prep_poll_add(first_sqe, server_sock, POLL_IN);

    QueueData listen_data{server_sock, EventType::kListen};
    io_uring_sqe_set_data(first_sqe, &listen_data);

    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    while (true) {
        io_uring_submit(&ring);

        io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret != 0) {
            fprintf(stderr, "Failed io_uring_wait_cqe: %s\n", strerror(-ret));
            return ret;
        }

        io_uring_cqe *cqes[BACKLOG];
        const unsigned int entries = io_uring_peek_batch_cqe(&ring, cqes, BACKLOG);
        for (size_t i = 0; i < entries; ++i) {
            auto *queue_data = static_cast<QueueData *>(io_uring_cqe_get_data(cqes[i]));
            switch (queue_data->type_) {
            case EventType::kListen: {
                io_uring_cqe_seen(&ring, cqes[i]);

                int client_sock;
                while ((client_sock = accept4(queue_data->fd_, reinterpret_cast<sockaddr *>(&client_addr), &client_len,
                                              SOCK_NONBLOCK | SOCK_CLOEXEC)) != -1) {
                    io_uring_sqe *pollin_sqe = io_uring_get_sqe(&ring);
                    auto *pollin_data = new QueueData(client_sock, EventType::kPollin);

                    pollin_data->buf_.iov_base = new uint8_t[MAX_LENGTH];
                    pollin_data->buf_.iov_len = MAX_LENGTH;

                    io_uring_prep_poll_add(pollin_sqe, client_sock, POLL_IN);
                    io_uring_sqe_set_data(pollin_sqe, pollin_data);
                }

                io_uring_sqe *listen_sqe = io_uring_get_sqe(&ring);
                io_uring_prep_poll_add(listen_sqe, server_sock, POLL_IN);
                io_uring_sqe_set_data(listen_sqe, &listen_data);
                break;
            }
            case EventType::kPollin: {
                auto *data = static_cast<QueueData *>(io_uring_cqe_get_data(cqes[i]));

                io_uring_cqe_seen(&ring, cqes[i]);

                io_uring_sqe *read_sqe = io_uring_get_sqe(&ring);
                io_uring_prep_readv(read_sqe, data->fd_, &data->buf_, 1, 0);

                data->type_ = EventType::kRead;
                io_uring_sqe_set_data(read_sqe, data);
                break;
            }
            case EventType::kRead: {
                auto *data = static_cast<QueueData *>(io_uring_cqe_get_data(cqes[i]));

                const int32_t bytes = cqes[i]->res;
                io_uring_cqe_seen(&ring, cqes[i]);
                if (bytes < 0) {
                    delete data;
                    break;
                }

                io_uring_sqe *write_sqe = io_uring_get_sqe(&ring);
                io_uring_prep_write(write_sqe, data->fd_, data->buf_.iov_base, bytes, 0);

                data->type_ = EventType::kWrite;
                io_uring_sqe_set_data(write_sqe, data);
                break;
            }
            case EventType::kWrite: {
                auto *data = static_cast<QueueData *>(io_uring_cqe_get_data(cqes[i]));
                io_uring_cqe_seen(&ring, cqes[i]);

                io_uring_sqe *pollin_sqe = io_uring_get_sqe(&ring);
                io_uring_prep_poll_add(pollin_sqe, data->fd_, POLL_IN);

                data->type_ = EventType::kPollin;
                io_uring_sqe_set_data(pollin_sqe, data);
                break;
            }
            default:
                assert(false); // never reach here
                break;
            }
        }
    }
}
