
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> //for read(),write()
#include <sys/types.h>
#include <sys/socket.h> //for socket
#include <netinet/in.h> //for Byte ordering
#include <arpa/inet.h>  // for inet_adder
#include <pthread.h>
#include <sys/ipc.h> // for semaphores
#include <sys/sem.h>
#include <time.h> //for log timestamp
#include <sys/stat.h> //for mkdir (create client storage folder)
#include <signal.h>   // or graceful shutdown on Ctrl+C
#include <utime.h>    //for utime() _ keep an uploaded file's on-disk mtime in sync with metadata
#include <math.h>
#include <GL/freeglut.h>

#define INITIAL_CAPACITY 8 //starting size for the dynamic file-info arrays below; they grow (doubling) with realloc as needed, so there's no hard cap on file count anymore

struct file_info //to stor information about a sigle file 
{
    char filename[100];
    long file_size;
    long modification_time;
};


typedef enum { CLIENT_CONNECTED, CLIENT_SYNCING, CLIENT_TRANSFER, CLIENT_DONE, CLIENT_ERROR, CLIENT_DISCONNECTED } client_ui_kind_t;
//store client information for the GUI
struct tracked_client
{
    int used;
    char client_id[50];
    char status_text[100];
    client_ui_kind_t kind;
};
//GUI data
#define MAX_TRACKED_CLIENTS 50
static struct tracked_client g_tracked_clients[MAX_TRACKED_CLIENTS];
#define MAX_LOG_LINES 200
static char g_log_lines[MAX_LOG_LINES][220];
static int g_log_count = 0;
static int g_log_scroll = 0;
static int g_log_sb_dragging = 0;
static float g_log_sb_drag_offset = 0;
static float g_log_sb_x = 0, g_log_sb_w = 0, g_log_sb_top = 0, g_log_sb_bottom = 0;
static float g_log_sb_thumb_top = 0, g_log_sb_thumb_bottom = 0;
static int g_log_sb_max_scroll = 0;
static int g_log_sb_page = 1;
static pthread_mutex_t g_ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_ip[50];
//Server configuration
static int g_port;
static char g_folder[100];
static int g_block_size;
static int g_win_width = 1000;
static int g_win_height = 620;


struct file_info *grow_file_array(struct file_info *files, int *capacity)// make the file array bigger when it becomes full
{
    int new_capacity = (*capacity) * 2;
    struct file_info *bigger = realloc(files, new_capacity * sizeof(struct file_info));
    if (bigger == NULL)
    {
        printf("Memory allocation failed while growing file list\n");
        return NULL;
    }
    *capacity = new_capacity;
    return bigger;
}

int is_safe_name(char *name) //check that the name is safe to use
{
    if (name[0] == '\0')
    {
        return 0;
    }
    if (strstr(name, "..") != NULL)
    {
        return 0;
    }
    if (strchr(name, '/') != NULL)
    {
        return 0;
    }
    return 1;
}

// Make sure the client's storage folder exists (first time we see this client)
void ensure_client_folder(char *folder, char *client_id) 
{
    char client_folder[200];
    strcpy(client_folder, folder);
    strcat(client_folder, "/");
    strcat(client_folder, client_id);
    mkdir(client_folder, 0755); //ignored if it already exists
}

 //store data needed for handling with client connection
struct client_thread_data
{
    int client_socket;
    char folder[100];
    int block_size;
};
//store a mutex for each client
#define MAX_CLIENT_LOCKS 100
struct client_lock
{
    char client_id[50];
    pthread_mutex_t mutex;
    int used;
};

struct client_lock client_locks[MAX_CLIENT_LOCKS];

pthread_mutex_t client_locks_mutex = PTHREAD_MUTEX_INITIALIZER;//to protect tha array itself

// print log message with time and store it for the GUI
void log_event(char *message)
{
    time_t current_time;
    struct tm time_info_storage;
    struct tm *time_info;
    char time_string[20];
    char line[220];

    current_time = time(NULL);
    time_info = localtime_r(&current_time, &time_info_storage);

    strftime(time_string,20,"%H:%M:%S",time_info);

    printf("[%s] %s\n",time_string,message);
    fflush(stdout); //flush so events show up immediately, even when redirected to a log file

    snprintf(line, sizeof(line), "[%s] %s", time_string, message);
    pthread_mutex_lock(&g_ui_mutex);
    if (g_log_count < MAX_LOG_LINES)
    {
        strcpy(g_log_lines[g_log_count++], line);
    }
    else
    {
        // remove the oldest log line
        for (int i = 1; i < MAX_LOG_LINES; i++)
        {
            strcpy(g_log_lines[i - 1], g_log_lines[i]);
        }
        strcpy(g_log_lines[MAX_LOG_LINES - 1], line);
    }
    pthread_mutex_unlock(&g_ui_mutex);
}
// update the client status in the GUI

void ui_set_client_status(const char *client_id, const char *status, client_ui_kind_t kind)
{
    pthread_mutex_lock(&g_ui_mutex);
    int slot = -1;
    // search if the client already exists
    for (int i = 0; i < MAX_TRACKED_CLIENTS; i++)
    {
        if (g_tracked_clients[i].used && strcmp(g_tracked_clients[i].client_id, client_id) == 0)
        {
            slot = i;
            break;
        }
    }
    //if client is new, find empty place
    if (slot == -1)
    {
        for (int i = 0; i < MAX_TRACKED_CLIENTS; i++)
        {
            if (!g_tracked_clients[i].used)
            {
                slot = i;
                g_tracked_clients[i].used = 1;
                strncpy(g_tracked_clients[i].client_id, client_id, sizeof(g_tracked_clients[i].client_id) - 1);
                g_tracked_clients[i].client_id[sizeof(g_tracked_clients[i].client_id) - 1] = '\0';
                break;
            }
        }
    }
    //save the new status
    if (slot != -1)
    {
        strncpy(g_tracked_clients[slot].status_text, status, sizeof(g_tracked_clients[slot].status_text) - 1);
        g_tracked_clients[slot].status_text[sizeof(g_tracked_clients[slot].status_text) - 1] = '\0';
        g_tracked_clients[slot].kind = kind;
    }
    pthread_mutex_unlock(&g_ui_mutex);
}

