#define STACK_SIZE 1024*1024
#include <time.h>
#include <ucontext.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "queue.h"
#include "sut.h"


bool running = false; 
static struct timespec times = { .tv_sec = 0, .tv_nsec = 100000 };
struct queue QueueReady, QueueWait;
pthread_mutex_t QueueReadyMutex;
pthread_mutex_t QueueWaitMutex;
pthread_t *EXECc;
pthread_t *EXECi;
ucontext_t EXECcMain;
TCB *task = NULL;
TCB *IOtask = NULL;


typedef struct {
    ucontext_t context;   
    char *stack;          
    sut_task_f func;     
    bool task_is_complete; 

    io_operation_t opIO;
    bool pendingIO;        
    char *resultIO;      
    char *fileIO;         
    char *bufIO;       
    int sizeIO;           
    int fileDesc;       
  
} TCB;

typedef enum {
    noneIO,
    openIO,
    readIO,
    writeIO,
    closeIO 
} io_operation_t;


void Cexec() {
    while (running) {
        pthread_mutex_lock(&QueueReadyMutex);

        struct queue_entry *nextNode = queue_peek_front(&QueueReady);

        if (nextNode) {
            queue_pop_head(&QueueReady);

            TCB* nextTask = (TCB*)nextNode->data;
            task = nextTask;

            free(nextNode);
            pthread_mutex_unlock(&QueueReadyMutex);
            printf("Switching context to task at %p\n", (void*)nextTask);
            swapcontext(&EXECcMain, &nextTask->context);

            if (task != NULL && task->task_is_complete) {
                free(task->stack);

                if (task->fileIO != NULL) {
                    free(task->fileIO);
                    task->fileIO = NULL;
                }

                free(task);
                task = NULL;
            }
        } else {
            pthread_mutex_unlock(&QueueReadyMutex);
            nanosleep(&times, NULL);
        }
    }
}


void Iexec() {

    while (running) {

        pthread_mutex_lock(&QueueWaitMutex);

        struct queue_entry *nextNode = queue_peek_front(&QueueWait);

        if (IOtask->opIO == openIO) {
                IOtask->fileDesc = open(IOtask->fileIO, O_RDWR | O_CREAT, 0666);
                IOtask->pendingIO = false;   
    
                pthread_mutex_lock(&QueueReadyMutex);
                queue_insert_tail(&QueueReady, queue_new_node(IOtask));
                pthread_mutex_unlock(&QueueReadyMutex);

                IOtask = NULL;

        if (nextNode) {
            queue_pop_head(&QueueWait);
            IOtask = (TCB*)nextNode->data;
            free(nextNode);
            pthread_mutex_unlock(&QueueWaitMutex);

            }else if (IOtask->opIO == readIO) {
                ssize_t ReadBytes = read(IOtask->fileDesc, IOtask->bufIO, IOtask->sizeIO);

                IOtask->resultIO = (ReadBytes > 0) ? IOtask->bufIO : NULL;
                IOtask->pendingIO = false;

                pthread_mutex_lock(&QueueReadyMutex);
                queue_insert_tail(&QueueReady, queue_new_node(IOtask));
                pthread_mutex_unlock(&QueueReadyMutex);

                IOtask = NULL;

            } else if (IOtask->opIO == writeIO) {
                ssize_t WrittenBytes = write(IOtask->fileDesc, IOtask->bufIO, IOtask->sizeIO);
                IOtask->pendingIO = false;

                pthread_mutex_lock(&QueueReadyMutex);
                queue_insert_tail(&QueueReady, queue_new_node(IOtask));
                pthread_mutex_unlock(&QueueReadyMutex);

                IOtask = NULL;

            } else if (IOtask->opIO == closeIO){
                   if (close(IOtask->fileDesc) == -1) {
                         perror("close");
                    } else {
                pthread_mutex_lock(&QueueReadyMutex);
                queue_insert_tail(&QueueReady, queue_new_node(IOtask));
                pthread_mutex_unlock(&QueueReadyMutex);

                IOtask->pendingIO = false;
                IOtask = NULL;
                    }
            }
        } else {
            pthread_mutex_unlock(&QueueWaitMutex);
            nanosleep(&times, NULL);
        }
    }
}



void sut_init() {
    running = true;

    QueueReady = queue_create();
    QueueWait = queue_create();

    queue_init(&QueueReady);
    queue_init(&QueueWait);

    EXECc = (pthread_t *)malloc(sizeof(pthread_t));
    EXECi = (pthread_t *)malloc(sizeof(pthread_t));

    pthread_mutex_init(&QueueReadyMutex, NULL);
    pthread_mutex_init(&QueueWaitMutex, NULL);

    if (pthread_create(EXECc, NULL, (void *(*)(void *))Cexec, NULL) != 0) {
        fprintf(stderr, "Error creating C-EXEC thread\n");
        return;
    }

    if (pthread_create(EXECi, NULL, (void *(*)(void *))Iexec, NULL) != 0) {
        fprintf(stderr, "Error creating I-EXEC thread\n");
        return;
    }

    getcontext(&EXECcMain);
}


