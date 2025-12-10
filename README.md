# WinAPI File Transfer System

A high-performance Client-Server application engineered in C++ (C++20) using the Windows Sockets API (WinSock2). This project demonstrates advanced systems programming concepts, specifically **multithreading**, **socket synchronization**, and **custom application-layer protocols**.

The system utilizes a unique **Dual-Socket Architecture**: distinct TCP connections are maintained for control signals (JSON) and data transmission (Binary), ensuring that large file transfers never block command execution.

### Features

  * **Dual-Channel Protocol:** Uses one port for commands (Control) and another for file streams (Transfer).
  * **Concurrency:** Multithreaded server using `std::thread` and `std::mutex` to handle multiple clients simultaneously.
  * **File Operations:** Supports `put`, `get`, `delete`, `list`, and `info`.
  * **State Management:** Session-based directory isolation using client nicknames.

### 🏗️ System Architecture

The application implements a custom protocol to manage state across two asynchronous sockets per client:

1.  **Control Channel (Port N):** Handles command signaling, status codes, and metadata exchange using serialized JSON.
2.  **Transfer Channel (Port N+1):** Dedicated exclusively to raw binary stream transmission for files and directory listings.

### 💻 Supported Command

| Command | Syntax | Description |
| :--- | :--- | :--- |
| **Put** | `put <filename>` | Uploads a file from client to server (User's folder). |
| **Get** | `get <filename>` | Downloads a file from server to client. |
| **Info** | `info <filename>` | Requests file metadata (Last write time, size in bytes). |
| **List** | `list` | Retrieves a list of all files in the user's server directory. |
| **Delete** | `delete <filename>` | Removes a specific file from the server. |

### 🛠️ Tech Stack

  * **Language:** C++20
  * **Networking:** WinSock2 (`<winsock2.h>`, `<ws2tcpip.h>`)
  * **Serialization:** [nlohmann/json](https://github.com/nlohmann/json)
  * **File System:** `std::filesystem` for cross-platform path handling.
  * **Standard Lib:** `<thread>`, `<mutex>`, `<atomic>`, `<regex>`, `<chrono>`.

### Build

Requires `nlohmann/json.hpp` and a C++20 compiler (MSVC).

```powershell
# Server
cl server_source.cpp /std:c++20 /EHsc /link Ws2_32.lib /out:server.exe

# Client
cl client_source.cpp /std:c++20 /EHsc /link Ws2_32.lib /out:client.exe
```