//Read the server info from the configuration file
int read_configuration(char *filename, char *ip, int *port, char *folder, int *block_size)
{
    FILE *file;
    file = fopen(filename, "r");
    //to make sure  that the configuration file was opened
    if (file == NULL)
    {
        printf("Cannot open configuration file\n");
        return -1;
    }
    fscanf(file, "%49s", ip);    //width-limited: ip[] is 50 bytes, leave room for the terminator
    fscanf(file, "%d", port);
    fscanf(file, "%99s", folder); //width-limited: folder[] is 100 bytes
    fscanf(file, "%d", block_size);
    fclose(file);
    return 1;
}

//Create the server socket (the main one )
int create_TCP_socket()
{
    int server_socket;
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        printf("Error while creating socket\n");
        return -1;
    }

    //Allow immediate restart of the server on the same port during testing
    int reuse = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    printf("Socket created successfully\n");
    return server_socket;
}
//Set the server address
void make_server_address(struct sockaddr_in *address, char *ip, int port)
{
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    address->sin_addr.s_addr = inet_addr(ip);
}

//Bind and start listening
int start_listen(int server_socket, struct sockaddr_in *server_address)
{
    if (bind(server_socket, (struct sockaddr *)server_address, sizeof(*server_address)) < 0)
    {
        printf("error while Binding \n");
        return -1;
    }

    if (listen(server_socket, 16) < 0) 
    {
        printf("error while listening\n");
        return -1;
    }
    printf("Server is listening\n");
    return 1;
}

//Accept a client connection
int accept_client(int server_socket)
{
    struct sockaddr_in client_address;
    int client_socket;
    int client_length = sizeof(client_address);

    printf("Waiting for a client...\n");
    client_socket = accept(server_socket, (struct sockaddr *)&client_address, &client_length);
    if (client_socket < 0)
    {
        printf("Error while accepting client\n");
        return -1;
    }
    printf("Client connected\n");
    return client_socket;
}

//Receive one line from the client
int receive_line(int client_socket, char *message, int max_size)
{
    int total = 0;
    int bytes_read;
    char one_char;
    while (total < max_size - 1)
    {
        bytes_read = read(client_socket, &one_char, 1); //read char by char until the end of the message(/n)
        if (bytes_read <= 0)
        {
            return -1;
        }
        if (one_char == '\n')
        {
            break;
        }
        message[total] = one_char;
        total++;
    }
    message[total] = '\0';
    return total;
}

//Receive the client ID
int receive_client_id(int client_socket, char *client_id)
{
    if (receive_line(client_socket, client_id, 50) < 0)
         //read client id from soket
        {
            printf("Error while receiving client ID\n");
            return -1;
        }

    printf("Client ID: %s\n", client_id);

    return 1;
}


int receive_file_information(int client_socket, struct file_info **client_files_ptr, int *capacity, int *number_of_files)
{
    char file_information[200];
    struct file_info temp_file;
    struct file_info *client_files = *client_files_ptr;
    *number_of_files = 0; //counter for received file
    while (1)
    {
        if (receive_line(client_socket, file_information, 200) < 0) //reacive on line file information
        {
            printf("Error while receiving file information\n");
            return -1;
        }

        if (strcmp(file_information, "END") == 0) //mean cliet finish sending file list
        {
            printf("Finished receiving file list\n");
            break;
        }

        if (sscanf(file_information, "%99s %ld %ld", temp_file.filename, &temp_file.file_size, &temp_file.modification_time) != 3)
        {
            printf("Malformed file information line, ignoring: %s\n", file_information);
            log_event("Rejected malformed file information line from client");
            continue;
        }

        //reject file names that could escape the client's folder (path traversal)
        if (!is_safe_name(temp_file.filename))
        {
            printf("Rejected unsafe file name: %s\n", temp_file.filename);
            log_event("Rejected file name (possible path traversal attempt)");
            continue;
        }

        //grow the array (doubling) instead of capping at a fixed size
        if (*number_of_files >= *capacity)
        {
            struct file_info *bigger = grow_file_array(client_files, capacity);
            if (bigger == NULL)
            {
                printf("Out of memory, ignoring remaining files\n");
                log_event("Out of memory while receiving file list, extra files ignored");
                continue; // keep draining the socket, just stop storing
            }
            client_files = bigger;
            *client_files_ptr = bigger; //keep the caller's pointer in sync
        }

        client_files[*number_of_files] = temp_file;
        printf("File name:%s\n", client_files[*number_of_files].filename);
        printf("File size:%ld\n", client_files[*number_of_files].file_size);
        printf("Modification time:%ld\n", client_files[*number_of_files].modification_time);

        (*number_of_files)++;
    }

    return 1;
}

//Save server metadata
int save_server_metadata(char *folder,char *client_id,struct file_info server_files[],int number_of_server_files)
{
    FILE *file;
    char metadata_path[200];
    int i;

    strcpy(metadata_path, folder);
    strcat(metadata_path, "/");
    strcat(metadata_path, client_id);
    strcat(metadata_path, "/metadata.txt");

    file = fopen(metadata_path, "w");

    if (file == NULL)
    {
    printf("Cannot open metadata file for writing\n");
    return -1;
    }

    for (i = 0; i <number_of_server_files; i++)
    {
        fprintf(file,"%s %ld %ld\n",server_files[i].filename,server_files[i].file_size,server_files[i].modification_time);
    }

    fclose(file);
    printf("Metadata updated\n");

    return 1;
}

