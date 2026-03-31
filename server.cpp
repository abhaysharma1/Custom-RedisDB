// stdlib
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// system
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/ip.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
// C++
#include <map>
#include <string>
#include <vector>
// proj
#include "hashtable.h"

#define container_of(ptr, T, member) ((T*)((char*)ptr - offsetof(T, member)))

static void msg(const char* msg) { fprintf(stderr, "%s\n", msg); }

static void die(const char* msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

// set connection to non-blocking
static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno) {
        die("fcntl error");
        return;
    }

    // append flags to include the non blocking flag
    // flags is a bit map
    // |= is OR operator
    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno) {
        die("fcntl error");
    }
}

const size_t k_max_msg = 32 << 2;

// maintain the conneciton details using a struct
struct Conn {
    int fd = -1;
    // Current Intention of the Connection
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    // Incoming and outgoing buffer
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

// append to the back
static void buf_append(std::vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}
// remove from the front
static void buf_consume(std::vector<uint8_t>& buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

// handle incoming connection on the listening fd
static Conn* handle_accept(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);

    int connfd = accept(fd, (struct sockaddr*)&client_addr, &addrlen);
    if (connfd < 0) {
        msg("accept() error");
        return NULL;
    }

    // logging the ip
    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255,
            ip >> 24, ntohs(client_addr.sin_port));

    // set new connection to non-blocking
    fd_set_nb(connfd);

    // create a connection object and return it
    Conn* conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;
}

const size_t k_max_args = 200 * 1000;

static bool read_u32(const uint8_t*& curr, const uint8_t* end, uint32_t& out) {
    if (curr + 4 > end) {
        return false;
    }
    memcpy(&out, curr, 4);
    curr += 4;
    return true;
}

static bool read_str(const uint8_t*& cur, const uint8_t* end, size_t n, std::string& out) {
    if (cur + n > end) {
        return false;
    }
    // edit the one empty string that we pushed before
    // instead of forming a temporary one and pushing it
    // saves memory
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

static int32_t parse_req(const uint8_t* data, size_t size, std::vector<std::string>& out) {
    // The request is in this format
    // │nstr│len│str1│len│str2│...│len│strn│
    //  nstr = no of strings
    //  len = length of corresponding string

    const uint8_t* end = data + size;
    uint32_t nstr = 0;  // no of strings in the request

    // find nstr
    if (!read_u32(data, end, nstr)) {
        return -1;
    }

    if (nstr > k_max_msg) {
        return -1;
    }
    // destructure the request
    while (out.size() < nstr) {
        uint32_t len = 0;
        // find length of current string
        if (!read_u32(data, end, len)) {
            return -1;
        }
        // push an empty string
        out.push_back(std::string());
        // read that string from the data put it into the empty one
        if (!read_str(data, end, len, out.back())) {
            return -1;
        }
    }
    if (data != end) {
        return -1;
    }
    return 0;
}

enum {
    RES_OK = 0,
    RES_NX = 1,
    RES_ERR = 2,
};

struct Response {
    uint32_t status = 0;
    std::vector<uint8_t> data;
};

static struct {
    HMap db;
} g_data;

struct Entry {
    struct HNode node;
    std::string key;
    std::string val;
};

// equality comparison for two Nodes
static bool entry_eq(HNode* lhs, HNode* rhs) {
    struct Entry* le = container_of(lhs, struct Entry, node);
    struct Entry* re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}

// FNV Hash
static uint64_t str_hash(const uint8_t* data, size_t len) {
    uint32_t h = 0x811C9DC5;
    for (size_t i = 0; i < len; i++) {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

static void do_get(std::vector<std::string>& cmd, Response& out) {
    // a dummy entry for the lookup
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());
    // hashtable lookup
    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) {
        out.status = RES_NX;
        return;
    }
    // copy the value
    const std::string& val = container_of(node, Entry, node)->val;
    assert(val.size() <= k_max_msg);
    out.data.assign(val.begin(), val.end());
}

static void do_set(std::vector<std::string>& cmd, Response&) {
    // a dummy entry for the lookup
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());
    // hashtable lookup
    HNode* node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        // if found update the key
        container_of(node, Entry, node)->val.swap(cmd[2]);
    } else {
        // if not found insert
        Entry* ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->val.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }
}

static void do_del(std::vector<std::string>& cmd, Response&) {
    // dummy entry for lookup
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t*)key.key.data(), key.key.size());
    // hashtable delete
    HNode* node = hm_delete(&g_data.db, &key.node, &entry_eq);
    if (node) {
        delete container_of(node, Entry, node);
    }
}

static void do_request(std::vector<std::string>& cmd, Response& out) {
    if (cmd.size() == 2 && cmd[0] == "get") {
        return do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        return do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        return do_del(cmd, out);
    } else {
        out.status = RES_ERR;  // unrecognized command
    }
}

static void make_response(const Response& resp, std::vector<uint8_t>& out) {
    uint32_t resp_len = 4 + (uint32_t)resp.data.size();
    buf_append(out, (const uint8_t*)&resp_len, 4);
    buf_append(out, (const uint8_t*)&resp.status, 4);
    buf_append(out, resp.data.data(), resp.data.size());
}

