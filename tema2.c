#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <mpi.h>

#define LINE_SIZE         300
#define MAX_CHUNKS        100
#define TRACKER_RANK      0
#define MAX_FILES         10
#define MAX_FILENAME      15
#define HASH_SIZE         32
#define MAX_CLIENTS       20

#define DIE(assertion, call_description)                    \
    do {                                                    \
        if (assertion) {                                    \
            fprintf(stderr, "(%s, %d): ",                   \
                    __FILE__, __LINE__);                    \
            perror(call_description);                       \
            exit(EXIT_FAILURE);                             \
        }                                                   \
    } while (0)
// client sends to tracker and init the files with the components
#define TAG_INIT_FILES    20
// the tracker responds and sends to client ACK as a sign that comunication between clients can start(and downloading)
#define TAG_INIT_ACK      21    
// client to tracker, when he wants a certain file
#define TAG_WANT_FILE     22
// tracker to client with information about the files (number of segments, seeds, hashes)
#define TAG_FILE_INFO     23
// between clients when he has a segment request
#define TAG_SEG_REQ       24
// simulate of response (ok, or no)
#define TAG_SEG_RSP       25
// client informs tracker that he is seed now
#define TAG_FILE_DONE     26
// client to tracker, ready with downloading all of his files
#define TAG_ALL_DONE      27
// Sent by tracker to client when all finished
#define TAG_FINISH        28   
#define TAG_WANT_UPDATE   29 

// Contains its hashes
typedef struct {
    char hash[HASH_SIZE + 1];
} Segment;

// Name, number of segments and hashes for each of them
typedef struct {
    char filename[MAX_FILENAME + 1];
    int  numSegments;
    Segment segments[MAX_CHUNKS];
} File;

// FileConstructor: client's owned files and wanted ones
typedef struct {
    int numFilesHave;
    File haveFiles[MAX_FILES];
    int numFilesWant;
    char wantFiles[MAX_FILES][MAX_FILENAME + 1];
} FileConstructor;

typedef struct {
    int rank;
    int numFilesHave;
    File haveFiles[MAX_FILES];
    int numFilesWant;
    char wantFiles[MAX_FILES][MAX_FILENAME + 1];
    // for download thread
    int downloadFinished;
    // final from tracker
    int final;
} Client;

// Files for tracker
typedef struct {
    char filename[MAX_FILENAME + 1];
    int  numSegments;
    char hashes[MAX_CHUNKS][HASH_SIZE + 1];
    // Array of clients' ids that have completed files
    int seeds[MAX_CLIENTS]; 
    int seedCount;
} Tracker;

static void strip_newline(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == '\n' ) {
        s[len - 1] = '\0';
        len--;
    }
}

