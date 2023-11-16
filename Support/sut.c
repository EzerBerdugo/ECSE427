// ECSE 427 - Assignment 2
// NAME: Ezer Berdugo
// ID: 260850558

#include <ucontext.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>

#include "queue.h"
#include "sut.h"

#define STACK_SIZE 1024*1024

bool running = false; 
static struct timespec timeS = { .tv_sec = 0, .tv_nsec = 100000 };


typedef enum {
    noneIO, 
    writeIO, 
    openIO,  
    readIO,   
    closeIO  
} io_operation_t;

typedef struct {
    ucontext_t ctxt;  
    char *stack;         
    sut_task_f function;      
    bool taskComplete; 
    
    io_operation_t IOop;  
    char *IOfile;            
    char *IObuf;    
    int IOsize;                 
    int file_descriptor;         
    bool IOpend;           
    char *IOreslt;   
} TCB;

struct queue QueueReady, QueueWait;

pthread_mutex_t QueueReadyMutex;
pthread_mutex_t QueueWaitMutex;
pthread_t *ExecC;  
pthread_t *ExecI;

ucontext_t CexecMain;

TCB *currentTask = NULL;
TCB *currentIOTask = NULL;

void Iexec() {

    while (running
) {

        pthread_mutex_lock(&QueueWaitMutex);

        struct queue_entry *NodeNxt = queue_peek_front(&QueueWait);
        if (NodeNxt) {
            queue_pop_head(&QueueWait);
            currentIOTask = (TCB*)NodeNxt->data;
            free(NodeNxt);
            pthread_mutex_unlock(&QueueWaitMutex);
        if (currentIOTask->IOop == openIO) {
                currentIOTask->file_descriptor = open(currentIOTask->IOfile, O_RDWR | O_CREAT, 0666);
                currentIOTask->IOpend = false;   
    
                pthread_mutex_lock(&QueueReadyMutex);
                queue_insert_tail(&QueueReady, queue_new_node(currentIOTask));
                pthread_mutex_unlock(&QueueReadyMutex);

                currentIOTask = NULL;

            }else if (currentIOTask->IOop == readIO) {
                ssize_t ReadBytes = read(currentIOTask->file_descriptor, currentIOTask->IObuf, currentIOTask->IOsize);

                currentIOTask->IOreslt = (ReadBytes > 0) ? currentIOTask->IObuf : NULL;
                currentIOTask->IOpend = false;

                pthread_mutex_lock(&QueueReadyMutex);
                queue_insert_tail(&QueueReady, queue_new_node(currentIOTask));
                pthread_mutex_unlock(&QueueReadyMutex);

                currentIOTask = NULL;

            } else if (currentIOTask->IOop == writeIO) {
                ssize_t WrittenBytes = write(currentIOTask->file_descriptor, currentIOTask->IObuf, currentIOTask->IOsize);
                currentIOTask->IOpend = false;

                pthread_mutex_lock(&QueueReadyMutex);
                queue_insert_tail(&QueueReady, queue_new_node(currentIOTask));
                pthread_mutex_unlock(&QueueReadyMutex);

                currentIOTask = NULL;

            } else if (currentIOTask->IOop == closeIO){
                   if (close(currentIOTask->file_descriptor) == -1) {
                         perror("close");
                    } else {
                pthread_mutex_lock(&QueueReadyMutex);
                queue_insert_tail(&QueueReady, queue_new_node(currentIOTask));
                pthread_mutex_unlock(&QueueReadyMutex);

                currentIOTask->IOpend = false;
                currentIOTask = NULL;
                    }
            }

        } else {
            pthread_mutex_unlock(&QueueWaitMutex);
            nanosleep(&timeS, NULL);
        }
    }
}

