// MulitiThreadSocketServer.cpp: 定义控制台应用程序的入口点。
//

#include "stdafx.h"
//#include <winsock.h>
#include <process.h>
#include <Windows.h>
#include <WinSock2.h>
/* Chapter 12. Client/Server. SERVER PROGRAM.  SOCKET VERSION	*/
/* Execute the command in the request and return a response.	*/
/* Commands will be exeuted in process if a shared library 	*/
/* entry point can be located, and out of process otherwise	*/
/* ADDITIONAL FEATURE: argv[1] can be the name of a DLL supporting */
/* in process services */

//#include "Everything.h"
//#include "ClientServer.h"	/* Defines the request and response records. */

struct sockaddr_in srvSAddr;		/* Server's Socket address structure */
//用于保存ip地址端口等信息
struct sockaddr_in connectSAddr;	/* Connected socket with client details   */
//同上 但是用为使用的TCP/UDP所有必须是要使用sockaddr_in
WSADATA WSStartData;				/* Socket library data structure   */
//用于返回windowssocket初始化的DLL信息，注：即包含的socketdll的相关信息
///以上结构都已在头文件中进行了定义
enum SERVER_THREAD_STATE {//建立线程状态的枚举类型
	SERVER_SLOT_FREE, SERVER_THREAD_STOPPED,
	SERVER_THREAD_RUNNING, SERVER_SLOT_INVALID
};
typedef struct SERVER_ARG_TAG { /* Server thread arguments */
								//服务器线程的参数
	CRITICAL_SECTION threadCs;//线程临界区，主要用于多线程资源访问写入的资源保护
	DWORD	number;//标记一个线程的序号
	SOCKET	sock;//传入的socket实例
	enum SERVER_THREAD_STATE thState;//线程的状态
	HANDLE	hSrvThread;//标记线程的handle
	HINSTANCE	 hDll; /* Shared libary handle *///已载入动态链接库的handle
} SERVER_ARG;

#define MAX_RQRS_LEN 0x1000
//定义了一个结构用于保存请求的结构，用于传出数据
//使用LONG32便于类型兼容
//BYTE也是为了兼容长度由MAX_RQRS_LEN宏定义
typedef struct {	/* Same as a message; only the length field has a different name */
	LONG32 rqLen;	/* Total length of request, not including this field */
	BYTE record[MAX_RQRS_LEN];
} REQUEST;//请求 包含长度/字节（内容）
//同上
typedef struct {	/* Same as a message; only the length field has a different name */
	LONG32 rsLen;	/* Total length of response, not including this field */
	BYTE record[MAX_RQRS_LEN];
} RESPONSE;//返回

static BOOL ReceiveRequestMessage(REQUEST *pRequest, SOCKET);//向SOCKET发送REQUEST
static BOOL SendResponseMessage(RESPONSE *pResponse, SOCKET);
static DWORD WINAPI Server(PVOID);//创建服务器的线程
static DWORD WINAPI AcceptThread(PVOID);//创建应答服务的线程
static BOOL  WINAPI Handler(DWORD);//

volatile static int shutFlag = 0;//全局静态标志--- 终止标识
static SOCKET SrvSock = INVALID_SOCKET, connectSock = INVALID_SOCKET;
//首先定义了两个SOCKET 一个是服务器SOCKET，一个是用来接受客户端的socket的

#define MAX_CLIENTS  4 ///***10 /* Maximum number of clients for serverNP */
//最大的队列客户端数量
#if defined(UTILITY_4_0_EXPORTS)
#define LIBSPEC __declspec (dllexport)
#elif defined(__cplusplus)
#define LIBSPEC extern "C" __declspec (dllimport)//定义LIBSPEC为函数输出结构
#else
#define LIBSPEC __declspec (dllimport)
#endif

LIBSPEC BOOL WindowsVersionOK(DWORD, DWORD);
LIBSPEC VOID ReportError(LPCTSTR, DWORD, BOOL);

//定义服务器端口
#define SERVER_PORT 50000   /* Well known port. */

LIBSPEC BOOL PrintMsg(HANDLE, LPCTSTR);

//定义了客户端的超时时间
#define CS_TIMEOUT 5000

