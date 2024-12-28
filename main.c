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

#define TAG_INIT_FILES    20    // client -> tracker: "Am fișiere X ... + segmente + hash"
#define TAG_INIT_ACK      21    // tracker->client: "Poți începe descărcarea"
#define TAG_WANT_FILE     22    // client -> tracker: "Vreau fișierul <fname>"
#define TAG_FILE_INFO     23    // tracker->client: "Fișierul are N segmente + hash-urile + seeds"
#define TAG_SEG_REQ       24    // client -> peer:   "Vreau segmentul i din fișierul X"
#define TAG_SEG_RSP       25    // peer   -> client: "OK" / "BAD" / etc. (simulare)
#define TAG_FILE_DONE     26    // client -> tracker: "Am descărcat complet fișierul => devin seed"
#define TAG_ALL_DONE      27    // client -> tracker: "Am terminat descărcarea tuturor fișierelor mele"
// Sent by tracker to client when all finished
#define TAG_FINISH        28    

// Contains its hash
typedef struct {
    char hash[HASH_SIZE + 1];
} Segment;

// Name, number of segments and hash for each of them
typedef struct {
    char filename[MAX_FILENAME + 1];
    int  numSegments;
    Segment segments[MAX_CHUNKS];
} File;

// FileConstructor: client's files and wanted ones
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
        if (sscanf(line,"%s %d", array,&segments_count) != 2) {
            fprintf(stderr,"Format error \n");
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
            fprintf(stderr,"Error reading wanted file\n");
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
// For initialization, at first the client reads the inputfile and take information from it
static Client* initClient(int rank)
{
    char file_name[64];
    snprintf(file_name, sizeof(file_name), "in%d.txt", rank);

    FileConstructor* fc = parseFile(file_name);
    if (!fc) 
        return NULL;

    Client* c = (Client*)calloc(1, sizeof(Client));
    DIE(c == NULL,"calloc() failed!\n");
    c->rank = rank;

    c->numFilesHave= fc->numFilesHave;
    memcpy(c->haveFiles, fc->haveFiles, sizeof(fc->haveFiles));
    c->numFilesWant= fc->numFilesWant;
    memcpy(c->wantFiles, fc->wantFiles, sizeof(fc->wantFiles));
    c->downloadFinished = 0;
    c->final = 0;

    free(fc);
    return c;
}

// Save the file in this format "clientR_<filename>"
static void saveFile(Client* c, int fileIdx)
{
    char outName[64];
    snprintf(outName, sizeof(outName), "client%d_%s", c->rank, c->haveFiles[fileIdx].filename);

    FILE* file = fopen(outName,"w");
    if (!file) {
        fprintf(stderr,"Can't open %s\n", outName);
        return;
    }
    // save the ordered list of hashes
    for (int s = 0; s < c->haveFiles[fileIdx].numSegments; s++) {
        fprintf(file,"%s\n", c->haveFiles[fileIdx].segments[s].hash);
    }
    fclose(file);
}

// used by seed or peer. get a SEG_REQ from other clients(asks for segment i from file j)
static void* upload_thread_func(void* arg)
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
            if (segIndex >= 0 && segIndex < c->haveFiles[ok].numSegments) {
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
    // for every wanted file ask the tracker (TAG_WANT_FILE)
    //      get answer TAG_FILE_INFO which contains number of segments and hashes. After that we try the download
    for (int f = 0; f < c->numFilesWant; f++) {
        char wantedName[MAX_FILENAME + 1];
        strncpy(wantedName, c->wantFiles[f], MAX_FILENAME);
        // ask the tracker
        MPI_Send(wantedName, MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, TAG_WANT_FILE, MPI_COMM_WORLD);
        //   -> numSegments, 
        //   -> un array de <hash-ul segmentului> 
        //   -> seedCount, ranks
        MPI_Status st;
        int numSeg;
        MPI_Recv(&numSeg,1,MPI_INT, TRACKER_RANK, TAG_FILE_INFO,MPI_COMM_WORLD, &st);
        // no file found
        if (numSeg <= 0) {
            continue;
        }
        File localFile; 
        memset(&localFile, 0, sizeof(localFile));
        strncpy(localFile.filename, wantedName, MAX_FILENAME);
        localFile.numSegments = numSeg;
        // the hashes
        for (int s=0; s< numSeg; s++) {
            MPI_Recv(localFile.segments[s].hash, HASH_SIZE+1, MPI_CHAR,
                     TRACKER_RANK, TAG_FILE_INFO, MPI_COMM_WORLD, &st);
            }
        // read seedCount
        int seedCount;
        MPI_Recv(&seedCount, 1, MPI_INT, TRACKER_RANK, TAG_FILE_INFO, MPI_COMM_WORLD, &st);

        int seeds[seedCount];
        if (seedCount > 0) {
            MPI_Recv(seeds, seedCount, MPI_INT, TRACKER_RANK, TAG_FILE_INFO, MPI_COMM_WORLD, &st);
        }

        // download segments
        for (int segment = 0; segment < numSeg; segment++) {
            int ok = 0;
            // load seeds in a ciclic way
            for(int seed = 0; seed < seedCount; seed++){
                int srank = seeds[(segment + seed) % seedCount];
                // ask for segments
                MPI_Send(wantedName, MAX_FILENAME + 1, MPI_CHAR, srank, TAG_SEG_REQ, MPI_COMM_WORLD);
                MPI_Send(&segment, 1, MPI_INT, srank, TAG_SEG_REQ, MPI_COMM_WORLD);

                char resp[10];
                MPI_Recv(resp, 10, MPI_CHAR, srank, TAG_SEG_RSP, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                // got a valid response
                if (strcmp(resp, "OK") == 0) {
                    //strcpy(localFile.segments[segment].hash, localFile.segments[segment].hash);
                    ok = 1;
                    break;
                }
            }
            // no complete file
            if (ok == 0) {
                break;
            }
        }
        // check the number of segments
        int finalSegments = 0;
        for(int s = 0; s < numSeg; s++) {
            if (strlen(localFile.segments[s].hash) > 0) {
                finalSegments++;
            }
        }
        // Finalize downloading file
        if (finalSegments == numSeg) {
            // complete, save, add to have list files
            int idxF = f;
            c->haveFiles[idxF] = localFile;
            saveFile(c, idxF);

            // say to the tracker, the client is seed now
            MPI_Send(wantedName, MAX_FILENAME+1, MPI_CHAR, TRACKER_RANK, TAG_FILE_DONE, MPI_COMM_WORLD);
        }
    }

    // TAG_ALL_DONE
    MPI_Send(NULL, 0, MPI_BYTE, TRACKER_RANK,TAG_ALL_DONE,MPI_COMM_WORLD);
    c->downloadFinished = 1;
    return NULL;
}

void tracker(int numtasks, int rank)
{
    // Stocăm info despre fișiere
    // Tracker => filename, numSegments, hashes, seeds
    Tracker tfiles[MAX_FILES];
    int fileCount = 0;

    int doneClients[MAX_CLIENTS];
    memset(doneClients, 0, sizeof(doneClients));

    for(int c = 1; c < numtasks; c++) {
        MPI_Status st;
        int numHave;
        // waits initial mssage of each client which contains the list of files
        MPI_Recv(&numHave, 1, MPI_INT, c, TAG_INIT_FILES, MPI_COMM_WORLD, &st);

        for (int i = 0; i < numHave; i++){
            char fname[MAX_FILENAME+1];
            int segCount;
            MPI_Recv(fname, MAX_FILENAME+1, MPI_CHAR, c, TAG_INIT_FILES, MPI_COMM_WORLD,&st);
            MPI_Recv(&segCount,1, MPI_INT, c, TAG_INIT_FILES, MPI_COMM_WORLD,&st);

            // search
            int found = -1;
            for(int ff = 0; ff < fileCount; ff++) {
                if (strcmp(tfiles[ff].filename, fname) == 0) {
                    found = ff;
                    break;
                }
            }
            if(found < 0) {
                found = fileCount++;
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

            // add c to seeds
            int check = 0;
            for(int sc=0; sc< tfiles[found].seedCount; sc++) {
                if (tfiles[found].seeds[sc] == c) {
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

    // stăm in buclă
    int finishedCount=0;
    while(1){
        MPI_Status st;
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &st);
        int src= st.MPI_SOURCE, tag= st.MPI_TAG;

        if(tag == TAG_WANT_FILE) {
            // client vrea un fișier => trimitem metadata + seeds
            char fname[MAX_FILENAME+1];
            MPI_Recv(fname, MAX_FILENAME+1, MPI_CHAR, src, TAG_WANT_FILE, MPI_COMM_WORLD,&st);

            // search
            int found = -1;
            for (int j = 0; j < fileCount; j++) {
                if(strcmp(tfiles[j].filename, fname)==0){
                    found = j;
                    break;
                }
            }
            if (found < 0) {
                // nu-l avem => trimitem numSeg=0
                int zero = 0;
                MPI_Send(&zero, 1, MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
            } else {
                // trimitem numSegments, hash pt fiecare, seedCount + seeds
                int segCount= tfiles[found].numSegments;
                MPI_Send(&segCount,1,MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                for(int s=0;s< segCount;s++){
                    MPI_Send(tfiles[found].hashes[s],HASH_SIZE+1, MPI_CHAR, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                }
                int scount= tfiles[found].seedCount;
                MPI_Send(&scount,1,MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                if(scount>0){
                    MPI_Send(tfiles[found].seeds, scount, MPI_INT, src, TAG_FILE_INFO, MPI_COMM_WORLD);
                }
            }
        }
        else if(tag == TAG_FILE_DONE) {
            // client becomes seed
            char fname[MAX_FILENAME + 1];
            MPI_Recv(fname, MAX_FILENAME + 1, MPI_CHAR, src, TAG_FILE_DONE, MPI_COMM_WORLD,&st);

            int found = -1;
            for (int ff = 0; ff< fileCount; ff++){
                if(strcmp(tfiles[ff].filename, fname)==0){
                    found=ff;break;
                }
            }
            if(found>=0){
                int dup=0;
                for(int sc=0; sc< tfiles[found].seedCount; sc++){
                    if(tfiles[found].seeds[sc]== src){dup=1;break;}
                }
                if(!dup){
                    tfiles[found].seeds[tfiles[found].seedCount++]= src;
                }
            }
        }
        else if (tag == TAG_ALL_DONE) {
            // client a terminat
            MPI_Recv(NULL,0,MPI_BYTE, src, TAG_ALL_DONE,MPI_COMM_WORLD,&st);
            doneClients[src] = 1;
        }
        else {
            // citim si ignoram
            MPI_Recv(NULL,0,MPI_BYTE, src, tag, MPI_COMM_WORLD,&st);
        }

        // check 
        finishedCount = 0;
        for (int c = 1; c < numtasks; c++) {
            if(doneClients[c]) finishedCount++;
        }
        if (finishedCount == (numtasks - 1)) {
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
    if (!cl) 
        return;
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
    // stop upload
    pthread_join(upload_thread,NULL);
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
