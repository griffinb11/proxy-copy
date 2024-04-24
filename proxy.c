/*
 * COMP 321 Project 6: Web Proxy
 *
 * This program implements a multithreaded HTTP proxy.
 *
 * Griffin Ball gab11
 */


#include <assert.h>
#include <stdbool.h>

#include "csapp.h"

#define NTHREADS 10
#define SBUFSIZE 40

struct conn_info
{
	int connfd; //client file descriptor
	struct sockaddr_in clientaddr; // client socket address
};

// structure to represent the shared buffer
struct sbuf_t
{
	struct conn_info *buf;	   
	int shared_cnt;			  
	int n;					   
	int front;				   
	int rear;				   
	pthread_mutex_t mutex;	  
	pthread_cond_t cond_empty; 
	pthread_cond_t cond_full;  
};

static void
client_error(int fd, const char *cause, int err_num, const char *short_msg, const char *long_msg);
static char *
create_log_entry(const struct sockaddr_in *sockaddr, const char *uri, int size);
static int
parse_uri(const char *uri, char **hostnamep, char **portp, char **pathnamep);


// helper function to get a listening socket
static int
open_listen(int port);


// helper function that opens a connection and returns a file descriptor
static int
open_client(char *hostname, char *port, int connfd, char *request, struct sockaddr_in clientaddr, char *request_url);


static void
doit(struct conn_info connection);


// function to read and rebuild request headers
static char *
read_requesthdrs(rio_t *rp, char *request);


// function that trys to continously remove request from shared buffer
static void *
start_routine(void *vargp);

// helper function to initialize shared buffer and prethreading
static void
sbuf_init(struct sbuf_t *sp, int n);

// helper function to insert a request into the shared buffer
static void
sbuf_insert(struct sbuf_t *sp, int connfd, struct sockaddr_in clientaddr);


// helper function to remove and handle a request from the shared buffer
static struct conn_info
sbuf_remove(struct sbuf_t *sp);


//log file
static FILE *logfd;

//shared buffer
struct sbuf_t *sbuf;

/*
 * Requires:
 *   argv[1] is a valid port number
 *
 * Effects:
 *   Creates the threads used to handle the requests, accepts requests and 
 *   inserts them in the shared buffer.
 *  
 */
int 
main(int argc, char **argv)
{
	char client_hostname[MAXLINE], haddrp[INET_ADDRSTRLEN];
	pthread_t threads;
	int connfd;
	int error; 
	int listenfd;
	int i; 
	int port;
	struct sockaddr_in clientaddr;
	socklen_t clientlen;

	
	//open log file in append mode, creates it if it doesn't exist
	logfd = fopen("proxy.log", "a+");
	if (logfd == NULL) {
		return -1;
	}

	/* Check the arguments. */
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
		exit(0);
	}
	
	port = atoi(argv[1]);
	
	// listen for requests
	listenfd = open_listen(port);

	if (listenfd < 0) {
		unix_error("open_listen error");
	}

	// ignore SIGPIPE 
	Signal(SIGPIPE, SIG_IGN);

	// call helper function to initialize shared buffer
	sbuf_init(sbuf, SBUFSIZE);
	
	// create each thread 
	for (i = 0; i < NTHREADS; i++) {
		
		Pthread_create(&threads, NULL, start_routine, NULL);
	}

	
	while (true) {
		clientlen = sizeof(struct sockaddr_in);

		// accept a connection and assign a file descriptor
		connfd = accept(listenfd, (struct sockaddr *)&clientaddr,
						&clientlen);

		// get the client info
		error = getnameinfo((struct sockaddr *)(&clientaddr), clientlen,
						client_hostname, sizeof(client_hostname), NULL, 0, 0);
						
		// close the connection if an error occurs
		if (error != 0) {
			fprintf(stderr, "ERROR: %s\n", gai_strerror(error));
			Close(connfd);
			continue;
		}

		Inet_ntop(AF_INET, &clientaddr.sin_addr, haddrp,
				  INET_ADDRSTRLEN);
		
		// call helper function to insert into shared buffer
		sbuf_insert(sbuf, connfd, clientaddr);

	}
	//Close(logfd);

	/* Return success. */
	return (0);
}

