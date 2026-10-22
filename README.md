# BigFS

BigFS is a C-based distributed file storage prototype. It stores file metadata in a name server and stores file blocks on multiple data servers.

## Features

- Splits files into `1 MB` blocks.
- Places blocks round-robin across configured data servers.
- Maintains directory and file metadata in `FileNameServer`.
- Stores physical block files in `FileDataServer` local storage.
- Uses TCP for all client/server and server/server communication.
- Uses `select()` with nonblocking accepted sockets in both servers.
- Uses client-side pthreads for parallel block upload/download.
- Supports recursive local directory copy into BigFS and recursive BigFS copy back to local storage.

## Build

```sh
make
```

## Run Locally

Start three data servers:

```sh
./ds 5000
./ds 6000
./ds 7000
```

Start the name server:

```sh
./ns
```

Start the client:

```sh
./client
```

## Configure Data Servers

By default the name server uses:

```text
127.0.0.1:5000,127.0.0.1:6000,127.0.0.1:7000
```

To run data servers on different hosts, set `BIGFS_DATA_SERVERS` before starting `ns`:

```sh
BIGFS_DATA_SERVERS="10.0.0.11:5000,10.0.0.12:5000,10.0.0.13:5000" ./ns
```

## Client Commands

```text
upload <local-file-or-dir> [bigfs-path]
cp local:<local-file-or-dir> <bigfs-path>
cp <bigfs-path> local:<local-path>
cp <bigfs-src> <bigfs-dst>
cat <bigfs-file>
ls [bigfs-dir]
cd <bigfs-dir>
mkdir <bigfs-dir>
rm <bigfs-file-or-dir>
mv <bigfs-src> <bigfs-dst>
pwd
exit
```

Example:

```text
mkdir docs
cp local:bigfile.txt docs/bigfile.txt
ls docs
cat docs/bigfile.txt
cp docs/bigfile.txt local:copy.txt
```
