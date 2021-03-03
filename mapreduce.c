#include "mapreduce.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <time.h>

// -------------- Sizes --------------
#define INITIAL_PARTITION_CAPACITY 8
#define INPUT_FILE_BUF_CAP 4

// -------------- Pthread Wrappers --------------
void Pthread_mutex_lock(pthread_mutex_t *mutex){
    int rc = pthread_mutex_lock(mutex);
    assert(rc == 0);
}
void Pthread_mutex_unlock(pthread_mutex_t *mutex){
    int rc = pthread_mutex_unlock(mutex);
    assert(rc == 0);
}
void Pthread_create(pthread_t * __newthread, const pthread_attr_t * __attr, void *(*__start_routine)(void *), void * __arg){
    assert(pthread_create(__newthread, __attr, __start_routine, __arg) == 0);
}
void Pthread_join(pthread_t __th, void **__thread_return){
    assert(pthread_join(__th, __thread_return) == 0);
}
void Pthread_cond_wait(pthread_cond_t * __cond, pthread_mutex_t * __mutex){
    assert(pthread_cond_wait(__cond, __mutex) == 0);
}
void Pthread_cond_signal(pthread_cond_t * __cond){
    assert(pthread_cond_signal(__cond) == 0);
}
void Pthread_cond_broadcast(pthread_cond_t * __cond){
    assert(pthread_cond_broadcast(__cond) == 0);
}

// -------------- Malloc wrapper --------------
void* Malloc(size_t size){
    void* ptr = malloc(size);
    assert(ptr != NULL);
    return ptr;
}

// -------------- Data Structures --------------
typedef struct __keyinfo__ {
    size_t start, getterNext, end;
    char* key;
    struct __keyinfo__* next;
} Keyinfo;

typedef struct __entry__ {
    char *key, *value;
} Entry;

typedef struct __partition__ {
    pthread_mutex_t lock;
    Keyinfo* keyinfo;
    Entry* data;
    size_t capacity; // The real usable capacity is `capacity - 1`, since the last element of `data` should be a null pointer indicating the end of the data array.
    size_t cur_size;
} Partition;

typedef struct __input_bounded_buffer__ {
    pthread_mutex_t lock;
    pthread_cond_t wake_producer, wake_consumer;
    char** file_arr;
    size_t capacity;
    size_t cur_size, cur_cons, cur_prod;
    char finish;
} Input_buffer;

// -------------- Global Data Structures --------------
Input_buffer input_buffer;
Partition* partition_arr;

// -------------- Global Variables --------------
Partitioner current_partitioner;
int num_partitions;
Mapper mapper;
Reducer reducer;

// -------------- Helper Functions -------------- 
int compareEntry(const void *l, const void *r){
    return strcmp(((Entry *)l)->key, ((Entry *)r)->key);
}

// -------------- Library Functions --------------
int MR_partition_expand(Partition* ptn){
    // Needs to hold the lock before entering here
    ptn->capacity <<= 1;
    Entry* tmpEntry = realloc(ptn->data, sizeof(Entry) * ptn->capacity);
    if(tmpEntry == NULL) return -1;
    ptn->data = tmpEntry;
    tmpEntry = tmpEntry + ptn->capacity - 1; // Setting the last `data` entry to be null pointer
    tmpEntry = NULL; 
    return 0;
    // Needs to release the lock afterwards
}

void MR_Emit(char *key, char *value){
    unsigned long partition_number = current_partitioner(key, num_partitions);
    Partition* thisPtn = &partition_arr[partition_number];
    Pthread_mutex_lock(&thisPtn->lock);
    while(thisPtn->capacity - thisPtn->cur_size < 2)
        MR_partition_expand(thisPtn);
    thisPtn->data[(thisPtn->cur_size)].key = strdup(key);
    thisPtn->data[(thisPtn->cur_size)++].value = strdup(value);
    Pthread_mutex_unlock(&thisPtn->lock);
}

void* MR_sort(void* arg){
    Partition* thisPtn = arg;
    if(thisPtn->cur_size == 0) return NULL;
    qsort(thisPtn->data, thisPtn->cur_size, sizeof(Entry), compareEntry);
    return NULL;
}

