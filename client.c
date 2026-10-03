#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <utime.h>
#include <math.h>
#include <pthread.h>
#include <GL/freeglut.h>

#define INITIAL_CAPACITY 8

struct file_info
{
    char filename[100];
    long file_size;
    long modification_time;
};

typedef enum { ROW_NEVER_SYNCED, ROW_RUNNING, ROW_SYNCED, ROW_ERROR } row_state_t; // state of a client row in the GUI

struct client_row // stores the state of one client row in the GUI
{
    int used;
    char client_id[50];
    char folder[100];
    row_state_t state;
    char status_text[100];
    int selected;        
    time_t last_sync;
};

#define MAX_CLIENTS 50
static struct client_row g_clients[MAX_CLIENTS];
static int g_client_count = 0;
#define MAX_LOG_LINES 300
static char g_log_lines[MAX_LOG_LINES][240];
static int g_log_count = 0;
static int g_log_scroll = 0;
static int g_log_sb_dragging = 0;
static float g_log_sb_drag_offset = 0;
static float g_log_sb_x = 0, g_log_sb_w = 0, g_log_sb_top = 0, g_log_sb_bottom = 0;
static float g_log_sb_thumb_top = 0, g_log_sb_thumb_bottom = 0;
static int g_log_sb_max_scroll = 0;
static int g_log_sb_page = 1;

// this mutex protects both the g_clients array and the g_log_lines ring buffer, which are shared state that can be updated by multiple threads (the main GUI thread and the per-client sync threads)
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;

static char g_ip[50];
static int g_port;
static int g_block_size;
static int g_sync_interval = 10;
static int g_auto_sync_on = 0; 
static int g_win_width = 1180;
static int g_win_height = 700;

typedef enum { FIELD_NONE, FIELD_ID, FIELD_FOLDER } focus_field_t;
static focus_field_t g_focused_field = FIELD_NONE;
static char g_input_id[50] = "";
static char g_input_folder[100] = "";

// main logic start 

struct file_info *grow_file_array(struct file_info *files, int *capacity)
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

int is_safe_name(char *name)
{
    if (name[0] == '\0') return 0;
    if (strstr(name, "..") != NULL) return 0;
    if (strchr(name, '/') != NULL) return 0;
    return 1;
}

int create_TCP_socket()
{
    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket < 0)
    {
        printf("Error while creating socket\n");
        return -1;
    }
    return client_socket;
}

