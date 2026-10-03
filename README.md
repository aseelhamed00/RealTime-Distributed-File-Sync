# RealTime Distributed File Sync

A client-server file synchronization and backup system implemented in C using sockets and multi-threading.

The project allows multiple clients to connect to a server and synchronize files between their local directories and a central cloud storage directory.

## Features

- Client-server communication using TCP sockets
- Multi-client support using POSIX threads
- File upload and download
- File synchronization based on file metadata
- Comparison of file name, size, and modification time
- Automatic decision between upload, download, or no action
- Separate configuration files for client and server
- Support for multiple clients connecting concurrently
- Basic handling of client disconnections and file transfer operations

## Technologies

- C
- Linux
- TCP Sockets
- POSIX Threads
- File I/O
- Client-Server Architecture

## Project Files

```text
client.c
server.c
client_config.txt
server_config.txt
.gitignore
```

### `client.c`

Implements the client side of the system, including:

- Connecting to the server
- Sending client information
- Scanning local files
- Sending file metadata
- Uploading files
- Downloading files
- Synchronizing the local directory with the server

### `server.c`

Implements the server side of the system, including:

- Accepting multiple client connections
- Creating a separate thread for each client
- Managing cloud storage
- Comparing client and server file metadata
- Handling upload and download requests
- Synchronizing files between clients and the server

### Configuration Files

`client_config.txt` contains the client configuration.

`server_config.txt` contains the server configuration.

The configuration files can be modified depending on the environment where the program is executed.

## Compilation

Compile the server:

```bash
gcc server.c -o server -pthread
```

Compile the client:

```bash
gcc client.c -o client -pthread
```

## Running the Project

Start the server first:

```bash
./server
```

Then run one or more clients:

```bash
./client
```

Each client can use its own local directory while communicating with the same server.

## Synchronization

The system compares file metadata between the client and the server.

Depending on the comparison result, the system can:

- Upload a newer file from the client to the server
- Download a newer file from the server to the client
- Ignore the file when both versions are already synchronized

## Academic Project

This project was developed as part of a Real-Time Systems course and demonstrates practical use of:

- Socket programming
- Multi-threading
- Synchronization
- File management
- Client-server communication

## Usage Notice

Copyright © 2026 Aseel Hamed and Project Team. All Rights Reserved.

This project is publicly available for viewing and portfolio purposes only.

Copying, reproducing, redistributing, modifying, or using any part of this project in another academic, personal, or commercial project without explicit permission from the authors is prohibited.

This repository is not open source and no license is granted for reuse of the source code.
