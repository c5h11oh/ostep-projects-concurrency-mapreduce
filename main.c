#include "mapreduce.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// #define NUM_MAPPER 2
// #define NUM_REDUCER 1

void Map(char *file_name) {
    FILE *fp = fopen(file_name, "r");
    assert(fp != NULL);

    char *line = NULL;
    size_t size = 0;
    while (getline(&line, &size, fp) != -1) {
        char *token, *dummy = line;
        while ((token = strsep(&dummy, " \t\n\r")) != NULL) {
            MR_Emit(token, "1");
        }
    }
    free(line);
    fclose(fp);
}

void Reduce(char *key, Getter get_next, int partition_number) {
    int count = 0;
    char *value;
    while ((value = get_next(key, partition_number)) != NULL)
        count++;
    printf("%s %d\n", key, count);
}

static inline void MR_Run_Wrapper(int argc, char *argv[], char* pathname, int j, int k){
    snprintf(pathname, sizeof(char) * 1024, "testresult/out-%03d-%03d.txt", j, k);
    assert(close(1) == 0);
    assert(open(pathname, O_WRONLY | O_CREAT | O_TRUNC) == 1);
    MR_Run(argc, argv, Map, j, Reduce, k, MR_DefaultHashPartition);
}

int main(int argc, char *argv[]) {
    if(argc <= 1){
    	printf("Usage: mapreduce [filenames...]\n");
    	return -1;
    }
    int num_mapper = -1, num_reducer = -1, rv = 0;
    do{
        printf("How many mappers do you want at most? Type an integer [1..100]: ");
        rv = scanf("%d", &num_mapper);
        assert(rv != 0); // no matter how many digits entered, rv equals 1.
    }while( num_mapper < 1 || num_mapper > 100 );

    do{
        printf("How many reducers do you want at most? Type an integer [1..100]: ");
        rv = scanf("%d", &num_reducer);
        assert(rv != 0);
    }while( num_reducer < 1 || num_reducer > 100 );
    
    time_t now;
    time(&now);
    fprintf(stderr, "Current timestamp: %s\n", ctime(&now));
    char* pathname = malloc(sizeof(char) * 1024);
    memset(pathname, 0, 1024);

    // Variate mappers
    assert(close(2) == 0);
    assert(open("testresult/stat-x-map-1-red.txt", O_WRONLY | O_CREAT | O_TRUNC) == 2);
    int i = 1;
    while(i <= num_mapper){
        MR_Run_Wrapper(argc, argv, pathname, i, 1);
        i <<= 1;
    }
    assert(close(2) == 0);
    assert(open("testresult/stat-x-map-M-red.txt", O_WRONLY | O_CREAT | O_TRUNC) == 2);
    i = 1;
    while(i <= num_mapper){
        MR_Run_Wrapper(argc, argv, pathname, i, num_reducer);
        i <<= 1;
    }

    // Variate reducers
    assert(close(2) == 0);
    assert(open("testresult/stat-1-map-x-red.txt", O_WRONLY | O_CREAT | O_TRUNC) == 2);
    i = 1;
    while(i <= num_mapper){
        MR_Run_Wrapper(argc, argv, pathname, 1, i);
        i <<= 1;
    }
    assert(close(2) == 0);
    assert(open("testresult/stat-M-map-x-red.txt", O_WRONLY | O_CREAT | O_TRUNC) == 2);
    i = 1;
    while(i <= num_mapper){
        MR_Run_Wrapper(argc, argv, pathname, num_mapper, i);
        i <<= 1;
    }

    return 0;
}