char* MR_DefaultGetter(char *key, int partition_number){
    Partition* thisPtn = &partition_arr[partition_number];
    Keyinfo* curKin = thisPtn->keyinfo;
    while(curKin && (strcmp(curKin->key, key) != 0))
        curKin = curKin->next;
    if(!curKin) return NULL; // Key is not found in this partition
    if(curKin->getterNext >= curKin->end) return NULL; // all values of this key have been iterated
    return thisPtn->data[(curKin->getterNext)++].value;
}

// MR_DefaultHashPartition is copied directly from https://github.com/remzi-arpacidusseau/ostep-projects/tree/master/concurrency-mapreduce
unsigned long MR_DefaultHashPartition(char *key, int num_partitions) {
    unsigned long hash = 5381;
    int c;
    while ((c = *key++) != '\0')
        hash = hash * 33 + c;
    return hash % num_partitions;
}

void* MR_Mapper(void* arg){
    char* file_name;
    while(1){
        Pthread_mutex_lock(&input_buffer.lock);
        while(input_buffer.cur_size == 0 && input_buffer.finish == 0)
            Pthread_cond_wait(&input_buffer.wake_consumer, &input_buffer.lock);
        if(input_buffer.cur_size == 0 && input_buffer.finish == 1){ 
            // No more input to work on. Aborting.
            Pthread_mutex_unlock(&input_buffer.lock);
            break;
        }
        file_name = strdup(input_buffer.file_arr[input_buffer.cur_cons]);
        free(input_buffer.file_arr[input_buffer.cur_cons]);
        input_buffer.file_arr[input_buffer.cur_cons] = NULL;
        ++input_buffer.cur_cons;
        input_buffer.cur_cons %= input_buffer.capacity;
        --input_buffer.cur_size;
        Pthread_cond_signal(&input_buffer.wake_producer);
        Pthread_mutex_unlock(&input_buffer.lock);
        mapper(file_name);
        free(file_name);
    }
    return NULL;
}

void* MR_Reducer(void* arg){
    Partition* thisPtn = arg;

    // Build Keyinfo first
    thisPtn->keyinfo = Malloc(sizeof(Keyinfo));
    Keyinfo* curKin = thisPtn->keyinfo;
    curKin->start = curKin->getterNext = 0;
    curKin->key = thisPtn->data->key;
    curKin->next = NULL;
    for(int i = 1; i < thisPtn->cur_size; ++i){
        if(strcmp(thisPtn->data[i].key, curKin->key) != 0){
            curKin->end = i;
            curKin->next = Malloc(sizeof(Keyinfo));
            curKin = curKin->next;
            curKin->key = thisPtn->data[i].key;
            curKin->start = curKin->getterNext = i;
            curKin->next = NULL;
        }
    }
    curKin->end = thisPtn->cur_size;

    // Iterate through the keys, Reduce
    curKin = thisPtn->keyinfo;
    while(curKin){
        reducer(curKin->key, MR_DefaultGetter, (Partition*)arg - partition_arr);
        curKin = curKin->next;
    }
    return NULL;
}

