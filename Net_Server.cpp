// Net_Server.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <conio.h>
#include <string.h>

#include <vector>

#include "Net_Server.h"
#include "proto.h"

using namespace std;

//there should be only ONE Global Server context ever..ideally a singleton, but for now this should be fine;
ServerContext g_ServerContext;

int resolveHostName(const char* hostname, struct in_addr* addr);

struct SmartLock
{
	SmartLock(CRITICAL_SECTION& robjLock):m_objLock(robjLock){
		EnterCriticalSection(&m_objLock);
	}
	~SmartLock() {
		LeaveCriticalSection(&m_objLock);
	}
	void Lock() {
		EnterCriticalSection(&m_objLock);
	}
	void Unlock() {
		LeaveCriticalSection(&m_objLock);
	}
private:
	CRITICAL_SECTION & m_objLock;
};


int main(int argc, char** argv)
{
	// Process command line arguments
	if (argc < 3 || argc > 4) {
		printf("usage: %s <workers> <port> <ip>\n", argv[0]);
		exit(-1);
	}

	int workers = atoi(argv[1]);
	int port = atoi(argv[2]);
	string ip;
	if (argc == 4) {
		ip = argv[3];
	}

	if (false == g_ServerContext.Initialize())
	{
		return 1;
	}

	SOCKET ListenSocket;

	struct sockaddr_in ServerAddress;

	//Overlapped I/O follows the model established in Windows and can be performed only on 
	//sockets created through the WSASocket function 
	ListenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

	if (INVALID_SOCKET == ListenSocket)
	{
		printf("\nError occurred while opening socket: %d.", WSAGetLastError());
		goto error;
	}

	//Cleanup and Init with 0 the ServerAddress
	ZeroMemory((char *)&ServerAddress, sizeof(ServerAddress));

	//Port number will be supplied as a command line argument
	int nPortNo;
	nPortNo = atoi(argv[2]);

	

	//Fill up the address structure
	ServerAddress.sin_family = AF_INET;
	ServerAddress.sin_addr.s_addr = INADDR_ANY; //WinSock will supply address
	ServerAddress.sin_port = htons(nPortNo);    //comes from commandline
	if (resolveHostName(argv[3], &(ServerAddress.sin_addr)) != 0) {
		inet_pton(PF_INET, argv[3], &(ServerAddress.sin_addr));
	}
												//Assign local address and port number
	if (SOCKET_ERROR == bind(ListenSocket, (struct sockaddr *) &ServerAddress, sizeof(ServerAddress)))
	{
		closesocket(ListenSocket);
		printf("\nError occurred while binding.");
		goto error;
	}

	//Make the socket a listening socket
	if (SOCKET_ERROR == listen(ListenSocket, SOMAXCONN))
	{
		closesocket(ListenSocket);
		printf("\nError occurred while listening.");
		goto error;
	}

	g_hAcceptEvent = WSACreateEvent();

	if (WSA_INVALID_EVENT == g_hAcceptEvent)
	{
		printf("\nError occurred while WSACreateEvent().");
		goto error;
	}

	if (SOCKET_ERROR == WSAEventSelect(ListenSocket, g_hAcceptEvent, FD_ACCEPT))
	{
		printf("\nError occurred while WSAEventSelect().");
		WSACloseEvent(g_hAcceptEvent);
		goto error;
	}

	printf("\nTo exit this server, hit a key at any time on this console...");

	DWORD nThreadID;
	g_hAcceptThread = CreateThread(0, 0, AcceptThread, (void *)ListenSocket, 0, &nThreadID);

	//Hang in there till a key is hit
	while (!_kbhit())
	{
		Sleep(0);  //switch to some other thread
	}

	WriteToConsole("\nServer is shutting down...");

	//Start cleanup
	g_ServerContext.CleanUp();

	//Close open sockets
	closesocket(ListenSocket);

	g_ServerContext.DeInitialize();

	return 0; //success

error:
	closesocket(ListenSocket);
	g_ServerContext.DeInitialize();
	return 1;
}