// Parse in<R>.txt
static FileConstructor* parseFile(const char *file_name)
{
    FILE* file = fopen(file_name,"r");
    if (!file) {
        fprintf(stderr,"Cannot open %s\n", file_name);
        return NULL;
    }
    FileConstructor* file_constructor = (FileConstructor*)calloc(1, sizeof(FileConstructor));
    DIE(file_constructor == NULL,"calloc() failed!\n");
    // number of files that the client have
    char line[LINE_SIZE];
    if(!fgets(line, sizeof(line), file)){
        fprintf(stderr,"Error reading number of files!\n");
        fclose(file); 
        free(file_constructor);
        return NULL;
    }

    strip_newline(line);
    file_constructor->numFilesHave = atoi(line);

    for (int i = 0 ; i < file_constructor->numFilesHave; i++) {
        if (!fgets(line, sizeof(line), file)) {
            fprintf(stderr,"Error! \n");
            fclose(file); 
            free(file_constructor);
            return NULL;
        }
        strip_newline(line);

        char array[MAX_FILENAME + 1];
        int segments_count;
        if (sscanf(line,"%s %d", array,&segments_count) < 2 || sscanf(line,"%s %d", array,&segments_count) > 2) {
            fprintf(stderr,"Format error! \n");
            fclose(file); 
            free(file_constructor);
            return NULL;
        }

        strncpy(file_constructor->haveFiles[i].filename, array, MAX_FILENAME);
        file_constructor->haveFiles[i].numSegments = segments_count;

        for(int s = 0; s < segments_count; s++) {
            if (!fgets(line, sizeof(line), file)) {
                fprintf(stderr,"Error reading segment! \n");
                fclose(file); 
                free(file_constructor);
                return NULL;
            }
            strip_newline(line);
            strncpy(file_constructor->haveFiles[i].segments[s].hash, line, HASH_SIZE);
        }
    }

    // number wanted files
    if(!fgets(line, sizeof(line), file)){
        fprintf(stderr,"Error! \n");
        fclose(file);
        free(file_constructor);
        return NULL;
    }
    strip_newline(line);
    file_constructor->numFilesWant = atoi(line);

    // wanted files
    for (int i = 0; i < file_constructor->numFilesWant; i++) {
        if (!fgets(line,sizeof(line),file)){
            fprintf(stderr,"Error reading wanted files!\n");
            fclose(file); 
            free(file_constructor);
            return NULL;
        }
        strip_newline(line);
        strncpy(file_constructor->wantFiles[i], line, MAX_FILENAME);
    }

    fclose(file);
    return file_constructor;
}
// For initialization, at first the client reads the input file and take information from it
static Client* initClient(int rank)
{
    char file_name[MAX_FILENAME];
    snprintf(file_name, sizeof(file_name), "in%d.txt", rank);

    FileConstructor* fc = parseFile(file_name);
    if (!fc) {
        return NULL;
    }

    Client* c = (Client*)calloc(1, sizeof(Client));
    DIE(c == NULL,"calloc() failed!\n");
    
    c->rank = rank;
    c->numFilesHave = fc->numFilesHave;
    memcpy(c->haveFiles, fc->haveFiles, sizeof(fc->haveFiles));
    c->numFilesWant= fc->numFilesWant;
    memcpy(c->wantFiles, fc->wantFiles, sizeof(fc->wantFiles));
    c->downloadFinished = 0;
    c->final = 0;

    free(fc);
    return c;
}

// Save the file in this format clientR_<filename>
static void saveFile(Client* c, int file_index)
{
    char out_name[64];
    snprintf(out_name, sizeof(out_name), "client%d_%s", c->rank, c->haveFiles[file_index].filename);

    FILE* file = fopen(out_name,"w");
    if (!file) {
        fprintf(stderr,"Can't open %s\n", out_name);
        return;
    }
    // save the ordered list of hashes
    for (int s = 0; s < c->haveFiles[file_index].numSegments; s++) {
        fprintf(file,"%s\n", c->haveFiles[file_index].segments[s].hash);
    }
    fclose(file);
}

// used by seed or peer. get a SEG_REQ from other clients(asks for segment i from file j)
void* upload_thread_func(void* arg)
{
    Client* c = (Client*)arg;
    // we don t have the final signal from tracker
    while (!c->final) {
        // wait for a request
        MPI_Status st;
        int flag = 0;
        // in order to stop blocking the execution
        MPI_Iprobe(MPI_ANY_SOURCE, TAG_SEG_REQ, MPI_COMM_WORLD, &flag, &st);
        if (flag == 0) {
            continue;
        }
        // receave filename and index
        char file_name[MAX_FILENAME + 1];
        int segIndex = -1;

        MPI_Recv(file_name, MAX_FILENAME + 1, MPI_CHAR, st.MPI_SOURCE, TAG_SEG_REQ, MPI_COMM_WORLD, &st);
        MPI_Recv(&segIndex, 1, MPI_INT, st.MPI_SOURCE, TAG_SEG_REQ, MPI_COMM_WORLD, &st);
        // search for the file
        int ok = -1;
        for (int i = 0; i < c->numFilesHave; i++) {
            if (strcmp(c->haveFiles[i].filename, file_name) == 0) {
                ok = i;
                break;
            }
        }
        char resp[6];
        // at first
        strcpy(resp, "NO");
        if (ok != -1) {
            // valid index
            if (segIndex >= 0 && segIndex < c->haveFiles[ok].numSegments && c->haveFiles[ok].segments[segIndex].hash[0] != '\0') {
                // we have index
                strcpy(resp, "OK");
            }
        }
        // Send the response
        MPI_Send(resp, 10, MPI_CHAR, st.MPI_SOURCE, TAG_SEG_RSP, MPI_COMM_WORLD);
    }
    return NULL;
}