// read the metadata for this client
//(growable array): grow_file_array() may replace the pointer if itneeds more room,so the caller's copy is kept in sync via server_files_ptr.
int read_server_metadata(char *folder, char *client_id, struct file_info **server_files_ptr, int *capacity, int *number_of_server_files)
{
    FILE *file;
    char metadata_path[200];
    struct file_info temp_file;
    struct file_info *server_files = *server_files_ptr;
    *number_of_server_files = 0;   //start with zero file server
    strcpy(metadata_path, folder); //builde path for metadata file
    strcat(metadata_path, "/");
    strcat(metadata_path, client_id);
    strcat(metadata_path, "/metadata.txt");
    file = fopen(metadata_path, "r");
    if (file == NULL)
    {
        printf("Metadata file not found for client %s\n", client_id);
        return 0;
    }

   //if metadata does not match the real files, we update it later
    int metadata_needs_rewrite = 0;
     //read one file information from metadata each time
    while (fscanf(file, "%99s %ld %ld", temp_file.filename, &temp_file.file_size, &temp_file.modification_time) == 3) // read file information from metadata file (width-limited: filename[] is 100 bytes)
    {
        //check the real file because it may be changed manually
        char actual_path[300];
        struct stat file_stat;
        //build the real file path
        strcpy(actual_path, folder);
        strcat(actual_path, "/");
        strcat(actual_path, client_id);
        strcat(actual_path, "/");
        strcat(actual_path, temp_file.filename);
         //check if the file still exists on disk
        if (stat(actual_path, &file_stat) < 0)
        {
            // recorded in metadata but missing on disk (deleted/moved outside the protocol) then drop the stale entry instead of claiming to have a file that isn't really there; the client will just re-upload it
            
            printf("Metadata entry for %s has no matching file on disk, ignoring stale entry\n", temp_file.filename);
            log_event("Metadata entry missing on disk, treated as absent");
            metadata_needs_rewrite = 1;
            continue;
            //file is in metadata but not found in the folder
            //ignore it so the client can upload it again
        }
          //check if size or modification time changed
        if (file_stat.st_size != temp_file.file_size || file_stat.st_mtime != temp_file.modification_time)
        {
            printf("Detected manual change to %s outside the sync protocol, using the live file state\n", temp_file.filename);
            log_event("Detected out-of-band change to a stored file, refreshed from disk");
            metadata_needs_rewrite = 1;
        }
         //use the real file information
        temp_file.file_size = file_stat.st_size;
        temp_file.modification_time = file_stat.st_mtime;
        //make the array bigger if it is full
        if (*number_of_server_files >= *capacity)
        {
            struct file_info *bigger = grow_file_array(server_files, capacity);
            if (bigger == NULL)
            {
                printf("Out of memory while loading metadata, stopping early\n");
                break;
            }
            server_files = bigger;
            *server_files_ptr = bigger;
        }
        // save the file information in the array
        server_files[*number_of_server_files] = temp_file;
        (*number_of_server_files)++;
    }

    fclose(file);
     //rewrite metadata if something was changed
    if (metadata_needs_rewrite)
    {
        save_server_metadata(folder, client_id, server_files, *number_of_server_files);
        log_event("Rewrote metadata.txt to match the out-of-band changes found on disk");
    }

    printf("Server metadata loaded\n");
    printf("Number of server files: %d\n", *number_of_server_files);

    return 1;
}

//Write all the required bytes
int write_all(int socket, char *message, int size)
{
    int total = 0;
    int bytes_written;
     //keep writing until all bytes are sent
    while (total< size)
    {
        bytes_written = write(socket, message + total, size - total);

        if (bytes_written <= 0)
        {
        return -1;
        }

        total = total + bytes_written;
    }

    return 1;
}

//Receive one file from the client
int receive_file(int client_socket,char *folder,char *client_id,char *filename,long file_size,int block_size)
{
    FILE *file;
    char file_path[300];
    char buffer[block_size];
    long total_received = 0;
    int bytes_read;
    int bytes_to_read;
    //build the path where the uploaded file will be saved
    strcpy(file_path, folder);
    strcat(file_path, "/");
    strcat(file_path, client_id);
    strcat(file_path, "/");
    strcat(file_path, filename);
    //open the file for writing
    file = fopen(file_path,"wb");
    if (file == NULL)
    {
    printf("Cannot open file for upload\n");
    return -1;
    }
    //keep receiving until the whole file is received
    while (total_received< file_size)
    {
        bytes_to_read = block_size;
          //for the last block, read only the remaining bytes
        if (file_size - total_received< block_size)
        {
        bytes_to_read = file_size - total_received;
        }

        bytes_read = read(client_socket,buffer,bytes_to_read);

        if (bytes_read <= 0)
        {
        printf("error while uploading\n");
        log_event("Upload interrupted before completion");
        fclose(file);
        return -1;
        }

       //wach element is 1 byte and bytes_read is the total size of data
        if (fwrite(buffer,1,bytes_read,file) != bytes_read)
        {
            printf("Error while writing file\n");
             fclose(file);
             return -1;
        }
        total_received= total_received + bytes_read;
    }

    fclose(file);

    printf("%s upload completed\n", filename);

    return 1;
}

//Send one file to the client
int send_file(int client_socket,char *folder,char *client_id,char *filename,long file_size,int block_size)
{
    FILE *file;
    char file_path[300];
    char buffer[block_size];

    long total_sent = 0;
    int bytes_read;

    strcpy(file_path, folder);
    strcat(file_path, "/");
    strcat(file_path, client_id);
    strcat(file_path, "/");
    strcat(file_path, filename);

    file = fopen(file_path, "rb");

    if (file == NULL)
    {
    printf("Cannot open file for download\n");
    return -1;
    }
     //keep sending until the whole file is sent
    while (total_sent< file_size)
    {
         //read one block from the file
        bytes_read = fread(buffer,1,block_size,file);

        if (bytes_read<= 0)
        {

        printf("Error while reading file\n");
        log_event("Download failed while reading file");
        fclose(file);
        return -1;
        }
         //send all bytes that were read
        if (write_all(client_socket,buffer,bytes_read) < 0)
        {
        printf("Error while downloading\n");
        log_event("Download interrupted before completion");
        fclose(file);
        return -1;
        }

        total_sent = total_sent + bytes_read;
    }

    fclose(file);

    printf("%s download completed\n", filename);

    return 1;
}
//Request one file upload from client