int connect_to_server(int client_socket, char *ip, int port)
{
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = inet_addr(ip);
    if (connect(client_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        return -1;
    }
    return 1;
}

int write_all(int socket, char *message, int size)
{
    int total = 0;
    int bytes_written;
    while (total < size)
    {
        bytes_written = write(socket, message + total, size - total);
        if (bytes_written <= 0) return -1;
        total = total + bytes_written;
    }
    return 1;
}

int receive_line(int server_socket, char *message, int max_size)
{
    int total = 0;
    int bytes_read;
    char one_char;
    while (total < max_size - 1)
    {
        bytes_read = read(server_socket, &one_char, 1);
        if (bytes_read <= 0) return -1;
        if (one_char == '\n') break;
        message[total] = one_char;
        total++;
    }
    message[total] = '\0';
    return total;
}

int send_client_id(int server_socket, char *client_id)
{
    char message[60];
    sprintf(message, "%s\n", client_id);
    if (write_all(server_socket, message, strlen(message)) < 0) return -1;
    return 1;
}

int scan_directory(char *folder, struct file_info **files_ptr, int *capacity, int *number_of_files)
{
    DIR *directory;
    struct dirent *entry;
    struct stat file_stat;
    char file_path[300];
    struct file_info *files = *files_ptr;
    *number_of_files = 0;
    directory = opendir(folder);
    if (directory == NULL) return -1;
    while ((entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        strcpy(file_path, folder);
        strcat(file_path, "/");
        strcat(file_path, entry->d_name);
        if (stat(file_path, &file_stat) < 0) continue; // to check size, mtime & file type
        if (!S_ISREG(file_stat.st_mode)) continue; // skip if not a regular file
        if (*number_of_files >= *capacity)
        {
            struct file_info *bigger = grow_file_array(files, capacity);
            if (bigger == NULL) continue;
            files = bigger;
            *files_ptr = bigger;
        }
        strcpy(files[*number_of_files].filename, entry->d_name);
        files[*number_of_files].file_size = file_stat.st_size;
        files[*number_of_files].modification_time = file_stat.st_mtime;
        (*number_of_files)++;
    }
    closedir(directory);
    return 1;
}

int send_file_list(int server_socket, struct file_info files[], int number_of_files)
{
    char line[250];
    int i;
    for (i = 0; i < number_of_files; i++)
    {
        sprintf(line, "%s %ld %ld\n", files[i].filename, files[i].file_size, files[i].modification_time);
        if (write_all(server_socket, line, strlen(line)) < 0) return -1;
    }
    if (write_all(server_socket, "END\n", strlen("END\n")) < 0) return -1;
    return 1;
}

int send_file(int server_socket, char *folder, char *filename, long file_size, int block_size)
{
    FILE *file;
    char file_path[300];
    char buffer[block_size];
    long total_sent = 0;
    int bytes_read;
    strcpy(file_path, folder);
    strcat(file_path, "/");
    strcat(file_path, filename);
    file = fopen(file_path, "rb");
    if (file == NULL) return -1;
    while (total_sent < file_size)
    {
        bytes_read = fread(buffer, 1, block_size, file);
        if (bytes_read <= 0) { fclose(file); return -1; }
        if (write_all(server_socket, buffer, bytes_read) < 0) { fclose(file); return -1; }
        total_sent += bytes_read;
    }
    fclose(file);
    return 1;
}

int receive_file(int server_socket, char *folder, char *filename, long file_size, int block_size)
{
    FILE *file;
    char file_path[300];
    char buffer[block_size];
    long total_received = 0;
    int bytes_read;
    int bytes_to_read;
    strcpy(file_path, folder);
    strcat(file_path, "/");
    strcat(file_path, filename);
    file = fopen(file_path, "wb");
    if (file == NULL) return -1;
    while (total_received < file_size)
    {
        bytes_to_read = block_size;
        if (file_size - total_received < block_size) bytes_to_read = file_size - total_received;
        bytes_read = read(server_socket, buffer, bytes_to_read);
        if (bytes_read <= 0) { fclose(file); return -1; }
        if (fwrite(buffer, 1, bytes_read, file) != bytes_read) { fclose(file); return -1; }
        total_received += bytes_read;
    }
    fclose(file);
    return 1;
}

static void ui_log(const char *client_id, const char *message)
{
    time_t current_time = time(NULL);
    struct tm time_info_storage;
    struct tm *time_info = localtime_r(&current_time, &time_info_storage);
    char time_string[20];
    char line[240];
    strftime(time_string, sizeof(time_string), "%H:%M:%S", time_info);
    if (client_id != NULL)
        snprintf(line, sizeof(line), "[%s] [%s] %s", time_string, client_id, message);
    else
        snprintf(line, sizeof(line), "[%s] %s", time_string, message);

    printf("%s\n", line);
    fflush(stdout);

    pthread_mutex_lock(&g_state_mutex);
    if (g_log_count < MAX_LOG_LINES)
    {
        strcpy(g_log_lines[g_log_count++], line);
    }
    else
    {
        for (int i = 1; i < MAX_LOG_LINES; i++) strcpy(g_log_lines[i - 1], g_log_lines[i]);
        strcpy(g_log_lines[MAX_LOG_LINES - 1], line);
    }
    pthread_mutex_unlock(&g_state_mutex);
}

static struct client_row *find_row_locked(const char *client_id)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (g_clients[i].used && strcmp(g_clients[i].client_id, client_id) == 0) return &g_clients[i];
    }
    return NULL;
}

static void set_row_status(const char *client_id, row_state_t state, const char *text)
{
    pthread_mutex_lock(&g_state_mutex);
    struct client_row *row = find_row_locked(client_id);
    if (row != NULL)
    {
        row->state = state;
        strncpy(row->status_text, text, sizeof(row->status_text) - 1);
        row->status_text[sizeof(row->status_text) - 1] = '\0';
        if (state == ROW_SYNCED) row->last_sync = time(NULL);
    }
    pthread_mutex_unlock(&g_state_mutex);
}

struct sync_job
{
    char client_id[50];
    char folder[100];
};

