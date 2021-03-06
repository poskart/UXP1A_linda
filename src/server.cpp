#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <wait.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <stdlib.h>
#include <iostream>
#include <string>
#include <string.h>
#include <vector>
#include <signal.h>
#include <pthread.h>
#include <fstream>
#include <unordered_map>
#include <list>
#include "Request.h"
#include "Reply.h"
#include "GuardedQueue.h"
#include "GuardedList.h"
#include <semaphore.h>

using namespace std;
const char* serverPath = "/tmp/fifo.server";
const char * inFifoSemaphoreName = "/serverInFifoSemaphore";
const char * logFileName = "Linda_server.log";
ofstream logFile;
GuardedQueue writeReqQueue;			// Queue to write
GuardedQueue readReqQueue;			// Queue to read
GuardedList pendingRequests;		// Pending requests buffer
sem_t * inputFifoSemaphore;			// Input fifo semaphore
unordered_multimap<size_t, Tuple> tupleSpace; //Global tuple space

void sig_handler(int signo) {
	if(signo == SIGINT) {
		if (sem_wait(inputFifoSemaphore) < 0)
			perror("sem_wait(3) failed on child");
		unlink(serverPath);
		if (sem_post(inputFifoSemaphore) < 0)
			perror("sem_post(3) error on child");

		logFile<<"Server's pipe's been unlinked"<<endl;
		logFile.close();
		exit(0);
	}
}

void createServerPipe(const char* serverPath) {
	umask(0);
	mkfifo(serverPath, 0666);
	logFile<<"Server's FIFO's been created at ";
	logFile<<serverPath<<endl;
}

void sendRequest(ofstream * outStream, sem_t * semaphore, const Request * req)
{
	if (sem_wait(semaphore) < 0)
		perror("sem_wait(3) failed on child");
	(*outStream)<<(*req);
	if (sem_post(semaphore) < 0)
		perror("sem_post(3) error on child");
}

int init(bool noSigint) {
	logFile.open(logFileName, ios::out);
	pid_t serverPid = getpid();
	logFile<<"Server's starting..."<<endl<<"Server's PID: "<<serverPid<<endl;

	if(access(serverPath, F_OK) != -1) {
		logFile<<"Server's already existed, exiting...";
		return 1;
	}

	// Initialize input fifo semaphore
	inputFifoSemaphore = sem_open(
			inFifoSemaphoreName, O_CREAT | O_RDWR, (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP), 1);
	if (inputFifoSemaphore == SEM_FAILED)
	{
	        perror("sem_open(3) error");
	        exit(EXIT_FAILURE);
	}

	if(!noSigint)
	{
		signal(SIGINT, sig_handler);
		logFile << "SIGINT handler attached"<<endl;
	}
	createServerPipe(serverPath);
	return 0;
}

/*
 * @brief checks sign before string/integer
 *
 * returns:
 * 		0: no sign (==)
 * 		1: >
 * 		2: >=
 * 		3: <
 * 		4: <=
 * 		5: *
 */
int checkIfSign(string pattern) {
	if (pattern.size() > 1) {
		if (pattern[0] == '>' && !(pattern[1] == '='))
			return 1;
		else if (pattern[0] == '>' && pattern[1] == '=')
			return 2;
		else if (pattern[0] == '<' && !(pattern[1] == '='))
			return 3;
		else if (pattern[0] == '<' && pattern[1] == '=')
			return 4;
	}
	if (pattern.size() == 1 && pattern[0] == '*')
		return 5;
	return 0; //only 1 char, cannot be sign
}

/*
 * @brief compares string lexically
 *
 * returns:
 * 		-1: str1 < str2
 * 		 0: str1 == str2
 * 		 1: str1 > str2
 */
int compareLex(string str1, string str2) {
	for (unsigned i = 0; i < min(str1.size(), str2.size()); ++i) {
		if (str1[i] == str2[i] || str2[i] == '*')
			continue;
		else if (str1[i] < str2[i])
			return -1;
		else if (str1[i] > str2[i])
			return 1;
	}
	if (str1.size() < str2.size())
		return -1;
	else if (str1.size() > str2.size())
		return 1;
	else //same size
		return 0;
}

/*
 * @brief checks if element matches pattern
 */
bool checkPattern(Elem elemFromSpace, Elem elemPattern) {
	if(elemFromSpace.isString != elemPattern.isString)
		return false;
	int sign = checkIfSign(elemPattern.pattern);
	int i; //offset in string
	switch (sign) {
		case 0: i = 0; break;
		case 1:
		case 3: i = 1; break;
		case 2:
		case 4: i = 2; break;
		case 5: return true; //* - can be anything
	}
	if(elemFromSpace.isString) {
		int cmp = compareLex(elemFromSpace.pattern, elemPattern.pattern.substr(i));
		switch (cmp) {
			case -1:
				if (sign == 3 || sign == 4) //str1<str2, sign < or <=
					return true;
				break;
			case 0:
				if (sign == 0 || sign == 2 || sign == 4) //str1==str2, no sign
					return true;
				break;
			case 1:
				if (sign == 1 || sign == 2) //str1>str2, sign > or >=
					return true;
				break;
		}
		return false;
	}
	else { //integer
		int first, second;
		try {
			first = stoi(elemFromSpace.pattern);
			second = stoi(elemPattern.pattern.substr(i));
		}
		catch (invalid_argument& e) {
			return false;
		}
		switch (sign) {
			case 0: return first == second;
			case 1: return first > second;
			case 2: return first >= second;
			case 3: return first < second;
			case 4: return first <= second;
		}
	}
	return false;
}

