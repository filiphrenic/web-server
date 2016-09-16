#include "mojweb.h"

void Log(const char* fmt, ...){
    va_list args;
    va_start(args, fmt);
    if (is_daemon)
        vsyslog(LOG_INFO, fmt, args);
    else
        vfprintf(stderr, fmt, args);
    va_end(args);
}

int TurnOn(const char* udp_port){
	int socket = UDPserver(udp_port);
	char* buff = MLC(char, 10);
	ssize_t len;

	while(1){
		len = Recvfrom(socket, buff, 10, 0, NULL, NULL);
		if (len < 0) Error("udp recvfrom");
		if (len == 0) continue;
		if (len >= 2 && !strncmp("ON", buff, 2))
			break;
	}

	Log("Recieved 'ON'\n");
	free(buff);
	return socket;
}

int TurnOff(int socket){
	char* buff = MLC(char, 10);
	ssize_t len;
	int ret = 0;

	len = Recvfrom(socket, buff, 10, 0, NULL, NULL);
	if (len < 0) Error("udp recvfrom");
	if (len >= 3 && !strncmp("OFF", buff, 3)){
		Log("Recieved OFF\n");
		ret = 1;
	}

	free(buff);
	return ret;
}

char* Status(int code){
    switch (code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 500:
            return "Internal Server Error";
        default:
            return "";
    }
}

void WriteHeader(int socket, int code, int close_conn, int content_length, const char* type) {

	char* buff = MLC(char, BUFFER_LEN);
	char* status = Status(code);

	sprintf(buff, "HTTP/1.1 %d %s\r\n", code, status);
	Writen(socket, buff, strlen(buff));

	if ( content_length ){
		sprintf(buff, "Content-Length: %d\r\n", content_length);
		Writen(socket, buff, strlen(buff));
	}

	if ( type != NULL ){
		sprintf(buff, "Content-Type: %s\r\n", type);
		Writen(socket, buff, strlen(buff));
	}

	if ( close_conn != -1 ){
		sprintf(buff, "Connection: %s\r\n",  close_conn ? "close" : "keep alive");
		Writen(socket, buff, strlen(buff));
	}

	Writen(socket, "\r\n", 2);

	char* ip = GetClientInfo(socket, NULL);
	Log("%s <- [%d %s]\n", ip, code, status);
	free(ip);
}

void HttpError(int client, int code){
	// if server error, close connection
	int close_conn = code == 500;

	char* buff = MLC(char, 50);
	sprintf(buff, "<html><body><h1>%d %s</h1></body></html>", code, Status(code));
	int len = strlen(buff);
	WriteHeader(client, code, close_conn, len, "text/html");
	Writen(client, buff, len);
	free(buff);
}

void* ProcessClient(void* args){
	int i,j, socket = *((int*) args);
	char* path = MLC(char, BUFFER_LEN_SMALL);
	char* request = MLC(char, BUFFER_LEN);
	int req_len;
	char* ip = GetClientInfo(socket, NULL);

	SetTimeout(socket, WAIT_SECS, 0);

	while(1){
		memset(request, 0, BUFFER_LEN);
		req_len = Recv(socket, request, BUFFER_LEN, 0);

		if (req_len < 0){
			if (errno == EWOULDBLOCK)
				Log("No requests for %d seconds, exiting client thread\n", WAIT_SECS);
			else
				Warnx("recv: %s\n", strerror(errno));
			break;
		}
		else if (req_len == 0)
			break;

		if (!strncmp(request, "GET", 3) || !strncmp(request, "get", 3)){

			if (req_len < 4){
				HttpError(socket, 400);
				continue;
			}

			for(i=3; i<req_len && isspace(request[i]) ; i++);
			for(j=0; i<req_len && !isspace(request[i]); i++)
				path[j++] = request[i];

			if (j==0){
				HttpError(socket, 400);
				continue;
			}

			path[j]=0;
			Log("%s -> GET %s\n", ip, path);
			Get(socket, path);

		} else
			HttpError(socket, 405);
	}

	Close(socket);
	free(ip);
	free(request);
	free(path);
	pthread_exit(0);
}

int main(int argc, char** argv){

	char* tcp_port = MLC(char, PORT_LEN);
	char* root_dir = MLC(char, PATH_LEN);
	char* udp_port = NULL;
	char* ip;
	int s, make_daemon = 0;
	char ch;

	// connection
	int tcp_sock = -1;
	int udp_sock = -1;
	int max_sock = -1;
	fd_set sockets, tmp;

	// client
	int client_sock;
	struct sockaddr client;
	socklen_t client_len;

	// threads
	int num_threads = 0;
	pthread_t tids[MAX_THREAD];

	// init options
	strcpy(root_dir, ROOT_DEFAULT);
	while ( (ch=getopt(argc, argv, "dr:")) != -1 ){
		switch (ch) {
			case 'd':
				make_daemon = 1;
				break;
			case 'r':
				strcpy(root_dir, optarg);
				break;
			default:
				Usage(argv[0]);
		}
	}

	CheckRootDir(root_dir);

	switch (argc - optind) {
		case 0:
			strcpy(tcp_port, PORT_DEFAULT);
			break;
		case 2:
			udp_port = MLC(char, PORT_LEN);
			strcpy(udp_port, argv[optind+1]);
		case 1:
			strcpy(tcp_port, argv[optind]);
			break;
		default:
			Usage(argv[0]);
	}

	if (chdir(root_dir)) Error("chdir");

	if (udp_port != NULL)
		udp_sock = TurnOn(udp_port);

	tcp_sock = TCPserver(tcp_port, BACKLOG);
	max_sock = MAX(udp_sock, tcp_sock);

	if (make_daemon){
		Log("Daemonizing\n");
		Daemon(1,0); // dont change dir, close
		openlog("fh47758:mrepro mojweb", LOG_PID, LOG_LOCAL0);
	}

	FD_ZERO(&sockets);
	if (tcp_sock != -1) FD_SET(tcp_sock, &sockets);
	if (udp_sock != -1) FD_SET(udp_sock, &sockets);

	while(1){

        if (num_threads >= MAX_THREAD){
            FD_CLR(tcp_sock, &sockets);
            Close(tcp_sock);
        }

		tmp = sockets;

		if (select(max_sock+1, &sockets, NULL, NULL, NULL) == -1)
			Error("select");

		for (s = 0; s <= max_sock; s++) {
			if (!FD_ISSET(s, &sockets))
				continue;
			if (s==udp_sock){
				if (TurnOff(udp_sock))
					break;
			}
			if (s==tcp_sock){
				client_sock = Accept(tcp_sock, &client, &client_len);
				ip = GetClientInfo(client_sock, NULL);
				Log("New client: %s\n", ip);
				free(ip);
				pthread_create(tids + num_threads++, NULL, ProcessClient, (void*) &client_sock);
			}
		}

		// check if udp got OFF
		if (s<=max_sock) break;

		sockets = tmp;
	}

	Log("Waiting for threads to finish\n");

	// wait for clients
	for(s=0;s<num_threads;s++)
		pthread_join(tids[s],NULL);

	Log("Threads done, exiting\n");

	// release resources
	Close(tcp_sock);
	free(root_dir);
	free(tcp_port);
	return 0;
}