static void run_client_sync(const char *client_id, const char *folder)
{
    struct file_info *local_files;
    int local_files_capacity;
    int number_of_files;
    int client_socket;
    char msg[200];

    set_row_status(client_id, ROW_RUNNING, "Connecting...");

    local_files_capacity = INITIAL_CAPACITY;
    local_files = malloc(local_files_capacity * sizeof(struct file_info));
    if (local_files == NULL)
    {
        set_row_status(client_id, ROW_ERROR, "Out of memory");
        return;
    }

    client_socket = create_TCP_socket();
    if (client_socket < 0)
    {
        set_row_status(client_id, ROW_ERROR, "Could not create socket");
        free(local_files);
        return;
    }

    if (connect_to_server(client_socket, g_ip, g_port) < 0)
    {
        set_row_status(client_id, ROW_ERROR, "Could not connect to server");
        ui_log(client_id, "Could not connect to server");
        close(client_socket);
        free(local_files);
        return;
    }
    ui_log(client_id, "Connected to server");

    if (send_client_id(client_socket, (char *)client_id) < 0)
    {
        set_row_status(client_id, ROW_ERROR, "Failed sending client ID");
        close(client_socket);
        free(local_files);
        return;
    }

    mkdir(folder, 0755); // if the folder doesn't exist, create it

    if (scan_directory((char *)folder, &local_files, &local_files_capacity, &number_of_files) < 0)
    {
        set_row_status(client_id, ROW_ERROR, "Could not read local folder");
        ui_log(client_id, "Could not read local folder");
        close(client_socket);
        free(local_files);
        return;
    }

    snprintf(msg, sizeof(msg), "Synchronization started (%d local files)", number_of_files);
    ui_log(client_id, msg);
    set_row_status(client_id, ROW_RUNNING, "Syncing...");

    if (send_file_list(client_socket, local_files, number_of_files) < 0)
    {
        set_row_status(client_id, ROW_ERROR, "Failed sending file list");
        close(client_socket);
        free(local_files);
        return;
    }


    {
        char line[300];
        char command[20];
        char filename[100];
        long file_size;
        long mod_time;
        int failed = 0;
        int uploaded_count = 0;
        int downloaded_count = 0;
        int transfer_failed_count = 0;

        while (1)
        {
            if (receive_line(client_socket, line, sizeof(line)) < 0)
            {
                ui_log(client_id, "Server disconnected during synchronization");
                failed = 1;
                break;
            }
            if (strcmp(line, "SYNC_COMPLETE") == 0)
            {
                ui_log(client_id, "Synchronization completed");
            
                snprintf(msg, sizeof(msg), "Sync summary for %s: %d uploaded, %d downloaded, %d failed",
                         client_id, uploaded_count, downloaded_count, transfer_failed_count);
                ui_log(client_id, msg);
                break;
            }
            if (sscanf(line, "%19s %99s %ld %ld", command, filename, &file_size, &mod_time) != 4)
            {
                ui_log(client_id, "Received malformed instruction from server");
                failed = 1;
                break;
            }
            if (strcmp(command, "UPLOAD") == 0)
            {
                snprintf(msg, sizeof(msg), "Uploading %s", filename);
                ui_log(client_id, msg);
                set_row_status(client_id, ROW_RUNNING, msg);
                if (send_file(client_socket, (char *)folder, filename, file_size, g_block_size) < 0)
                {
                    snprintf(msg, sizeof(msg), "Upload failed: %s", filename);
                    ui_log(client_id, msg);
                    transfer_failed_count++;
                    failed = 1;
                    break;
                }
                snprintf(msg, sizeof(msg), "%s upload completed", filename);
                ui_log(client_id, msg);
                uploaded_count++;
            }
            else if (strcmp(command, "DOWNLOAD") == 0)
            {
                if (!is_safe_name(filename))
                {
                    ui_log(client_id, "Rejected unsafe file name from server");
                    failed = 1;
                    break;
                }
                snprintf(msg, sizeof(msg), "Downloading %s", filename);
                ui_log(client_id, msg);
                set_row_status(client_id, ROW_RUNNING, msg);
                if (receive_file(client_socket, (char *)folder, filename, file_size, g_block_size) < 0)
                {
                    snprintf(msg, sizeof(msg), "Download failed: %s", filename);
                    ui_log(client_id, msg);
                    transfer_failed_count++;
                    failed = 1;
                    break;
                }
                {
                    char downloaded_path[300];
                    struct utimbuf times;
                    strcpy(downloaded_path, folder);
                    strcat(downloaded_path, "/");
                    strcat(downloaded_path, filename);
                    times.actime = mod_time;
                    times.modtime = mod_time;
                    utime(downloaded_path, &times);
                }
                snprintf(msg, sizeof(msg), "%s download completed", filename);
                ui_log(client_id, msg);
                downloaded_count++;
            }
            else
            {
                ui_log(client_id, "Received unknown instruction from server");
                failed = 1;
                break;
            }
        }

        close(client_socket);
        free(local_files);

        if (failed)
        {
            set_row_status(client_id, ROW_ERROR, "Synchronization failed");
        }
        else
        {
            set_row_status(client_id, ROW_SYNCED, "Synchronized");
        }
    }
}

static void *sync_thread_main(void *arg)
{
    struct sync_job *job = (struct sync_job *)arg;
    run_client_sync(job->client_id, job->folder);
    free(job);
    return NULL;
}