/**
 * 	@brief	Checks if selected tuple matches requested tuple (pattern)
 *
 * 	@return	true if selected tuple matches pattern tuple
 */
bool tuplesMatch(const Tuple & selTuple, const Tuple & patternTuple) {
	if(selTuple.elems.size() != patternTuple.elems.size())
		return false;
	for (unsigned i = 0; i < patternTuple.elems.size(); ++i) {
			if (!checkPattern(selTuple.elems[i], patternTuple.elems[i]))
				return false;
	}
	return true;
}

/**
 * 	@brief	Finds tuple in tuple space and returns reply.
 */
Reply* search(const Tuple* reqTup, unsigned opType) {
	Reply* reply = new Reply();
	pair <unordered_multimap<size_t, Tuple>::iterator, unordered_multimap<size_t, Tuple>::iterator> ret;
	const size_t tupHash = reqTup->getHash();
	ret = tupleSpace.equal_range(tupHash);
	for (unordered_multimap<size_t, Tuple>::iterator it = ret.first; it != ret.second; ++it)
	{
		if(!tuplesMatch(it->second, *reqTup))
			continue;
		reply->isFound = true;
		reply->setTuple(new Tuple(it->second));
		if (opType == 0) //if input - delete tuple
			tupleSpace.erase(it);
		return reply;
	}
	reply->isFound = false;
	return reply;
}

/**
 *	@brief	Function which searches for the request which insertedTuple matches to.
 *	Additionally when iterating through the pendingRequests list out-of-date requests
 *	are removed.
 *
 *	@param	insertedTuple is pointer to newly inserted (into tuple space) tuple
 */
void updatePendingQueue(const Tuple * insertedTuple){
	std::unique_lock<std::mutex> uLock(pendingRequests.getMutex());
	std::list<Request *> & pending = pendingRequests.getList();
	bool tupleMatched = false;
	logFile<<"**********Pending Queue - size: "<<pending.size()<<endl;
	for(std::list<Request *>::iterator it = pending.begin(); it != pending.end(); ++it){
		if((*it)->timeout < std::time(nullptr)){
			readReqQueue.producerEnter(*it);	// push to readThread queue to send empty reply
			it = pending.erase(it);
			logFile<<"**********Pending Queue - 1 out-of-date element removed"<<endl;
		}
		else if(insertedTuple != nullptr && !tupleMatched
				&& tuplesMatch(*insertedTuple, *((*it)->tuple))){
			readReqQueue.pushToFront(*it);
			it = pending.erase(it);
			tupleMatched = true;
			logFile<<"**********Pending Queue - updated 1 element."<<endl;
		}
	}
}

/**
 * 	@brief	Adds sent tuple to tuple space
 */
void* writeService(void *) {
	while(true)
	{
		Request* req = writeReqQueue.consumerEnter();
		logFile<<"-> New Request in server write queue: "<<endl;
		logFile<<"-> Request from: " << req->procId <<endl;
		logFile<<"-> Request type: ";
		switch(req->reqType) {
			case Request::Output:
				logFile<<"output\n";break;
			case Request::UpdatePendingRequests:	// remove out-of-date requests
				logFile<<"UpdatePendingReauests"<<endl;
				updatePendingQueue(nullptr);
				delete req;
				continue;
			case Request::Input:
			case Request::Read:
			case Request::Stop:
				logFile<<"stop or wrong type"<<endl;
				logFile<<"Exiting..."<<endl;
				delete req;
				return 0;
		}

		//insert new tuple
		Tuple* tup = new Tuple(*(req->tuple));
		tupleSpace.insert(make_pair(tup->getHash(), *tup));
		updatePendingQueue(tup);

		logFile<<"New tuple's been added"<<endl;
		delete tup;
		delete req;
	}
	return 0;
}

void sendReplyToClient(Reply * rep, unsigned procId)
{
	string clientFIFO = "/tmp/fifo.";
	clientFIFO.append(to_string(procId));
	ofstream outFIFO(clientFIFO.c_str(), ofstream::binary);

	if(!outFIFO.fail()) {
		outFIFO << *rep;
		logFile<<"_____Reply to client"<<procId <<", has been sent"<<endl;
	}
	else
		logFile<<"_____Cannot send reply to client "<<procId <<". client's FIFO is broken"<<endl;
}