int request_upload(int client_socket,char *folder,char *client_id,struct file_info *client_file,int block_size)
{
    char request[200];
    char log_message[200];

    //send file name, size and modification time to the client
    sprintf(request,"UPLOAD %s %ld %ld\n",client_file->filename,client_file->file_size,client_file->modification_time);

    if (write_all(client_socket,request,strlen(request)) < 0)
    {
        printf("Error while sending upload request\n");
        log_event("Error while sending upload request");
        return -1;
    }

    printf("Requesting upload: %s\n",client_file->filename);
    //add upload start to the log
    sprintf(log_message,"Upload started: %s from %s",client_file->filename,client_id);
    log_event(log_message);

    {
        //show the current upload in the GUI
        char ui_line[160];
        snprintf(ui_line, sizeof(ui_line), "Uploading %s", client_file->filename);
        ui_set_client_status(client_id, ui_line, CLIENT_TRANSFER);
    }
      //receive the file from the client
    if (receive_file(client_socket,folder,client_id,client_file->filename,client_file->file_size,block_size) < 0)
    {
        sprintf(log_message,"Upload failed: %s from %s",client_file->filename,client_id);
        log_event(log_message);
        return -1;
    }
    //keep the saved file modification time the same as the client file

    {
        char file_path[300];
        struct utimbuf times;
        strcpy(file_path, folder);
        strcat(file_path, "/");
        strcat(file_path, client_id);
        strcat(file_path, "/");
        strcat(file_path, client_file->filename);
        times.actime = client_file->modification_time;
        times.modtime = client_file->modification_time;
        utime(file_path, &times);
    }
   //add successful upload to the log
    sprintf(log_message,"Upload completed: %s from %s",client_file->filename,client_id);
    log_event(log_message);

    return 1;
}

//Send one file download to client
int request_download(int client_socket,char *folder,char *client_id,struct file_info *server_file,int block_size)
{
    char request[200];
    char log_message[200];
    //send file name, size and modification time to the client
    sprintf(request,"DOWNLOAD %s %ld %ld\n",server_file->filename,server_file->file_size,server_file->modification_time);

    if (write_all(client_socket,request,strlen(request)) < 0)
    {
        printf("Error while sending download request\n");
        log_event("Error while sending download request");
        return -1;
    }

    printf("Sending download: %s\n",server_file->filename);

    sprintf(log_message,"Download started: %s to %s",server_file->filename,client_id);
    log_event(log_message);

    {
        //show the current download in the GUI
        char ui_line[160];
        snprintf(ui_line, sizeof(ui_line), "Downloading %s", server_file->filename);
        ui_set_client_status(client_id, ui_line, CLIENT_TRANSFER);
    }

    if (send_file(client_socket,folder,client_id,server_file->filename,server_file->file_size,block_size) < 0)
    {
        sprintf(log_message,"Download failed: %s to %s",server_file->filename,client_id);
        log_event(log_message);
        return -1;
    }

    sprintf(log_message,"Download completed: %s to %s",server_file->filename,client_id);
    log_event(log_message);

    return 1;
}
//compare client files with server files and decide what to do
int compare_files(struct file_info client_files[],int number_of_client_files,struct file_info **server_files_ptr,int *server_capacity,int *number_of_server_files_ptr,int client_socket,char *folder,char *client_id,int block_size)
{
    struct file_info *server_files = *server_files_ptr;
    int number_of_server_files = *number_of_server_files_ptr;
    int i;
    int j;
    int found;
    char log_message[200];
     //counters for the synchronization result
    int uploaded_count = 0;
    int downloaded_count = 0;
    int unchanged_count = 0;
    int failed_count = 0;
    //check all files that the client has
    for (i = 0; i < number_of_client_files; i++)
    {
        found = 0;
        //search for the same file on the server
        for (j = 0; j < number_of_server_files; j++)
        {
            if (strcmp(client_files[i].filename,server_files[j].filename) == 0)
            {
                found = 1;

                //client file is newer, so upload it to the server
                if (client_files[i].modification_time >server_files[j].modification_time)
                {
                    printf("%s -> UPLOAD\n",client_files[i].filename);

                    //note: one file failing to transfer should not abort the whole synchronization (the other files/clients shouldn't be affected),so we log it and move on to the next file instead of returning -1.
                    if (request_upload(client_socket,folder,client_id,&client_files[i],block_size) < 0)
                    {
                        failed_count++;
                        break;
                    }
                    uploaded_count++;
                       //update the server information with the new client file
                    server_files[j].file_size =client_files[i].file_size;
                    server_files[j].modification_time =client_files[i].modification_time;

                    save_server_metadata(folder,client_id,server_files,number_of_server_files);
                }

                //server file is newer, so send it to the client
                else if (client_files[i].modification_time <server_files[j].modification_time)
                {
                    printf("%s -> DOWNLOAD\n",client_files[i].filename);
                    if (request_download(client_socket,folder,client_id,&server_files[j],block_size) < 0)
                    {
                        failed_count++;
                        break;
                    }
                    downloaded_count++;
                }
                //both files have the same modification time
                else
                {
                    printf("%s -> NO TRANSFER\n",client_files[i].filename);
                    sprintf(log_message,"%s already synchronized",client_files[i].filename);
                    log_event(log_message);
                    unchanged_count++;
                }

                break;
            }
        }

         //file exists only on the client, so upload it
        if (found == 0)
        {
            printf("%s -> UPLOAD\n",client_files[i].filename);

            if (request_upload(client_socket,folder,client_id,&client_files[i],block_size) < 0)
            {
                failed_count++;
                continue; //this file failed, keep syncing the rest
            }
            uploaded_count++;

            //make the server file array bigger if there is no more space
            if (number_of_server_files >= *server_capacity)
            {
                struct file_info *bigger = grow_file_array(server_files, server_capacity);
                if (bigger == NULL)
                {
                    printf("Out of memory, cannot record new file in server metadata\n");
                    log_event("Out of memory while updating server file list");
                    continue;
                }
                server_files = bigger;
                *server_files_ptr = bigger; //update the caller pointer too
            }
              //add the new file to the server file list
             strcpy(server_files[number_of_server_files].filename,client_files[i].filename);
            server_files[number_of_server_files].file_size =client_files[i].file_size;

            server_files[number_of_server_files].modification_time =client_files[i].modification_time;

                number_of_server_files++;

                            save_server_metadata(folder,client_id,server_files,number_of_server_files);

        }
    }

    //Check files that exist only on server
    for (i = 0; i < number_of_server_files; i++)
    {
        found = 0;

        for (j = 0; j < number_of_client_files; j++)
        {
            if (strcmp(server_files[i].filename,client_files[j].filename) == 0)
            {
                found = 1;
                break;
            }
        }
         //server has the file but the client does not, so download it
        if (found == 0)
        {
            printf("%s -> DOWNLOAD\n",server_files[i].filename);
             if (request_download(client_socket,folder,client_id,&server_files[i],block_size) < 0)
            {
                failed_count++;
                continue; //this file failed, keep syncing the rest
            }
            downloaded_count++;
        }
    }

    {
        //save a short summary in the log
        char summary_message[220];
        snprintf(summary_message, sizeof(summary_message),
                 "Sync summary for %s: %d uploaded, %d downloaded, %d unchanged, %d failed",
                 client_id, uploaded_count, downloaded_count, unchanged_count, failed_count);
        log_event(summary_message); //show the summary in the log
    }

    //update the number of server files after adding new files
    *number_of_server_files_ptr = number_of_server_files;

    //Tell client that the Synchronization has finished
    if (write_all(client_socket,"SYNC_COMPLETE\n",strlen("SYNC_COMPLETE\n")) < 0)
    {
        return -1;
    }

    return 1;
}