static void trigger_sync(const char *client_id, const char *folder, row_state_t current_state)
{
    // to avoid do mutliple sync to the same client at the same time
    if (current_state == ROW_RUNNING) return;
   // client job has client id & folder, so we can pass it to the thread
    struct sync_job *job = malloc(sizeof(struct sync_job));
    if (job == NULL) return;
    strncpy(job->client_id, client_id, sizeof(job->client_id) - 1);
    job->client_id[sizeof(job->client_id) - 1] = '\0';
    strncpy(job->folder, folder, sizeof(job->folder) - 1);
    job->folder[sizeof(job->folder) - 1] = '\0';
    pthread_t t;
    if (pthread_create(&t, NULL, sync_thread_main, job) == 0)
    {   // once the thread is done it will free its resources
        pthread_detach(t);
    }
    else
    {
        free(job);
    }
}

static void draw_text(float x, float y, void *font, const char *text)
{
    glRasterPos2f(x, y);
    for (const char *c = text; *c; c++) glutBitmapCharacter(font, *c);
}

static void draw_panel_bg(float x, float y, float w, float h)
{
    glColor3f(0.15f, 0.16f, 0.19f);
    glBegin(GL_QUADS);
        glVertex2f(x, y); glVertex2f(x + w, y); glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
    glColor3f(0.26f, 0.28f, 0.33f);
    glBegin(GL_LINES);
        glVertex2f(x, y + h); glVertex2f(x + w, y + h);
    glEnd();
}

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

static void row_state_color(row_state_t state)
{
    switch (state)
    {
        case ROW_NEVER_SYNCED: glColor3f(0.55f, 0.58f, 0.62f); break;
        case ROW_RUNNING:      glColor3f(0.90f, 0.65f, 0.10f); break;
        case ROW_SYNCED:       glColor3f(0.20f, 0.70f, 0.30f); break;
        case ROW_ERROR:        glColor3f(0.85f, 0.20f, 0.20f); break;
    }
}

// layout stuff
#define TOPBAR_Y_FROM_TOP   96
#define ADDBAR_H            42
#define ID_FIELD_X          20
#define ID_FIELD_W          170
#define FOLDER_FIELD_X      206
#define FOLDER_FIELD_W      230
#define ADD_BTN_X           452
#define ADD_BTN_W           140
#define FIELD_H             34
#define ROW_H               40
#define LIST_W              620

static void draw_text_field(float x, float y, float w, float h, const char *value, const char *placeholder, int focused)
{
    if (focused) glColor3f(0.20f, 0.45f, 0.85f); else glColor3f(0.24f, 0.26f, 0.30f);
    glBegin(GL_LINE_LOOP);
        glVertex2f(x, y); glVertex2f(x + w, y); glVertex2f(x + w, y + h); glVertex2f(x, y + h);
    glEnd();
    glColor3f(0.17f, 0.18f, 0.21f);
    glBegin(GL_QUADS);
        glVertex2f(x + 1, y + 1); glVertex2f(x + w - 1, y + 1); glVertex2f(x + w - 1, y + h - 1); glVertex2f(x + 1, y + h - 1);
    glEnd();
    if (value[0] == '\0')
    {
        glColor3f(0.45f, 0.47f, 0.50f);
        draw_text(x + 8, y + h / 2.0f - 5, GLUT_BITMAP_8_BY_13, placeholder);
    }
    else
    {
        glColor3f(0.92f, 0.93f, 0.96f);
        draw_text(x + 8, y + h / 2.0f - 5, GLUT_BITMAP_8_BY_13, value);
        if (focused)
        {
            char cursor_probe[52];
            snprintf(cursor_probe, sizeof(cursor_probe), "%s_", value);
            /* blink-free simple caret: draw a trailing underscore */
            draw_text(x + 8, y + h / 2.0f - 5, GLUT_BITMAP_8_BY_13, cursor_probe);
        }
    }
}