int resolveHostName(const char* hostname, struct in_addr* addr)
{
	struct addrinfo *res;

	int result = getaddrinfo(hostname, NULL, NULL, &res);
	if (result == 0) {
		memcpy(addr, &((struct sockaddr_in *) res->ai_addr)->sin_addr, sizeof(struct in_addr));
		freeaddrinfo(res);
	}
	return result;
}

bool ServerContext::Initialize()
{
	//Find out number of processors and threads
	g_nThreads = WORKER_THREADS_PER_PROCESSOR * GetNoOfProcessors();

	printf("\nNumber of processors on host: %d", GetNoOfProcessors());

	printf("\nThe following number of worker threads will be created: %d", g_nThreads);

	//Allocate memory to store thread handless
	g_phWorkerThreads = new HANDLE[g_nThreads];

	//Initialize the Console Critical Section
	InitializeCriticalSection(&g_csConsole);

	//Create shutdown event
	g_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	// Initialize Winsock
	WSADATA wsaData;

	int nResult;
	nResult = WSAStartup(MAKEWORD(2, 2), &wsaData);

	if (NO_ERROR != nResult)
	{
		printf("\nError occurred while executing WSAStartup().");
		return false; //error
	}
	else
	{
		printf("\nWSAStartup() successful.");
	}

	if (false == InitializeIOCP())
	{
		printf("\nError occurred while initializing IOCP");
		return false;
	}
	else
	{
		printf("\nIOCP initialization successful.");
	}

	return true;
}

//Function to Initialize IOCP
bool ServerContext::InitializeIOCP()
{
	//Create I/O completion port
	g_hIOCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

	if (NULL == g_hIOCompletionPort)
	{
		printf("\nError occurred while creating IOCP: %d.", WSAGetLastError());
		return false;
	}

	DWORD nThreadID;

	//Create worker threads
	for (int ii = 0; ii < g_nThreads; ii++)
	{
		g_phWorkerThreads[ii] = CreateThread(0, 0, WorkerThread, (void *)(ii + 1), 0, &nThreadID);
	}

	return true;
}


bool ServerContext::AssociateWithIOCP(CClientContext   *pClientContext)
{
	//Associate the socket with IOCP
	HANDLE hTemp = CreateIoCompletionPort((HANDLE)pClientContext->GetSocket(), g_hIOCompletionPort, (DWORD)pClientContext, 0);

	if (NULL == hTemp)
	{
		WriteToConsole("\nError occurred while executing CreateIoCompletionPort().");

		//Let's not work with this client
		m_ClientManager.RemoveFromClientListAndFreeMemory(pClientContext);

		return false;
	}

	return true;
}

void ServerContext::CleanUp()
{
	//Ask all threads to start shutting down
	SetEvent(g_hShutdownEvent);

	//Let Accept thread go down
	WaitForSingleObject(g_hAcceptThread, INFINITE);

	for (int i = 0; i < g_nThreads; i++)
	{
		//Help threads get out of blocking - GetQueuedCompletionStatus()
		PostQueuedCompletionStatus(g_hIOCompletionPort, 0, (DWORD)NULL, NULL);
	}

	//Let Worker Threads shutdown
	WaitForMultipleObjects(g_nThreads, g_phWorkerThreads, TRUE, INFINITE);

	//We are done with this event
	WSACloseEvent(g_hAcceptEvent);

}

void ServerContext::DeInitialize()
{
	//Delete the Console Critical Section.
	DeleteCriticalSection(&g_csConsole);

	//Cleanup IOCP.
	CloseHandle(g_hIOCompletionPort);

	//Clean up the event.
	CloseHandle(g_hShutdownEvent);

	//Clean up memory allocated for the storage of thread handles
	delete[] g_phWorkerThreads;

	//Cleanup Winsock
	WSACleanup();
}

