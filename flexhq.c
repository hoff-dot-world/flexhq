/*  flexhq.c - thread pool and epoll socket server for flexutils
    Copyright (C) 2024  hoff.industries

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include <signal.h>
#include <unistd.h>
#include <string.h>

#include <libgen.h>
#include <pthread.h>

#include <netinet/in.h>

#include <sys/socket.h>
#include <sys/epoll.h>

#include "libflex.h"

#define EXT_ERR_INIT_MUTEX 1
#define EXT_ERR_INIT_SOCKET 2
#define EXT_ERR_INIT_EPOLL 3
#define EXT_ERR_INIT_THREAD 4

#define EXT_ERR_SOCKET_BIND 5
#define EXT_ERR_SOCKET_LISTEN 6
#define EXT_ERR_SOCKET_ACCEPT 7

#define EXT_ERR_REALLOC_TABLE 8

#define MAX_TABLE_SIZE 256
#define MAX_EPOLL_EVENTS 8

#define MAX_WORKERS 8
#define MAX_CLIENTS MAX_TABLE_SIZE / MAX_WORKERS

#define MAX_FILES 8
#define TABLE_CHUNK 8

#define PORT 5521

#define MATCH_SUBFD 0x01
#define MATCH_PUBFD 0x10
#define MATCH_PATH 0x100
#define MATCH_UNLOADED 0x1000
#define MATCH_AVAILABLE 0x10000

char *ProgramTitle = "flexhq";
pthread_mutex_t PrintLock;

int AnyPubMagicVal = -2;

struct watchEntry {
	int subFd;
	char *watchPath;

	int pubFd;
	int *activeFiles;
};

struct sharedWorkerData {
	struct watchEntry *subTable;
	int subTableSize;
	pthread_mutex_t subTableLock;
};

struct privateWorkerData {
	int threadNum;
	pthread_t threadId;

	struct sharedWorkerData *sharedData;

	int epollFd;
	int ieventQueue;

	int fileCounterTable[MAX_CLIENTS];

	struct epoll_event socketEvents[MAX_CLIENTS];
	struct epoll_event socketEventQueue[MAX_EPOLL_EVENTS];

	pthread_mutex_t socketEventsLock;

};

void watch_entry_factory(struct watchEntry *entry) {
	entry->subFd = 0;
	entry->watchPath = NULL;
	entry->pubFd = 0;
	entry->activeFiles = NULL;
}

void watch_entry_reset(struct watchEntry *entry) {

	if (entry->watchPath != NULL) {
		free(entry->watchPath);
	}
	watch_entry_factory(entry);
}

void private_worker_data_factory(struct privateWorkerData *threadData) {

	threadData->threadNum = -1;
	threadData->epollFd = -1;
	threadData->sharedData = NULL;

	for (int i = 0; i < MAX_CLIENTS; i++) {
		threadData->socketEvents[i].data.fd = 0;
		threadData->fileCounterTable[i] = -1;
	}
}

void initialise_data_table(int start, int end, struct watchEntry *table) {
	for (int i = start; i < end; i++) {
		watch_entry_factory(&(table[i]));
	}
}

// DIFF TO VID
// make sure to pass struct by reference with pointer,
// missed that in my video thanks benhetland for commenting
int find_table_entry(struct watchEntry *matchEntry,
		int tableSize, struct watchEntry *table, uint32_t matchMask, bool clear) {

	uint32_t matched = 0x000000;
	int matchCounter = -1;

	for (int i = 0; i < tableSize; i++) {

		matched = 0x0000;

		if (matchEntry->subFd == table[i].subFd) {
			matched |= MATCH_SUBFD;
		}

		if (matchEntry->watchPath == NULL) {

			if (table[i].watchPath == NULL) {
				matched |= MATCH_PATH;
			}

		} else {

			if (table[i].watchPath != NULL &&
					!strcmp(matchEntry->watchPath, table[i].watchPath)) {
				matched |= MATCH_PATH;
			}
		}

		if (matchEntry->pubFd == table[i].pubFd || matchEntry->pubFd == AnyPubMagicVal) {
			matched |= MATCH_PUBFD;
		}

		if (table[i].activeFiles == NULL) {
			matched |= MATCH_AVAILABLE;

		} else {

			if (*table[i].activeFiles < MAX_FILES) {
				// this implies a rolexhound instance is active for it
				matched |= MATCH_UNLOADED;
			}
		}

		if ((matched & matchMask) == matchMask) {
			matchCounter++;

			if (clear) {
				watch_entry_reset(&table[i]);
				continue;
			}

			watch_entry_factory(matchEntry);
			return i;
		}
	}

	watch_entry_factory(matchEntry);
	return matchCounter;
}

void *hq_thread_worker(void *voidClientData) {

	struct privateWorkerData *workerData = (struct privateWorkerData *)voidClientData;
	uint8_t socketBuffer[FLX_PKT_MAXIMUM_SIZE];

	int clientId = -1;

	int bytesRead = -1;
	int eventsReady = -1;

	int watchIndex = -1;
	int subIndex = -1;

	struct flex_msg readMsg, sendMsg;
	struct serialize_result result;
	struct watchEntry matchEntry;

	struct sharedWorkerData *sharedData = workerData->sharedData;

	flex_msg_factory(&readMsg);
	flex_msg_factory(&sendMsg);
	serialize_result_factory(&result);
	watch_entry_factory(&matchEntry);

	while (true) {

		eventsReady = epoll_wait(workerData->epollFd,
			workerData->socketEventQueue, MAX_EPOLL_EVENTS, -1);

		if (eventsReady == -1) {
			fprintf(stderr, "Thread %d: Error waiting for epoll events!\n", workerData->threadNum);
			pthread_exit(NULL);
		}

		pthread_mutex_lock(&PrintLock);
		printf("Thread %d got %d epoll event(s).\n", workerData->threadNum, eventsReady);
		pthread_mutex_unlock(&PrintLock);

		for (int i = 0; i < eventsReady; i++) {

			flex_msg_reset(&sendMsg);
			flex_msg_reset(&readMsg);

			subIndex = -1;
			watchIndex = -1;
			clientId = -1;

			pthread_mutex_lock(&workerData->socketEventsLock);
			for (int j = 0; j < MAX_CLIENTS; j++) {
				if (workerData->socketEvents[j].data.fd ==
						workerData->socketEventQueue[i].data.fd) {

					clientId = j;
					break;
				}
			}

			if (clientId == -1) {
				fprintf(stderr, "Thread %d: Couldn't map client ID!\n", workerData->threadNum);
				pthread_mutex_unlock(&workerData->socketEventsLock);
				continue;
			}

			bytesRead = read(workerData->socketEventQueue[i].data.fd,
				socketBuffer, sizeof(socketBuffer));

			if (bytesRead == 0 || bytesRead == -1) {
				fprintf(stderr, "Thread %d: Error reading from socket!\n", workerData->threadNum);

				subIndex = workerData->socketEventQueue[i].data.fd;

				epoll_ctl(workerData->epollFd, EPOLL_CTL_DEL,
					workerData->socketEvents[clientId].data.fd, &workerData->socketEvents[clientId]);

				close(workerData->socketEvents[clientId].data.fd);
				workerData->socketEventQueue[i].data.fd = 0;

				bzero(&workerData->socketEvents[clientId], sizeof(struct epoll_event));
				pthread_mutex_unlock(&workerData->socketEventsLock);


				if (workerData->fileCounterTable[clientId] != -1) {
					workerData->fileCounterTable[clientId] = -1;
					matchEntry.pubFd = subIndex;

					pthread_mutex_lock(&sharedData->subTableLock);
					printf("Thread %d: cleared %d entry(s) in the sub table.\n", workerData->threadNum,
						find_table_entry(&matchEntry, sharedData->subTableSize, sharedData->subTable, MATCH_PUBFD, true) + 1);
					pthread_mutex_unlock(&sharedData->subTableLock);
				} else {
					matchEntry.subFd = subIndex;

					pthread_mutex_lock(&sharedData->subTableLock);
					printf("Thread %d: cleared %d entry(s) in the sub table.\n", workerData->threadNum,
						find_table_entry(&matchEntry, sharedData->subTableSize, sharedData->subTable, MATCH_SUBFD, true) + 1);
					pthread_mutex_unlock(&sharedData->subTableLock);
				}

				continue;
			}

			pthread_mutex_unlock(&workerData->socketEventsLock);

			deserialize(socketBuffer, &readMsg, &result);
			if (result.reply != FLX_REPLY_VALID) {
				fprintf(stderr, "Thread %d: Received %x from smartwatch!\n", workerData->threadNum, result.reply);
				continue;
			}

			switch (readMsg.action) {

				case FLX_ACT_STATUS:
					if (readMsg.option == FLX_STATUS_REG_ROLEXHOUND) {

						if (workerData->fileCounterTable[clientId] != -1) {
							fprintf(stderr, "Thread %d: Someone tried to re-register...\n", workerData->threadNum);
							continue;
						}

						pthread_mutex_lock(&sharedData->subTableLock);

						matchEntry.watchPath = NULL;
						matchEntry.pubFd = 0;
						watchIndex = find_table_entry(&matchEntry, sharedData->subTableSize,
							sharedData->subTable, MATCH_PUBFD | MATCH_PATH, false);

						if (watchIndex == -1) {
							fprintf(stderr, "Thread %d: No space in table for new rolexhound!\n", workerData->threadNum);
							pthread_mutex_unlock(&sharedData->subTableLock);
							continue;
						}
						workerData->fileCounterTable[clientId] = 0;

						sharedData->subTable[watchIndex].activeFiles = &(workerData->fileCounterTable[clientId]);
						sharedData->subTable[watchIndex].pubFd = workerData->socketEvents[clientId].data.fd;
						pthread_mutex_unlock(&sharedData->subTableLock);
						printf("Thread %d: Initialised a rolexhound instance at index %d.\n", workerData->threadNum, watchIndex);
						continue;
					}

				case FLX_ACT_WATCH:

					pthread_mutex_lock(&sharedData->subTableLock);

					matchEntry.subFd = workerData->socketEventQueue[i].data.fd;
					matchEntry.watchPath = readMsg.data[0];
					subIndex = find_table_entry(&matchEntry, sharedData->subTableSize,
						sharedData->subTable, MATCH_SUBFD | MATCH_PATH, false);

					if (readMsg.option == FLX_WATCH_REM) {

						if (subIndex < 0) {
							fprintf(stderr, "Thread %d: Couldn't find subscription requested for removal.\n", workerData->threadNum);
							pthread_mutex_unlock(&sharedData->subTableLock);
							continue;
						}

						watch_entry_reset(&sharedData->subTable[subIndex]);
						pthread_mutex_unlock(&sharedData->subTableLock);
						continue;
					}

					break;

				case FLX_ACT_NOTIFY:

					watchIndex = 0;
					pthread_mutex_lock(&sharedData->subTableLock);
					for (int j = 0; j < sharedData->subTableSize; j++) {

						if (sharedData->subTable[j].watchPath == NULL) {
							continue;
						}

						if (!strcmp(sharedData->subTable[j].watchPath, readMsg.data[0])) {

							watchIndex++;

							bzero(socketBuffer, sizeof(socketBuffer));
							serialize(socketBuffer, &readMsg, &result);
							write(sharedData->subTable[j].subFd, socketBuffer, sizeof(socketBuffer));
						}
					}
					pthread_mutex_unlock(&sharedData->subTableLock);
					printf("Thread %d: Published a notification to %d client(s).\n", workerData->threadNum, watchIndex);

					if (watchIndex == 0) {

						sendMsg.action = FLX_ACT_WATCH;
						sendMsg.option = FLX_WATCH_REM;
						sendMsg.dataLen = FLX_DLEN_WATCH;
						sendMsg.data = (char **)malloc(sizeof(char *)*FLX_DLEN_WATCH);
						sendMsg.data[0] = strdup(readMsg.data[0]);

						bzero(socketBuffer, sizeof(socketBuffer));
						serialize(socketBuffer, &sendMsg, &result);
						write(workerData->socketEventQueue[i].data.fd, socketBuffer, sizeof(socketBuffer));

						printf("Thread %d: Told rolexhound to stop watching that file; no subscribers.\n", workerData->threadNum);
					}
					continue;

				default:
					continue;
			}

			if (subIndex >= 0) {
				// he's already subscribed
				fprintf(stderr, "Thread %d: Client is already watching that file!\n", workerData->threadNum);
				pthread_mutex_unlock(&sharedData->subTableLock);
				continue;
			}

			// try and find if watcher already exists
			matchEntry.watchPath = readMsg.data[0];
			watchIndex = find_table_entry(&matchEntry,
				sharedData->subTableSize, sharedData->subTable, MATCH_PATH, false);

			// if not let's allocate it
			if (watchIndex == -1) {

				// check if rolexhound is available to service request
				matchEntry.pubFd = AnyPubMagicVal;
				watchIndex = find_table_entry(&matchEntry, sharedData->subTableSize,
					sharedData->subTable, MATCH_PUBFD | MATCH_UNLOADED, false);

				if (watchIndex == -1) {
					fprintf(stderr, "Thread %d: rolexhound was not available to service the request.\n", workerData->threadNum);
					pthread_mutex_unlock(&sharedData->subTableLock);
					continue;
				}

				bzero(socketBuffer, sizeof(socketBuffer));
				serialize(socketBuffer, &readMsg, &result);
				write(sharedData->subTable[watchIndex].pubFd, socketBuffer, sizeof(socketBuffer));

				*sharedData->subTable[watchIndex].activeFiles = *sharedData->subTable[watchIndex].activeFiles + 1;

				printf("Thread %d: Found a rolexhound client to send to.\n", workerData->threadNum);
			}

			matchEntry.watchPath = NULL;
			matchEntry.activeFiles = NULL;
			subIndex = find_table_entry(&matchEntry, sharedData->subTableSize,
				sharedData->subTable, MATCH_AVAILABLE, false);

			if (subIndex == -1) {
				// no space in the sub table
				if (sharedData->subTableSize + TABLE_CHUNK > MAX_TABLE_SIZE) {

					fprintf(stderr, "Thread %d: A client requested we exceed the max sub table size.\n", workerData->threadNum);
					pthread_mutex_unlock(&sharedData->subTableLock);
					continue;
				}
				subIndex = sharedData->subTableSize;
				sharedData->subTableSize = subIndex + TABLE_CHUNK;

				sharedData->subTable = (struct watchEntry *)realloc(sharedData->subTable,
					sizeof(struct watchEntry) * (sharedData->subTableSize));
				initialise_data_table(subIndex, sharedData->subTableSize, sharedData->subTable);

				printf("Thread %d: Allocated another chunk to the sub table (%d to %d).\n", workerData->threadNum, subIndex, sharedData->subTableSize);
			}

			sharedData->subTable[subIndex].pubFd = sharedData->subTable[watchIndex].pubFd;
			sharedData->subTable[subIndex].activeFiles = sharedData->subTable[watchIndex].activeFiles;

			sharedData->subTable[subIndex].watchPath = strdup(readMsg.data[0]);
			sharedData->subTable[subIndex].subFd = workerData->socketEventQueue[i].data.fd;

			pthread_mutex_unlock(&sharedData->subTableLock);
			printf("Thread %d: Added a client to the sub table at index %d from rolexhound assigned by index %d.\n",
				   workerData->threadNum, subIndex, watchIndex);
		}
	}
}

int main() {

	int clientId = -1;
	int threadId = -1;

	int socketFd = -1;
	int clientFd = -1;

	struct sockaddr_in serverAddress, clientAddress;
	socklen_t addressSize = sizeof(struct sockaddr_in);

	struct sharedWorkerData sharedData;
	struct privateWorkerData threadWorkerTable[MAX_CLIENTS];

	sharedData.subTable = (struct watchEntry *)malloc(
		sizeof(struct watchEntry) * TABLE_CHUNK);
	sharedData.subTableSize = TABLE_CHUNK;
	initialise_data_table(0, TABLE_CHUNK, sharedData.subTable);

	if (pthread_mutex_init(&PrintLock, NULL) != 0 ||
			pthread_mutex_init(&sharedData.subTableLock, NULL) != 0) {

		fprintf(stderr, "%s: Couldn't create muteces!\n", ProgramTitle);
		exit(EXT_ERR_INIT_MUTEX);
	}

	if ((socketFd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		fprintf(stderr, "%s: Error creating socket!\n", ProgramTitle);
		exit(EXT_ERR_INIT_SOCKET);
	}

	bzero(&serverAddress, sizeof(serverAddress));
	serverAddress.sin_family = AF_INET;
	serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddress.sin_port = htons(PORT);

	if (bind(socketFd, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) == -1) {
		fprintf(stderr, "%s: Error binding to socket!\n", ProgramTitle);
		exit(EXT_ERR_SOCKET_BIND);
	}

	printf("%s: about to spawn %d threads each supporting %d clients\n",
		   ProgramTitle, MAX_WORKERS, MAX_CLIENTS);

	for (int i = 0; i < MAX_WORKERS; i++) {

		private_worker_data_factory(&threadWorkerTable[i]);
		threadWorkerTable[i].epollFd = epoll_create1(0);

		if (threadWorkerTable[i].epollFd == -1) {

			fprintf(stderr, "%s: Error initialising epoll!\n", ProgramTitle);
			exit(EXT_ERR_INIT_EPOLL);
		}

		threadWorkerTable[i].sharedData = &sharedData;
		threadWorkerTable[i].threadNum = i;

		if (pthread_mutex_init(&threadWorkerTable[i].socketEventsLock, NULL) != 0) {
			fprintf(stderr, "%s: Error initialising thread socket event mutex!\n", ProgramTitle);
			exit(EXT_ERR_INIT_MUTEX);
		}

		if (pthread_create(&threadWorkerTable[i].threadId,
				NULL, hq_thread_worker, &threadWorkerTable[i]) != 0) {

			fprintf(stderr, "%s: Error creating client handler thread!\n", ProgramTitle);
			exit(EXT_ERR_INIT_THREAD);
		}

		pthread_mutex_lock(&PrintLock);
		printf("%s spawned thread %d.\n", ProgramTitle, i);
		pthread_mutex_unlock(&PrintLock);
	}

	if (listen(socketFd, 1)) {
		fprintf(stderr, "%s: Error listening!\n", ProgramTitle);
		exit(EXT_ERR_SOCKET_LISTEN);
	}

	printf("%s is listening on %d...\n", ProgramTitle, PORT);

	while (true) {
		clientFd = -1;

		if ((clientFd = accept(socketFd, (struct sockaddr *)&clientAddress,
			(socklen_t *)&addressSize)) == -1) {

			fprintf(stderr, "%s: Error accepting connection!\n", ProgramTitle);
			exit(EXT_ERR_SOCKET_ACCEPT);
		}

		pthread_mutex_lock(&PrintLock);
		printf("%s accepted a connection.\n", ProgramTitle);
		pthread_mutex_unlock(&PrintLock);

		threadId = -1;
		clientId = -1;

		for (int i = 0; i < MAX_WORKERS; i++) {

			pthread_mutex_lock(&threadWorkerTable[i].socketEventsLock);

			for (int j = 0; j < MAX_CLIENTS; j++) {

				if (threadWorkerTable[i].socketEvents[j].data.fd == 0) {
					clientId = j;
					threadId = i;
					break;
				}
			}

			if (clientId != -1) {
				break;
			}

			pthread_mutex_unlock(&threadWorkerTable[i].socketEventsLock);
		}

		if (threadId == -1) {

			pthread_mutex_lock(&PrintLock);
			printf("%s: too many clients active; terminating new connection.\n", ProgramTitle);
			pthread_mutex_unlock(&PrintLock);

			close(clientFd);
			continue;
		}

		threadWorkerTable[threadId].socketEvents[clientId].events = EPOLLIN;
		threadWorkerTable[threadId].socketEvents[clientId].data.fd = clientFd;

		if (epoll_ctl(threadWorkerTable[threadId].epollFd, EPOLL_CTL_ADD, clientFd,
				&threadWorkerTable[threadId].socketEvents[clientId]) != 0) {

			fprintf(stderr, "%s: Couldn't add socket event to epoll!\n", ProgramTitle);

			pthread_mutex_unlock(&threadWorkerTable[threadId].socketEventsLock);
			close(clientFd);
			continue;
		}
		pthread_mutex_unlock(&threadWorkerTable[threadId].socketEventsLock);

		pthread_mutex_lock(&PrintLock);
		printf("%s assigned client %d to thread %d.\n", ProgramTitle, clientId, threadId);
		pthread_mutex_unlock(&PrintLock);
	}
}