static void display(void)
{
    struct client_row local_rows[MAX_CLIENTS];
    int local_row_count = 0;
    static char local_log[MAX_LOG_LINES][240];
    int local_log_count;

    pthread_mutex_lock(&g_state_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (g_clients[i].used) local_rows[local_row_count++] = g_clients[i];
    }
    local_log_count = g_log_count;
    for (int i = 0; i < local_log_count; i++) strcpy(local_log[i], g_log_lines[i]);
    pthread_mutex_unlock(&g_state_mutex);

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
    draw_text(48, top - 28, GLUT_BITMAP_HELVETICA_18, "Distributed Cloud Sync");

    char subtitle[220];
    snprintf(subtitle, sizeof(subtitle), "Server %s:%d   Block size: %d   Auto-sync: %s (every %ds)",
             g_ip, g_port, g_block_size, g_auto_sync_on ? "ON" : "OFF", g_sync_interval);
    glColor3f(0.65f, 0.68f, 0.72f);
    draw_text(20, top - 48, GLUT_BITMAP_HELVETICA_12, subtitle);

    float toggle_x = g_win_width - 190, toggle_y = top - 36, toggle_w = 170, toggle_h = 26;
    if (g_auto_sync_on) glColor3f(0.20f, 0.55f, 0.30f); else glColor3f(0.30f, 0.32f, 0.36f);
    glBegin(GL_QUADS);
        glVertex2f(toggle_x, toggle_y); glVertex2f(toggle_x + toggle_w, toggle_y);
        glVertex2f(toggle_x + toggle_w, toggle_y + toggle_h); glVertex2f(toggle_x, toggle_y + toggle_h);
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
    draw_text(toggle_x + 14, toggle_y + toggle_h / 2.0f - 5, GLUT_BITMAP_8_BY_13,
              g_auto_sync_on ? "AUTO-SYNC: ON" : "AUTO-SYNC: OFF");

    float addbar_top = top - TOPBAR_Y_FROM_TOP;
    glColor3f(0.75f, 0.78f, 0.85f);
    draw_text(20, addbar_top + ADDBAR_H - 6, GLUT_BITMAP_HELVETICA_12, "Add a client:");

    float field_y = addbar_top;
    draw_text_field(ID_FIELD_X, field_y, ID_FIELD_W, FIELD_H, g_input_id, "client id", g_focused_field == FIELD_ID);
    draw_text_field(FOLDER_FIELD_X, field_y, FOLDER_FIELD_W, FIELD_H, g_input_folder, "local folder", g_focused_field == FIELD_FOLDER);

    glColor3f(0.20f, 0.45f, 0.85f);
    glBegin(GL_QUADS);
        glVertex2f(ADD_BTN_X, field_y); glVertex2f(ADD_BTN_X + ADD_BTN_W, field_y);
        glVertex2f(ADD_BTN_X + ADD_BTN_W, field_y + FIELD_H); glVertex2f(ADD_BTN_X, field_y + FIELD_H);
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
    draw_text(ADD_BTN_X + 14, field_y + FIELD_H / 2.0f - 5, GLUT_BITMAP_HELVETICA_12, "+ ADD CLIENT");

    float sync_sel_x = ADD_BTN_X + ADD_BTN_W + 20, sync_sel_w = 150;
    glColor3f(0.20f, 0.55f, 0.35f);
    glBegin(GL_QUADS);
        glVertex2f(sync_sel_x, field_y); glVertex2f(sync_sel_x + sync_sel_w, field_y);
        glVertex2f(sync_sel_x + sync_sel_w, field_y + FIELD_H); glVertex2f(sync_sel_x, field_y + FIELD_H);
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
    draw_text(sync_sel_x + 10, field_y + FIELD_H / 2.0f - 5, GLUT_BITMAP_8_BY_13, "SYNC SELECTED");

    float sync_all_x = sync_sel_x + sync_sel_w + 14, sync_all_w = 110;
    glColor3f(0.20f, 0.45f, 0.85f);
    glBegin(GL_QUADS);
        glVertex2f(sync_all_x, field_y); glVertex2f(sync_all_x + sync_all_w, field_y);
        glVertex2f(sync_all_x + sync_all_w, field_y + FIELD_H); glVertex2f(sync_all_x, field_y + FIELD_H);
    glEnd();
    glColor3f(1.0f, 1.0f, 1.0f);
    draw_text(sync_all_x + 14, field_y + FIELD_H / 2.0f - 5, GLUT_BITMAP_8_BY_13, "SYNC ALL");

    float header_bottom = addbar_top - 14;
    glColor3f(0.30f, 0.32f, 0.36f);
    glBegin(GL_LINES);
        glVertex2f(20, header_bottom); glVertex2f(g_win_width - 20, header_bottom);
    glEnd();

    float content_top = header_bottom - 16;
    float content_bottom = 12;
    float content_h = content_top - content_bottom;

    float list_x = 20;
    draw_panel_bg(list_x, content_bottom, LIST_W, content_h);

    char list_heading[60];
    snprintf(list_heading, sizeof(list_heading), "Clients (%d)", local_row_count);
    glColor3f(0.80f, 0.83f, 0.90f);
    draw_text(list_x + 14, content_top - 22, GLUT_BITMAP_HELVETICA_12, list_heading);

    float ry = content_top - 50;
    for (int i = 0; i < local_row_count && ry > content_bottom + 10; i++)
    {
        float cb_x = list_x + 16, cb_y = ry - 4, cb_s = 14;
        glColor3f(0.5f, 0.52f, 0.56f);
        glBegin(GL_LINE_LOOP);
            glVertex2f(cb_x, cb_y); glVertex2f(cb_x + cb_s, cb_y);
            glVertex2f(cb_x + cb_s, cb_y + cb_s); glVertex2f(cb_x, cb_y + cb_s);
        glEnd();
        if (local_rows[i].selected)
        {
            glColor3f(0.30f, 0.65f, 0.95f);
            glBegin(GL_QUADS);
                glVertex2f(cb_x + 2, cb_y + 2); glVertex2f(cb_x + cb_s - 2, cb_y + 2);
                glVertex2f(cb_x + cb_s - 2, cb_y + cb_s - 2); glVertex2f(cb_x + 2, cb_y + cb_s - 2);
            glEnd();
        }

        row_state_color(local_rows[i].state);
        draw_dot(list_x + 46, ry + 3, 5);

        glColor3f(0.90f, 0.92f, 0.95f);
        char id_folder[160]; 
        snprintf(id_folder, sizeof(id_folder), "%-16s %s", local_rows[i].client_id, local_rows[i].folder);
        draw_text(list_x + 60, ry, GLUT_BITMAP_8_BY_13, id_folder);

        row_state_color(local_rows[i].state);
        draw_text(list_x + 60, ry - 16, GLUT_BITMAP_8_BY_13, local_rows[i].status_text);

        float rb_x = list_x + LIST_W - 90, rb_y = ry - 10, rb_w = 74, rb_h = 26;
        if (local_rows[i].state == ROW_RUNNING) glColor3f(0.35f, 0.37f, 0.40f); else glColor3f(0.20f, 0.45f, 0.85f);
        glBegin(GL_QUADS);
            glVertex2f(rb_x, rb_y); glVertex2f(rb_x + rb_w, rb_y);
            glVertex2f(rb_x + rb_w, rb_y + rb_h); glVertex2f(rb_x, rb_y + rb_h);
        glEnd();
        glColor3f(1.0f, 1.0f, 1.0f);
        draw_text(rb_x + 12, rb_y + rb_h / 2.0f - 4, GLUT_BITMAP_8_BY_13, local_rows[i].state == ROW_RUNNING ? "..." : "SYNC");

        ry -= ROW_H;
    }
    if (local_row_count == 0)
    {
        glColor3f(0.45f, 0.47f, 0.50f);
        draw_text(list_x + 14, content_top - 48, GLUT_BITMAP_8_BY_13, "No clients yet -- add one above.");
    }

    float log_x = list_x + LIST_W + 20;
    float log_w = g_win_width - log_x - 20;
    draw_panel_bg(log_x, content_bottom, log_w, content_h);

    glColor3f(0.80f, 0.83f, 0.90f);
    draw_text(log_x + 14, content_top - 22, GLUT_BITMAP_HELVETICA_12, "Activity log (all clients)");

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

    glutSwapBuffers();
}