//This thread will look for accept event
DWORD WINAPI AcceptThread(LPVOID lParam)
{
	SOCKET ListenSocket = (SOCKET)lParam;

	WSANETWORKEVENTS WSAEvents;

	//Accept thread will be around to look for accept event, until a Shutdown event is not Signaled.
	while (WAIT_OBJECT_0 != WaitForSingleObject(g_hShutdownEvent, 0))
	{
		if (WSA_WAIT_TIMEOUT != WSAWaitForMultipleEvents(1, &g_hAcceptEvent, FALSE, WAIT_TIMEOUT_INTERVAL, FALSE))
		{
			WSAEnumNetworkEvents(ListenSocket, g_hAcceptEvent, &WSAEvents);
			if ((WSAEvents.lNetworkEvents & FD_ACCEPT) && (0 == WSAEvents.iErrorCode[FD_ACCEPT_BIT]))
			{
				//Process it
				AcceptConnection(ListenSocket);
			}
		}
	}

	return 0;
}

//This function will process the accept event
void AcceptConnection(SOCKET ListenSocket)
{
	sockaddr_in ClientAddress;
	int nClientLength = sizeof(ClientAddress);

	//Accept remote connection attempt from the client
	SOCKET Socket = accept(ListenSocket, (sockaddr*)&ClientAddress, &nClientLength);

	if (INVALID_SOCKET == Socket)
	{
		WriteToConsole("\nError occurred while accepting socket: %ld.", WSAGetLastError());
	}

	//Display Client's IP
	WriteToConsole("\nClient connected from: %s", inet_ntoa(ClientAddress.sin_addr));

	//Create a new ClientContext for this newly accepted client
	CClientContext   *pClientContext = new CClientContext;

	pClientContext->SetOpCode(OP_READ);
	pClientContext->SetSocket(Socket);

	//Store this object
	g_ServerContext.m_ClientManager.AddToClientList(pClientContext);

	if (true == g_ServerContext.AssociateWithIOCP(pClientContext))
	{
		//Once the data is successfully received, we will print it.
		pClientContext->SetOpCode(OP_WRITE);

		WSABUF *p_wbuf = pClientContext->GetWSABUFPtr();
		OVERLAPPED *p_ol = pClientContext->GetOVERLAPPEDPtr();

		//Get data.
		DWORD dwFlags = 0;
		DWORD dwBytes = 0;

		//Post initial Recv
		//This is a right place to post a initial Recv
		//Posting a initial Recv in WorkerThread will create scalability issues.
		int nBytesRecv = WSARecv(pClientContext->GetSocket(), p_wbuf, 1,
			&dwBytes, &dwFlags, p_ol, NULL);

		if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
		{
			WriteToConsole("\nError in Initial Post.");
		}
	}
}


