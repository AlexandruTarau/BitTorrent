#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

typedef struct {
    char file_name[MAX_FILENAME + 1];
    int number_of_chunks;
    char chunks[MAX_CHUNKS][HASH_SIZE + 1];
} file;

typedef struct {
    file f;
    int swarm_size;
    int swarm[100];  // Do dynamic allocation later
} tracker_file_list;

typedef struct {
    int* rank;
    int* number_of_wanted_files;
    tracker_file_list* wanted_files_list;
    file* owned_files;
    int* number_of_owned_files;
} download_thread_arg;

typedef struct {
    int* rank;
    int* stop;
    file* owned_files;
    int* number_of_owned_files;
} upload_thread_arg;

void *download_thread_func(void *arg)
{
    download_thread_arg* thread_arg = (download_thread_arg*) arg;
    int rank = *(thread_arg->rank);
    int number_of_wanted_files = *(thread_arg->number_of_wanted_files);
    tracker_file_list* wanted_files_list = thread_arg->wanted_files_list;
    file* owned_files = thread_arg->owned_files;
    int* number_of_owned_files = thread_arg->number_of_owned_files;

    // Request swarm and hashes from tracker
    {
        // Signal the tracker that we want to download files
        int tag = 1;
        MPI_Send(&tag, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        // Send the list of wanted files
        MPI_Send(&number_of_wanted_files, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);

        for (int i = 0; i < number_of_wanted_files; i++) {
            MPI_Send(wanted_files_list[i].f.file_name, MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);

            MPI_Recv(&wanted_files_list[i].swarm_size, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for (int j = 0; j < wanted_files_list[i].swarm_size; j++) {
                MPI_Recv(&wanted_files_list[i].swarm[j], 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            }

            MPI_Recv(&wanted_files_list[i].f.number_of_chunks, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            for (int j = 0; j < wanted_files_list[i].f.number_of_chunks; j++) {
                MPI_Recv(wanted_files_list[i].f.chunks[j], HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            }
        }
    }

    // Search for missing chunks
    for (int i = 0; i < number_of_wanted_files; i++) {
        strcpy(owned_files[(*number_of_owned_files)].file_name, wanted_files_list[i].f.file_name);
        owned_files[(*number_of_owned_files)].number_of_chunks = 0;
        (*number_of_owned_files)++;
        int provider_index = 0;
        int count = 0;

        for (int j = 0; j < wanted_files_list[i].f.number_of_chunks; j++) {
            int found = 0;
            char chunk[HASH_SIZE + 1];

            // Update the swarm every 10 chunks
            if (count == 10) {
                count = 0;
                
                // Signal the tracker that we want to update our peers and seeders list
                int tag = 2;
                MPI_Send(&tag, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

                // Send the list of wanted files
                MPI_Send(&number_of_wanted_files, 1, MPI_INT, TRACKER_RANK, 2, MPI_COMM_WORLD);

                for (int i = 0; i < number_of_wanted_files; i++) {
                    MPI_Send(wanted_files_list[i].f.file_name, MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, 2, MPI_COMM_WORLD);

                    MPI_Recv(&wanted_files_list[i].swarm_size, 1, MPI_INT, TRACKER_RANK, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    for (int j = 0; j < wanted_files_list[i].swarm_size; j++) {
                        MPI_Recv(&wanted_files_list[i].swarm[j], 1, MPI_INT, TRACKER_RANK, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    }
                }
            }

            strcpy(chunk, wanted_files_list[i].f.chunks[j]);

            for (int k = 0; k < owned_files[(*number_of_owned_files) - 1].number_of_chunks; k++) {
                if (strcmp(owned_files[(*number_of_owned_files) - 1].chunks[k], chunk) == 0) {
                    found = 1;
                    break;
                }
            }

            // If the chunk is missing, search for it in the swarm
            if (!found) {
                if (wanted_files_list[i].swarm_size == 0) {
                    continue;
                } else if (wanted_files_list[i].swarm[provider_index] != rank) {
                    int try_again = 1;
                
                    while (try_again) {
                        try_again = 0;

                        // Send a request to the peer
                        MPI_Send(chunk, HASH_SIZE + 1, MPI_CHAR, wanted_files_list[i].swarm[provider_index], 6, MPI_COMM_WORLD);
                        
                        // Receive ACK and add the chunk to the owned_files list
                        char ack[4];
                        MPI_Status status;
                        MPI_Recv(ack, 4, MPI_CHAR, wanted_files_list[i].swarm[provider_index], 7, MPI_COMM_WORLD, &status);

                        if (strcmp(ack, "ACK") == 0) {
                            count++;
                            strcpy(owned_files[(*number_of_owned_files) - 1].chunks[owned_files[(*number_of_owned_files) - 1].number_of_chunks++], chunk);
                        } else {
                            try_again = 1;
                            provider_index = (provider_index + 1) % wanted_files_list[i].swarm_size;
                        }
                    }
                }
            }
        }

        // Notify the tracker that we finished downloading the file
        int tag = 3;
        MPI_Send(&tag, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        // Save the file in a new output file
        FILE *out;
        char out_filename[50];
        sprintf(out_filename, "client%d_%s", rank, wanted_files_list[i].f.file_name);
        out = fopen(out_filename, "w");
        if (out == NULL) {
            printf("Eroare la deschiderea fisierului de iesire\n");
            exit(-1);
        }

        for (int j = 0; j < owned_files[(*number_of_owned_files) - 1].number_of_chunks; j++) {
            fprintf(out, "%s", owned_files[(*number_of_owned_files) - 1].chunks[j]);
            if (j != owned_files[(*number_of_owned_files) - 1].number_of_chunks - 1) {
                fprintf(out, "\n");
            }
        }
    }

    // Notify the tracker we finished downloading all files
    int tag = 4;
    MPI_Send(&tag, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

    return NULL;
}

void *upload_thread_func(void *arg)
{
    upload_thread_arg* thread_arg = (upload_thread_arg*) arg;
    //int rank = *(thread_arg->rank);
    int* stop = thread_arg->stop;
    file* owned_files = thread_arg->owned_files;
    int* number_of_owned_files = thread_arg->number_of_owned_files;
    char chunk[HASH_SIZE + 1];
    MPI_Status status;
    MPI_Request request;

    // Receive chunk request
    MPI_Irecv(chunk, HASH_SIZE + 1, MPI_CHAR, MPI_ANY_SOURCE, 6, MPI_COMM_WORLD, &request);

    while ((*stop) == 0) {
        int flag = 0;
        MPI_Test(&request, &flag, &status);

        if (!flag) {
            continue;
        }

        // Search for the chunk in the owned_files list
        int found = 0;
        for (int i = 0; i < (*number_of_owned_files); i++) {
            for (int j = 0; j < owned_files[i].number_of_chunks; j++) {
                if (strcmp(owned_files[i].chunks[j], chunk) == 0) {
                    found = 1;
                    break;
                }
            }
        }

        // If the chunk is found, send it to the peer
        if (found) {
            char ack[4] = "ACK";
            MPI_Send(ack, 4, MPI_CHAR, status.MPI_SOURCE, 7, MPI_COMM_WORLD);
        } else {
            char nack[4] = "NCK";
            MPI_Send(nack, 4, MPI_CHAR, status.MPI_SOURCE, 7, MPI_COMM_WORLD);
        }

        // Receive the next chunk request
        MPI_Irecv(chunk, HASH_SIZE + 1, MPI_CHAR, MPI_ANY_SOURCE, 6, MPI_COMM_WORLD, &request);
    }

    return NULL;
}

void tracker(int numtasks, int rank) {
    tracker_file_list files_list[MAX_FILES];
    int number_of_clients = 0;
    int files_list_size = 0;
    int clients_done = 0;

    // Receive files from seeders
    while (number_of_clients < numtasks - 1) {
        int number_of_files;
        MPI_Status status;

        MPI_Recv(&number_of_files, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

        for (int i = 0; i < number_of_files; i++) {
            char file_name[MAX_FILENAME + 1];
            int number_of_chunks;
            char chunks[MAX_CHUNKS][HASH_SIZE + 1];
            MPI_Recv(file_name, MAX_FILENAME + 1, MPI_CHAR, status.MPI_SOURCE, 0, MPI_COMM_WORLD, &status);
            MPI_Recv(&number_of_chunks, 1, MPI_INT, status.MPI_SOURCE, 0, MPI_COMM_WORLD, &status);

            for (int j = 0; j < number_of_chunks; j++) {
                MPI_Recv(chunks[j], HASH_SIZE + 1, MPI_CHAR, status.MPI_SOURCE, 0, MPI_COMM_WORLD, &status);
            }

            // Check if file already exists
            int file_index = -1;
            for (int j = 0; j < files_list_size; j++) {
                if (strcmp(files_list[j].f.file_name, file_name) == 0) {
                    file_index = j;
                    break;
                }
            }

            // If the file doesn't exist yet, add it
            if (file_index == -1) {
                file_index = files_list_size++;
                strcpy(files_list[file_index].f.file_name, file_name);
                files_list[file_index].f.number_of_chunks = number_of_chunks;
                for (int j = 0; j < number_of_chunks; j++) {
                    strcpy(files_list[file_index].f.chunks[j], chunks[j]);
                }
            }

            // Add seeder to the swarm
            files_list[file_index].swarm[files_list[file_index].swarm_size++] = status.MPI_SOURCE;

            number_of_clients++;
        }
    }

    // Send an "ACK" to all clients
    {
        char ack[4] = "ACK";
        for (int i = 0; i < numtasks; i++) {
            if (i != TRACKER_RANK) {
                MPI_Send(ack, 4, MPI_CHAR, i, 0, MPI_COMM_WORLD);
            }
        }
    }

    // Receive requests from peers
    while (1) {
        int tag;
        MPI_Status status;

        MPI_Recv(&tag, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

        switch (tag) {
            case 1: {  // Peer wants to download files
                int number_of_wanted_files;
                MPI_Recv(&number_of_wanted_files, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD, &status);

                for (int i = 0; i < number_of_wanted_files; i++) {
                    char wanted_file[MAX_FILENAME + 1];
                    MPI_Recv(wanted_file, MAX_FILENAME + 1, MPI_CHAR, status.MPI_SOURCE, 1, MPI_COMM_WORLD, &status);

                    // Search for the file in the files_list
                    int file_index = -1;
                    for (int j = 0; j < files_list_size; j++) {
                        if (strcmp(files_list[j].f.file_name, wanted_file) == 0) {
                            file_index = j;
                            break;
                        }
                    }

                    // If the file is found,
                    if (file_index != -1) {
                        // Add the peer to the swarm
                        files_list[file_index].swarm[files_list[file_index].swarm_size++] = status.MPI_SOURCE;

                        // Send the swarm and chunks to the peer
                        MPI_Send(&files_list[file_index].swarm_size, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD);
                        for (int j = 0; j < files_list[file_index].swarm_size; j++) {
                            MPI_Send(&files_list[file_index].swarm[j], 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD);
                        }

                        MPI_Send(&files_list[file_index].f.number_of_chunks, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD);
                        for (int j = 0; j < files_list[file_index].f.number_of_chunks; j++) {
                            MPI_Send(files_list[file_index].f.chunks[j], HASH_SIZE + 1, MPI_CHAR, status.MPI_SOURCE, 1, MPI_COMM_WORLD);
                        }
                    }
                }
                break;
            }
            case 2: {  // Peer wants to update the swarm
                int number_of_wanted_files;
                MPI_Recv(&number_of_wanted_files, 1, MPI_INT, status.MPI_SOURCE, 2, MPI_COMM_WORLD, &status);

                for (int i = 0; i < number_of_wanted_files; i++) {
                    char wanted_file[MAX_FILENAME + 1];
                    MPI_Recv(wanted_file, MAX_FILENAME + 1, MPI_CHAR, status.MPI_SOURCE, 2, MPI_COMM_WORLD, &status);

                    // Search for the file in the files_list
                    int file_index = -1;
                    for (int j = 0; j < files_list_size; j++) {
                        if (strcmp(files_list[j].f.file_name, wanted_file) == 0) {
                            file_index = j;
                            break;
                        }
                    }

                    // If the file is found,
                    if (file_index != -1) {
                        // Add the peer to the swarm if it's not already there
                        int found = 0;
                        for (int j = 0; j < files_list[file_index].swarm_size; j++) {
                            if (files_list[file_index].swarm[j] == status.MPI_SOURCE) {
                                found = 1;
                                break;
                            }
                        }

                        if (!found) {
                            files_list[file_index].swarm[files_list[file_index].swarm_size++] = status.MPI_SOURCE;
                        }

                        // Send the swarm and chunks to the peer
                        MPI_Send(&files_list[file_index].swarm_size, 1, MPI_INT, status.MPI_SOURCE, 2, MPI_COMM_WORLD);
                        for (int j = 0; j < files_list[file_index].swarm_size; j++) {
                            MPI_Send(&files_list[file_index].swarm[j], 1, MPI_INT, status.MPI_SOURCE, 2, MPI_COMM_WORLD);
                        }
                    }
                }
                break;
            }
            case 3: {  // Peer finished downloading a file
                // The peer becomes a seeder
                break;
            }
            case 4: {  // Peer finished downloading all files
                // The peer becomes a seeder
                clients_done++;
                break;
            }
        }

        // If all clients are done, signal the peers to stop and also stop the tracker
        if (clients_done == numtasks - 1) {
            for (int i = 0; i < numtasks; i++) {
                if (i != TRACKER_RANK) {
                    MPI_Send(NULL, 0, MPI_CHAR, i, 5, MPI_COMM_WORLD);
                }
            }
            break;
        }
    }
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;
    FILE *f;
    char filename[MAX_FILENAME + 1] = {0};
    file owned_files[MAX_FILES];
    int stop = 0;

    int number_of_owned_files;
    int number_of_wanted_files;
    tracker_file_list wanted_files_list[MAX_FILES];

    // Read input file
    {
        sprintf(filename, "in%d.txt", rank);

        f = fopen(filename, "r");
        if (f == NULL) {
            printf("Eroare la deschiderea fisierului de intrare\n");
            exit(-1);
        }

        fscanf(f, "%d", &number_of_owned_files);

        for (int i = 0; i < number_of_owned_files; i++) {
            fscanf(f, "%s", owned_files[i].file_name);
            fscanf(f, "%d", &owned_files[i].number_of_chunks);
            for (int j = 0; j < owned_files[i].number_of_chunks; j++) {
                fscanf(f, "%s", owned_files[i].chunks[j]);
            }
        }

        fscanf(f, "%d", &number_of_wanted_files);

        for (int i = 0; i < number_of_wanted_files; i++) {
            fscanf(f, "%s", wanted_files_list[i].f.file_name);
        }

        fclose(f);
    }

    // Send owned files to tracker
    {
        MPI_Send(&number_of_owned_files, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        for (int i = 0; i < number_of_owned_files; i++) {
            MPI_Send(owned_files[i].file_name, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
            MPI_Send(&owned_files[i].number_of_chunks, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
            for (int j = 0; j < owned_files[i].number_of_chunks; j++) {
                MPI_Send(owned_files[i].chunks[j], HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
            }
        }
    }

    // Receive ACK
    {
        char ack[4];
        MPI_Status status;
        MPI_Recv(ack, 4, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, &status);

        if (strcmp(ack, "ACK") != 0) {
            printf("Eroare la primirea ACK de la tracker\n");
            exit(-1);
        }
    }

    download_thread_arg download_thread_argument;
    download_thread_argument.rank = &rank;
    download_thread_argument.number_of_wanted_files = &number_of_wanted_files;
    download_thread_argument.wanted_files_list = wanted_files_list;
    download_thread_argument.owned_files = owned_files;
    download_thread_argument.number_of_owned_files = &number_of_owned_files;

    upload_thread_arg upload_thread_argument;
    upload_thread_argument.rank = &rank;
    upload_thread_argument.stop = &stop;
    upload_thread_argument.owned_files = owned_files;
    upload_thread_argument.number_of_owned_files = &number_of_owned_files;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &download_thread_argument);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &upload_thread_argument);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    MPI_Recv(NULL, 0, MPI_CHAR, TRACKER_RANK, 5, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    stop = 1;

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