static void mouse(int button, int state, int x, int y)
{
    if (button == GLUT_LEFT_BUTTON && state == GLUT_UP)
    {
        g_log_sb_dragging = 0;
        return;
    }
    if (button != GLUT_LEFT_BUTTON || state != GLUT_DOWN) return;
    float gx = (float)x;
    float gy = (float)(g_win_height - y);
    float top = (float)g_win_height;

    if (g_log_sb_max_scroll > 0 &&
        gx >= g_log_sb_x - 4 && gx <= g_log_sb_x + g_log_sb_w + 4 &&
        gy >= g_log_sb_bottom && gy <= g_log_sb_top)
    {
        if (gy >= g_log_sb_thumb_bottom && gy <= g_log_sb_thumb_top)
        {
            g_log_sb_dragging = 1;
            g_log_sb_drag_offset = gy - g_log_sb_thumb_bottom;
        }
        else if (gy > g_log_sb_thumb_top)
        {
            g_log_scroll += g_log_sb_page;
            if (g_log_scroll > g_log_sb_max_scroll) g_log_scroll = g_log_sb_max_scroll;
        }
        else
        {
            g_log_scroll -= g_log_sb_page;
            if (g_log_scroll < 0) g_log_scroll = 0;
        }
        return;
    }

    float toggle_x = g_win_width - 190, toggle_y = top - 36, toggle_w = 170, toggle_h = 26;
    if (gx >= toggle_x && gx <= toggle_x + toggle_w && gy >= toggle_y && gy <= toggle_y + toggle_h)
    {
        g_auto_sync_on = !g_auto_sync_on;
        ui_log(NULL, g_auto_sync_on ? "Auto-sync enabled" : "Auto-sync disabled");
        return;
    }

    float addbar_top = top - TOPBAR_Y_FROM_TOP;
    float field_y = addbar_top;

    if (gx >= ID_FIELD_X && gx <= ID_FIELD_X + ID_FIELD_W && gy >= field_y && gy <= field_y + FIELD_H)
    {
        g_focused_field = FIELD_ID;
        return;
    }
    if (gx >= FOLDER_FIELD_X && gx <= FOLDER_FIELD_X + FOLDER_FIELD_W && gy >= field_y && gy <= field_y + FIELD_H)
    {
        g_focused_field = FIELD_FOLDER;
        return;
    }

    if (gx >= ADD_BTN_X && gx <= ADD_BTN_X + ADD_BTN_W && gy >= field_y && gy <= field_y + FIELD_H)
    {
        g_focused_field = FIELD_NONE;
        if (g_input_id[0] == '\0' || g_input_folder[0] == '\0')
        {
            ui_log(NULL, "Enter both a client ID and a local folder before adding");
            return;
        }
        pthread_mutex_lock(&g_state_mutex);
        if (find_row_locked(g_input_id) != NULL)
        {
            pthread_mutex_unlock(&g_state_mutex);
            ui_log(NULL, "A client with that ID is already in the list");
            return;
        }
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) if (!g_clients[i].used) { slot = i; break; }
        if (slot == -1)
        {
            pthread_mutex_unlock(&g_state_mutex);
            ui_log(NULL, "Client list is full");
            return;
        }
        g_clients[slot].used = 1;
        strncpy(g_clients[slot].client_id, g_input_id, sizeof(g_clients[slot].client_id) - 1);
        strncpy(g_clients[slot].folder, g_input_folder, sizeof(g_clients[slot].folder) - 1);
        g_clients[slot].state = ROW_NEVER_SYNCED;
        strcpy(g_clients[slot].status_text, "Never synced -- click SYNC");
        g_clients[slot].selected = 0;
        g_clients[slot].last_sync = 0;
        if (g_client_count < MAX_CLIENTS) g_client_count++;
        char added_msg[180];
        snprintf(added_msg, sizeof(added_msg), "Added client \"%s\" (folder: %s)", g_input_id, g_input_folder);
        pthread_mutex_unlock(&g_state_mutex);
        ui_log(NULL, added_msg);
        g_input_id[0] = '\0';
        g_input_folder[0] = '\0';
        return;
    }

    float sync_sel_x = ADD_BTN_X + ADD_BTN_W + 20, sync_sel_w = 150;
    if (gx >= sync_sel_x && gx <= sync_sel_x + sync_sel_w && gy >= field_y && gy <= field_y + FIELD_H)
    {
        pthread_mutex_lock(&g_state_mutex);
        int any = 0;
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (g_clients[i].used && g_clients[i].selected)
            {
                any = 1;
                trigger_sync(g_clients[i].client_id, g_clients[i].folder, g_clients[i].state);
            }
        }
        pthread_mutex_unlock(&g_state_mutex);
        ui_log(NULL, any ? "Syncing selected clients..." : "No clients selected (tick a checkbox first)");
        return;
    }
    float sync_all_x = sync_sel_x + sync_sel_w + 14, sync_all_w = 110;
    if (gx >= sync_all_x && gx <= sync_all_x + sync_all_w && gy >= field_y && gy <= field_y + FIELD_H)
    {
        pthread_mutex_lock(&g_state_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (g_clients[i].used) trigger_sync(g_clients[i].client_id, g_clients[i].folder, g_clients[i].state);
        }
        pthread_mutex_unlock(&g_state_mutex);
        ui_log(NULL, "Syncing all clients...");
        return;
    }

    float header_bottom = addbar_top - 14;
    float content_top = header_bottom - 16;
    float list_x = 20;
    float ry = content_top - 50;

    pthread_mutex_lock(&g_state_mutex);
    int idx = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (!g_clients[i].used) continue;
        float cb_x = list_x + 16, cb_y = ry - 4, cb_s = 14;
        if (gx >= cb_x - 4 && gx <= cb_x + cb_s + 4 && gy >= cb_y - 4 && gy <= cb_y + cb_s + 4)
        {
            g_clients[i].selected = !g_clients[i].selected;
            pthread_mutex_unlock(&g_state_mutex);
            return;
        }
        float rb_x = list_x + LIST_W - 90, rb_y = ry - 10, rb_w = 74, rb_h = 26;
        if (gx >= rb_x && gx <= rb_x + rb_w && gy >= rb_y && gy <= rb_y + rb_h)
        {
            char cid[50]; char cf[100]; row_state_t st = g_clients[i].state;
            strcpy(cid, g_clients[i].client_id);
            strcpy(cf, g_clients[i].folder);
            pthread_mutex_unlock(&g_state_mutex);
            trigger_sync(cid, cf, st);
            return;
        }
        ry -= ROW_H;
        idx++;
    }
    pthread_mutex_unlock(&g_state_mutex);
    g_focused_field = FIELD_NONE;
}