//Worker thread will service IOCP requests
DWORD WINAPI WorkerThread(LPVOID lpParam)
{
	int nThreadNo = (int)lpParam;

	void *lpContext = NULL;
	OVERLAPPED       *pOverlapped = NULL;
	CClientContext   *pClientContext = NULL;
	DWORD            dwBytesTransfered = 0;
	int nBytesRecv = 0;
	int nBytesSent = 0;
	DWORD             dwBytes = 0, dwFlags = 0;

	//Worker thread will be around to process requests, until a Shutdown event is not Signaled.
	while (WAIT_OBJECT_0 != WaitForSingleObject(g_hShutdownEvent, 0))
	{
		BOOL bReturn = GetQueuedCompletionStatus(
			ServerContext::g_hIOCompletionPort,
			&dwBytesTransfered,
			(PULONG_PTR)&lpContext,
			&pOverlapped,
			INFINITE);

		if (NULL == lpContext)
		{
			//We are shutting down
			break;
		}

		//Get the client context
		pClientContext = (CClientContext *)lpContext;

		if ((FALSE == bReturn) || ((TRUE == bReturn) && (0 == dwBytesTransfered)))
		{
			//Client connection gone, remove it.
			g_ServerContext.m_ClientManager.RemoveFromClientListAndFreeMemory(pClientContext);
			continue;
		}

		WSABUF *p_wbuf = pClientContext->GetWSABUFPtr();
		OVERLAPPED *p_ol = pClientContext->GetOVERLAPPEDPtr();

		switch (pClientContext->GetOpCode())
		{
		case OP_READ:

			pClientContext->IncrSentBytes(dwBytesTransfered);

			//Write operation was finished, see if all the data was sent.
			//Else post another write.
			if (pClientContext->GetSentBytes() < pClientContext->GetTotalBytes())
			{
				//EnterCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);

				SmartLock objLock(g_ServerContext.m_ClientManager.g_csClientList);

				pClientContext->SetOpCode(OP_READ);

				p_wbuf->buf += pClientContext->GetSentBytes();
				p_wbuf->len = pClientContext->GetTotalBytes() - pClientContext->GetSentBytes();

				dwFlags = 0;

				//Overlapped send
				nBytesSent = WSASend(pClientContext->GetSocket(), p_wbuf, 1,
					&dwBytes, dwFlags, p_ol, NULL);

				if ((SOCKET_ERROR == nBytesSent) && (WSA_IO_PENDING != WSAGetLastError()))
				{
					//Let's not work with this client
					objLock.Unlock();
					g_ServerContext.m_ClientManager.RemoveFromClientListAndFreeMemory(pClientContext);
					objLock.Lock();
				}
				//LeaveCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);
			}
			else
			{
				SmartLock objLock(g_ServerContext.m_ClientManager.g_csClientList);

				//EnterCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);

				//Once the data is successfully received, we will print it.
				pClientContext->SetOpCode(OP_WRITE);
				pClientContext->ResetWSABUF();

				dwFlags = 0;

				//Get the data.
				nBytesRecv = WSARecv(pClientContext->GetSocket(), p_wbuf, 1,
					&dwBytes, &dwFlags, p_ol, NULL);

				if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
				{
					WriteToConsole("\nThread %d: Error occurred while executing WSARecv() in OP_READ", nThreadNo);

					//Let's not work with this client
					objLock.Unlock();
					g_ServerContext.m_ClientManager.RemoveFromClientListAndFreeMemory(pClientContext);
					objLock.Lock();
				}
				//LeaveCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);
			}

			break;

		case OP_WRITE:
		{
			string strCurr;
			strCurr.resize(pClientContext->GetWSABUFPtr()->len+1);
			strncpy(const_cast<char*>(strCurr.c_str()), pClientContext->GetWSABUFPtr()->buf,pClientContext->GetWSABUFPtr()->len);
			pClientContext->AddToBuffer(pClientContext->GetWSABUFPtr()->buf);

			//char szBuffer[MAX_BUFFER_LEN];
			string strBuffer;
			int len = pClientContext->GetBufferLen();
			strBuffer.resize(len);
			//Get the complete data we received till now
			pClientContext->GetBuffer(const_cast<char*>(strBuffer.c_str()));

			Proto prot;
			prot.Parse(strBuffer);

			if (!prot.IsAllDataReceived())
			{
				SmartLock objLock(g_ServerContext.m_ClientManager.g_csClientList);

				//EnterCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);

				pClientContext->SetOpCode(OP_WRITE);
				pClientContext->ResetWSABUF();

				dwFlags = 0;

				//Get the data.
				nBytesRecv = WSARecv(pClientContext->GetSocket(), p_wbuf, 1,
					&dwBytes, &dwFlags, p_ol, NULL);

				if ((SOCKET_ERROR == nBytesRecv) && (WSA_IO_PENDING != WSAGetLastError()))
				{
					WriteToConsole("\nThread %d: Error occurred while executing WSARecv() in OP_WRITE", nThreadNo);

					//Let's not work with this client
					objLock.Unlock();
					g_ServerContext.m_ClientManager.RemoveFromClientListAndFreeMemory(pClientContext);
					objLock.Lock();
				}
				//LeaveCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);

				break;
			}
			else
			{
				WriteToConsole("\nThread %d: Message Received: %s", nThreadNo, strBuffer.c_str());

				//once we have received the message we are supposed to broadcast it back to all other clients
				//there could have been a better model than this, 
				
				SmartLock objLock(g_ServerContext.m_ClientManager.g_csClientList);

				//EnterCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);

				std::vector <CClientContext *>::iterator IterClientContext;

				//Remove the supplied ClientContext from the list and release the memory
				for (IterClientContext = g_ServerContext.m_ClientManager.g_ClientContext.begin(); IterClientContext != g_ServerContext.m_ClientManager.g_ClientContext.end(); IterClientContext++)
				{
					CClientContext   *pCurrentContext = *IterClientContext;

					WSABUF *p_wbufCurr = new WSABUF;
					p_wbufCurr->buf = (char*)strBuffer.c_str();
					p_wbufCurr->len = strBuffer.size();

					//Send the message back to the client.
					pCurrentContext->SetOpCode(OP_READ);


					pCurrentContext->SetTotalBytes(dwBytesTransfered);
					pCurrentContext->SetSentBytes(0);

					p_wbuf->len = dwBytesTransfered;

					dwFlags = 0;

					//Overlapped send
					nBytesSent = WSASend(pCurrentContext->GetSocket(), p_wbufCurr, 1,
						&dwBytes, dwFlags, p_ol, NULL);

					if ((SOCKET_ERROR == nBytesSent) && (WSA_IO_PENDING != WSAGetLastError()))
					{
						WriteToConsole("\nThread %d: Error occurred while executing WSASend().", nThreadNo);

						//Let's not work with this client
						//LeaveCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);
						objLock.Unlock();
						g_ServerContext.m_ClientManager.RemoveFromClientListAndFreeMemory(pCurrentContext);
						//EnterCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);
						objLock.Lock();
					}
				}

				//LeaveCriticalSection(&g_ServerContext.m_ClientManager.g_csClientList);
			}

			break;
		}
		default:
			//We should never be reaching here, under normal circumstances.
			break;
		} // switch
	} // while

	return 0;
}