#define RQ_SIZE sizeof (REQUEST)//宏定义获取请求的大小
#define RQ_HEADER_LEN RQ_SIZE-MAX_RQRS_LEN

#define RS_SIZE sizeof (RESPONSE)//宏定义回应的大小
#define RS_HEADER_LEN RS_SIZE-MAX_RQRS_LEN
/* Timeout period for named pipe connections and performance monitoring. */
//命名管道连接和性能监视的时间节点
int _tmain(int argc, LPCTSTR argv[])
{
	/* Server listening and connected sockets. */
	//服务器监听并连接socket
	DWORD iThread, tStatus;
	SERVER_ARG sArgs[MAX_CLIENTS];//定义一个服务器参数的数组
	HANDLE hAcceptThread = NULL;//初始化一个应答线程，
	HINSTANCE hDll = NULL;//初始化一个载入DLL的handle

	if (!WindowsVersionOK(3, 1))//初始化windows套接字，失败后报错
		ReportError(_T("This program requires Windows NT 3.1 or greater"), 1, FALSE);

	/* Console control handler to permit server shutdown */
	//控制台控制句柄来许可服务器的关闭
	if (!SetConsoleCtrlHandler(Handler, TRUE))
		ReportError(_T("Cannot create Ctrl handler"), 1, TRUE);

	/*	Initialize the WS library. Ver 2.0 */
	//初始化windows套接字动态链接库
	if (WSAStartup(MAKEWORD(2, 0), &WSStartData) != 0)
		ReportError(_T("Cannot support sockets"), 1, TRUE);

	/* Open the shared command library DLL if it is specified on command line */
	//打开一个共享命令动态链接库如果制定了一个命令行
	if (argc > 1) {
		hDll = LoadLibrary(argv[1]);//在传入的参数中取到第二个载入
		if (hDll == NULL) ReportError(argv[1], 0, TRUE);//载入错误提出错误
	}

	/* Intialize thread arg array */
	//初始化线程参数数组
	for (iThread = 0; iThread < MAX_CLIENTS; iThread++) {//当线程小于最大线程数时，新建线程
		InitializeCriticalSection(&sArgs[iThread].threadCs);//初始化一个临界对象用于资源访问
		sArgs[iThread].number = iThread;//线程序号
		sArgs[iThread].thState = SERVER_SLOT_FREE;//设置线程状态
		sArgs[iThread].sock = 0;//socket初始化为0
		sArgs[iThread].hDll = hDll;//服务器handle
		sArgs[iThread].hSrvThread = NULL;//dll的实例初始化为空
	}
	/*	Follow the standard server socket/bind/listen/accept sequence */
	//遵循标准的服务器socket 绑定  监听 应答 序列
	SrvSock = socket(PF_INET, SOCK_STREAM, 0);//初始化TCP socket
	if (SrvSock == INVALID_SOCKET)//初始化socket失败的时候，报告错误
		ReportError(_T("Failed server socket() call"), 1, TRUE);

	/*	Prepare the socket address structure for binding the
	server socket to port number "reserved" for this service.
	Accept requests from any client machine.  */
	//准备socket地址结构来绑定服务器socket用端口号reserved对于此服务
	//应答来自于任意服务器的请求

	srvSAddr.sin_family = AF_INET;//tcp/udp
	srvSAddr.sin_addr.s_addr = htonl(INADDR_ANY);//初始化本机地址
	srvSAddr.sin_port = htons(SERVER_PORT);//监听端口
	if (bind(SrvSock, (struct sockaddr *)&srvSAddr, sizeof(srvSAddr)) == SOCKET_ERROR)//绑定失败报错
		ReportError(_T("Failed server bind() call"), 2, TRUE);
	if (listen(SrvSock, MAX_CLIENTS) != 0)//舰艇失败  报错
		ReportError(_T("Server listen() error"), 3, TRUE);

	/* Main thread becomes listening/connecting/monitoring thread */
	//主线程变为  监听 连接 显示 线程
	/* Find an empty slot in the server thread arg array */
	//在服务器线程参数数组中找到一个空绑定状态的端口
	while (!shutFlag) {//如果shutFlag为假
		iThread = 0;//将线程设置为0
		while (!shutFlag) {
			/* Continously poll the thread thState of all server slots in the sArgs table */
			//连续的在服务器参数列表数组中检查所有的服务器线程状态
			EnterCriticalSection(&sArgs[iThread].threadCs);//进入线程的互斥量
			__try {
				if (sArgs[iThread].thState == SERVER_THREAD_STOPPED) {
					//当本线程正常停止，或者有一个关闭请求
					/* This thread stopped, either normally or there's a shutdown request */
					//等待它关闭，并将释放端口来接受其他线程
					/* Wait for it to stop, and make the slot free for another thread */
					tStatus = WaitForSingleObject(sArgs[iThread].hSrvThread, INFINITE);//无限时间内挂起线程
					if (tStatus != WAIT_OBJECT_0)//如果状态不是WAIT_OBJECT_0
						ReportError(_T("Server thread wait error"), 4, TRUE);
					CloseHandle(sArgs[iThread].hSrvThread);//处于等待状态，那么关闭线程
					sArgs[iThread].hSrvThread = NULL;//将线程制空
					sArgs[iThread].thState = SERVER_SLOT_FREE;//改变线程状态
				}
				/* Free slot identified or shut down. Use a free slot for the next connection */
				//释放槽识别码或关闭，使用一个空槽开建立下一次连接
				if (sArgs[iThread].thState == SERVER_SLOT_FREE || shutFlag) break;//如果线程的状态已经是free退出
			}
			__finally { LeaveCriticalSection(&sArgs[iThread].threadCs); }
			//线程推出临界区
			/* Fixed July 25, 2014: iThread = (iThread++) % MAX_CLIENTS; */

			iThread = (iThread + 1) % MAX_CLIENTS;//判断是否还有线程
			if (iThread == 0) Sleep(50); /* Break the polling loop */
			//破坏轮询循环
										 /* An alternative would be to use an event to signal a free slot */
			//另一种不要常用的方法是使用事件来标记一个空闲槽
		}
		if (shutFlag) break;
		/* sArgs[iThread] == SERVER_SLOT_FREE */
		/* Wait for a connection on this socket */
		//在这个socket中等待一个连接
		/* Use a separate thread so that we can poll the shutFlag flag */
		//使用一个分离的线程让我们可以轮询关闭标志
		hAcceptThread = (HANDLE)_beginthreadex(NULL, 0, AcceptThread, &sArgs[iThread], 0, NULL);//创建应答线程
		if (hAcceptThread == NULL)//如果应答线程没有成功创建
			ReportError(_T("Error creating AcceptThreadread."), 1, TRUE);//报告错误
		while (!shutFlag) {//如果关闭标志是false
			tStatus = WaitForSingleObject(hAcceptThread, CS_TIMEOUT);//按等待事件等待线程
			if (tStatus == WAIT_OBJECT_0) {//如果线程处于等待状态
				/* Connection is complete. sArgs[iThread] == SERVER_THREAD_RUNNING */
				if (!shutFlag) {
					CloseHandle(hAcceptThread);//关闭线程
					hAcceptThread = NULL;//制空handle
				}
				break;
			}
		}
	}  /* OUTER while (!shutFlag) */

	   /* shutFlag == TRUE */
	_tprintf(_T("Server shutdown in process. Wait for all server threads\n"));
	/* Wait for any active server threads to terminate */
	//等待任何活动的服务器线程来终止
	/* Try continuously as some threads may be long running. */
	//连续尝试，因为有些线程可能运行很长时间。

	while (TRUE) {
		int nRunningThreads = 0;//运行线程计数输出化 0
		for (iThread = 0; iThread < MAX_CLIENTS; iThread++) {//遍历线程
			EnterCriticalSection(&sArgs[iThread].threadCs);//进入互斥量
			__try {
				if (sArgs[iThread].thState == SERVER_THREAD_RUNNING || sArgs[iThread].thState == SERVER_THREAD_STOPPED) {//线程运行中或者终止
					if (WaitForSingleObject(sArgs[iThread].hSrvThread, 10000) == WAIT_OBJECT_0) {//线程等待
						_tprintf(_T("Server thread on slot %d stopped.\n"), iThread);
						CloseHandle(sArgs[iThread].hSrvThread);
						sArgs[iThread].hSrvThread = NULL;
						sArgs[iThread].thState = SERVER_SLOT_INVALID;
					}
					else
						if (WaitForSingleObject(sArgs[iThread].hSrvThread, 10000) == WAIT_TIMEOUT) {//没有关闭则仍在运行当中
							_tprintf(_T("Server thread on slot %d still running.\n"), iThread);
							nRunningThreads++;
						}
						else {//否则出错
							_tprintf(_T("Error waiting on server thread in slot %d.\n"), iThread);
							ReportError(_T("Thread wait failure"), 0, TRUE);
						}

				}
			}
			__finally { LeaveCriticalSection(&sArgs[iThread].threadCs); }//离开互斥量
		}
		if (nRunningThreads == 0) break;//如果没有运行中的线程则终止
	}

	if (hDll != NULL) FreeLibrary(hDll);//释放动态链接库

	/* Redundant shutdown */
	shutdown(SrvSock, SD_BOTH);//停止socket
	closesocket(SrvSock);//关闭socket
	WSACleanup();//清理初始化的socket
	if (hAcceptThread != NULL && WaitForSingleObject(hAcceptThread, INFINITE) != WAIT_OBJECT_0)//应答线程为空或者线程不处于等待
		ReportError(_T("Failed waiting for accept thread to terminate."), 7, FALSE);
	return 0;
}