// drains the incoming data validates it and put it into outgoing buffer
static bool try_one_request(Conn* conn) {
    if (conn->incoming.size() < 4) {
        return false;
    }

    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;  // want read
    }

    if (4 + len > conn->incoming.size()) {
        return false;  // want read
    }

    const uint8_t* request = &conn->incoming[4];

    // Process the Request
    //  3 steps to handle a request:
    //  1. Parse the command.
    //  2. Process the command and generate a response.
    //  3. Append the response to the output buffer.

    std::vector<std::string> cmd;
    if (parse_req(request, len, cmd) < 0) {
        conn->want_close = true;
        return false;
    }
    Response resp;
    // form the response
    do_request(cmd, resp);
    make_response(resp, conn->outgoing);

    // got one request, do some application logic
    printf("client says: len:%d data:%.*s\n", len, len < 100 ? len : 100, request);

    // add to outgoing
    buf_append(conn->outgoing, (const uint8_t*)&len, 4);
    buf_append(conn->outgoing, request, len);

    // remove from incoming
    buf_consume(conn->incoming, 4 + len);

    return true;
}

static void handle_write(Conn* conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());
    if (rv < 0 && errno == EAGAIN) {
        // not ready
        return;
    }
    if (rv < 0) {
        msg("write() error");
        conn->want_close = true;
        close;
    }

    buf_consume(conn->outgoing, (size_t)rv);

    if (conn->outgoing.size() == 0) {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn* conn) {
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if (rv < 0 && errno == EAGAIN) {
        return;
    }
    if (rv < 0) {
        msg("read() error");
        conn->want_close = true;
        return;
    }

    if (rv == 0) {
        if (conn->incoming.size() == 0) {
            msg("client closed");
        } else {
            msg("unexpected EOF");
        }
        conn->want_close = true;
        return;
    }

    buf_append(conn->incoming, buf, (size_t)rv);

    // a single read can contain multiple requests
    // this here processes that entire read into multiple requests until buffer is empty
    // this is the implementation of Pipelining
    // Never assume: 1 read = 1 request
    while (try_one_request(conn)) {
    }

    if (conn->outgoing.size() > 0) {
        conn->want_read = false;
        conn->want_write = true;

        return handle_write(conn);
    }
}

int main() {
    // take file descriptior from the system
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    int val = 1;
    setsockopt(
        fd, SOL_SOCKET, SO_REUSEADDR, &val,
        sizeof(val));  // allows resuing of the same ports when server restart without TCP time wait

    struct sockaddr_in addr = {};                                    // init address component
    addr.sin_family = AF_INET;                                       // setting familt
    addr.sin_port = htons(1234);                                     // setting port
    addr.sin_addr.s_addr = htonl(0);                                 // setting address as local
    int rv = bind(fd, (const struct sockaddr*)&addr, sizeof(addr));  // binding address,port to fd
    if (rv) {
        die("bind()");
    }

    fd_set_nb(fd);  // set listening server as non blocking

    // listen
    rv = listen(fd, SOMAXCONN);  // starting server
    if (rv) {
        die("listen()");
    }

    // fd to connection map
    std::vector<Conn*> fd2conn;
    std::vector<struct pollfd> poll_args;

    while (true)  // infinte server listening
    {
        // poll args are the operations on fd that we want to perform on each loop
        poll_args.clear();
        // the first operation that we want to be is listen on listening fd
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        // for each connection present create a poll_arg
        // a pollarg.event is a bitmap for telling the kernel to check whether the fd is ready for
        // us to perform certain task or not
        // the pollarg.revents tells us if it is ready to perform the tasks we specified
        for (Conn* conn : fd2conn) {
            if (!conn) {
                continue;
            }
            // return a err if there
            struct pollfd pfd = {conn->fd, POLLERR, 0};

            // if we want to read from the connection flip the POLLIN bit
            if (conn->want_read) {
                // pfd.events is a bitmap
                pfd.events |= POLLIN;
            }
            // if we want to write to it flip the POLLOUT bit
            if (conn->want_write) {
                pfd.events |= POLLOUT;
            }
            // put the created pollarg into the array
            poll_args.push_back(pfd);
        }

        // now poll all the present poll_args
        // the poll checks all the events we want to perform(bitmap of operations given by us)
        // and tell if we can perform them using revents(bitmap of the operations the fd is ready
        // for)
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (rv < 0 && errno == EINTR) {
            // Process was interrupted by another syscall
            continue;
        }
        if (rv < 0) {
            // error
            die("poll");
        }

        // handle the listening socket
        // if the kernel tell us that there is POLLIN is avaliable on the listening fd
        if (poll_args[0].revents) {
            // accept that connection create a fd and a connection and map them to each other
            if (Conn* conn = handle_accept(fd)) {
                if (fd2conn.size() <= (size_t)conn->fd) {
                    fd2conn.resize(conn->fd + 1);
                }
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        // handle the connection sockets
        // skipping the listening socket at 0th index
        for (size_t i = 1; i < poll_args.size(); ++i) {
            uint32_t ready = poll_args[i].revents;
            if (ready == 0) {
                continue;
            }
            // get the connection based on the fd
            Conn* conn = fd2conn[poll_args[i].fd];
            if (ready & POLLIN) {
                assert(conn->want_read);
                // server reads the data from the client
                handle_read(conn);
            }
            if (ready & POLLOUT) {
                assert(conn->want_write);
                handle_write(conn);
            }

            if ((ready & POLLERR) || conn->want_close) {
                (void)close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
            }
        }
    }
    return 0;
}
