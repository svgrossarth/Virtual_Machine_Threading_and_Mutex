#include "VirtualMachine.h"
#include "Machine.h"
#include <unistd.h>
#include <stdint.h>
#include <iostream>
#include <fcntl.h>
#include <vector>
#include <queue>
#include <list>
#include <stdlib.h>
#include <string.h>

using namespace std;

extern "C" {
TVMMainEntry VMLoadModule(const char *module);
void VMUnloadModule(void);
}

typedef struct{
    TVMThreadID id;
    void (*entry)(void *);
    void *param;
    void *stackaddr;
    size_t stacksize;
    SMachineContext cont;
    TVMThreadState state;
    TVMThreadPriority prio;
    int retVal;
    int sleepTicks;
} TCB;

TVMThreadID CurThreadID;

vector<TCB> TCBList;

queue<TVMThreadID> HighPriorityQ;
queue<TVMThreadID> MedPriorityQ;
queue<TVMThreadID> LowPriorityQ;

vector<TVMThreadID> SleepyThreads;

int tickCount;
int tickDur; //How long a tick is

const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 1;

unsigned int numPools = 2; //This will be used to assign pool identifiers

typedef struct{
    void *base;
    TVMMemorySize size;
    bool free;
} MemoryChunk;

typedef struct{
    TVMMemoryPoolID mpID;
    void *base;
    TVMMemorySize size;
    vector<MemoryChunk> memList;
} MemoryPool;

typedef struct{
    TVMMutexID muxID;
    TVMThreadID ownerID;
    bool locked;
    queue<TVMThreadID> highMuxQ;
    queue<TVMThreadID> medMuxQ;
    queue<TVMThreadID> lowMuxQ;
    bool deleted;
} Mux;

vector<Mux> MuxList;

vector<MemoryPool> MemoryPoolList;

MemoryPool SharedPool;

MemoryPool MainPool;

TVMMemorySize VMHeapSize;
TVMMemorySize VMSharedSize;

Mux sharedLock; //The owner of this lock is the next thread to have access to the shared space

void changeMuxOwner(TVMMutexID mutex, TVMThreadID myTurn);

void pushThreadToCorrectQ(TVMThreadID idPushing);

/* The idle thread. This thread is to run only when there are no other threads or all other threads are waiting.*/
void VMIdleThread( void * param){
    MachineEnableSignals();
    ////cout << "\nIdle Thread Is Running!!!\n";
    while(true);
}

/*Once the new thread to be run is determined a context switch happens here so the new thread will become
 * the current thread.*/
void Dispatcher(TVMThreadID newThreadId){
    //cout << "\nDispatcher has been entered\n";
    TVMThreadID oldThread = CurThreadID;
    CurThreadID = newThreadId;
    TCBList[CurThreadID].state = VM_THREAD_STATE_RUNNING;

    //cout << "\nDISPATCHER: RIGHT NOW thread " << oldThread << " with priority "<< TCBList[oldThread].prio << " is going to be switched to thread " << CurThreadID << "with priority " << TCBList[CurThreadID].prio << "\n";
    //cout << "\nDISPATCHER: The queue contains: " << HighPriorityQ.size() << " " << MedPriorityQ.size() << " " << LowPriorityQ.size() << "\n";
    MachineContextSwitch(&(TCBList[oldThread].cont), &(TCBList[CurThreadID].cont));
}


/* When a it is time for a new thread to be scheduled the current thread is compared to theads in the different queues.
 * If the current thread is running and a thread with a higher or equal priority is found then the current thread will
 * be put at the  end of the appropriate queue and the new thread will be scheduled. If the current thread isn't running
 * and no other thread is in any queue then the idle thread will run.*/
void VMSchedule(){
    TVMThreadID currId = CurThreadID;
    /*Checks if thread is currently running.*/
    if(TCBList[currId].state == VM_THREAD_STATE_RUNNING){

        /*High Priority*/
        if(!HighPriorityQ.empty()){
            TVMThreadID next_thread = HighPriorityQ.front();
            HighPriorityQ.pop();
            TCBList[currId].state = VM_THREAD_STATE_READY;
            if(currId != 0) {
              pushThreadToCorrectQ(currId); //this was push to highQ
            }
            Dispatcher(next_thread);
        }

        /*Medium Priority*/
        else if(TCBList[currId].prio < VM_THREAD_PRIORITY_HIGH && !MedPriorityQ.empty()){
            TVMThreadID next_thread = MedPriorityQ.front();
            MedPriorityQ.pop();
            TCBList[currId].state = VM_THREAD_STATE_READY;
            if(currId != 0){
              pushThreadToCorrectQ(currId); //this was push to medQ
            }
            Dispatcher(next_thread);
        }

        /*Low Priority*/
        else if(TCBList[currId].prio < VM_THREAD_PRIORITY_NORMAL && !LowPriorityQ.empty()){
            TVMThreadID next_thread = LowPriorityQ.front();
            LowPriorityQ.pop();
            TCBList[currId].state = VM_THREAD_STATE_READY;
            if(currId != 0) {
              pushThreadToCorrectQ(currId); //this was push to lowQ
            }
            Dispatcher(next_thread);
        }
    }

    /*If thread is not running and thread is found then idle thread will run next.*/
    else{

        /*High Priority*/
        if(!HighPriorityQ.empty()){
            TVMThreadID next_thread = HighPriorityQ.front();
            HighPriorityQ.pop();
            Dispatcher(next_thread);
        }

        /*Medium Priority*/
        else if(!MedPriorityQ.empty()){
            TVMThreadID next_thread = MedPriorityQ.front();
            MedPriorityQ.pop();
            Dispatcher(next_thread);
        }

        /*Low Priority*/
        else if(!LowPriorityQ.empty()){
            TVMThreadID next_thread = LowPriorityQ.front();
            LowPriorityQ.pop();
            Dispatcher(next_thread);
        }

        /*Idel thread*/
        else{
            Dispatcher(0);
        }
    }

}