static DWORD WINAPI AcceptThread(PVOID pArg)//应答线程
{
	LONG addrLen;
	SERVER_ARG * pThArg = (SERVER_ARG *)pArg;//将参数存入新量

	addrLen = sizeof(connectSAddr);//获取连接信息的大小
	pThArg->sock = accept(SrvSock, (sockaddr *)&connectSAddr, (int *)&addrLen);//socket赋值
	if (pThArg->sock == INVALID_SOCKET) {//如果应答失败
		ReportError(_T("accept: invalid socket error"), 1, TRUE);
		return 1;
	}
	/* A new connection. Create a server thread */
	EnterCriticalSection(&(pThArg->threadCs));//进入互斥量
	__try {
		pThArg->hSrvThread = (HANDLE)_beginthreadex(NULL, 0, Server, pThArg, 0, NULL);//开始线程
		if (pThArg->hSrvThread == NULL)//如果新建县城失败
			ReportError(_T("Failed creating server thread"), 1, TRUE);
		pThArg->thState = SERVER_THREAD_RUNNING;
		_tprintf(_T("Client accepted on slot: %d, using server thread %d.\n"), pThArg->number, GetThreadId(pThArg->hSrvThread));
		/* Exercise: Display client machine and process information */
		//练习：显示客户机和进程信息
	}
	__finally { LeaveCriticalSection(&(pThArg->threadCs)); }//退出互斥量

	return 0;
}