static void* download_thread_func(void* arg)
{
    Client* c = (Client*)arg;
    // send to the tracker owned files : number of segments and the hashes
    MPI_Send(&c->numFilesHave, 1, MPI_INT, TRACKER_RANK, TAG_INIT_FILES, MPI_COMM_WORLD);
    for (int i = 0; i < c->numFilesHave; i++) {
        // send filename
        MPI_Send(c->haveFiles[i].filename, MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, TAG_INIT_FILES, MPI_COMM_WORLD);
        // send numSegments
        MPI_Send(&c->haveFiles[i].numSegments, 1, MPI_INT, TRACKER_RANK, TAG_INIT_FILES, MPI_COMM_WORLD);
        // send hash
        for (int s = 0; s < c->haveFiles[i].numSegments; s++) {
            MPI_Send(c->haveFiles[i].segments[s].hash, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, TAG_INIT_FILES, MPI_COMM_WORLD);
        }
    }
    // wait for ACK
    char ack[4];
    MPI_Recv(ack, 4, MPI_CHAR, TRACKER_RANK, TAG_INIT_ACK, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    // confirmation received.
    // for every wanted file ask the tracker (TAG_WANT_FILE) and get answer TAG_FILE_INFO which contains number of segments and hashes. After that we try the download
    for (int f = 0; f < c->numFilesWant; f++) {
        char wantedName[MAX_FILENAME + 1];
        strncpy(wantedName, c->wantFiles[f], MAX_FILENAME);
        // ask the tracker for swarm information, list of seeds/peers
        MPI_Send(wantedName, MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, TAG_WANT_FILE, MPI_COMM_WORLD);
        // receave
        MPI_Status st;
        int numSeg;
        MPI_Recv(&numSeg, 1, MPI_INT, TRACKER_RANK, TAG_FILE_INFO, MPI_COMM_WORLD, &st);
        // no file found
        if (numSeg <= 0) {
            continue;
        }
        // used to simulate the download
        File localFile; 
        memset(&localFile, 0, sizeof(localFile));
        strncpy(localFile.filename, wantedName, MAX_FILENAME);
        localFile.numSegments = numSeg;

        // the hashes  
        for (int s = 0; s < numSeg; s++) {
            MPI_Recv(localFile.segments[s].hash, HASH_SIZE + 1, MPI_CHAR,
                     TRACKER_RANK, TAG_FILE_INFO, MPI_COMM_WORLD, &st);
            }
        // Receive seeds and number of seeds
        int seedCount;
        MPI_Recv(&seedCount, 1, MPI_INT, TRACKER_RANK, TAG_FILE_INFO, MPI_COMM_WORLD, &st);

        int seeds[seedCount];
        if (seedCount > 0) {
            MPI_Recv(seeds, seedCount, MPI_INT, TRACKER_RANK, TAG_FILE_INFO, MPI_COMM_WORLD, &st);
        }
        // contor for downloaded segments
        int counter = 0;
        // download segment
        // actualizez fisierele pe care le am
        c->numFilesHave++;
        strcpy(c->haveFiles[c->numFilesHave-1].filename, localFile.filename);
        c->haveFiles[c->numFilesHave-1].numSegments = localFile.numSegments;
        for (int i = 0; i < localFile.numSegments; i++) {
            c->haveFiles[c->numFilesHave - 1].segments[i].hash[0] = '\0';
        }
        for (int segment = 0; segment < numSeg; segment++) {
            int ok = 0;
            // load seeds/peers in a ciclic way for eficiency
            for(int seed = 0; seed < seedCount; seed++) {
                int srank = seeds[(segment + seed) % seedCount];
                // ask for segment
                MPI_Send(wantedName, MAX_FILENAME + 1, MPI_CHAR, srank, TAG_SEG_REQ, MPI_COMM_WORLD);
                MPI_Send(&segment, 1, MPI_INT, srank, TAG_SEG_REQ, MPI_COMM_WORLD);

                char resp[10];
                MPI_Recv(resp, 10, MPI_CHAR, srank, TAG_SEG_RSP, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                // got a valid response, so increment the counter
                if (strcmp(resp, "OK") == 0) {
                    ok = 1;
                    counter++;
                    strcpy(c->haveFiles[c->numFilesHave - 1].segments[segment].hash, localFile.segments[segment].hash);
                    break;
                }
            }
            // no complete file
            if (ok == 0) {
                break;
            }
            // after each 10 downloaded segments update the swarm
            if (counter % 10 == 0) {
            MPI_Send(wantedName, MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, TAG_WANT_UPDATE, MPI_COMM_WORLD);
            // updated seeds list
            MPI_Recv(&seedCount, 1, MPI_INT, TRACKER_RANK, TAG_FILE_INFO, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            if (seedCount > 0) {
                MPI_Recv(seeds, seedCount, MPI_INT, TRACKER_RANK, TAG_FILE_INFO, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                }
            }
        }
        // check the number of segments
        int final_number = 0;
        for(int s = 0; s < numSeg; s++) {
            if (strlen(localFile.segments[s].hash) > 0) {
                final_number++;
            }
        }
        // Finalize downloading file
        if (final_number == numSeg) {
            // complete, save, add to have list files
            int index = f;
            c->haveFiles[index] = localFile;
            saveFile(c, index);

            // say to the tracker add client to the list
            MPI_Send(wantedName, MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, TAG_FILE_DONE, MPI_COMM_WORLD);
        }
    }

    // TAG_ALL_DONE
    MPI_Send(NULL, 0, MPI_BYTE, TRACKER_RANK, TAG_ALL_DONE,MPI_COMM_WORLD);
    c->downloadFinished = 1;
    return NULL;
}

void tracker(int numtasks, int rank)
{
    Tracker tfiles[MAX_FILES];
    int count = 0;
    int doneClients[MAX_CLIENTS];
    memset(doneClients, 0, sizeof(doneClients));
    // init
    for (int c = 1; c < numtasks; c++) {
        MPI_Status st;
        int numHave;
        // waits initial message of each client which contains the list of owned files
        MPI_Recv(&numHave, 1, MPI_INT, c, TAG_INIT_FILES, MPI_COMM_WORLD, &st);

        for (int i = 0; i < numHave; i++) {
            char fname[MAX_FILENAME + 1];
            int segCount;
            MPI_Recv(fname, MAX_FILENAME + 1, MPI_CHAR, c, TAG_INIT_FILES, MPI_COMM_WORLD, &st);
            MPI_Recv(&segCount, 1, MPI_INT, c, TAG_INIT_FILES, MPI_COMM_WORLD, &st);

            // search for the file
            int found = -1;
            for(int i = 0; i < count; i++) {
                if (strcmp(tfiles[i].filename, fname) == 0) {
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                int new = count++;
                found = new;
                memset(&tfiles[found], 0, sizeof(Tracker));
                strncpy(tfiles[found].filename, fname, MAX_FILENAME);
                tfiles[found].seedCount = 0;
                tfiles[found].numSegments = segCount;
            }
            // hashes
            for (int s = 0; s < segCount; s++) {
                char segHash[HASH_SIZE + 1];
                MPI_Recv(segHash, HASH_SIZE+1, MPI_CHAR, c, TAG_INIT_FILES, MPI_COMM_WORLD,&st);
                // save in tfiles[found]
                strcpy(tfiles[found].hashes[s], segHash);
            }

            // add client to seeds
            int check = 0;
            for(int s = 0; s < tfiles[found].seedCount; s++) {
                if (tfiles[found].seeds[s] == c) {
                    check = 1;
                    break;
                }
            }
            if (check == 0) {
                tfiles[found].seeds[tfiles[found].seedCount++] = c;
            }
        }
        // ack
        char ack[4] = "ACK";
        MPI_Send(ack,4, MPI_CHAR, c, TAG_INIT_ACK, MPI_COMM_WORLD);
    }

    // start analyzing mesages from client
    int finished = 0;
    while (1) {
        MPI_Status st;
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &st);
        int src = st.MPI_SOURCE;
        int tag = st.MPI_TAG;

        if (tag == TAG_WANT_FILE) {
            // identify the file
            char fname[MAX_FILENAME+1];
            MPI_Recv(fname, MAX_FILENAME+1, MPI_CHAR, src, TAG_WANT_FILE, MPI_COMM_WORLD,&st);

            // search 
            int found = -1;
            for (int j = 0; j < count; j++) {
                if (strcmp(tfiles[j].filename, fname) == 0) {
                    found = j;
                    break;
                }
            }
            // don t have it 
            if (found < 0) {
                int zero = 0;
                MPI_Send(&zero, 1, MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
            } else {
                //send
                int segment_count = tfiles[found].numSegments;
                MPI_Send(&segment_count, 1, MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                for (int s = 0; s < segment_count; s++) {
                    MPI_Send(tfiles[found].hashes[s], HASH_SIZE + 1, MPI_CHAR, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                }
                int seeds_count = tfiles[found].seedCount;
                MPI_Send(&seeds_count, 1, MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                if (seeds_count > 0) {
                    MPI_Send(tfiles[found].seeds, seeds_count, MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                }
            }
        }
        else if (tag == TAG_FILE_DONE) {
            // client becomes seed
            char fname[MAX_FILENAME + 1];
            MPI_Recv(fname, MAX_FILENAME + 1, MPI_CHAR, src, TAG_FILE_DONE, MPI_COMM_WORLD, &st);

            int found = -1;
            for (int i = 0; i < count; i++) {
                if (strcmp(tfiles[i].filename, fname) == 0) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                int duplicate = 0;
                for (int sc = 0; sc < tfiles[found].seedCount; sc++) {
                    if(tfiles[found].seeds[sc] == src) {
                        duplicate = 1;
                        break;
                    }
                }
                if (duplicate == 0) {
                    tfiles[found].seeds[tfiles[found].seedCount++] = src;
                }
            }
        } else if (tag == TAG_ALL_DONE) {
            // client is done
            MPI_Recv(NULL, 0, MPI_BYTE, src, TAG_ALL_DONE, MPI_COMM_WORLD, &st);
            doneClients[src] = 1;
        } else if (tag == TAG_WANT_UPDATE) {
            // same as for TAG_WANT_FILE
            char fname[MAX_FILENAME + 1];
            MPI_Recv(fname, MAX_FILENAME + 1, MPI_CHAR, src, TAG_WANT_UPDATE, MPI_COMM_WORLD, &st);

            // check for file
            int found = -1;
            for (int j = 0; j < count; j++) {
                if (strcmp(tfiles[j].filename, fname) == 0) {
                    found = j;
                    break;
                }
            }
            if (found < 0) {
                // send 0 if the file doesn t exist
                int zero = 0;
                MPI_Send(&zero, 1, MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
            } else {
            // actual list of seeds or peers
                int seeds_count = tfiles[found].seedCount;
                MPI_Send(&seeds_count, 1, MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                if (seeds_count > 0) {
                    MPI_Send(tfiles[found].seeds, seeds_count, MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                }
            }
         }
        //  else {
        //     MPI_Recv(NULL, 0, MPI_BYTE, src, tag, MPI_COMM_WORLD, &st);
        // }

        // check , in order to inform the tracker when the process is over
        finished = 0;
        for (int c = 1; c < numtasks; c++) {
            if (doneClients[c]) {
                finished++;
            }
        }
        if (finished == (numtasks - 1)) {
            break;
        }
    }
    // finally from tracker to client
    for (int c = 1; c < numtasks; c++) { 
        MPI_Send(NULL, 0, MPI_BYTE, c, TAG_FINISH, MPI_COMM_WORLD);
    }
}

static void peer(int numtasks, int rank)
{   
    pthread_t download_thread;
    pthread_t upload_thread;
    Client* cl = initClient(rank);
    if (!cl) {
        return;
        }
    pthread_create(&download_thread, NULL, download_thread_func, (void*)cl);
    pthread_create(&upload_thread, NULL, upload_thread_func, (void*)cl);
    // waiting for download
    pthread_join(download_thread, NULL);
    // waiting for final signal from tracker TAG_FINISH
    while (!cl->final) {
        MPI_Status status;
        int flag;
        MPI_Iprobe(TRACKER_RANK, TAG_FINISH, MPI_COMM_WORLD, &flag, &status);
        if (flag) {
            MPI_Recv(NULL, 0, MPI_BYTE, TRACKER_RANK, TAG_FINISH, MPI_COMM_WORLD, &status);
            cl->final = 1;
        }
    }
    pthread_join(upload_thread, NULL);
    free(cl);
}

int main(int argc, char *argv[]) {
    int numtasks, rank;
    
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pt multithreading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK){
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
