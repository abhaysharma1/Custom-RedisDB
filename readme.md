# Custom Redis

Custom Redis is a small Redis-like key-value store written in C++. It targets a POSIX environment such as Linux, macOS, or WSL. The project contains a single-threaded network server, a companion command-line client, and a custom hash table implementation with progressive rehashing.

## What is implemented

- A TCP server that listens on `127.0.0.1:1234`
- A minimal client that sends framed requests and prints framed responses
- An in-memory key-value database backed by a custom hash map
- Non-blocking socket handling with `poll()`
- Request pipelining, so multiple commands can be processed from one read
- Progressive rehashing so table growth is spread across operations instead of blocking all at once
- A small binary protocol for requests and responses
- Four database commands: `get`, `set`, `del`, and `keys`

## Project layout

- `server.cpp` - the Redis-like server, request parser, response encoder, and event loop
- `client.cpp` - a simple CLI client for sending commands to the server
- `hashtable.h` - intrusive hash table and hash map declarations
- `hashtable.cpp` - hash table implementation with progressive rehashing

## Server overview

The server is event-driven and single-threaded. It opens a listening socket, enables `SO_REUSEADDR`, switches sockets to non-blocking mode, and uses `poll()` to manage the listening socket plus all active client connections.

Each connection tracks:

- `want_read` - whether the server should watch for incoming data
- `want_write` - whether there is buffered output waiting to be written
- `want_close` - whether the connection should be closed after processing
- `incoming` - buffered request bytes that have been read but not yet parsed
- `outgoing` - buffered response bytes waiting to be written

The server supports request pipelining: if a single read contains multiple complete requests, it will process them one after another before returning to the event loop.

## Wire protocol

The project uses a custom binary protocol instead of text commands.

### Request format

Every request is framed as:

1. `u32` total payload length
2. `u32` number of strings in the command
3. Repeated command arguments:
   - `u32` string length
   - raw string bytes

Example shape:

```text
[len][argc][arg1_len][arg1][arg2_len][arg2]...
```

### Response format

Every response starts with a length prefix, followed by a tagged payload.

Supported tags:

- `0` - nil
- `1` - error
- `2` - string
- `3` - int64
- `4` - double
- `5` - array

Error responses include an error code and message. Array responses include the number of elements, followed by each serialized element.

## Supported commands

The database currently understands four commands:

### `get key`

Looks up a key in the in-memory database.

- Returns the stored string value when the key exists
- Returns nil when the key does not exist

### `set key value`

Stores or overwrites a string value for a key.

- Creates a new entry if the key does not exist
- Overwrites the current value if the key already exists
- Returns nil

### `del key`

Deletes a key if it exists.

- Returns `1` if a key was removed
- Returns `0` if the key was not found

### `keys`

Returns all keys currently stored in the database as an array.

## Hash table implementation

The database uses a custom intrusive hash map:

- `HNode` is embedded directly inside each stored entry
- Collisions are handled with linked-list chaining
- Slot selection uses bit masking with power-of-two table sizes
- Hash map growth is handled through progressive rehashing

Progressive rehashing is the main implementation detail here. When the table grows, the old table is not migrated in one blocking pass. Instead, each database operation moves a small amount of data forward, which keeps the server responsive during resizing.

The current implementation uses:

- an initial table size of 4 slots
- a maximum load factor of 8
- a migration budget of 128 nodes per help step

## Storage model

The database is fully in-memory and stores string keys and string values only.

Entries are represented by:

- an intrusive hash node
- the key string
- the value string

Because the data lives only in memory, all contents are lost when the server stops.

## Client overview

`client.cpp` is a small command-line companion that:

- connects to the server
- serializes command-line arguments into the custom request format
- reads a framed response
- prints the response based on its tag

It is intended as a lightweight way to exercise the server protocol from the terminal.

## Build

There is no build system checked in, so the project can be compiled directly with a C++ compiler in a POSIX shell.

Example commands:

```bash
g++ -std=c++17 -O2 -Wall -Wextra server.cpp hashtable.cpp -o server
g++ -std=c++17 -O2 -Wall -Wextra client.cpp -o client
```

## Run

Start the server first:

```bash
./server
```

Then run the client with a command such as:

```bash
./client set mykey hello
./client get mykey
./client keys
./client del mykey
```

## Limitations

- Data is not persisted to disk
- The server is single-threaded
- Only `get`, `set`, `del`, and `keys` are implemented
- There is no authentication or access control
- Values are plain strings only; there are no Redis-style native types
- The protocol is custom, so standard Redis clients cannot talk to this server
- Message sizes are bounded by the server and client limits in the source code

## Summary

This project is best viewed as an educational Redis-style implementation. It demonstrates socket programming, incremental rehashing, request framing, and a tiny command dispatcher while keeping the codebase intentionally small.