//Get the mutex for one client
pthread_mutex_t *get_client_mutex(char *client_id)
{
    int i;
    //lock the mutex array while searching or adding a client
    pthread_mutex_lock(&client_locks_mutex);

    //search if this client already has a mutex
    for (i = 0; i < MAX_CLIENT_LOCKS; i++)
    {
        if (client_locks[i].used == 1 &&strcmp(client_locks[i].client_id, client_id) == 0)
        {
            pthread_mutex_unlock(&client_locks_mutex);
            return &client_locks[i].mutex;
        }
    }

    //Create mutex for new client
    for (i = 0; i < MAX_CLIENT_LOCKS; i++)
    {
        if (client_locks[i].used== 0) // empty palce for a new client
        {
            strcpy(client_locks[i].client_id, client_id);
            pthread_mutex_init(&client_locks[i].mutex, NULL);
            client_locks[i].used = 1;
            pthread_mutex_unlock(&client_locks_mutex);
            return &client_locks[i].mutex;
        }
    }

    pthread_mutex_unlock(&client_locks_mutex);

    return NULL;
}

//Handle one client
void *handle_client(void *data_pointer)
{
    struct client_thread_data *client_data;
    int client_socket;
    char folder[100];
    char client_id[50];
    char log_message[200];
    struct file_info *client_files;    //dynamic arrays for client and server files
    int client_files_capacity;
    int number_of_files;
    struct file_info *server_files;    
    int server_files_capacity;
    int number_of_server_files;
    int block_size;
    pthread_mutex_t *client_mutex;
     //get the client information sent to this thread
    client_data = (struct client_thread_data *)data_pointer;
    client_socket = client_data->client_socket;
    strcpy(folder,client_data->folder);
    block_size = client_data->block_size;
    free(client_data);
    printf("Client thread started\n");

    if (receive_client_id(client_socket,client_id) < 0)
    {
        log_event("Client disconnected before sending ID");
        close(client_socket);
        pthread_exit(NULL);
    }

    //make sure the client ID is safe to use as a folder name
    if (!is_safe_name(client_id))
    {
        printf("Invalid client ID: %s\n", client_id);
        log_event("Invalid client ID rejected");
        close(client_socket);
        pthread_exit(NULL);
    }
      //log the new client and show it in the GUI
    sprintf(log_message,"Client connected: %s",client_id);
    log_event(log_message);
    ui_set_client_status(client_id, "Connected", CLIENT_CONNECTED);

    //First time this client connects, it has no storage folder yet then create it.
    ensure_client_folder(folder, client_id);
      //get the mutex used for this client
    client_mutex = get_client_mutex(client_id);
    if (client_mutex == NULL)
    {
        printf("Cannot get client mutex\n");
        log_event("Cannot get client mutex");
        ui_set_client_status(client_id, "Error: no mutex available", CLIENT_ERROR);
        close(client_socket);
        pthread_exit(NULL);
    }

    //start with an initial size for both file arrays they can grow later if more files are received
    client_files_capacity = INITIAL_CAPACITY;
    client_files = malloc(client_files_capacity * sizeof(struct file_info));
    server_files_capacity = INITIAL_CAPACITY;
    server_files = malloc(server_files_capacity * sizeof(struct file_info));
    if (client_files == NULL || server_files == NULL)
    {
        printf("Memory error while allocating file lists\n");
        log_event("Memory error while allocating file lists");
        ui_set_client_status(client_id, "Error: out of memory", CLIENT_ERROR);
        free(client_files);
        free(server_files);
        close(client_socket);
        pthread_exit(NULL);
    }
      //receive the list of files from the client
    if (receive_file_information(client_socket,&client_files,&client_files_capacity,&number_of_files) < 0)
    {
        sprintf(log_message,"Error receiving file list from %s",client_id);
        log_event(log_message);
        ui_set_client_status(client_id, "Error: bad file list", CLIENT_ERROR);
        free(client_files);
        free(server_files);
        close(client_socket);
        pthread_exit(NULL);
    }

    printf("Number of client files: %d\n",number_of_files);
      //only one connection for the same client can synchronize at a time
    pthread_mutex_lock(client_mutex);

    printf("Client %s entered synchronization\n",client_id);
    sprintf(log_message,"Synchronization started: %s",client_id);
    log_event(log_message);
    ui_set_client_status(client_id, "Synchronizing...", CLIENT_SYNCING);
    //read the files already stored on the server for this client
    read_server_metadata(folder,client_id,&server_files,&server_files_capacity,&number_of_server_files);
     //compare both file lists and do the needed uploads and downloads
    if (compare_files(client_files,number_of_files,&server_files,&server_files_capacity,&number_of_server_files,client_socket,folder,client_id,block_size) < 0)
    {
        printf("Error while transferring files\n");

        sprintf(log_message,"Synchronization failed: %s",client_id);
        log_event(log_message);
        ui_set_client_status(client_id, "Synchronization failed", CLIENT_ERROR);

        pthread_mutex_unlock(client_mutex);
        free(client_files);
        free(server_files);
        close(client_socket);
        pthread_exit(NULL);
    }
     //synchronization for this client is finished
    pthread_mutex_unlock(client_mutex);

    printf("Client %s finished synchronization\n",client_id);

    sprintf(log_message,"Synchronization completed: %s",client_id);
    log_event(log_message);
    ui_set_client_status(client_id, "Synchronized", CLIENT_DONE);

    sprintf(log_message,"Client disconnected: %s",client_id);
    log_event(log_message);
    ui_set_client_status(client_id, "Disconnected", CLIENT_DISCONNECTED);
     //clean the memory and close this client connection
    free(client_files);
    free(server_files);
    close(client_socket);
    pthread_exit(NULL);
}