/*
 *	Requires:
 *		none
 *	
 *	Effects:
 *		Removes and handles requests from the shared buffer.
 */
void *
start_routine(void *vargp)
{
	(void)vargp;

	Pthread_detach(pthread_self());
	while (true) {
		// call helper function to remove request from shared bufer
		struct conn_info connfd_actual = sbuf_remove(sbuf);

		doit(connfd_actual);

		// close the connection after calling doit to handle the request
		Close(connfd_actual.connfd);
	}

	return (NULL);
}

/* 
 * Requires:
 *   sp is a pointer to the shared buffer.
 *
 * Effects:
 *   Using mutex locks inserts a connection into the shared buffer.
 */
void 
sbuf_insert(struct sbuf_t *sp, int connfd, struct sockaddr_in clientaddr)
{
	//acquire lock
	pthread_mutex_lock(&(sbuf->mutex));

	// the condition the buffer is full
	while (sbuf->shared_cnt == sbuf->n) {
		pthread_cond_wait(&(sbuf->cond_empty), &(sbuf->mutex));
	}

	// insert and update count
	sp->buf[(++sp->rear) % (sp->n)] = (struct conn_info){connfd, clientaddr};


	sp->shared_cnt++;

	// release lock
	pthread_cond_broadcast(&sbuf->cond_full);
	pthread_mutex_unlock(&sbuf->mutex);
}

/* 
 * Requires:
 *   sp is a pointer to the shared buffer
 *
 * Effects:
 *  Using mutex locks removes a request from the shared buffer
 */
struct conn_info 
sbuf_remove(struct sbuf_t *sp)
{
	// acquire lock
	pthread_mutex_lock(&sbuf->mutex);

	// the condition the buffer is empty
	while (sbuf->shared_cnt == 0) {
		pthread_cond_wait(&(sbuf->cond_full), &(sbuf->mutex));
	}


	struct conn_info connection;
	int front = ++sp->front;
	int bottom = sp->n;
	int mid = (front) % (bottom);
	connection = (sp->buf[mid]); 

	// update count
	sp->shared_cnt--;

	// release lock
	pthread_cond_broadcast(&sbuf->cond_empty);

	pthread_mutex_unlock(&sbuf->mutex);

	return (connection);
}

/* 
 * Requires:
 *   a positive integer n
 *
 * Effects:
 *   Initializes the shared buffer and prethreading 
 */
void 
sbuf_init(struct sbuf_t *sp, int n)
{
	
	(void)sp;

	// allocate memory for the shared buffer
	sbuf = Malloc(sizeof(struct sbuf_t));

	// allocate memory for connections
	sbuf->buf = Calloc(n, sizeof(struct conn_info));

	sbuf->n = n; 
	sbuf->front = 0;
	sbuf->rear = 0;
	sbuf->shared_cnt = 0;
	

	
	pthread_mutex_init(&sbuf->mutex, NULL);
	pthread_cond_init(&sbuf->cond_empty, NULL);
	pthread_cond_init(&sbuf->cond_full, NULL);

}

/* 
 * Requires:
 *   none. 
 *
 * Effects:
 *    Handles the clients request and calls open_client
 */
