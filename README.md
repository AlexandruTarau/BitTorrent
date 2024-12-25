# BitTorrent Protocol

This project implements the BitTorrent protocol using MPI and pthreads. The system supports file uploading, downloading, and tracker coordination for managing peers and files.

## Features

- **File Sharing**: Peers can upload and download files in chunks.
- **Swarm Management**: Peers share information about available files and maintain a swarm for each file.
- **Chunk Distribution**: Files are broken into chunks, and missing chunks are downloaded from other peers.
- **Tracker Management**: A centralized tracker coordinates file availability and peer swarms.
- **Multi-threading**: Download and upload tasks run on separate threads for concurrency.
- **Periodic Swarm Updates**: Peers update their swarm list periodically to ensure efficient file sharing.

## Components

### 1. Tracker
The tracker manages:
- File lists and their corresponding swarms.
- Seeder and downloader registration.
- Sorting the swarm based on peer upload activity for efficient downloads.

### 2. Peer
Each peer:
- Uploads files it owns to other peers.
- Downloads missing file chunks from other peers in the swarm.
- Periodically updates its swarm list with new information from the tracker.

### 3. Threads
- **Download Thread**: Manages file downloads, including chunk verification and swarm updates.
- **Upload Thread**: Handles file upload requests from other peers and busyness level requests from the tracker.

## Code Structure

- `file` struct: Represents a file, including its name and chunks.
- `tracker_file_list` struct: Maintains information about a file, including its swarm of peers and seeders.
- `download_thread_arg` struct: Argument for the download threads.
- `upload_thread_arg` struct: Argument for the upload threads.
- `download_thread_func`: Handles file downloading in a separate thread.
- `upload_thread_func`: Handles file uploading in a separate thread.
- `tracker()`: Manages files and coordinates peers.
- `peer()`: Initializes peer behavior for uploading and downloading files.

## How It Works

1. **Initialization**:
   - Each peer provides an input file (`inX.txt`), which specifies the files it owns and wants to download. This input is sent to the tracker and a response is awaited.
   - The tracker waits to receive file data from each client and populates the swarm with the corresponding seeders. Finally, it sends an "ACK" response to each client, signaling that they can begin downloading or uploading.

2. **Tracker Role**:
   - Receives different types of requests from clients:
      - `download files` (tag 1): Receives the files to download and sends the data for each file, including the swarm and chunks. The swarm is first sorted based on the number of uploads each peer has completed (busyness level). The current peer is then added to the end of the swarm list. This ensures that the peer prioritizes downloading missing chunks from the least busy peers, while the current peer is always last in the list (since you cannot download chunks from yourself).
      - `update swarm` (tag 2): Receives the current file to update its swarm and sends back the updated swarm. The same process as in `download files` is applied to sort the swarm and send it.
      - `finished downloading a file` (tag 3): The peer has completely downloaded a file and is now a seeder for that file. (In my implementation, there is no need to differentiate between seeders and peers, so the tracker does nothing.)
      - `finished downloading all files` (tag 4): The peer has successfully downloaded all files. The tracker keeps track of this using a counter. Once the counter matches the total number of peers, the tracker signals all clients to stop and then terminates itself.

3. **Peer Role**:
   - Reads its input file to determine the files it owns and wants to download. These informations are then sent to the tracker and an "ACK" is awaited to begin downloading and uploading.
   - The peer is then split in two different threads:
      - `download_thread_func`: 
         - Handles the process of downloading wanted files chunk by chunk from other peers.
         - Firstly, it requests the chunk hashes and the swarm of peers and seeders from the tracker.
         - For each missing chunk in a file, the client requests it from peers and seeders in the swarm. An "ACK" is received instead of the actual chunk to simulate the transfer and a counter `count` is incremented. If the current peer in the swarm doesn't have the missing chunk or the peer is actually itself, we try again with the next peer in the list.
         - When `count` reaches 10, a request to update the swarm for this file is sent to the tracker.
         - If **no** peer in the swarm has the requested chunk, we **force** a swarm update from the tracker.
         - After a file is completely downloaded, we signal the tracker and save the file in a new output file `client<N>_<file_name>`.
         - When all files are completely downloaded, we signal the tracker and exit the thread.
      - `upload_thread_func`:
         - Handles the process of uploading owned files to other peers.
         - We use non-blocking receive instruction `MPI_Irecv` and `MPI_Test` as the requests from either the peers or the tracker can happen anytime and this ensures we don't get stuck.
         - The thread receives requests for the number of uploads (busyness level) from the tracker and requests for chunks from peers.
         - Each time there is a chunk request, the thread checks if it owns that chunk and sends a response `ACK` or `NCK`. If affirmative, the number of uploaded chunks increments.
      - The stopping condition is met when the peer receives a termination signal from the tracker. A `stop` variable is set to 1 which breaks out of the while loop in the upload thread.