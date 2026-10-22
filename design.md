# BigFS Design

BigFS is a distributed file storage prototype written in C. It separates namespace metadata from block storage: the name server owns the file hierarchy and block map, while data servers store block bytes on local disk.

## Goals

- Store files larger than a single machine-friendly block size by splitting them into fixed-size blocks.
- Distribute blocks across multiple data servers.
- Provide a filesystem-like client interface: `cp`, `cat`, `ls`, `mv`, `rm`, `mkdir`, `cd`, and `pwd`.
- Use TCP for communication between client, name server, and data servers.
- Use event-driven server architecture with `select()` and nonblocking accepted sockets.
- Transfer file blocks in parallel from the client without creating unbounded threads.

## Architecture

```text
                         metadata requests
                    +------------------------+
                    |                        v
+--------+      +----------------+      +----------------+
| Client | ---> | FileNameServer | ---> | metadata store |
+--------+      +----------------+      +----------------+
    |
    | block PUT/GET/DEL requests
    v
+----------------+   +----------------+   +----------------+
| FileDataServer |   | FileDataServer |   | FileDataServer |
|    node 1      |   |    node 2      |   |    node 3      |
+----------------+   +----------------+   +----------------+
```

The client always contacts `FileNameServer` first. Namespace-only operations such as `ls`, `mkdir`, `cd`, and `mv` are handled by the name server. Data operations such as `cp`, `cat`, and `rm` use the name server to discover block placement, then contact data servers directly.

## Components

### Client

The client parses shell-like commands and normalizes BigFS paths. For file transfers, it asks the name server for block metadata, then communicates with data servers directly.

Block transfer uses a bounded pthread worker pool. The default maximum is 16 workers and can be configured with:

```sh
BIGFS_MAX_WORKERS=4 ./client
```

This preserves parallel block transfer while avoiding one thread per block for very large files.

### FileNameServer

The name server stores hierarchy and file metadata under `fhm.root/`. Each directory has a `.bigfs_meta` file containing:

- child directories
- file size
- block count
- data-server placement for each block

The server uses `select()` over the listening socket and accepted client sockets. Accepted sockets are nonblocking. Each client request is read, processed, and written back through the event loop.

Data-server addresses are configurable:

```sh
BIGFS_DATA_SERVERS="10.0.0.11:5000,10.0.0.12:5000,10.0.0.13:5000" ./ns
```

If unset, the default is:

```text
127.0.0.1:5000,127.0.0.1:6000,127.0.0.1:7000
```

### FileDataServer

Each data server stores blocks as files under `storage_<port>/`. It supports:

- `PUT <block-id> <size>`
- `GET <block-id>`
- `DEL <block-id>`

The data server also uses `select()` with nonblocking accepted sockets and per-client state:

- read command
- read PUT body
- write response or block data

## Block Placement

Files are split into `1 MB` blocks. For a file with `N` blocks, block `i` is placed on:

```text
data_server[i % data_server_count]
```

This gives simple round-robin distribution across configured storage nodes.

## Protocol Summary

Client to name server:

```text
MKDIR <path>
ISDIR <path>
LS <path>
ALLOC <path> <size>
LOOKUP <path>
RM <path>
MV <src> <dst>
```

Name server block map response:

```text
OK <file-size> <block-count>
BLOCK <ip> <port> <block-id> <block-size>
...
```

Client to data server:

```text
PUT <block-id> <size>
<raw bytes>

GET <block-id>

DEL <block-id>
```