static void 
doit(struct conn_info connection)
{
	struct sockaddr_in *clientaddr;
	char *hostnamep;
	char *portp; 
	char*pathnamep; 
	char *request_url; 
	char *buf;
	rio_t rio;
	char method[MAXLINE], request[MAXBUF], version[MAXLINE],
		 temp_buf[MAXLINE];
	int fd;
	int bufSize;
	int temp_size;

	fd = connection.connfd;
	clientaddr = &connection.clientaddr;

	// buffer memory allocation
	buf = malloc(MAXLINE + 1);


	
	rio_readinitb(&rio, fd);

	// copy the request line to buffer
	bufSize = rio_readlineb(&rio, buf, MAXLINE);

	// if the request is longer than MAXLINE
	if (bufSize == MAXLINE - 1) {
		temp_size = bufSize;
		while(temp_buf[temp_size - 1] != '\n') {
			if ((temp_size = rio_readlineb(&rio, temp_buf, MAXLINE)) == -1) {
				free(buf);
				fprintf(stdout, "rio_readlineb Error: Connection closed!\n");
				return;
			}
			bufSize += temp_size;
			buf = realloc(buf, MAXLINE + 1);
			sprintf(buf, "%s%s", buf, temp_buf);
		}

	}
	
	// allocate memory for the request url
	request_url = malloc(bufSize);

	
	sscanf(buf, "%s %s %s", method, request_url, version);
	
	// If its not a get request
	if (strstr(method, "GET") == NULL) {
		client_error(fd, "Request is not a GET request.", 400, "Bad Request",
		 "The given request does not contain a 'GET'. Try again!");
		free(buf);
		free(request_url);
	// read the request otherwise
	} else if (parse_uri(request_url, &hostnamep, &portp, &pathnamep) == -1) {
		client_error(fd, "Requested file cannot be found.", 404, "Not found",
			 "The requested file does not exist.");
		free(buf);
		free(request_url);
	} else {
		// format the request
		sprintf(request, "GET %s %s\r\n", pathnamep, version);

		// call function to read headers
		read_requesthdrs(&rio, request);

		
		open_client(hostnamep, portp, fd, request, *clientaddr, request_url);

		// free memory
		free(hostnamep);
		free(portp);
		free(pathnamep);
		free(buf);
		free(request_url);
	}

	
}


/* 
 * Requires:
 *   none
 *
 * Effects:
 *   reads and reformats request headers 
 */
static char *
read_requesthdrs(rio_t *rp, char *request)
{
	
	char buf[MAXLINE];

	// read to the buffer
	rio_readlineb(rp, buf, MAXLINE);

	// read one line at a time
	while (strcmp(buf, "\r\n")) {

		// append headers that don't include connection
		if (strstr(buf, "Connection") == NULL) {
			sprintf(request, "%s%s", request, buf);
		}
		// go to next line
		rio_readlineb(rp, buf, MAXLINE);
	}

	sprintf(request, "%sConnection: close\r\n", request);

	return (NULL);
}

/*
 *	Requires:
 *		input port is an unused TCP port number
 *
 *	Effects:
 *		returns a listening socket on the input port number
 *
 */
static int
open_listen(int port)
{

	int listenfd, optval;
	struct sockaddr_in serveraddr;
	

	// create new stream socket
	if ((listenfd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
		return (-1);
	
	optval = 1;
	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
				   (const void *)&optval, sizeof(int)) == -1)
		return (-1);
	memset(&serveraddr, 0, sizeof(serveraddr));

	
	// set the port to the input port and the ip adrress in serveraddr
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons((unsigned short)port);
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

	//set the address of listenfd to serveraddr
	if (bind(listenfd, (SA *)&serveraddr, sizeof(serveraddr)) == -1)
		return (-1);

	// set the backlog to 8 and get the socket ready for requests
	if (listen(listenfd, 8) == -1)
		return (-1);

	return (listenfd);
}

/*
 * Requires:
 *   hostname is a valid hostname and port is a valid port number
 *
 * Effects:
 *   Opens a connection to the server and returns a file descriptor.  
 *   Returns -1 and sets errno on a Unix error and returns -2 on a 
 *   getaddrinfo error.
 */
static int
open_client(char *hostname, char *port, int connfd, char *request, 
			struct sockaddr_in clientaddr, char *request_url)
{
	struct addrinfo *ai, hints, *listp;
	int fd;
	int error;
	int n;
	rio_t rio;
	char buf[MAXLINE];