//Store client related information in a vector
void ClientManager::AddToClientList(CClientContext   *pClientContext)
{
	EnterCriticalSection(&g_csClientList);

	//Store these structures in vectors
	g_ClientContext.push_back(pClientContext);

	LeaveCriticalSection(&g_csClientList);
}

//This function will allow to remove one single client out of the list
void ClientManager::RemoveFromClientListAndFreeMemory(CClientContext   *pClientContext)
{
	EnterCriticalSection(&g_csClientList);

	std::vector <CClientContext *>::iterator IterClientContext;

	//Remove the supplied ClientContext from the list and release the memory
	for (IterClientContext = g_ClientContext.begin(); IterClientContext != g_ClientContext.end(); IterClientContext++)
	{
		if (pClientContext == *IterClientContext)
		{
			g_ClientContext.erase(IterClientContext);

			//i/o will be cancelled and socket will be closed by destructor.
			delete pClientContext;
			break;
		}
	}

	LeaveCriticalSection(&g_csClientList);
}

//Clean up the list, this function will be executed at the time of shutdown
void ClientManager::CleanClientList()
{
	EnterCriticalSection(&g_csClientList);

	std::vector <CClientContext *>::iterator IterClientContext;

	for (IterClientContext = g_ClientContext.begin(); IterClientContext != g_ClientContext.end(); IterClientContext++)
	{
		//i/o will be cancelled and socket will be closed by destructor.
		delete *IterClientContext;
	}

	g_ClientContext.clear();

	LeaveCriticalSection(&g_csClientList);
}

//Function to synchronize console output
//Threads need to be synchronized while they write to console.
//WriteConsole() API can be used, it is thread-safe, I think.
//I have created my own function.
void WriteToConsole(const char* szFormat, ...)
{
	EnterCriticalSection(&g_csConsole);

	va_list args;
	va_start(args, szFormat);

	vprintf(szFormat, args);

	va_end(args);

	LeaveCriticalSection(&g_csConsole);
}

//The use of static variable will ensure that 
//we will make a call to GetSystemInfo() 
//to find out number of processors, 
//only if we don't have the information already.
//Repeated use of this function will be efficient.
int GetNoOfProcessors()
{
	static int nProcessors = 0;

	if (0 == nProcessors)
	{
		SYSTEM_INFO si;

		GetSystemInfo(&si);

		nProcessors = si.dwNumberOfProcessors;
	}

	return nProcessors;
}