BOOL WINAPI Handler(DWORD CtrlEvent)
{
	/* Recives ^C. Shutdown the system */
	//接收到^C后关闭系统
	_tprintf(_T("In console control handler\n"));
	InterlockedIncrement((long *)&shutFlag);
	return TRUE;
}

static DWORD WINAPI Server(PVOID pArg)//服务器线程

/* Server thread function. There is a thread for every potential client. */
//服务器线程函数，创建每一个潜在服务线程
{
	/* Each thread keeps its own request, response,
	//每个线程保持自己的请求，回复
	and bookkeeping data structures on the stack. */
	//和栈中的数据结构
	/* NOTE: All messages are in 8-bit characters */
	//注意所有的信息都是使用的8为结构
	BOOL done = FALSE;//完成标志
	STARTUPINFO startInfoCh;//开启信息
	SECURITY_ATTRIBUTES tempSA = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
	PROCESS_INFORMATION procInfo;
	SOCKET connectSock;//客户端socket
	int commandLen;//命令长度
	REQUEST request;	/* Defined in ClientServer.h */
	RESPONSE response;	/* Defined in ClientServer.h.*/
	char sysCommand[MAX_RQRS_LEN];
	TCHAR tempFile[100];
	HANDLE hTmpFile;
	FILE *fp = NULL;
	int(__cdecl *dl_addr)(char *, char *);//定义一个函数指针
	SERVER_ARG * pThArg = (SERVER_ARG *)pArg;
	enum SERVER_THREAD_STATE threadState;//服务器线程状态

	GetStartupInfo(&startInfoCh);

	connectSock = pThArg->sock;
	/* Create a temp file name */

	tempFile[sizeof(tempFile) / sizeof(TCHAR) - 1] = _T('\0');
	_stprintf_s(tempFile, sizeof(tempFile) / sizeof(TCHAR) - 1, _T("ServerTemp%d.tmp"), pThArg->number);

	while (!done && !shutFlag) { 	/* Main Server Command Loop. */
		//主服务器命令循环
		//重置完成标志   终止标志
		done = ReceiveRequestMessage(&request, connectSock);

		request.record[sizeof(request.record) - 1] = '\0';
		commandLen = (int)(strcspn((char *)request.record, "\n\t"));
		memcpy(sysCommand, request.record, commandLen);
		sysCommand[commandLen] = '\0';
		_tprintf(_T("Command received on server slot %d: %s\n"), pThArg->number, sysCommand);

		/* Restest shutFlag, as it can be set from the console control handler. */
		//重新测试关闭标志，因为它可以从控制台控件处理程序中设置。
		done = done || (strcmp((char *)request.record, "$Quit") == 0) || shutFlag;
		if (done) continue;//如果done为真那么继续

		/* Open the temporary results file. */
		//打开临时结果文件
		hTmpFile = CreateFile(tempFile, GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &tempSA,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hTmpFile == INVALID_HANDLE_VALUE)
			ReportError(_T("Cannot create temp file"), 1, TRUE);

		/* Check for a shared library command. For simplicity, shared 	*/
		//检查一个共享的库命令，为了简单一点
		/* library commands take precedence over process commands 	*/
		//共享库命令有限处理进程命令
		/* First, extract the command name (space delimited) */
		//首先 解压命令名 空格分隔
		dl_addr = NULL; /* will be set if GetProcAddress succeeds */
		//如果GetProcAddress成功了 那么将会被设置
		if (pThArg->hDll != NULL) { /* Try Server "In process" *///尝试进程内服务器
			char commandName[256] = "";//名称制空
			int commandNameLength = (int)(strcspn(sysCommand, " "));
			strncpy_s(commandName, sizeof(commandName), sysCommand, min(commandNameLength, sizeof(commandName) - 1));
			dl_addr = (int(*)(char *, char *))GetProcAddress(pThArg->hDll, commandName);
			/* You really need to trust this DLL not to corrupt the server */
			//你必须信任改dll不会让服务终止
			/* This assumes that we don't allow the DLL to generate known exceptions */
			//假设我们不允许dll产生已知异常
			if (dl_addr != NULL) __try {
				(*dl_addr)((char *)request.record, (char *)tempFile);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { /* Exception in the DLL *///dll中的异常
				_tprintf(_T("Unhandled Exception in DLL. Terminate server. There may be orphaned processes."));
				return 1;
			}
		}

		if (dl_addr == NULL) { /* No inprocess support *///没有进程内支持
							   /* Create a process to carry out the command. *///创建一个进程来执行命令
			startInfoCh.hStdOutput = hTmpFile;
			startInfoCh.hStdError = hTmpFile;
			startInfoCh.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			startInfoCh.dwFlags = STARTF_USESTDHANDLES;
			if (!CreateProcess(NULL, (LPWSTR)request.record, NULL,
				NULL, TRUE, /* Inherit handles. */
				0, NULL, NULL, &startInfoCh, &procInfo)) {
				PrintMsg(hTmpFile, _T("ERR: Cannot create process."));
				procInfo.hProcess = NULL;
			}
			CloseHandle(hTmpFile);
			if (procInfo.hProcess != NULL) {
				CloseHandle(procInfo.hThread);
				WaitForSingleObject(procInfo.hProcess, INFINITE);
				CloseHandle(procInfo.hProcess);
			}
		}

		/* Respond a line at a time. It is convenient to use
		C library line-oriented routines at this point. */

		/* Send the temp file, one line at a time, with header, to the client. */
		if (_tfopen_s(&fp, tempFile, _T("r")) == 0) {
			{
				response.rsLen = MAX_RQRS_LEN;
				while ((fgets((char *)response.record, MAX_RQRS_LEN, fp) != NULL))
					SendResponseMessage(&response, connectSock);
			}
			/* Send a zero length message. Messages are 8-bit characters, not UNICODE. */
			response.record[0] = '\0';
			SendResponseMessage(&response, connectSock);
			fclose(fp); fp = NULL;
			DeleteFile(tempFile);
		}
		else {
			ReportError(_T("Failed to open temp file with command results"), 0, TRUE);
		}

	}   /* End of main command loop. Get next command */

		/* done || shutFlag */
		/* End of command processing loop. Free resources and exit from the thread. */
	_tprintf(_T("Shuting down server thread number %d\n"), pThArg->number);
	/* Redundant shutdown. There are no further attempts to send or receive */
	shutdown(connectSock, SD_BOTH);
	closesocket(connectSock);

	EnterCriticalSection(&(pThArg->threadCs));
	__try {
		threadState = pThArg->thState = SERVER_THREAD_STOPPED;
	}
	__finally { LeaveCriticalSection(&(pThArg->threadCs)); }

	return threadState;
}

BOOL ReceiveRequestMessage(REQUEST *pRequest, SOCKET sd)
{
	BOOL disconnect = FALSE;
	LONG32 nRemainRecv = 0, nXfer;
	LPBYTE pBuffer;

	/*	Read the request. First the header, then the request text. */
	nRemainRecv = RQ_HEADER_LEN;
	pBuffer = (LPBYTE)pRequest;

	while (nRemainRecv > 0 && !disconnect) {
		nXfer = recv(sd, (char *)pBuffer, nRemainRecv, 0);
		if (nXfer == SOCKET_ERROR)
			ReportError(_T("server request recv() failed"), 11, TRUE);
		disconnect = (nXfer == 0);
		nRemainRecv -= nXfer; pBuffer += nXfer;
	}

	/*	Read the request record */
	nRemainRecv = pRequest->rqLen;
	/* Exclude buffer overflow */
	nRemainRecv = min(nRemainRecv, MAX_RQRS_LEN);

	pBuffer = (LPBYTE)(LPSTR)pRequest->record;
	while (nRemainRecv > 0 && !disconnect) {
		nXfer = recv(sd, (char *)pBuffer, nRemainRecv, 0);
		if (nXfer == SOCKET_ERROR)
			ReportError(_T("server request recv() failed"), 12, TRUE);
		disconnect = (nXfer == 0);
		nRemainRecv -= nXfer; pBuffer += nXfer;
	}

	return disconnect;
}

BOOL SendResponseMessage(RESPONSE *pResponse, SOCKET sd)
{
	BOOL disconnect = FALSE;
	LONG32 nRemainRecv = 0, nXfer, nRemainSend;
	LPBYTE pBuffer;

	/*	Send the response up to the string end. Send in
	two parts - header, then the response string. */
	nRemainSend = RS_HEADER_LEN;
	pResponse->rsLen = (long)(strlen((const char *)pResponse->record) + 1);
	pBuffer = (LPBYTE)pResponse;
	while (nRemainSend > 0 && !disconnect) {
		nXfer = send(sd, (const char *)pBuffer, nRemainSend, 0);
		if (nXfer == SOCKET_ERROR) ReportError(_T("server send() failed"), 13, TRUE);
		disconnect = (nXfer == 0);
		nRemainSend -= nXfer; pBuffer += nXfer;
	}

	nRemainSend = pResponse->rsLen;
	pBuffer = (LPBYTE)(LPSTR)pResponse->record;
	while (nRemainSend > 0 && !disconnect) {
		nXfer = send(sd, (const char *)pBuffer, nRemainSend, 0);
		if (nXfer == SOCKET_ERROR) ReportError(_T("server send() failed"), 14, TRUE);
		disconnect = (nXfer == 0);
		nRemainSend -= nXfer; pBuffer += nXfer;
	}
	return disconnect;
}