	//set the hints that should be passed to getaddrinfo()
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;  
	hints.ai_flags = AI_NUMERICSERV; 
	hints.ai_flags |= AI_ADDRCONFIG; 

	
	// try to get the servers address and retur -2 on failure
	error = getaddrinfo(hostname, port, &hints, &listp);
	if (error != 0) {
		fprintf(stderr, "getaddrinfo failed (%s:%s): %s\n",
				hostname, port, gai_strerror(error));
		return (-2);
	}


	// find a address that can be connected to
	for (ai = listp; ai != NULL; ai = ai->ai_next)
	{
		
		if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
			continue;
		}
			

		// break the loop if connect suceeds
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) != -1) {
			break;
		}
			
		if (close(fd) == -1)
			unix_error("close");
	}

	// print the request
	fprintf(stdout, "%s\n", request);


	rio_readinitb(&rio, fd);

	// write the request to the end server 
	rio_writen(fd, request, strlen(request));
	rio_writen(fd, "\r\n", 2);


	
	// write the repsonse back to the client and keep track of its size   
	int size_server = 0;
	while ((n = rio_readlineb(&rio, buf, MAXLINE)) != 0)
	{
		rio_writen(connfd, buf, n);
		size_server += strlen(buf);
	}

	// write to log file

	// create log entry
	char *log_entry = create_log_entry(&clientaddr, request_url, size_server);

	sprintf(log_entry, "%s\n", log_entry);

	fprintf(logfd, log_entry);
	fflush(logfd);

	// free memory
	freeaddrinfo(listp);
	free(log_entry);
	Close(fd);
	Close(logfd);


	if (ai == NULL) {
		return (-1);
	} else {
		return (fd);
	}
}




/*
 * Requires:
 *   The parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Given a URI from an HTTP proxy GET request (i.e., a URL), extract the
 *   host name, port, and path name.  Create strings containing the host name,
 *   port, and path name, and return them through the parameters "hostnamep",
 *   "portp", "pathnamep", respectively.  (The caller must free the memory
 *   storing these strings.)  Return -1 if there are any problems and 0
 *   otherwise.
 */
static int
parse_uri(const char *uri, char **hostnamep, char **portp, char **pathnamep)
{
	const char *pathname_begin, *port_begin, *port_end;

	if (strncasecmp(uri, "http://", 7) != 0)
		return (-1);

	/* Extract the host name. */
	const char *host_begin = uri + 7;
	const char *host_end = strpbrk(host_begin, ":/ \r\n");
	if (host_end == NULL)
		host_end = host_begin + strlen(host_begin);
	int len = host_end - host_begin;
	char *hostname = Malloc(len + 1);
	strncpy(hostname, host_begin, len);
	hostname[len] = '\0';
	*hostnamep = hostname;

	/* Look for a port number.  If none is found, use port 80. */
	if (*host_end == ':') {
		port_begin = host_end + 1;
		port_end = strpbrk(port_begin, "/ \r\n");
		if (port_end == NULL)
			port_end = port_begin + strlen(port_begin);
		len = port_end - port_begin;
	} else {
		port_begin = "80";
		port_end = host_end;
		len = 2;
	}
	char *port = Malloc(len + 1);
	strncpy(port, port_begin, len);
	port[len] = '\0';
	*portp = port;

	/* Extract the path. */
	if (*port_end == '/') {
		pathname_begin = port_end;
		const char *pathname_end = strpbrk(pathname_begin, " \r\n");
		if (pathname_end == NULL)
			pathname_end = pathname_begin + strlen(pathname_begin);
		len = pathname_end - pathname_begin;
	} else {
		pathname_begin = "/";
		len = 1;
	}
	char *pathname = Malloc(len + 1);
	strncpy(pathname, pathname_begin, len);
	pathname[len] = '\0';
	*pathnamep = pathname;

	return (0);
}