void* readService(void *) {
	while(true)
	{
		Request* req = readReqQueue.consumerEnter();
		logFile<<"_____New Request in server read queue: "<<endl;
		logFile<<"_____Request from: " << req->procId <<endl;
		logFile<<"_____Request type: ";
		switch(req->reqType) {
			case Request::Input:
				logFile<<"_____input\n";break;
			case Request::Read:
				logFile<<"_____read\n";break;
			case Request::Output:
			case Request::Stop:
			case Request::UpdatePendingRequests:
				logFile<<"_____stop or wrong type"<<endl;
				logFile<<"_____Exiting..."<<endl;
				delete req;
				return 0;
		}

		//Send reply
		Reply * reply;
		if(req->timeout != 0 && req->timeout < std::time(nullptr))	// Send empty reply if timeout exceeded (without searching)
		{
			reply = new Reply();
			reply->isFound = false;
			sendReplyToClient(reply, req->procId);
			delete req;
		}
		else
		{
			reply = search(req->tuple, req->reqType);
			if(!reply->isFound)		// if requested tuple not found in tuple space
			{
				if(req->timeout > std::time(nullptr)){	// request is still valid (timout ok)
					pendingRequests.push_back(req);
					logFile<<"_____request for non-existing tuple, Request pushed to pendingQueue...\n";
				}
				else		// request invalid (timeout) - send empty reply and delete request
				{
					sendReplyToClient(reply, req->procId);
					delete req;
				}
			}
			else					// tuple found in tuple space
			{
				sendReplyToClient(reply, req->procId);
				delete req;
			}
		}
		delete reply;
	}
	return 0;
}

void * receptionistThread(void * iStream)
{
	ifstream * inFifo = static_cast<ifstream *>(iStream);
	std::time_t currTime;
	while(true)
	{
		Request * incomingReq = new Request();
		*inFifo >> *incomingReq;
		switch(incomingReq->reqType){
			case Request::Output:
			case Request::UpdatePendingRequests:
				writeReqQueue.producerEnter(incomingReq);
				break;
			case Request::Stop:
			{
				Request * doubledReq = new Request(*incomingReq);
				readReqQueue.producerEnter(doubledReq);
				writeReqQueue.producerEnter(incomingReq);
				return 0;
			}
			case Request::Input:
			case Request::Read:
			{
				if(incomingReq->timeout != 0)
				{
					currTime = std::time(nullptr);		// get current time (in seconds)
					incomingReq->timeout += currTime;	// set expiration time
				}
				readReqQueue.producerEnter(incomingReq);
				break;
			}
			default:
				break;
		}
		logFile<<"\t\t\t\t\tNew request received in server receptionist thread..."<<endl;
		logFile<<"\t\t\t\t\tFrom process: "<<incomingReq->procId<<endl;
		logFile<<"\t\t\t\t\tRequest type: "<<incomingReq->reqType<<endl;
	}
	return 0;
}

void sendStopRequest(ofstream * os)
{
	Request * r = new Request();
	r->procId = getpid();
	r->reqType = Request::Stop;
	sendRequest(os, inputFifoSemaphore, r);
	delete r;
}

void sendUpdatePendingRequest(ofstream * os)
{
	Request * r = new Request();
	r->procId = getpid();
	r->reqType = Request::UpdatePendingRequests;
	sendRequest(os, inputFifoSemaphore, r);
	delete r;
}

int main(int argc, char * argv[]) {

	bool noSigint = false;
	if(argc > 1 && strcmp(argv[1], "-nosigint") == 0)
		noSigint = true;

	if (init(noSigint) != 0)
		return 0;

	// Input fifo for clients
	ifstream inFIFO(serverPath, ifstream::binary);
	// Output fifo to self threads
	ofstream outServerTmpFifo(serverPath, ofstream::binary);

	pthread_t recThread, readerThread, writerThread;
	pthread_create(&recThread, NULL, &receptionistThread, static_cast<void *>(&inFIFO));
	pthread_create(&readerThread, NULL, &readService, NULL);
	pthread_create(&writerThread, NULL, &writeService, NULL);

	while(1)
	{
		sendUpdatePendingRequest(&outServerTmpFifo);
		sleep(1);
	}

	// Stop children threads - send Stop request
	sendStopRequest(&outServerTmpFifo);

	// Join children threads
	pthread_join(recThread, NULL);
	pthread_join(readerThread, NULL);
	pthread_join(writerThread, NULL);
	unlink(serverPath);
	logFile<<"Server's pipe's been unlinked (main)"<<endl;
	logFile.close();
	// Close and unlink semaphore
	if (sem_close(inputFifoSemaphore) < 0) {
		perror("sem_close(3) failed");
		sem_unlink(inFifoSemaphoreName);
		exit(EXIT_FAILURE);
	}
	if (sem_unlink(inFifoSemaphoreName) < 0)
	        perror("sem_unlink(3) failed");
	cout<<"Server stopped"<<endl;
	return 0;
}
