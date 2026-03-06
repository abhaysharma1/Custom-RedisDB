#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
// system
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
// C++
#include <vector>

static void msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg)
{
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

// set connection to non-blocking
static void fd_set_nb(int fd)
{
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if (errno)
    {
        die("fcntl error");
        return;
    }

    // append flags to include the non blocking flag
    // flags is a bit map
    // |= is OR operator
    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if (errno)
    {
        die("fcntl error");
    }
}

const size_t k_max_msg = 32 << 2;

// maintain the conneciton details using a struct
struct Conn
{
    int fd = -1;
    // Current Intention of the Connection
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    // Incoming and outgoing buffer
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> outgoing;
};

// handle incoming connection on the listening fd
static Conn *handle_accept(int fd)
{
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);

    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0)
    {
        msg_errno("accept() error");
        return NULL;
    }

    // logging the ip
    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
            ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
            ntohs(client_addr.sin_port));

    // set new connection to non-blocking
    fd_set_nb(connfd);
    
    // create a connection object and return it
    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;
}

// append to the back
static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len)
{
    buf.insert(buf.end(), data, data + len);
}
// remove from the front
static void buf_consume(std::vector<uint8_t> &buf, size_t n)
{
    buf.erase(buf.begin(), buf.begin() + n);
}

//drains the incoming data validates it and put it into outgoing buffer
static bool try_one_request(Conn *conn)
{
    
    if (conn->incoming.size() < 4)
    {
        return false;
    }

    uint32_t len = 0;
    memccpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg)
    {
        msg("too long");
        conn->want_close = true;
        return false; // want read
    }

    if (4 + len > conn->incoming.size())
    {
        return false; // want read
    }

    const uint8_t *request = &conn->incoming[4];

    // got one request, do some application logic
    printf("client says: len:%d data:%.*s\n",
           len, len < 100 ? len : 100, request);

    // add to outgoing
    buf_append(conn->outgoing, (const uint8_t *)&len, 4);
    buf_append(conn->outgoing, request, len);

    // remove from incoming
    buf_consume(conn->incoming, 4 + len);

    return true;
}

static void handle_write(Conn *conn)
{
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());
    if (rv < 0 && errno == EAGAIN)
    {
        // not ready
        return;
    }
    if (rv < 0)
    {
        msg_errno("write() error");
        conn->want_close = true;
        close;
    }

    buf_consume(conn->outgoing, (size_t)rv);

    if (conn->outgoing.size() == 0)
    {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn *conn)
{
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if (rv < 0 && errno == EAGAIN)
    {
        return;
    }
    if (rv < 0)
    {
        msg_errno("read() error");
        conn->want_close = true;
        return;
    }

    if (rv == 0)
    {
        if (conn->incoming.size() == 0)
        {
            msg("client closed");
        }
        else
        {
            msg("unexpected EOF");
        }
        conn->want_close = true;
        return;
    }

    buf_append(conn->incoming, buf, (size_t)rv);

    while (try_one_request(conn))
    {
    }

    if (conn->outgoing.size() > 0)
    {
        conn->want_read = false;
        conn->want_write = true;

        return handle_write(conn);
    }
}

int main()
{
    // take file descriptior from the system
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)); // allows resuing of the same ports when server restart without TCP time wait

    struct sockaddr_in addr = {};                                    // init address component
    addr.sin_family = AF_INET;                                       // setting familt
    addr.sin_port = htons(1234);                                     // setting port
    addr.sin_addr.s_addr = htonl(0);                                 // setting address as local
    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr)); // binding address,port to fd
    if (rv)
    {
        die("bind()");
    }

    fd_set_nb(fd);

    // listen
    rv = listen(fd, SOMAXCONN); // starting server
    if (rv)
    {
        die("listen()");
    }

    // fd to connection map
    std::vector<Conn *> fd2conn;
    std::vector<struct pollfd> poll_args;

    while (true) // infinte server listening
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
        for (Conn *conn : fd2conn)
        {
            if (!conn)
            {
                continue;
            }
            // return a err if there
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            // if we want to read from the connection flip the POLLIN bit
            if (conn->want_read)
            {
                pdf.events |= POLLIN;
            }
            //if we want to write to it flip the POLLOUT bit
            if (conn->want_write)
            {
                pfd.events |= POLLOUT;
            }
            // pust the created pollarg into the array
            poll_args.push_back(pfd);
        }
        // now poll all the present poll_args
        // the poll checks all the events we want to perform(bitmap of operations given by us)
        // and tell if we can perform them using revents(bitmap of the operations the fd is ready for)
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (rv < 0 && errno == EINTR)
        {
            // Process was interrupted by another syscall
            continue;
        }
        if (rv < 0)
        {
            // error
            die("poll")
        }

        // handle the listening socket
        // if the kernel tell us that there is POLLIN is avaliable on the listening fd
        if (poll_args[0].revents)
        {   
            // accept that connection create a fd and a connection and map them to each other
            if (Conn *conn = handle_accept(fd))
            {
                if (fd2conn.size() <= (size_t)conn->fd)
                {
                    fd2conn.resize(conn->fd + 1);
                }
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        // handle the connection sockets
        // skipping the listening socket at 0th index
        for (size_t i = 1; i < poll_args.size(); ++i)
        {
            uint32_t ready = poll_args[i].revents;
            if (ready == 0)
            {
                continue;
            }
            // get the connection based on the fd
            Conn *conn = fd2conn[poll_args[i].fd];
            if (ready & POLLIN)
            {
                assert(conn->want_read);
                handle_read(conn);
            }
            if (ready & POLLOUT)
            {
                assert(conn->want_write);
                handle_write(conn);
            }

            if ((ready & POLLERR) || conn->want_close)
            {
                (void)close(conn->fd);
                fd2conn[conn->fd] = null;
                delete conn;
            }
        }
    }
    return 0;
}