void Cexec() {
    while (running) { 
        pthread_mutex_lock(&QueueReadyMutex); 

        struct queue_entry *NodeNxt = queue_peek_front(&QueueReady);
        if (NodeNxt) {

            queue_pop_head(&QueueReady);

            TCB* TaskNxt = (TCB*)NodeNxt->data;
            currentTask = TaskNxt;
            free(NodeNxt);
            pthread_mutex_unlock(&QueueReadyMutex);

            printf("Switching at %p\n", (void*)TaskNxt);

            swapcontext(&CexecMain, &TaskNxt->ctxt);

            if (currentTask != NULL && currentTask->taskComplete) {
                free(currentTask->stack);

                if (currentTask->IOfile != NULL) {
                    free(currentTask->IOfile);
                    currentTask->IOfile = NULL;
                }

                free(currentTask);
                currentTask = NULL;
            }
        } else {
            pthread_mutex_unlock(&QueueReadyMutex);
            nanosleep(&timeS, NULL);
        }
    }
}



bool sut_create(sut_task_f fn) {
    TCB* taskNew = (TCB*)malloc(sizeof(TCB));
    if (!taskNew) {
        return false;
    }

    taskNew->stack = (char *)malloc(STACK_SIZE); 
    if (!taskNew->stack) {
        free(taskNew);
        return false;
    }

    if (getcontext(&taskNew->ctxt) == -1) {
        free(taskNew->stack);
        free(taskNew);
        return false;
    }

    taskNew->function = fn;
    taskNew->taskComplete = false;
    taskNew->IOop = noneIO;
    taskNew->IOfile = NULL;
    taskNew->IObuf = NULL;
    taskNew->IOsize = 0;
    taskNew->file_descriptor = -1;
    taskNew->IOpend = false;

    taskNew->ctxt.uc_stack.ss_sp = taskNew->stack;
    taskNew->ctxt.uc_stack.ss_size = STACK_SIZE;
    taskNew->ctxt.uc_stack.ss_flags = 0;
    taskNew->ctxt.uc_link = &CexecMain;
    makecontext(&taskNew->ctxt, (void(*)())fn, 0);

    struct queue_entry *node = queue_new_node(taskNew);
    if (!node) {
        free(taskNew->stack);
        free(taskNew); 
        return false;
    }

    pthread_mutex_lock(&QueueReadyMutex); 
    queue_insert_tail(&QueueReady, node); 
    pthread_mutex_unlock(&QueueReadyMutex); 

    return true; 
}

void sut_init() {
    running= true;

    QueueReady = queue_create();
    QueueWait = queue_create();

    queue_init(&QueueReady);
    queue_init(&QueueWait);

    ExecC = (pthread_t *)malloc(sizeof(pthread_t));
    ExecI = (pthread_t *)malloc(sizeof(pthread_t));

    pthread_mutex_init(&QueueReadyMutex, NULL);
    pthread_mutex_init(&QueueWaitMutex, NULL);

    if (pthread_create(ExecC, NULL, (void *(*)(void *))Cexec, NULL) != 0) {
        fprintf(stderr, "Error - C-EXEC \n");
        return;
    }

    if (pthread_create(ExecI, NULL, (void *(*)(void *))Iexec, NULL) != 0) {
        fprintf(stderr, "Error - I-EXEC \n");
        return;
    }

    getcontext(&CexecMain);
}



void sut_yield() {
    pthread_mutex_lock(&QueueReadyMutex);

    struct queue_entry *node = queue_new_node(currentTask);
    if (!node) {

        pthread_mutex_unlock(&QueueReadyMutex); 
        return;
    }
    queue_insert_tail(&QueueReady, node); 

    pthread_mutex_unlock(&QueueReadyMutex); 

    if (swapcontext(&currentTask->ctxt, &CexecMain) == -1) {
    }
}




int sut_open(char *fname) {
    pthread_mutex_lock(&QueueReadyMutex);
    pthread_mutex_lock(&QueueWaitMutex);

    currentTask->IOop = openIO;
    currentTask->IOfile = strdup(fname); 
    currentTask->IOpend = true; 

    queue_insert_tail(&QueueWait, queue_new_node(currentTask));

    pthread_mutex_unlock(&QueueReadyMutex);
    pthread_mutex_unlock(&QueueWaitMutex);

    if (swapcontext(&currentTask->ctxt, &CexecMain) == -1) {
    }

    return currentTask->file_descriptor;
}