static int g_server_socket_global = -1; // needed so the Ctrl+C handler can close it

// Close the listening socket and log the shutdown before exiting on Ctrl+C
void handle_shutdown(int signal_number)
{
    log_event("Server shutting down");
    if (g_server_socket_global >= 0)
    {
        close(g_server_socket_global);
    }
    exit(0);
}
//main networking thread
void *network_main(void *arg)
{
    (void)arg;
    int server_socket;
    struct sockaddr_in server_address;
     //create the main server socket
    server_socket = create_TCP_socket();
    if (server_socket < 0)
    {
        log_event("Could not create server socket, shutting down");
        exit(1);
    }
   //prepare server address
    make_server_address(&server_address, g_ip, g_port);
    printf("Server address is made\n");
    //bind the socket and start listening
    if (start_listen(server_socket, &server_address) < 0)
    {
        close(server_socket);
        log_event("Could not bind/listen, shutting down");
        exit(1);
    }
     //save the socket so the shutdown function can close it
    g_server_socket_global = server_socket;
    signal(SIGINT, handle_shutdown); // Ctrl+C now closes the socket and logs shutdown instead of just dying

    log_event("Server started");
      // keep accepting clients
    while (1)
    {
        struct client_thread_data *client_data;
        pthread_t client_thread;

        client_data = malloc(sizeof(struct client_thread_data)); // separate socket for each client
        if (client_data == NULL)
        {
            printf("Memory error\n");
            continue;
        }

        client_data->client_socket = accept_client(server_socket); // accept new client connection
        if (client_data->client_socket < 0)                        // error
        {
            free(client_data);
            continue;
        }
        //give this client the server folder and block size
        strcpy(client_data->folder, g_folder);
        client_data->block_size = g_block_size;
         //create a separate thread to handle this client
        if (pthread_create(&client_thread, NULL, handle_client, client_data) != 0) // creat separat thradto handel client
        {
            printf("Error while creating client thread\n");
            close(client_data->client_socket);
            free(client_data);
        }
        else
        {
            //let the thread clean its resources automatically when it finishes
            pthread_detach(client_thread);
            printf("Server returned to wait for another client\n");
        }
    }

    return NULL;
}
//draw text on the GUI
static void draw_text(float x, float y, void *font, const char *text)
{
    glRasterPos2f(x, y);
    for (const char *c = text; *c; c++)
    {
        glutBitmapCharacter(font, *c);
    }
}
//draw the background of a GUI panel
static void draw_panel_bg(float x, float y, float w, float h)
{
    glColor3f(0.15f, 0.16f, 0.19f);
    glBegin(GL_QUADS);
        glVertex2f(x, y);
        glVertex2f(x + w, y);
        glVertex2f(x + w, y + h);
        glVertex2f(x, y + h);
    glEnd();

    glColor3f(0.26f, 0.28f, 0.33f);
    glBegin(GL_LINES);
        glVertex2f(x, y + h);
        glVertex2f(x + w, y + h);
    glEnd();
}
// draw a small circle used for status
static void draw_dot(float cx, float cy, float radius)
{
    glBegin(GL_POLYGON);
    for (int i = 0; i < 20; i++)
    {
        float a = (float)i / 20.0f * 2.0f * 3.14159f;
        glVertex2f(cx + radius * cosf(a), cy + radius * sinf(a));
    }
    glEnd();
}
//choose the color based on the client status
static void client_kind_color(client_ui_kind_t kind)
{
    switch (kind)
    {
        case CLIENT_CONNECTED:    glColor3f(0.55f, 0.58f, 0.62f); break;
        case CLIENT_SYNCING:      glColor3f(0.90f, 0.65f, 0.10f); break;
        case CLIENT_TRANSFER:     glColor3f(0.30f, 0.65f, 0.95f); break; 
        case CLIENT_DONE:         glColor3f(0.20f, 0.70f, 0.30f); break; 
        case CLIENT_ERROR:        glColor3f(0.85f, 0.20f, 0.20f); break; 
        case CLIENT_DISCONNECTED: glColor3f(0.40f, 0.42f, 0.46f); break; 
    }
}
//draw and update everything shown in the GUI window
static void display(void)
{

// copy the GUI data while the mutex is locked then draw using the copied data
    struct tracked_client local_clients[MAX_TRACKED_CLIENTS];
    int local_client_count = 0;
    static char local_log[MAX_LOG_LINES][220];
    int local_log_count;

    pthread_mutex_lock(&g_ui_mutex);
    for (int i = 0; i < MAX_TRACKED_CLIENTS; i++)
    {
        if (g_tracked_clients[i].used)
        {
            local_clients[local_client_count++] = g_tracked_clients[i];
        }
    }
    local_log_count = g_log_count;
    for (int i = 0; i < local_log_count; i++)
    {
        strcpy(local_log[i], g_log_lines[i]);
    }
    pthread_mutex_unlock(&g_ui_mutex);

    glClearColor(0.11f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, g_win_width, 0, g_win_height);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    float top = (float)g_win_height;

    
    glColor3f(0.30f, 0.65f, 0.95f);
    draw_dot(30, top - 24, 6);
    glColor3f(0.95f, 0.95f, 0.98f);
    draw_text(48, top - 28, GLUT_BITMAP_HELVETICA_18, "Distributed Cloud Sync Server");

    char subtitle[260];
    snprintf(subtitle, sizeof(subtitle), "Listening on %s:%d   Storage: %s   Block size: %d",
             g_ip, g_port, g_folder, g_block_size);
    glColor3f(0.65f, 0.68f, 0.72f);
    draw_text(20, top - 48, GLUT_BITMAP_HELVETICA_12, subtitle);

   
    glColor3f(0.20f, 0.70f, 0.30f);
    draw_dot(g_win_width - 110, top - 24, 6);
    glColor3f(0.85f, 0.87f, 0.90f);
    draw_text(g_win_width - 96, top - 28, GLUT_BITMAP_HELVETICA_12, "LISTENING");

    
    float header_bottom = top - 76;
    glColor3f(0.30f, 0.32f, 0.36f);
    glBegin(GL_LINES);
        glVertex2f(20, header_bottom);
        glVertex2f(g_win_width - 20, header_bottom);
    glEnd();

    float content_top = header_bottom - 16;
    float content_bottom = 36;
    float content_h = content_top - content_bottom;

  
    float left_x = 20;
    float left_w = 360;
    draw_panel_bg(left_x, content_bottom, left_w, content_h);

    char clients_heading[60];
    snprintf(clients_heading, sizeof(clients_heading), "Connected clients (%d)", local_client_count);
    glColor3f(0.80f, 0.83f, 0.90f);
    draw_text(left_x + 14, content_top - 22, GLUT_BITMAP_HELVETICA_12, clients_heading);

    float cy = content_top - 48;
    for (int i = 0; i < local_client_count && cy > content_bottom + 10; i++)
    {
        client_kind_color(local_clients[i].kind);
        draw_dot(left_x + 22, cy + 4, 5);

        glColor3f(0.90f, 0.92f, 0.95f);
        draw_text(left_x + 38, cy, GLUT_BITMAP_8_BY_13, local_clients[i].client_id);

        client_kind_color(local_clients[i].kind);
        draw_text(left_x + 38, cy - 16, GLUT_BITMAP_8_BY_13, local_clients[i].status_text);

        cy -= 42;
    }
    if (local_client_count == 0)
    {
        glColor3f(0.45f, 0.47f, 0.50f);
        draw_text(left_x + 14, content_top - 48, GLUT_BITMAP_8_BY_13, "Waiting for clients to connect...");
    }

    float log_x = left_x + left_w + 20;
    float log_w = g_win_width - log_x - 20;
    draw_panel_bg(log_x, content_bottom, log_w, content_h);

    glColor3f(0.80f, 0.83f, 0.90f);
    draw_text(log_x + 14, content_top - 22, GLUT_BITMAP_HELVETICA_12, "Activity log");

    float ly = content_top - 46;
    int max_lines_that_fit = (int)((ly - content_bottom) / 16);
    g_log_sb_page = max_lines_that_fit > 0 ? max_lines_that_fit : 1;
    g_log_sb_max_scroll = local_log_count > max_lines_that_fit ? local_log_count - max_lines_that_fit : 0;
    if (g_log_scroll > g_log_sb_max_scroll) g_log_scroll = g_log_sb_max_scroll;
    if (g_log_scroll < 0) g_log_scroll = 0;
    int end = local_log_count - g_log_scroll;
    int start = end > max_lines_that_fit ? end - max_lines_that_fit : 0;
    glColor3f(0.78f, 0.80f, 0.84f);
    for (int i = start; i < end; i++)
    {
        draw_text(log_x + 14, ly, GLUT_BITMAP_8_BY_13, local_log[i]);
        ly -= 16;
    }
    g_log_sb_x = log_x + log_w - 16;
    g_log_sb_w = 8;
    g_log_sb_top = content_top - 40;
    g_log_sb_bottom = content_bottom + 6;
    if (g_log_sb_max_scroll > 0)
    {
        float track_h = g_log_sb_top - g_log_sb_bottom;
        glColor3f(0.20f, 0.21f, 0.24f);
        glBegin(GL_QUADS);
            glVertex2f(g_log_sb_x, g_log_sb_bottom); glVertex2f(g_log_sb_x + g_log_sb_w, g_log_sb_bottom);
            glVertex2f(g_log_sb_x + g_log_sb_w, g_log_sb_top); glVertex2f(g_log_sb_x, g_log_sb_top);
        glEnd();

        float visible_fraction = (float)max_lines_that_fit / (float)local_log_count;
        if (visible_fraction > 1.0f) visible_fraction = 1.0f;
        float thumb_h = track_h * visible_fraction;
        if (thumb_h < 24) thumb_h = 24;
        if (thumb_h > track_h) thumb_h = track_h;
        float scroll_fraction = (float)g_log_scroll / (float)g_log_sb_max_scroll;
        float thumb_bottom = g_log_sb_bottom + scroll_fraction * (track_h - thumb_h);
        g_log_sb_thumb_bottom = thumb_bottom;
        g_log_sb_thumb_top = thumb_bottom + thumb_h;

        glColor3f(0.45f, 0.48f, 0.55f);
        glBegin(GL_QUADS);
            glVertex2f(g_log_sb_x, thumb_bottom); glVertex2f(g_log_sb_x + g_log_sb_w, thumb_bottom);
            glVertex2f(g_log_sb_x + g_log_sb_w, thumb_bottom + thumb_h); glVertex2f(g_log_sb_x, thumb_bottom + thumb_h);
        glEnd();
    }
    else
    {
        g_log_sb_thumb_top = g_log_sb_thumb_bottom = 0;
    }

    glColor3f(0.45f, 0.47f, 0.50f);
    draw_text(20, 10, GLUT_BITMAP_HELVETICA_10, "Press ESC to quit.");

    glutSwapBuffers();
}
//handle mouse clicks on the log scrollbar

