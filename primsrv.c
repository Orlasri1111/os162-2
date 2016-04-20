#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"
//#include <unistd.h>
struct worker{
		int pid;
		int working;
		int query;
		int value;
	};

void runWorker();
void workerHandler(int pid, int value);
void serverHandler(int pid, int value);
int serverPid;
int numOfWorkers;
struct worker *workers;
int main(int argc, char **argv){
	//struct for keeping worker information
		

	//administrative stuff
	serverPid = getpid();
	sigset(serverHandler);

	int pid;
	if(argv[1] == 0){
		printf(1,"num of workers missing \n");
		return 0;
	}
	numOfWorkers = atoi(argv[1]);
	//initializing workers array
	struct worker workersP[numOfWorkers];
	workers = workersP;
	struct worker *w;
	for(w = workers; w < &workers[numOfWorkers]; w++){
		pid = fork();
		if(pid == 0){ //child process = worker
			runWorker();
			return 0;
		}
		else{ //parent, registering process
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
				w->working = 1;
				//sigsend(w->pid,nextNum);
			}

		}
	}
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
		sleep(1000);
	}
	//for(;;){
	//	sigpause();
	//	sigset(workerHandler);
	//}
}
void workerHandler(int pid, int value){
	int i = value+1;
	int found = 0;
	int j;
	while(!found){
		for(j = 2; j < i/2; j++){
			if(i % j == 0)
				break;
		}
		//found a prime number
		if(j == i/2){
			found = 1;
		}
		//didn't found
		else{
			i++;
		}
	}
	//i is the prime number, put it as worker's value
	struct worker *w;
	for(w = workers; w < &workers[numOfWorkers]; w++){
		if(w->pid == pid){
			w->query = value;
			w->value = i;
			break;
		}
	}
	//send a signal to primsrv, let him know the worker got the prime number
	//sigsand(serverPid,pid);
}

void serverHandler(int pid, int value){
	//find the worker that send the signal (value)
	//and print it's "value" = the prime number needed
	struct worker *w;
	for(w = workers; w < &workers[numOfWorkers]; w++){
		if(w->pid == value){
			printf(1,"worker %d returned %d as a result for %d \n",w->pid,w->value,w->query);
			w->working = 0;
			break;
		}
	}
	sigset(serverHandler);
}