/*
 * Requires:
 *   The parameter "sockaddr" must point to a valid sockaddr_in structure.  The
 *   parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Returns a string containing a properly formatted log entry.  This log
 *   entry is based upon the socket address of the requesting client
 *   ("sockaddr"), the URI from the request ("uri"), and the size in bytes of
 *   the response from the server ("size").
 */
static char *
create_log_entry(const struct sockaddr_in *sockaddr, const char *uri, int size)
{
	struct tm result;

	/*
	 * Create a large enough array of characters to store a log entry.
	 * Although the length of the URI can exceed MAXLINE, the combined
	 * lengths of the other fields and separators cannot.
	 */
	const size_t log_maxlen = MAXLINE + strlen(uri);
	char *const log_str = Malloc(log_maxlen + 1);

	/* Get a formatted time string. */
	time_t now = time(NULL);
	int log_strlen = strftime(log_str, MAXLINE,
	    "%a %d %b %Y %H:%M:%S %Z: ", localtime_r(&now, &result));

	/*
	 * Convert the IP address in network byte order to dotted decimal
	 * form.
	 */
	Inet_ntop(AF_INET, &sockaddr->sin_addr, &log_str[log_strlen],
	    INET_ADDRSTRLEN);
	log_strlen += strlen(&log_str[log_strlen]);

	/*
	 * Assert that the time and IP address fields occupy less than half of
	 * the space that is reserved for the non-URI fields.
	 */
	assert(log_strlen < MAXLINE / 2);

	/*
	 * Add the URI and response size onto the end of the log entry.
	 */
	snprintf(&log_str[log_strlen], log_maxlen - log_strlen, " %s %d", uri,
	    size);

	return (log_str);
}

/*
 * Requires:
 *   The parameter "fd" must be an open socket that is connected to the client.
 *   The parameters "cause", "short_msg", and "long_msg" must point to properly
 *   NUL-terminated strings that describe the reason why the HTTP transaction
 *   failed.  The string "short_msg" may not exceed 32 characters in length,
 *   and the string "long_msg" may not exceed 80 characters in length.
 *
 * Effects:
 *   Constructs an HTML page describing the reason why the HTTP transaction
 *   failed, and writes an HTTP/1.0 response containing that page as the
 *   content.  The cause appearing in the HTML page is truncated if the
 *   string "cause" exceeds 2048 characters in length.
 */
static void
client_error(int fd, const char *cause, int err_num, const char *short_msg,
    const char *long_msg)
{
	char body[MAXBUF], headers[MAXBUF], truncated_cause[2049];

	assert(strlen(short_msg) <= 32);
	assert(strlen(long_msg) <= 80);
	/* Ensure that "body" is much larger than "truncated_cause". */
	assert(sizeof(truncated_cause) < MAXBUF / 2);

	/*
	 * Create a truncated "cause" string so that the response body will not
	 * exceed MAXBUF.
	 */
	strncpy(truncated_cause, cause, sizeof(truncated_cause) - 1);
	truncated_cause[sizeof(truncated_cause) - 1] = '\0';

	/* Build the HTTP response body. */
	snprintf(body, MAXBUF,
	    "<html><title>Proxy Error</title><body bgcolor=\"ffffff\">\r\n"
	    "%d: %s\r\n"
	    "<p>%s: %s\r\n"
	    "<hr><em>The COMP 321 Web proxy</em>\r\n",
	    err_num, short_msg, long_msg, truncated_cause);

	/* Build the HTTP response headers. */
	snprintf(headers, MAXBUF,
	    "HTTP/1.0 %d %s\r\n"
	    "Content-type: text/html\r\n"
	    "Content-length: %d\r\n"
	    "\r\n",
	    err_num, short_msg, (int)strlen(body));

	/* Write the HTTP response. */
	if (rio_writen(fd, headers, strlen(headers)) != -1)
		rio_writen(fd, body, strlen(body));
}

// Prevent "unused function" and "unused variable" warnings.
static const void *dummy_ref[] = { client_error, create_log_entry, dummy_ref,
	parse_uri };