static void mouse(int button, int state, int x, int y)
{
    if (button == GLUT_LEFT_BUTTON && state == GLUT_UP)
    {
        g_log_sb_dragging = 0;
        return;
    }
    if (button != GLUT_LEFT_BUTTON || state != GLUT_DOWN) return;
    float gl_x = (float)x;
    float gl_y = (float)(g_win_height - y); 

    if (g_log_sb_max_scroll > 0 &&
        gl_x >= g_log_sb_x - 4 && gl_x <= g_log_sb_x + g_log_sb_w + 4 &&
        gl_y >= g_log_sb_bottom && gl_y <= g_log_sb_top)
    {
        if (gl_y >= g_log_sb_thumb_bottom && gl_y <= g_log_sb_thumb_top)
        {
            g_log_sb_dragging = 1;
            g_log_sb_drag_offset = gl_y - g_log_sb_thumb_bottom;
        }
        else if (gl_y > g_log_sb_thumb_top)
        {
            g_log_scroll += g_log_sb_page;
            if (g_log_scroll > g_log_sb_max_scroll) g_log_scroll = g_log_sb_max_scroll;
        }
        else
        {
            g_log_scroll -= g_log_sb_page;
            if (g_log_scroll < 0) g_log_scroll = 0;
        }
    }
}
// handle dragging the log scrollbar
static void motion(int x, int y)
{
    (void)x;
    if (!g_log_sb_dragging) return;
    float gl_y = (float)(g_win_height - y);
    float track_h = g_log_sb_top - g_log_sb_bottom;
    float thumb_h = g_log_sb_thumb_top - g_log_sb_thumb_bottom;
    float slide = track_h - thumb_h;
    if (slide <= 0 || g_log_sb_max_scroll <= 0) return;
    float new_thumb_bottom = gl_y - g_log_sb_drag_offset;
    float fraction = (new_thumb_bottom - g_log_sb_bottom) / slide;
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    g_log_scroll = (int)(fraction * g_log_sb_max_scroll + 0.5f);
}
// handle mouse wheel scrolling in the activity log
static void mouse_wheel(int wheel, int direction, int x, int y)
{
    (void)wheel; (void)x; (void)y;
    if (direction > 0) g_log_scroll += 3; 
    else g_log_scroll -= 3;
    if (g_log_scroll < 0) g_log_scroll = 0;
    if (g_log_scroll > g_log_sb_max_scroll) g_log_scroll = g_log_sb_max_scroll;
}
// handle keyboard input, mainly ESC to close
static void keyboard(unsigned char key, int x, int y)
{
    (void)x; (void)y;
    if (key == 27)
    {
        exit(0);
    }
}
//update the GUI size when the window is resized
static void reshape(int w, int h)
{
    g_win_width = w;
    g_win_height = h;
    glViewport(0, 0, w, h);
}
//refresh the GUI every short period of time
static void timer_tick(int value)
{
    (void)value;
    glutPostRedisplay();
    glutTimerFunc(150, timer_tick, 0);
}
// start the server network thread and then start the GUI
int main(int argc, char *argv[])
{
    // to make sure that The configuration file is passed when running the server
    if (argc != 2)
    {
        printf("Please enter the configuration file\n");
        return 1;
    }

    // Read server info
    if (read_configuration(argv[1], g_ip, &g_port, g_folder, &g_block_size) < 0) // if there is an error in the file
    {
        return 1;
    }

    // to make sure everything was read correctly
    printf("Server configuration loaded\n");
    printf("Server IP: %s\n", g_ip);
    printf("Port: %d\n", g_port);
    printf("Storage directory: %s\n", g_folder);
    printf("Block size: %d\n", g_block_size);

    pthread_t net_thread;
    if (pthread_create(&net_thread, NULL, network_main, NULL) != 0)
    {
        printf("Error while starting network thread\n");
        return 1;
    }
    pthread_detach(net_thread);

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(g_win_width, g_win_height);
    glutCreateWindow("Distributed Cloud Sync Server");
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutMouseFunc(mouse);
    glutMotionFunc(motion);
    glutMouseWheelFunc(mouse_wheel);
    glutKeyboardFunc(keyboard);
    glutTimerFunc(150, timer_tick, 0);

    glutMainLoop();
    return 0;
}