void MR_Run(int argc, char *argv[], Mapper map, int num_mappers, Reducer reduce, int num_reducers, Partitioner partition){
    // -------------- 0. Printout current settings. start timer --------------
    fprintf(stderr, "======================================\n");
    fprintf(stderr, "INPUT_FILE_BUF_CAP:\t\t%d\n",       INPUT_FILE_BUF_CAP);
    fprintf(stderr, "Number of Mappers:\t\t%d\n",        num_mappers);
    fprintf(stderr, "Number of Reducers:\t\t%d\n",       num_reducers);
    fprintf(stderr, "Number of input files:\t\t%d\n",    argc - 1);
    // char **fname = argv + 1;
    // while(*fname != NULL){
    //     fprintf(stderr, "Filename:\t\t%s\n",             *fname);
    //     fname++;
    // }
    fprintf(stderr, "--------------------------------------\n");
    clock_t begin = clock();
    fprintf(stderr, "Start time:\t\t%ld\n", begin);

    // -------------- 1. Initialization. Create partitions. --------------
    // Initialize global variables
    num_partitions = num_reducers;
    current_partitioner = partition;
    mapper = map;
    reducer = reduce;

    // Initialize local threads
    pthread_t mapper_thread[num_mappers], sort_thread[num_partitions], reducer_thread[num_reducers];
    
    // Initialize global data structures
        // Partition array
    partition_arr = Malloc(sizeof(Partition) * num_partitions);
            // Each partition in partition array
    for(int i = 0; i < num_partitions; ++i){
        pthread_mutex_init(&partition_arr[i].lock, NULL);
        partition_arr[i].data = Malloc(sizeof(Entry) * INITIAL_PARTITION_CAPACITY);
        partition_arr[i].keyinfo = NULL;
        partition_arr[i].capacity = INITIAL_PARTITION_CAPACITY;
        partition_arr[i].cur_size = 0;
    }
        // Input buffer
    input_buffer.file_arr = Malloc(sizeof(char*) * INPUT_FILE_BUF_CAP);
    input_buffer.capacity = INPUT_FILE_BUF_CAP;
    input_buffer.cur_size = input_buffer.cur_cons = input_buffer.cur_prod = 0;
    input_buffer.finish = 0; // false
    pthread_mutex_init(&input_buffer.lock, NULL);
    pthread_cond_init(&input_buffer.wake_producer, NULL);
    pthread_cond_init(&input_buffer.wake_consumer, NULL);

    // -------------- 2. Create mappers --------------
    for(int i=0; i < num_mappers; ++i)
        Pthread_create(&mapper_thread[i], NULL, MR_Mapper, NULL);

    // -------------- 3. Build input buffer --------------
    // TODO: can implement SJF when putting in files
    for(int i = 1; i < argc; ++i){
        Pthread_mutex_lock(&input_buffer.lock);
        while(input_buffer.cur_size == input_buffer.capacity)
            Pthread_cond_wait(&input_buffer.wake_producer, &input_buffer.lock);
        input_buffer.file_arr[input_buffer.cur_prod] = strdup(argv[i]);
        ++input_buffer.cur_prod;
        input_buffer.cur_prod %= input_buffer.capacity;
        ++input_buffer.cur_size;
        Pthread_cond_broadcast(&input_buffer.wake_consumer);
        Pthread_mutex_unlock(&input_buffer.lock);
    }
    // Finish all input creation. Change the variable `finish` to 1. Broadcast.
    Pthread_mutex_lock(&input_buffer.lock);
    input_buffer.finish = 1; // true
    Pthread_cond_broadcast(&input_buffer.wake_consumer);
    Pthread_mutex_unlock(&input_buffer.lock);

    // -------------- 4. Join mappers, sort partitions --------------
    for(int i = 0; i < num_mappers; ++i)
        Pthread_join(mapper_thread[i], NULL);
    for(int i = 0; i < num_partitions; ++i)
        Pthread_create(&sort_thread[i], NULL, MR_sort, &partition_arr[i]);
    for(int i = 0; i < num_partitions; ++i)
        Pthread_join(sort_thread[i], NULL);

    // -------------- 5. Reducers build Keyinfo and then reduce keys --------------
    for(int i = 0; i < num_reducers; ++i)
        // assume num_reducers == num_partitions
        Pthread_create(&reducer_thread[i], NULL, MR_Reducer, &partition_arr[i]);
    for(int i = 0; i < num_reducers; ++i)
        Pthread_join(reducer_thread[i], NULL);

    // free global data structures
        // Input buffer
    free(input_buffer.file_arr);
        // Partition array
    for(int i = 0; i < num_partitions; ++i){
        Keyinfo* Kin = partition_arr[i].keyinfo, *nxtKin;
        while(Kin){
            nxtKin = Kin->next;
            free(Kin);
            Kin = nxtKin;
        }
        for(int j = 0; j < partition_arr[i].cur_size; ++j){
            free(partition_arr[i].data[j].key);
            free(partition_arr[i].data[j].value);
        }
        free(partition_arr[i].data);
    }
    free(partition_arr);

    // -------------- 0. End timer. Output duration -------------
    clock_t end = clock();
    fprintf(stderr, "End time:\t\t%ld\n", end);
    fprintf(stderr, "Duration in sec:\t%.3f\n", (double)(end - begin) / CLOCKS_PER_SEC);
    fprintf(stderr, "======================================\n");
}
