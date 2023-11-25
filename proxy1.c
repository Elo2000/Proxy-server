#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

struct arguments {
    const char * url;
};

//Send an error reply over a socket
static void err_reply(const int sd, const int code, const char * hdr, const char * msg){
    const size_t cont_len = snprintf(NULL, 0,
                                     "<HTML><HEAD><TITLE>%d %s</TITLE></HEAD><BODY><H4>%d %s</H4>%s.</BODY></HTML>",
                                     code, hdr, code, hdr, msg);

    dprintf(sd, "HTTP/1.0 %d %s\r\n", code, hdr);
    dprintf(sd, "Content-Type: text/html\r\n");
    dprintf(sd, "Content-Length: %lu\r\n", cont_len);
    dprintf(sd, "Connection: closed\r\n\r\n");
    dprintf(sd, "<HTML><HEAD><TITLE>%d %s</TITLE></HEAD><BODY><H4>%d %s</H4>%s.</BODY></HTML>",
            code, hdr, code, hdr, msg);
}

//Read HTTP reqeust
static int read_headers(const int sd, char * buf, const size_t buf_size){
    size_t i = 0;
    while(recv(sd, &buf[i], 1, 0) > 0){
        if(++i >= buf_size){  //if buffer is full
            break;
        }
        //if we have the request end
        if((i >=4) && (strncmp(&buf[i - 4], "\r\n\r\n", 4) == 0)){
            break;
        }
    }
    buf[i-1] = '\0';
    return i;
}

//Extract host
static int parse_url(const char * uri, char hname[NI_MAXHOST], char pname[PATH_MAX]){
    char * end;
    memset(hname, 0, NI_MAXHOST);
    memset(pname, 0, PATH_MAX);
    if(strncmp(uri, "http://", 7) == 0){
        uri += 7;
    }
    end = strchr(uri, '/'); //find end of hostname
    if(end){
        strncpy(pname, end, PATH_MAX);
        end[0] = '\0';
        end = strchr(uri, ':');
        if(end){
            end[0] = '\0';
        }
    }
    strncpy(hname, uri, NI_MAXHOST);
    return 0;
}

//Check if we can get IP for that hostname
static int is_resolveable(const char * hname){
    int s;
    struct addrinfo hints, *result;
    /* Obtain address(es) matching host/port. */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        /* Allow IPv4 */
    hints.ai_socktype = SOCK_STREAM;  /* TCP */
    hints.ai_flags = 0;
    hints.ai_protocol = 0;          /* Any protocol */
    s = getaddrinfo(hname, "80", &hints, &result);
    if (s != 0) {
        perror("gethostbyname");
        freeaddrinfo(result);           /* No longer needed */
        return -1;
    }
    freeaddrinfo(result);
    return 0;
}

static int creat_cache_file(const char * hname, const char * pname){
    char fpath[PATH_MAX];
    if(strcmp(pname, "/") == 0){  //don't cache indexp pages
        //create the path
        snprintf(fpath, PATH_MAX, "%s/index.html", hname);
    }else{
        snprintf(fpath, PATH_MAX, "%s%s", hname, pname);
    }
    //create the path to file
    char * delim = strchr(fpath, '/');
    while(delim){ //if we have a directory
        //replace / with null, to end string temporarily
        delim[0] = '\0';
        if(mkdir(fpath, 0770) == -1){
            if(errno != EEXIST){
                perror("mkdir");
                break;
            }
        }
        //restore delimiter
        delim[0] = '/';
        //move to next delimiter
        delim = strchr(delim + 1, '/');
    }
    //create the file
    int fd = open(fpath, O_CREAT | O_RDWR, 0764);
    if(fd == -1){
        perror("open");
    }
    return fd;
}
//Open a file from cache, based on hostname and URL path
static int open_cache_file(const char * hname, const char * pname){
    char fpath[PATH_MAX];
    if(strcmp(pname, "/") == 0){  //don't cache indexp pages
        //create the path
        snprintf(fpath, PATH_MAX, "%s/index.html", hname);
    }else{
        snprintf(fpath, PATH_MAX, "%s%s", hname, pname);
    }

    return open(fpath, O_RDONLY);
}

static int connect_to(const char * hname){
    struct addrinfo hints, *result, *rp;
    int s, sd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        /* Allow IPv4 */
    hints.ai_socktype = SOCK_STREAM;

    s = getaddrinfo(hname, "80", &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return -1;
    }
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sd == -1){
            continue;
        }
        if (connect(sd, rp->ai_addr, rp->ai_addrlen) != -1)
            break;                  /* Success */

        close(sd);
    }
    freeaddrinfo(result);
    return sd;
}

static int writen(const int fd, const char * buf, const int len){
    int i=0;
    while(i < len){
        int n = write(fd, &buf[i], len - i);
        if(n <= 0){
            perror("write");
            return -1;
        }
        i += n;
    }
    return len;
}