bool sut_create(sut_task_f fn) {
    TCB* NewTask = (TCB*)malloc(sizeof(TCB));
    if (!NewTask) {
        return false;
    }

    NewTask->stack = (char *)malloc(STACK_SIZE);
    if (!NewTask->stack) {
        free(NewTask);
        return false;
    }

    if (getcontext(&NewTask->context) == -1) {
        free(NewTask->stack);
        free(NewTask);
        return false;
    }

    NewTask->func = fn;
    NewTask->task_is_complete = false;
    NewTask->opIO = noneIO;
    NewTask->fileIO = NULL;
    NewTask->bufIO = NULL;
    NewTask->sizeIO = 0;
    NewTask->fileDesc = -1;
    NewTask->pendingIO = false;

    NewTask->context.uc_stack.ss_sp = NewTask->stack;
    NewTask->context.uc_stack.ss_size = STACK_SIZE;
    NewTask->context.uc_stack.ss_flags = 0;
    NewTask->context.uc_link = &EXECcMain;
    makecontext(&NewTask->context, (void(*)())fn, 0);

    struct queue_entry *node = queue_new_node(NewTask);
    if (!node) {
        free(NewTask->stack);
        free(NewTask);
        return false;
    }

    pthread_mutex_lock(&QueueReadyMutex);
    queue_insert_tail(&QueueReady, node);
    pthread_mutex_unlock(&QueueReadyMutex);

    return true;
}

void sut_yield() {
    pthread_mutex_lock(&QueueReadyMutex);

    struct queue_entry *node = queue_new_node(task);
    if (!node) {

        pthread_mutex_unlock(&QueueReadyMutex);
        return;
    }
    queue_insert_tail(&QueueReady, node);

    pthread_mutex_unlock(&QueueReadyMutex);

    if (swapcontext(&task->context, &EXECcMain) == -1) {
    }
}


void sut_exit() {
    pthread_mutex_lock(&QueueReadyMutex);

    task->task_is_complete = true;

    printf("exiting: %p\n ", (void *)task);

    pthread_mutex_unlock(&QueueReadyMutex);

    setcontext(&EXECcMain);
}



int sut_open(char *fname) {
    pthread_mutex_lock(&QueueReadyMutex);
    pthread_mutex_lock(&QueueWaitMutex);

    task->opIO = openIO;
    task->fileIO = strdup(fname);
    task->pendingIO = true;

    queue_insert_tail(&QueueWait, queue_new_node(task));

    pthread_mutex_unlock(&QueueReadyMutex);
    pthread_mutex_unlock(&QueueWaitMutex);

    if (swapcontext(&task->context, &EXECcMain) == -1) {
    }

    return task->fileDesc;
}



void sut_write(int fd, char *buf, int size) {
    pthread_mutex_lock(&QueueReadyMutex);
    pthread_mutex_lock(&QueueWaitMutex);

    task->opIO = writeIO;
    task->fileDesc = fd;
    task->bufIO = buf;
    task->sizeIO = size;
    task->pendingIO = true;

    queue_insert_tail(&QueueWait, queue_new_node(task));

    pthread_mutex_unlock(&QueueReadyMutex);
    pthread_mutex_unlock(&QueueWaitMutex);

    if (swapcontext(&task->context, &EXECcMain) == -1) {
    }
}



void sut_close(int fd) {
    pthread_mutex_lock(&QueueReadyMutex);
    pthread_mutex_lock(&QueueWaitMutex);

    task->opIO = closeIO;
    task->fileDesc = fd;
    task->pendingIO = true;

    queue_insert_tail(&QueueWait, queue_new_node(task));

    pthread_mutex_unlock(&QueueReadyMutex);
    pthread_mutex_unlock(&QueueWaitMutex);

    if (swapcontext(&task->context, &EXECcMain) == -1) {
    }
}



char* sut_read(int fd, char *buf, int size) {
    pthread_mutex_lock(&QueueReadyMutex);
    pthread_mutex_lock(&QueueWaitMutex);

    task->opIO = readIO;
    task->fileDesc = fd;
    task->bufIO = buf;
    task->sizeIO = size;
    task->pendingIO = true;
    task->resultIO = NULL;

    queue_insert_tail(&QueueWait, queue_new_node(task));

    pthread_mutex_unlock(&QueueReadyMutex);
    pthread_mutex_unlock(&QueueWaitMutex);

    if (swapcontext(&task->context, &EXECcMain) == -1) {
    }

    return task->resultIO;
}


void queue_cleanup(struct queue *q) {
    while (!STAILQ_EMPTY(q)) {
        struct queue_entry *entry = STAILQ_FIRST(q);
        STAILQ_REMOVE_HEAD(q, entries);

        TCB *task = (TCB *)entry->data;
        if (task != NULL) {
            if (task->fileIO != NULL) {
                free(task->fileIO);
                task->fileIO = NULL;
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

        if (task != NULL) {
            tasks_completed &= task->task_is_complete;
        }
        if (IOtask != NULL) {
            tasks_completed &= IOtask->task_is_complete;
        }

        if (ready_queue_empty && wait_queue_empty && tasks_completed) {
            break;
        }

        nanosleep(&times, NULL);
    }


    queue_cleanup(&QueueReady);
    queue_cleanup(&QueueWait);

    running = false;

    if (EXECc != NULL) {
        pthread_join(*EXECc, NULL);
        free(EXECc);
        EXECc = NULL;
    }

    if (EXECi != NULL) {
        pthread_join(*EXECi, NULL);
        free(EXECi);
        EXECi = NULL;
    }

    pthread_mutex_destroy(&QueueReadyMutex);
    pthread_mutex_destroy(&QueueWaitMutex);
}