static void motion(int x, int y)
{
    (void)x;
    if (!g_log_sb_dragging) return;
    float gy = (float)(g_win_height - y);
    float track_h = g_log_sb_top - g_log_sb_bottom;
    float thumb_h = g_log_sb_thumb_top - g_log_sb_thumb_bottom;
    float slide = track_h - thumb_h;
    if (slide <= 0 || g_log_sb_max_scroll <= 0) return;
    float new_thumb_bottom = gy - g_log_sb_drag_offset;
    float fraction = (new_thumb_bottom - g_log_sb_bottom) / slide;
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    g_log_scroll = (int)(fraction * g_log_sb_max_scroll + 0.5f);
}

static void mouse_wheel(int wheel, int direction, int x, int y)
{
    (void)wheel; (void)x; (void)y;
    if (direction > 0) g_log_scroll += 3; 
    else g_log_scroll -= 3;
    if (g_log_scroll < 0) g_log_scroll = 0;
    if (g_log_scroll > g_log_sb_max_scroll) g_log_scroll = g_log_sb_max_scroll;
}

static void keyboard(unsigned char key, int x, int y)
{
    (void)x; (void)y;
    if (key == 27) { exit(0); }

    if (g_focused_field == FIELD_NONE) return;

    char *target = (g_focused_field == FIELD_ID) ? g_input_id : g_input_folder;
    size_t cap = (g_focused_field == FIELD_ID) ? sizeof(g_input_id) : sizeof(g_input_folder);
    size_t len = strlen(target);

    if (key == 8 || key == 127) 
    {
        if (len > 0) target[len - 1] = '\0';
    }
    else if (key == 13 || key == 9) 
    {
        g_focused_field = (g_focused_field == FIELD_ID) ? FIELD_FOLDER : FIELD_NONE;
    }
    else if (key >= 32 && key < 127)
    {
        if (len + 1 < cap)
        {
            target[len] = (char)key;
            target[len + 1] = '\0';
        }
    }
}