void sut_exit() {
    pthread_mutex_lock(&QueueReadyMutex);

    currentTask->taskComplete = true;

    printf("Exiting: %p\n ", (void *)currentTask);

    pthread_mutex_unlock(&QueueReadyMutex); 

    setcontext(&CexecMain);
}



void sut_close(int fd) {
    pthread_mutex_lock(&QueueReadyMutex);
    pthread_mutex_lock(&QueueWaitMutex);

    currentTask->IOop = closeIO;
    currentTask->file_descriptor = fd;
    currentTask->IOpend = true;

    queue_insert_tail(&QueueWait, queue_new_node(currentTask));

    pthread_mutex_unlock(&QueueReadyMutex);
    pthread_mutex_unlock(&QueueWaitMutex);

    if (swapcontext(&currentTask->ctxt, &CexecMain) == -1) {
    }
}


void sut_write(int fd, char *buf, int size) {
    pthread_mutex_lock(&QueueReadyMutex);
    pthread_mutex_lock(&QueueWaitMutex);

    currentTask->IOop = writeIO;
    currentTask->file_descriptor = fd;
    currentTask->IObuf = buf; 
    currentTask->IOsize = size;
    currentTask->IOpend = true; 

    queue_insert_tail(&QueueWait, queue_new_node(currentTask));

    pthread_mutex_unlock(&QueueReadyMutex);
    pthread_mutex_unlock(&QueueWaitMutex);

    if (swapcontext(&currentTask->ctxt, &CexecMain) == -1) {
    }
}

char* sut_read(int fd, char *buf, int size) {
    pthread_mutex_lock(&QueueReadyMutex);
    pthread_mutex_lock(&QueueWaitMutex);

    currentTask->IOop = readIO;
    currentTask->file_descriptor = fd;
    currentTask->IObuf = buf;
    currentTask->IOsize = size;
    currentTask->IOpend = true;
    currentTask->IOreslt = NULL;

    queue_insert_tail(&QueueWait, queue_new_node(currentTask));

    pthread_mutex_unlock(&QueueReadyMutex);
    pthread_mutex_unlock(&QueueWaitMutex);

    if (swapcontext(&currentTask->ctxt, &CexecMain) == -1) {
    }

    return currentTask->IOreslt;
}


void queue_cleanup(struct queue *q) {
    while (!STAILQ_EMPTY(q)) { 
        struct queue_entry *entry = STAILQ_FIRST(q); 
        STAILQ_REMOVE_HEAD(q, entries); 

        TCB *task = (TCB *)entry->data; 
        if (task != NULL) {
            if (task->IOfile != NULL) {
                free(task->IOfile); 
                task->IOfile = NULL; 
            }
            if (task->stack != NULL) {
                free(task->stack);
                task->stack = NULL;
            }
            free(task); 
        }

        free(entry);
    }
}



void sut_shutdown() {

    while (true) {
        pthread_mutex_lock(&QueueReadyMutex);
        pthread_mutex_lock(&QueueWaitMutex);
        
        bool ready_queue_empty = STAILQ_EMPTY(&QueueReady);
        bool wait_queue_empty = STAILQ_EMPTY(&QueueWait);
        
        pthread_mutex_unlock(&QueueWaitMutex);
        pthread_mutex_unlock(&QueueReadyMutex);

        bool tasks_completed = true; 

        if (currentTask != NULL) {
            tasks_completed &= currentTask->taskComplete;
        }
        if (currentIOTask != NULL) {
            tasks_completed &= currentIOTask->taskComplete;
        }

        if (ready_queue_empty && wait_queue_empty && tasks_completed) {
            break;
        }

        nanosleep(&timeS, NULL);
    }

    queue_cleanup(&QueueReady);
    queue_cleanup(&QueueWait);

    running= false;

    if (ExecC != NULL) {
        pthread_join(*ExecC, NULL);
        free(ExecC);
        ExecC = NULL;
    }

    if (ExecI != NULL) {
        pthread_join(*ExecI, NULL);
        free(ExecI);
        ExecI = NULL;
    }

    pthread_mutex_destroy(&QueueReadyMutex);
    pthread_mutex_destroy(&QueueWaitMutex);
}