/*Pushes the thread to the correct Queue based on its priority.*/
void pushThreadToCorrectQ(TVMThreadID idPushing){
    if(TCBList[idPushing].prio == VM_THREAD_PRIORITY_HIGH){
        HighPriorityQ.push(idPushing);
    }
    else if(TCBList[idPushing].prio == VM_THREAD_PRIORITY_NORMAL){
        MedPriorityQ.push(idPushing);
    }
    else if(idPushing != 0){
        LowPriorityQ.push(idPushing);
    }

}


/* Once the alarm has gone off, once each tick, there will be a check to see if any of
 * the sleeping threads to need to be woken up. Then the scheduler will be called. */
void AlarmCallback(void * param){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    tickCount++;

    /*Checking for to see if any threads want to be worken up.*/
    for(unsigned int i = 0; i < SleepyThreads.size(); i++){
        TCBList[SleepyThreads[i]].sleepTicks--;
        if(TCBList[SleepyThreads[i]].sleepTicks == 0){
            TCBList[SleepyThreads[i]].state = VM_THREAD_STATE_READY;
            pushThreadToCorrectQ(SleepyThreads[i]);
            SleepyThreads.erase(SleepyThreads.begin() + i);
            i--; //Decrement i since we lose a member
        }
    }
    VMSchedule();
    MachineResumeSignals(&sigState);
}



/* After IO call has finished the data is recieved and the waiting thread is put to ready
 * and the scheduler is called to schedule a new thread.*/
void IOCallback (void *calldata, int result){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    int IOThreadID = *((int *)calldata);
    TCBList[IOThreadID].state = VM_THREAD_STATE_READY;
    pushThreadToCorrectQ(IOThreadID);
    TCBList[IOThreadID].retVal = result;
    VMSchedule();
    MachineResumeSignals(&sigState);
}


/* When a new thread starts, it goes here first so that there is a container around the
 * function the thread will be running. This allows us to terminate the thread once it does executing
 * its function.*/
void skeleton(void *param){
    MachineEnableSignals();
    int threadID = *((int *)param);
    TCBList[threadID].entry(TCBList[threadID].param);
    VMThreadTerminate(threadID);
}


/*The virtual machine first starts up here.*/
TVMStatus VMStart(int tickms, TVMMemorySize heapsize, TVMMemorySize sharedsize, int argc, char *argv[]){
    /*Initialzing ticks*/
    tickCount = 0;
    tickDur = tickms;
    TVMMainEntry VMMain = VMLoadModule(argv[0]);
    if(VMMain == NULL){
        return VM_STATUS_FAILURE;
    }

    /*Initialzing heap and shared memory*/
    VMHeapSize = heapsize;
    VMSharedSize = sharedsize;
    vector<MemoryChunk> sharedList;
    SharedPool = {0, MachineInitialize(sharedsize), sharedsize, sharedList}; // Pool id 0 is the shared space (NOT on global list of pools)
    MemoryPoolList.push_back(SharedPool);
    void * mainBase = malloc(heapsize);
    TVMMemoryPoolID mainID = VM_MEMORY_POOL_ID_SYSTEM;
    VMMemoryPoolCreate(mainBase, heapsize, &mainID);

    /*Creatings mutex queues*/
    queue<TVMThreadID> highMuxQ;
    queue<TVMThreadID> medMuxQ;
    queue<TVMThreadID> lowMuxQ;
    sharedLock = {0, 0, false, highMuxQ, medMuxQ, lowMuxQ, false};

    /*Initializings and activates idle thread the TCB for the main thread.*/
    TVMThreadID VMIdleThreadId = 0;
    TVMThreadID VMMainThreadId = 1;
    VMThreadCreate(VMIdleThread, NULL, 6400000, 0, &VMIdleThreadId);
    VMThreadActivate(VMIdleThreadId);
    TCB TCBMain = {VMMainThreadId, NULL, NULL, NULL, 0, 0, VM_THREAD_STATE_RUNNING, VM_THREAD_PRIORITY_NORMAL, 0, 0};
    TCBMain.prio = VM_THREAD_PRIORITY_NORMAL;
    TCBList.push_back(TCBMain);
    CurThreadID = 1;

    /*Sets up the alarm*/
    MachineRequestAlarm(tickms*1000, AlarmCallback, NULL);
    MachineEnableSignals();


    VMMain(argc, argv);
    VMUnloadModule();
    VMMemoryPoolDelete(VM_MEMORY_POOL_ID_SYSTEM);
    MachineTerminate();
    return VM_STATUS_SUCCESS;
}


/*Opens a file and causes the current thread to wait for a callback till when the file is opened.
 * A new thread is scheduled.*/
TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(filename == NULL || filedescriptor == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        int IOThreadID = CurThreadID;
        TCBList[IOThreadID].state = VM_THREAD_STATE_WAITING;
        MachineFileOpen(filename, flags, mode, IOCallback, &IOThreadID);
        VMSchedule();
        *filedescriptor = TCBList[IOThreadID].retVal;
        if(*filedescriptor < 0){
            MachineResumeSignals(&sigState);
            return VM_STATUS_FAILURE;
        }
        else{
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
    }
}


/*Seeks with a already opened file and causes the current thread to wait for a callback till when the seeking is over.
 * A new thread is scheduled.*/
TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    int IOThreadID = CurThreadID;
    TCBList[IOThreadID].state = VM_THREAD_STATE_WAITING;
    MachineFileSeek(filedescriptor, offset, whence, IOCallback, &IOThreadID);
    VMSchedule();
    *newoffset = TCBList[IOThreadID].retVal;
    if(*newoffset < 0){
        MachineResumeSignals(&sigState);
        return VM_STATUS_FAILURE;
    }
    else{
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}


/* Reads from an already opened file. If there is room in the shared memory to read the file the current thread will wait for a callback
 * till the reading is done. A new thread is scheduled.*/
TVMStatus VMFileRead(int filedescriptor, void *data, int *length){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);

    if(data == NULL || length == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        int IOThreadID = CurThreadID;
        TCBList[IOThreadID].state = VM_THREAD_STATE_WAITING;
        void *sharedBase;

        if(sharedLock.locked){
            if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_HIGH){
                sharedLock.highMuxQ.push(CurThreadID);
            }
            else if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_NORMAL){
                sharedLock.medMuxQ.push(CurThreadID);
            }
            else{
                sharedLock.lowMuxQ.push(CurThreadID);
            }
            VMSchedule();
            VMMemoryPoolAllocate(0, 512, &sharedBase);
        }
        else if(VMMemoryPoolAllocate(0, 512, &sharedBase) != VM_STATUS_SUCCESS) {
            sharedLock.locked = true;
            if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_HIGH){
                sharedLock.highMuxQ.push(CurThreadID);
            }
            else if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_NORMAL){
                sharedLock.medMuxQ.push(CurThreadID);
            }
            else{
                sharedLock.lowMuxQ.push(CurThreadID);
            }
            VMSchedule();
            VMMemoryPoolAllocate(0, 512, &sharedBase);
        }
        int originalLength = *length;
        int bytesToRead = *length;
        *length = 0;
        while(bytesToRead > 0){
            if(bytesToRead > 512){
                TCBList[IOThreadID].state = VM_THREAD_STATE_WAITING;
                MachineFileRead(filedescriptor, sharedBase, 512, IOCallback, &IOThreadID);
                VMSchedule();
                if(TCBList[IOThreadID].retVal < 0){
                    MachineResumeSignals(&sigState);
                    return VM_STATUS_FAILURE;
                }
                else{
                    *length += TCBList[IOThreadID].retVal;
                }
                memcpy(data, sharedBase, 512);
                data = (uint8_t *)data + 512;
                bytesToRead -= 512;
            }
            else{
                TCBList[IOThreadID].state = VM_THREAD_STATE_WAITING;
                MachineFileRead(filedescriptor, sharedBase, bytesToRead, IOCallback, &IOThreadID);
                VMSchedule();
                if(TCBList[IOThreadID].retVal < 0){
                    MachineResumeSignals(&sigState);
                    return VM_STATUS_FAILURE;
                }
                else{
                    *length += TCBList[IOThreadID].retVal;
                }
                memcpy(data, sharedBase, bytesToRead);
                data = (uint8_t *)data + bytesToRead;
                bytesToRead = 0;
            }
        }
        data = (uint8_t *)data - originalLength;
        VMMemoryPoolDeallocate(0, sharedBase);
        if(sharedLock.highMuxQ.empty() && sharedLock.medMuxQ.empty() && sharedLock.lowMuxQ.empty()){
            sharedLock.locked = false;
        }
        else{
            if(!sharedLock.highMuxQ.empty()){
                TCBList[sharedLock.highMuxQ.front()].state = VM_THREAD_STATE_READY;
                pushThreadToCorrectQ(sharedLock.highMuxQ.front());
                sharedLock.highMuxQ.pop();
            }
            else if(!sharedLock.medMuxQ.empty()){
                TCBList[sharedLock.medMuxQ.front()].state = VM_THREAD_STATE_READY;
                pushThreadToCorrectQ(sharedLock.medMuxQ.front());
                sharedLock.medMuxQ.pop();
            }
            else{
                TCBList[sharedLock.lowMuxQ.front()].state = VM_THREAD_STATE_READY;
                pushThreadToCorrectQ(sharedLock.lowMuxQ.front());
                sharedLock.lowMuxQ.pop();
            }
            VMSchedule();
        }
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

/* Writes to an already opened file. If there is room in the shared memory to write to the file the current thread will wait for a callback
 * till the writing is done. A new thread is scheduled.*/
