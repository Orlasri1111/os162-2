#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"
struct worker{
		int pid;
		int working;
		int query;
		int value;
	};

void runWorker();
void workerHandler(int pid, int value);
void serverHandler(int pid, int value);
void test_handler(int pid, int value);
void test2();
int serverPid;
int numOfWorkers;
struct worker *workers;
int main(int argc, char **argv){
	//struct for keeping worker information
		

	//administrative stuff
	serverPid = getpid();
	sigset(&serverHandler);

	int pid;
	numOfWorkers = atoi(argv[1]);
	if(numOfWorkers == 0){
		printf(1,"num of workers missing \n");
		return 0;
	}
	workers = malloc((sizeof(struct worker)) * numOfWorkers);
	//initializing workers array
	struct worker *w;
	printf(1,"workers pids:\n");
	//initialize workers
	for(w = workers; w < &workers[numOfWorkers]; w++){
		pid = fork();
		if(pid == 0){ //child process = worker
			runWorker();
			return 0;
		}
		else{ //parent, registering process
			printf(1,"%d\n",pid);
			w->pid = pid;
			w->working = 0;
			w->query = 0;
			w->value = 0;
		}
	}
	//run manager
	int nextNum;
	char buf[10];
	//exit condition: '0' was entered
	while((nextNum = atoi(gets(buf,10))) != 0 || buf[0] == 10){//TODO: check that *enter* is not 0
		//search next available worker, if nextNum = 0 and we are here
		//it means that *enter* was prassed, so we will do nothing
		if(nextNum != 0){
			for(w = workers; w < &workers[numOfWorkers]; w++){
				if(w->working == 0)
					break;
			}
			//no idle workers
			if(w == &workers[numOfWorkers]){
				printf(1,"no idle workers\n");
			}
			else{ //found one, sending signal
				//printf(1,"found open worker\n");
				w->working = 1;
				w->query = nextNum;
				sigsend(w->pid,nextNum);
			}

		}
		//clean buffer
		for(nextNum = 0; nextNum < 10; nextNum++)
			buf[nextNum] = 0;
	}
	printf(1,"%d\n",buf[0]);
	//0 was send, kill all workers and exit
	for(w = workers; w < &workers[numOfWorkers]; w++){
		printf(1,"worker %d exit\n",w->pid);
		kill(w->pid);
		wait();
	}
	printf(1,"primesrv exit\n");
	exit();
	return 0;
}

void runWorker(){
	for(;;){
		sigset(&workerHandler);
		sigpause();
	}
}
void workerHandler(int pid, int value){
	int i = value+1;
	int found = 0;
	int j;
	//find the next prime number after "value"
	while(!found){
		for(j = 2; j < i; j++){
			if(i % j == 0)
				break;
		}
		//found a prime number
		if(j == i){
			found = 1;
		}
		//haven't found
		else{
			i++;
		}
	}
	//send a signal to primsrv, let him know the worker got the prime number
	sigsend(serverPid,i);
}

void serverHandler(int pid, int value){
	//find the worker that send the signal (value)
	//and print it's "value" = the prime number needed
	struct worker *w;
	for(w = workers; w < &workers[numOfWorkers]; w++){
		if(w->pid == pid){
			printf(1,"worker %d returned %d as a result for %d \n",pid,value,w->query);
			w->working = 0;
			break;
		}
	}
	sigset(&serverHandler);
}

void test2(){
  printf(1,"starting test\n");
  int pidt;
  pidt = fork();
  if(pidt == 0){//child
    printf(1,"child\n");
    sigset(&test_handler);
    sigpause();
  }
  else{
  	sleep(100);
    //cprintf("%d\n",pid);
    sigsend(pidt,98);
    wait();
    printf(1,"parent exiting\n");
    exit();
  }
}

void test_handler(int pid, int value){
  printf(1,"num is %d\n",value);
  printf(1,"child exiting\n");
  exit();
}