static int send_cache_file(const int sd, const int fd){
    char buf[100];
    int rv = 0, buf_len;
    while((buf_len = read(fd, buf, sizeof(buf))) > 0){
        if(writen(sd, buf, buf_len) != buf_len){
            rv = -1;
            break;
        }
        rv += buf_len;
    }
    close(fd);
    return rv;
}

static int save_cache_file(const int sd, const char * hname, const char * pname){
    char buf[100];
    int err=0, buf_len;
    int fd = creat_cache_file(hname, pname);
    if(fd == -1){
        return -1;
    }
    //send file and cache at the same time
    while((buf_len = read(sd, buf, sizeof(buf))) > 0){

        if(writen(fd, buf, buf_len) != buf_len){
            err = -1;
            break;
        }
    }
    if(err == -1){
        close(fd);  //close cache file
        fd = -1;
    }
    return fd;
}

static int cache_file(const int sd, const char *hname, const char * pname, char * hdr, const int hdr_size){
    int len = 0;
    const int serv_sd = connect_to(hname);
    if(serv_sd == -1){
        err_reply(fileno(stdout), 404, "Not Found", "File not found");
        return -1;
    }
    len = dprintf(serv_sd, "GET %s HTTP/1.0\r\n", pname);
    len += dprintf(serv_sd, "Host: %s\r\n\r\n", hname);
    //receive server reply headers
    int hdr_len = read_headers(serv_sd, hdr, hdr_size);
    if(hdr_len <= 0){
        err_reply(fileno(stdout), 500, "Some server side error", "Some server side error");
        return -1;
    }
    //re-send server reply to client
    if(writen(sd, hdr, hdr_len) != hdr_len){
        err_reply(fileno(stdout), 500, "Some server side error", "Some server side error");
        return -1;
    }
    const int fd = save_cache_file(serv_sd, hname, pname);
    if(fd > 0){
        //return to beginning of file
        lseek(fd, 0L, SEEK_SET);
    }
    shutdown(serv_sd, SHUT_RDWR);
    close(serv_sd);

    return fd;
}

static int send_hdr_file(const int sd, const int fd){
    struct stat st;
    int len = 0;
    if(fstat(fd, &st) == -1){
        perror("fstat");
        err_reply(fileno(stdout), 500, "Some server side error", "Some server side error");
        return -1;
    }
    //send header to client
    len =  dprintf(sd, "HTTP/1.0 200 OK\r\n");
    len += dprintf(sd, "Content-Length: %lu\r\n\r\n", st.st_size);
    return len;
}

int proxy_handler(const char * url){
    char hname[NI_MAXHOST], pname[PATH_MAX];
    const int sd = fileno(stdout);  //screen descriptor
    const size_t buf_size = 4*1024;
    char * buf = calloc(buf_size, sizeof(char));
    if(buf == NULL){
        perror("malloc");
        err_reply(fileno(stdout), 500, "Some server side error", "Some server side error");
        return -1;
    }
    //get hostname and path from URL
    if((parse_url(url, hname, pname) < 0) ){
        free(buf);
        return -1;
    }
    if(is_resolveable(hname) < 0){
        err_reply(fileno(stdout), 404, "Not Found", "File not found");
        free(buf);
        return -1;
    }
    size_t response_bytes = 0;
    //show on screen
    dprintf(sd, "HTTP Request:\n");
    int len = dprintf(sd, "GET %s HTTP/1.0\n", pname);
    len += dprintf(sd, "Host: %s\n\n", hname);
    dprintf(sd, "LEN=%d\n", len);
    int fd = open_cache_file(hname, pname);
    if(fd != -1){
        response_bytes = send_hdr_file(sd, fd);
        printf("File is given from local filesystem\n");
        printf("HTTP/1.0 200 OK \r\n Content-Length:");
    }else{
        fd = cache_file(sd, hname, pname, buf, buf_size);
        if(fd > 0){
            printf("File is given from origin filesystem\n");
        }
        //print server headers to screen
        printf("%s", buf);
        response_bytes = strlen(buf);
    }
    if(fd > 0){
        response_bytes += send_cache_file(sd, fd);
        printf("Total response bytes: %lu\n", response_bytes);
    }
    free(buf);
    return 0;
}

static int check_arguments(struct arguments * arg, const int argc, char * argv[]){
    if(argc != 2){
        //fprintf(stderr, "Usage: proxy1 <URL>\n");
        printf("Usage: proxy1 <URL>\n");
        return -1;
    }
    arg->url = argv[1];
    return 0;
}

static void sig_handler(int sig){
    return;
}

int main(const int argc, char * argv[]){
    struct arguments arg;
    struct sigaction sa;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = sig_handler;
    if( (sigaction(SIGTERM, &sa, NULL) == -1) ||
        (sigaction(SIGINT, &sa, NULL) == -1) ){
        perror("sigaction");
    }
    if(check_arguments(&arg, argc, argv) < 0){
        fflush(stderr);
        return EXIT_FAILURE;
    }
    proxy_handler(arg.url);
    return EXIT_SUCCESS;
}