TVMStatus VMFileWrite(int filedescriptor, void *data, int *length){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);

    if(data == NULL || length == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        int IOThreadID = CurThreadID;
        TCBList[IOThreadID].state = VM_THREAD_STATE_WAITING;
        void *sharedBase;
        if(sharedLock.locked){
            //cout << "sharedLock is locked\n";
            if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_HIGH){
                sharedLock.highMuxQ.push(CurThreadID);
            }
            else if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_NORMAL){
                sharedLock.medMuxQ.push(CurThreadID);
            }
            else{
                sharedLock.lowMuxQ.push(CurThreadID);
            }
            VMSchedule();
            VMMemoryPoolAllocate(0, 512, &sharedBase);
        }
        else if(VMMemoryPoolAllocate(0, 512, &sharedBase) != VM_STATUS_SUCCESS) {
            sharedLock.locked = true;
            if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_HIGH){
                sharedLock.highMuxQ.push(CurThreadID);
            }
            else if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_NORMAL){
                sharedLock.medMuxQ.push(CurThreadID);
            }
            else{
                sharedLock.lowMuxQ.push(CurThreadID);
            }
            VMSchedule();
            VMMemoryPoolAllocate(0, 512, &sharedBase);
        }
        int originalLength = *length;
        int bytesToWrite = *length;
        *length = 0;
        while(bytesToWrite > 0){
            if(bytesToWrite > 512){
                memcpy(sharedBase, data, 512);
                TCBList[IOThreadID].state = VM_THREAD_STATE_WAITING;
                MachineFileWrite(filedescriptor, sharedBase, 512, IOCallback, &IOThreadID);
                VMSchedule();
                if(TCBList[IOThreadID].retVal < 0){
                    MachineResumeSignals(&sigState);
                    return VM_STATUS_FAILURE;
                }
                else{
                    *length += TCBList[IOThreadID].retVal;
                }
                data = (uint8_t *)data + 512;
                bytesToWrite -= 512;
            }
            else{
                memcpy(sharedBase, data, (size_t)bytesToWrite);
                TCBList[IOThreadID].state = VM_THREAD_STATE_WAITING;
                MachineFileWrite(filedescriptor, sharedBase, bytesToWrite, IOCallback, &IOThreadID);
                VMSchedule();
                if(TCBList[IOThreadID].retVal < 0){
                    MachineResumeSignals(&sigState);
                    return VM_STATUS_FAILURE;
                }
                else{
                    *length += TCBList[IOThreadID].retVal;
                }
                data = (uint8_t *)data + bytesToWrite;
                bytesToWrite = 0;
            }
        }
        data = (uint8_t *)data - originalLength;

        VMMemoryPoolDeallocate(0, sharedBase);
        if(sharedLock.highMuxQ.empty() && sharedLock.medMuxQ.empty() && sharedLock.lowMuxQ.empty()){
            sharedLock.locked = false;
        }
        else{
            if(!sharedLock.highMuxQ.empty()){
                TCBList[sharedLock.highMuxQ.front()].state = VM_THREAD_STATE_READY;
                pushThreadToCorrectQ(sharedLock.highMuxQ.front());
                sharedLock.highMuxQ.pop();
            }
            else if(!sharedLock.medMuxQ.empty()){
                TCBList[sharedLock.medMuxQ.front()].state = VM_THREAD_STATE_READY;
                pushThreadToCorrectQ(sharedLock.medMuxQ.front());
                sharedLock.medMuxQ.pop();
            }
            else{
                TCBList[sharedLock.lowMuxQ.front()].state = VM_THREAD_STATE_READY;
                pushThreadToCorrectQ(sharedLock.lowMuxQ.front());
                sharedLock.lowMuxQ.pop();
            }
            VMSchedule();
        }
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMFileClose(int filedescriptor){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);

    int IOThreadID = CurThreadID;
    TCBList[IOThreadID].state = VM_THREAD_STATE_WAITING;

    MachineFileClose(filedescriptor, IOCallback, &IOThreadID);

    VMSchedule();
    if(TCBList[IOThreadID].retVal < 0){
        MachineResumeSignals(&sigState);
        return VM_STATUS_FAILURE;
    }
    else {
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(entry == NULL || tid == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        if(prio != 0){
            if(TCBList.size() > 2){
                for(unsigned int i = 2; i < TCBList.size(); i++){
                    if(TCBList[i].sleepTicks == -1){ //SLEEPTICKS IS WHAT WE WILL USE TO TRACK DEAD THREADS
                        *tid = i;
                        break;
                    }
                }
                *tid = TCBList.size();
            }
            else{
                *tid = 2;
            }
        }
        //SMachineContext cont;
        TVMThreadState state = VM_THREAD_STATE_DEAD;
        void * stackAddr;
        ////cout << "Allocating " << memsize << " bytes from the main memory pool\n";
        VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, memsize, &stackAddr);
        if(*tid == TCBList.size()){
            ////cout << "Creating thread " << *tid << " with priority " << prio <<"\n";
            TCB currThread = {*tid, entry, param, stackAddr, memsize, 0, state, prio, 0, 0};
            currThread.prio = prio;
            TCBList.push_back(currThread);
        }
        else{
            TCBList[*tid] = {*tid, entry, param, stackAddr, memsize, 0, state, prio, 0, 0};
            TCBList[*tid].prio = prio;
        }
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadActivate(TVMThreadID thread){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(thread >= TCBList.size() || thread < 0 || TCBList[thread].sleepTicks == -1){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(TCBList[thread].state != VM_THREAD_STATE_DEAD){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }else if(thread == 0){
        ////cout << "\nActivating the Idle thread \n";
        TCBList[thread].state = VM_THREAD_STATE_READY;
        MachineContextCreate(&(TCBList[thread].cont), VMIdleThread, &(TCBList[thread].id),
                             TCBList[thread].stackaddr, TCBList[thread].stacksize);
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;

    }
    else{
      //cout << "Activating " << thread << endl;
        TCBList[thread].state = VM_THREAD_STATE_READY;
        MachineContextCreate(&(TCBList[thread].cont), skeleton, &(TCBList[thread].id),
                             TCBList[thread].stackaddr, TCBList[thread].stacksize);
        pushThreadToCorrectQ(thread);
        if(TCBList[thread].prio > TCBList[CurThreadID].prio){ //this if is totally new not positive it is correct. But I think it is.
          VMSchedule();
        }
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadTerminate(TVMThreadID thread){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    ////cout << "\nThread "<< thread << " has been terminated\n";
    if(thread >= TCBList.size() || thread < 0 || TCBList[thread].sleepTicks == -1){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(TCBList[thread].state == VM_THREAD_STATE_DEAD){
        ////cout << "\nA DEAD THREAD " << thread << " TRIED TO BE TERMINATED\n";
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else if(TCBList[thread].state == VM_THREAD_STATE_WAITING){
        ////cout << "\nA WAITING THREAD " << thread << " IS ABOUT TO BE TERMINATED\n";
        TCBList[thread].state = VM_THREAD_STATE_DEAD;
        for(unsigned int i = 0; i < SleepyThreads.size(); i++){
            if(SleepyThreads[i] == thread){
                TCBList[thread].sleepTicks = 0;
                SleepyThreads.erase(SleepyThreads.begin() + i);
                break;
            }
        }
        if(!MuxList.empty()){
            for (unsigned int mutex = 0; mutex < MuxList.size(); mutex++) {
                if(MuxList[mutex].ownerID == thread){
                    MuxList[mutex].ownerID = CurThreadID;
                    VMMutexRelease(mutex);
                }
            }
        }
        VMSchedule();
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
    else if(TCBList[thread].state == VM_THREAD_STATE_RUNNING){
        ////cout << "\nA RUNNING THREAD " << thread << " IS ABOUT TO BE TERMINATED\n";
        TCBList[thread].state = VM_THREAD_STATE_DEAD;
        if(!MuxList.empty()){
            for (unsigned int mutex = 0; mutex < MuxList.size(); mutex++) {
                if(MuxList[mutex].ownerID == thread){
                    MuxList[mutex].ownerID = CurThreadID;
                    VMMutexRelease(mutex);
                }
            }
        }
        VMSchedule();
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
    else{
        ////cout << "\nA READY THREAD " << thread << " IS ABOUT TO BE TERMINATED\n";
        TCBList[thread].state = VM_THREAD_STATE_DEAD;
        if(TCBList[thread].prio == VM_THREAD_PRIORITY_HIGH){
            unsigned long qSize = HighPriorityQ.size();
            for(unsigned long i = 0; i < qSize; i++){
                if(HighPriorityQ.front() == thread){
                    HighPriorityQ.pop();
                }
                else{
                    TVMThreadID temp = HighPriorityQ.front();
                    HighPriorityQ.pop();
                    HighPriorityQ.push(temp);
                }
            }
        }
        else if(TCBList[thread].prio == VM_THREAD_PRIORITY_NORMAL){
            unsigned long qSize = MedPriorityQ.size();
            for(unsigned long i = 0; i < qSize; i++){
                if(MedPriorityQ.front() == thread){
                    MedPriorityQ.pop();
                }
                else{
                    TVMThreadID temp = MedPriorityQ.front();
                    MedPriorityQ.pop();
                    MedPriorityQ.push(temp);
                }
            }
        }
        else{
            unsigned long qSize = LowPriorityQ.size();
            for(unsigned long i = 0; i < qSize; i++){
                if(LowPriorityQ.front() == thread){
                    LowPriorityQ.pop();
                }
                else{
                    TVMThreadID temp = LowPriorityQ.front();
                    LowPriorityQ.pop();
                    LowPriorityQ.push(temp);
                }
            }
        }
        if(!MuxList.empty()){
            for (unsigned int mutex = 0; mutex < MuxList.size(); mutex++) {
                if(MuxList[mutex].ownerID == thread){
                    MuxList[mutex].ownerID = CurThreadID;
                    VMMutexRelease(mutex);
                }
            }
        }
        VMSchedule();
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadSleep(TVMTick tick){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(tick == VM_TIMEOUT_INFINITE){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else if(tick == VM_TIMEOUT_IMMEDIATE){
        TCBList[CurThreadID].state = VM_THREAD_STATE_READY;
        pushThreadToCorrectQ(CurThreadID);
        VMSchedule();
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
    else{
        TCBList[CurThreadID].state = VM_THREAD_STATE_WAITING;
        TCBList[CurThreadID].sleepTicks = tick;
        SleepyThreads.push_back(CurThreadID);
        VMSchedule();
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef state){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);

    if(thread >= TCBList.size() || thread < 0 || TCBList[thread].sleepTicks == -1){ //IF THREAD IS DELETED, GIVE ERROR INVALID ID
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(state == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        *state = TCBList[thread].state;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadDelete(TVMThreadID thread){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(thread >= TCBList.size() || thread < 0 || TCBList[thread].sleepTicks == -1){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(TCBList[thread].state != VM_THREAD_STATE_DEAD){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else{
        TCBList[thread].entry = NULL;
        TCBList[thread].param = NULL;
        VMMemoryPoolDeallocate(VM_MEMORY_POOL_ID_SYSTEM, TCBList[thread].stackaddr);
        TCBList[thread].stackaddr = NULL;
        TCBList[thread].stacksize = 0;
        TCBList[thread].state = 0;
        TCBList[thread].prio = 0;
        TCBList[thread].retVal = -1;
        TCBList[thread].sleepTicks = -1;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMTickMS(int *tickmsref){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(tickmsref == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        *tickmsref = tickDur;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMTickCount(TVMTickRef tickref){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(tickref == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        *tickref = tickCount;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadID(TVMThreadIDRef threadref){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(threadref == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        *threadref = CurThreadID;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMMutexCreate(TVMMutexIDRef mutexref){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(mutexref == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        TVMMutexID newMuxID = MuxList.size();
        for(unsigned int i = 0; i < MuxList.size(); i++){
            if(MuxList[i].deleted){
                newMuxID = i;
            }
        }
        ////cout << "Creating mutex " << newMuxID << "\n";
        queue<TVMThreadID> tempHigh;
        queue<TVMThreadID> tempMed;
        queue<TVMThreadID> tempLow;

        Mux newMux = {newMuxID, 0, false, tempHigh, tempMed, tempLow, false};
        if(newMuxID == MuxList.size()){
            MuxList.push_back(newMux);
        }
        else{
            MuxList[newMuxID] = newMux;
        }
        *mutexref = newMuxID;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMMutexDelete(TVMMutexID mutex){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(MuxList.empty() || mutex >= MuxList.size() || MuxList[mutex].deleted){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(MuxList[mutex].ownerID != 0){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        MuxList[mutex].deleted = true;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(MuxList.empty() || mutex >= MuxList.size() || MuxList[mutex].deleted){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(ownerref == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else if(!MuxList[mutex].locked){
        ////cout << "Lock is already locked!\n";
        *ownerref = VM_THREAD_ID_INVALID;
        ////cout << "set ownerref to " << *ownerref << " which should be " << VM_THREAD_ID_INVALID << "\n";
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
    else{
        ////cout << "The owner of the lock is " << MuxList[mutex].ownerID << "\n";
        *ownerref = MuxList[mutex].ownerID;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(MuxList.empty() || mutex >= MuxList.size() || MuxList[mutex].deleted){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(!MuxList[mutex].locked){
        ////cout << "Mutex " << mutex << " is unlocked\n";
        MuxList[mutex].locked = true;
        MuxList[mutex].ownerID = CurThreadID;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
    else{
        ////cout << "Mutex " << mutex << " is locked\n";
        if(timeout == VM_TIMEOUT_IMMEDIATE){
            MachineResumeSignals(&sigState);
            return VM_STATUS_FAILURE;
        }
        else if(timeout == VM_TIMEOUT_INFINITE){
            TCBList[CurThreadID].state = VM_THREAD_STATE_WAITING;
            if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_HIGH){
                MuxList[mutex].highMuxQ.push(CurThreadID);
            }
            else if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_NORMAL){
                MuxList[mutex].medMuxQ.push(CurThreadID);
            }
            else{
                MuxList[mutex].lowMuxQ.push(CurThreadID);
            }
            VMSchedule();
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
        else{
            TCBList[CurThreadID].state = VM_THREAD_STATE_WAITING;
            if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_HIGH){
                MuxList[mutex].highMuxQ.push(CurThreadID);
            }
            else if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_NORMAL){
                MuxList[mutex].medMuxQ.push(CurThreadID);
            }
            else{
                MuxList[mutex].lowMuxQ.push(CurThreadID);
            }
            VMThreadSleep(timeout);
            VMSchedule();
            if(MuxList[mutex].ownerID != CurThreadID){
                if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_HIGH){
                    unsigned long tempSize = MuxList[mutex].highMuxQ.size();
                    for(unsigned int i = 0; i < tempSize; i++){
                        if(MuxList[mutex].highMuxQ.front() == CurThreadID){
                            MuxList[mutex].highMuxQ.pop();
                        }
                        else{
                            TVMThreadID temp = MuxList[mutex].highMuxQ.front();
                            MuxList[mutex].highMuxQ.pop();
                            MuxList[mutex].highMuxQ.push(temp);
                        }
                    }
                }
                else if(TCBList[CurThreadID].prio == VM_THREAD_PRIORITY_NORMAL){
                    ////cout << "Main has failed to acquire the lock\n";
                    unsigned long tempSize = MuxList[mutex].medMuxQ.size();
                    for(unsigned int i = 0; i < tempSize; i++){
                        if(MuxList[mutex].medMuxQ.front() == CurThreadID){
                            MuxList[mutex].medMuxQ.pop();
                            ////cout << "Main is no longer waiting for the lock\n";
                        }
                        else{
                            TVMThreadID temp = MuxList[mutex].medMuxQ.front();
                            MuxList[mutex].medMuxQ.pop();
                            MuxList[mutex].medMuxQ.push(temp);
                        }
                    }
                }
                else{
                    unsigned long tempSize = MuxList[mutex].lowMuxQ.size();
                    for(unsigned int i = 0; i < tempSize; i++){
                        if(MuxList[mutex].lowMuxQ.front() == CurThreadID){
                            MuxList[mutex].lowMuxQ.pop();
                        }
                        else{
                            TVMThreadID temp = MuxList[mutex].lowMuxQ.front();
                            MuxList[mutex].lowMuxQ.pop();
                            MuxList[mutex].lowMuxQ.push(temp);
                        }
                    }
                }
                MachineResumeSignals(&sigState);
                return VM_STATUS_FAILURE;
            }
            else{
                MachineResumeSignals(&sigState);
                return VM_STATUS_SUCCESS;
            }
        }
    }
}

void changeMuxOwner(TVMMutexID mutex, TVMThreadID myTurn){
    ////cout << "Mutex " << mutex << " has been acquired by thread " << myTurn << "\n";
    for(unsigned int i = 0; i < SleepyThreads.size(); i++){
        if(SleepyThreads[i] == myTurn){
            TCBList[myTurn].sleepTicks = 0;
            SleepyThreads.erase(SleepyThreads.begin() + i);
            break;
        }
    }
    MuxList[mutex].ownerID = myTurn;
    TCBList[myTurn].state = VM_THREAD_STATE_READY;
    pushThreadToCorrectQ(myTurn);
}

TVMStatus VMMutexRelease(TVMMutexID mutex){
    ////cout << "Mutex " << mutex << " is trying to be released\n";
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(MuxList.empty() || mutex >= MuxList.size() || MuxList[mutex].deleted){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(MuxList[mutex].ownerID != CurThreadID){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else{
        ////cout << CurThreadID << " is releasing " << mutex << "\n";
        TVMThreadID myTurn = 0;
        if(!MuxList[mutex].highMuxQ.empty()){
            myTurn = MuxList[mutex].highMuxQ.front();
            MuxList[mutex].highMuxQ.pop();
            changeMuxOwner(mutex, myTurn);
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
        else if(!MuxList[mutex].medMuxQ.empty()){
            ////cout << "MedMuxQ is not empty\n";
            myTurn = MuxList[mutex].medMuxQ.front();
            ////cout << "MedMuxQ contains" << myTurn << "\n";
            MuxList[mutex].medMuxQ.pop();
            changeMuxOwner(mutex, myTurn);
            if(TCBList[myTurn].prio > TCBList[CurThreadID].prio){
              VMSchedule();
            }
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
        else if(!MuxList[mutex].lowMuxQ.empty()){
            myTurn = MuxList[mutex].lowMuxQ.front();
            MuxList[mutex].lowMuxQ.pop();
            changeMuxOwner(mutex, myTurn);
            if(TCBList[myTurn].prio > TCBList[CurThreadID].prio){
              VMSchedule();
            }
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
        else{
            MuxList[mutex].ownerID = 0;
            MuxList[mutex].locked = false;
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
    }
}



TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    if(base == NULL || memory == NULL || size == 0){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        if(*memory != VM_MEMORY_POOL_ID_SYSTEM){
            vector<MemoryChunk> temp;
            *memory = numPools;
            MemoryPool TempPool = {*memory, base, size, temp};
            numPools++;
            MemoryPoolList.push_back(TempPool);
        }
        else{
            vector<MemoryChunk> temp;
            MainPool = {*memory, base, size, temp};
            MemoryPoolList.push_back(MainPool);
        }
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    unsigned int elemPos = 0;
    if(memory >= numPools || size == 0 || pointer == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    for(unsigned int i = 0; i <= MemoryPoolList.size(); i++){
        if(i == MemoryPoolList.size()){
            MachineResumeSignals(&sigState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        else if(MemoryPoolList[i].mpID == memory){
            elemPos = i;
            break;
        }
    }

    if(size % 64 != 0){
        size = size + 64 - (size % 64);
    }
    ////cout << "Allocating " << size << " bytes from Memory Pool " << memory <<"\n";

    if(size > MemoryPoolList[elemPos].size){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
    }
    else{
        if(MemoryPoolList[elemPos].memList.empty()){
            ////cout << "Memory pool " << memory << " is empty\n";
            *pointer = MemoryPoolList[elemPos].base;
            MemoryChunk temp = {MemoryPoolList[elemPos].base, size, false};
            MemoryChunk tempBack = {(uint8_t *)MemoryPoolList[elemPos].base + size, MemoryPoolList[elemPos].size - size, true};
            MemoryPoolList[elemPos].memList.push_back(temp);
            MemoryPoolList[elemPos].memList.push_back(tempBack);
            MachineResumeSignals(&sigState);
            return VM_STATUS_SUCCESS;
        }
        else{
            ////cout << "Memory pool " << memory << " is not empty\n";
            for(unsigned int i = 0; i < MemoryPoolList[elemPos].memList.size(); i++){
                if(MemoryPoolList[elemPos].memList[i].free && MemoryPoolList[elemPos].memList[i].size >= size){
                    MemoryChunk temp = {MemoryPoolList[elemPos].memList[i].base, size, false};
                    MemoryPoolList[elemPos].memList.insert(MemoryPoolList[elemPos].memList.begin() + i, temp);
                    MemoryPoolList[elemPos].memList[i+1].base = (uint8_t *)MemoryPoolList[elemPos].memList[i+1].base + size;
                    MemoryPoolList[elemPos].memList[i+1].size -= size;
                    if(MemoryPoolList[elemPos].memList[i+1].size == 0){
                        MemoryPoolList[elemPos].memList.erase(MemoryPoolList[elemPos].memList.begin() + (i + 1));
                    }
                    *pointer = MemoryPoolList[elemPos].memList[i].base;
                    MachineResumeSignals(&sigState);
                    return VM_STATUS_SUCCESS;
                }
            }
            MachineResumeSignals(&sigState);
            return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
        }
    }
}

TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer){
    if(memory != 0) {
        TVMMemorySize tempBytes;
        VMMemoryPoolQuery(memory, &tempBytes);
        ////cout << tempBytes << " bytes in pool " << memory << "\n";
        ////cout << "Deallocating from pool " << memory << "\n";
    }
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    unsigned int elemPos = 0;
    if(memory >= numPools || pointer == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    for(unsigned int i = 0; i <= MemoryPoolList.size(); i++){
        if(i == MemoryPoolList.size()){
            MachineResumeSignals(&sigState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        else if(MemoryPoolList[i].mpID == memory){
            elemPos = i;
            break;
        }
    }
    if(MemoryPoolList[elemPos].memList.empty()){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else{
        ////cout << MemoryPoolList[elemPos].memList.size() << " blocks in pool " << memory << "\n";
        for(unsigned int i = 0; i < MemoryPoolList[elemPos].memList.size(); i++) {
            if(MemoryPoolList[elemPos].memList[i].base == pointer){
                ////cout << "Removing " << i << "\n";
                if((i  > 0 && MemoryPoolList[elemPos].memList[i - 1].free) &&
                    (i  < MemoryPoolList[elemPos].memList.size() - 1 && MemoryPoolList[elemPos].memList[i + 1].free)){
                    ////cout << "Before and after are free\n";
                    MemoryPoolList[elemPos].memList[i - 1].size += MemoryPoolList[elemPos].memList[i].size + MemoryPoolList[elemPos].memList[i + 1].size;
                    MemoryPoolList[elemPos].memList.erase(MemoryPoolList[elemPos].memList.begin() + i);
                    MemoryPoolList[elemPos].memList.erase(MemoryPoolList[elemPos].memList.begin() + i);
                }
                else if(i  > 0 && MemoryPoolList[elemPos].memList[i - 1].free){
                    ////cout << "Before is free\n";
                    MemoryPoolList[elemPos].memList[i - 1].size += MemoryPoolList[elemPos].memList[i].size;
                    MemoryPoolList[elemPos].memList.erase(MemoryPoolList[elemPos].memList.begin() + i);
                }
                else if(i  < MemoryPoolList[elemPos].memList.size() - 1 && MemoryPoolList[elemPos].memList[i + 1].free){
                    ////cout << "After is free\n";
                    MemoryPoolList[elemPos].memList[i].free = true;
                    MemoryPoolList[elemPos].memList[i].size += MemoryPoolList[elemPos].memList[i + 1].size;
                    MemoryPoolList[elemPos].memList.erase(MemoryPoolList[elemPos].memList.begin() + (i + 1));
                }
                else{
                    ////cout << "neither are free\n";
                    MemoryPoolList[elemPos].memList[i].free = true;
                }
                if(MemoryPoolList[elemPos].memList.size() == 1 && MemoryPoolList[elemPos].memList[0].free){
                    ////cout << "Final element removed\n";
                    MemoryPoolList[elemPos].memList.clear();
                }
                MachineResumeSignals(&sigState);
                return VM_STATUS_SUCCESS;
            }
        }
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
}


TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    unsigned int elemPos = 0;
    if(memory >= numPools){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    for(unsigned int i = 0; i <= MemoryPoolList.size(); i++){
        if(i == MemoryPoolList.size()){
            MachineResumeSignals(&sigState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        else if(MemoryPoolList[i].mpID == memory){
            elemPos = i;
            break;
        }
    }
    if(!MemoryPoolList[elemPos].memList.empty()){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else{
        MemoryPoolList.erase(MemoryPoolList.begin() + elemPos);
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}


TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft){
    TMachineSignalState sigState;
    MachineSuspendSignals(&sigState);
    unsigned int elemPos = 0;
    if(memory >= numPools || bytesleft == NULL){
        MachineResumeSignals(&sigState);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    for(unsigned int i = 0; i <= MemoryPoolList.size(); i++){
        if(i == MemoryPoolList.size()){
            MachineResumeSignals(&sigState);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        else if(MemoryPoolList[i].mpID == memory){
            elemPos = i;
            break;
        }
    }
    if(MemoryPoolList[elemPos].memList.empty()){
        *bytesleft = MemoryPoolList[elemPos].size;
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
    else{
        *bytesleft = 0;
        for(unsigned int i = 0; i < MemoryPoolList[elemPos].memList.size(); i++){
            if(MemoryPoolList[elemPos].memList[i].free){
                *bytesleft += MemoryPoolList[elemPos].memList[i].size;
            }
        }
        MachineResumeSignals(&sigState);
        return VM_STATUS_SUCCESS;
    }
}