static void reshape(int w, int h)
{
    g_win_width = w;
    g_win_height = h;
    glViewport(0, 0, w, h);
}

static void redraw_timer(int value)
{
    (void)value;
    glutPostRedisplay();
    glutTimerFunc(150, redraw_timer, 0);
}

static void autosync_timer(int value)
{
    (void)value;
    if (g_auto_sync_on)
    {
        pthread_mutex_lock(&g_state_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (g_clients[i].used) trigger_sync(g_clients[i].client_id, g_clients[i].folder, g_clients[i].state);
        }
        pthread_mutex_unlock(&g_state_mutex);
    }
    glutTimerFunc(g_sync_interval * 1000, autosync_timer, 0);
}

int read_configuration(char *filename, char *ip, int *port, int *block_size, int *sync_interval)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL)
    {
        printf("Cannot open configuration file\n");
        return -1;
    }
    fscanf(file, "%49s", ip);
    fscanf(file, "%d", port);
    fscanf(file, "%d", block_size);
    fscanf(file, "%d", sync_interval);
    fclose(file);
    return 1;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("Please enter the configuration file\n");
        return 1;
    }
    if (read_configuration(argv[1], g_ip, &g_port, &g_block_size, &g_sync_interval) < 0)
    {
        return 1;
    }
    if (g_sync_interval < 2) g_sync_interval = 2;

    printf("Client configuration loaded\n");
    printf("Server: %s:%d  Block size: %d  Sync interval: %ds\n", g_ip, g_port, g_block_size, g_sync_interval);

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(g_win_width, g_win_height);
    glutCreateWindow("Distributed Cloud Sync");
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutMouseFunc(mouse);
    glutMotionFunc(motion);
    glutMouseWheelFunc(mouse_wheel);
    glutKeyboardFunc(keyboard);
    glutTimerFunc(150, redraw_timer, 0);
    glutTimerFunc(g_sync_interval * 1000, autosync_timer, 0);

    glutMainLoop();
    return 0;
}
