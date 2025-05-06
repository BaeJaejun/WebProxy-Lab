/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp, int *content_length);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize, int is_head);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg,
                 char *longmsg);
void sigchild_handler(int sig); //11.8

int main(int argc, char **argv)
{
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  /* Check command line args */
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  // 11.8 자식 죽었을 때 핸들러 등록 
  // 핸들러는 항상 시그널이 발생하기 전에 미리 등록
  Signal(SIGCHLD, sigchild_handler); 
  Signal(SIGPIPE, SIG_IGN);  // 11.13: Broken pipe 무시

  listenfd = Open_listenfd(argv[1]);
  while (1)
  {
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA *)&clientaddr,
                    &clientlen); // line:netp:tiny:accept
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    doit(connfd);  // line:netp:tiny:doit
    Close(connfd); // line:netp:tiny:close
  }
}

void doit(int fd)
{
  int is_static;                  // 정적 콘텐츠인지(dynamic이 아니면 static)
  struct stat sbuf;               // 파일 정보(stat) 저장할 구조체
  char buf[MAXLINE],              // 클라이언트 요청 라인 버퍼
        method[MAXLINE],          // 요청 메서드(GET, POST 등)
        uri[MAXLINE],             // 요청된 URI
        version[MAXLINE];         // HTTP 버전(예: HTTP/1.0)
  char filename[MAXLINE],         // 로컬 파일 경로로 변환된 이름
        cgiargs[MAXLINE];         // CGI 프로그램 인자 (query string)
  rio_t rio;                      // RIO(Read-IO) 버퍼 구조체

  int is_head = 0;                // 11.11을 위한 플래그

  Rio_readinitb(&rio, fd);
  Rio_readlineb(&rio, buf, MAXLINE);
  printf("Request headers:\n");
  printf("%s", buf);
  sscanf(buf, "%s %s %s",method, uri, version);

  /* GET 이외의 메서드는 미구현 에러 반환 */
  if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD") && strcasecmp(method, "POST")){ // 11.11 & 11.12
    clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
    return;
  }

  if (!strcasecmp(method, "HEAD")) //11.11
    is_head = 1;

  int is_post = !strcasecmp(method, "POST"); //11.12
  int content_length = 0; //11.12
  read_requesthdrs(&rio, &content_length);

  is_static = parse_uri(uri, filename, cgiargs);

  //11.12
  if (is_post){
    Rio_readnb(&rio, cgiargs, content_length);
    cgiargs[content_length] = '\0';  // null-terminate
  }

  /* 파일 존재 검사 */
  if (stat(filename, &sbuf) < 0){
    clienterror(fd, filename, "404", "Not found","Tiny couldn't find this file");
    return;
  }

  if (is_static){ 
    /* 정적 파일 접근 권한 및 일반 파일 여부 확인 */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't read the file");
      return;
    }
    serve_static(fd, filename, sbuf.st_size, is_head);
  }
  else{  
    /* 동적( CGI ) 파일 실행 권한 및 일반 파일 여부 확인 */
    if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
      clienterror(fd, filename, "403", "Forbidden", "Tiny couldn't run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs);
  }
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF];

    /* 1) 응답 본문(body) HTML 생성 */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=\"ffffff\">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* 2) HTTP 응답 라인 및 헤더 전송 */

    // 1) 상태 라인
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));

    // 2) 헤더 #1: Content-type
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));

    // 3) 헤더 #2 + 빈 줄: Content-length
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));

    // 4) 본문
    Rio_writen(fd, body, strlen(body));
}

void read_requesthdrs(rio_t *rp, int *content_length)
{
  char buf[MAXLINE];

  Rio_readlineb(rp, buf, MAXLINE);
  while(strcmp(buf, "\r\n")){
    if (strncasecmp(buf, "Content-Length:", 15) == 0)
      *content_length = atoi(buf + 15);
    printf("Header: %s", buf);  // 11.6.A 버리기 전에 echo! 
    Rio_readlineb(rp, buf, MAXLINE);
  }
  return;
}

int parse_uri(char *uri, char *filename, char *cgiargs)
{
  char *ptr;

  /* 1) "cgi-bin"이 URI에 없으면 → 정적 파일 요청 */
  if (!strstr(uri, "cgi-bin")) {

    /* CGI 인자는 사용하지 않으므로 빈 문자열 설정 */
    strcpy(cgiargs, "");

    /* filename = "." + uri */
    strcpy(filename, ".");
    strcat(filename, uri);

    /* URI가 "/"로 끝나면 기본 파일명으로 "home.html" 사용 */
    if (uri[strlen(uri)-1] == '/')
      strcat(filename, "home.html");
    return 1;
  }
  /* 2) "cgi-bin"이 포함된 URI → 동적 CGI 요청 */
  else{
    /* '?' 이후의 문자열을 cgiargs에 복사 (쿼리 스트링) */
    ptr = index(uri, '?');
    if (ptr) {
      strcpy(cgiargs, ptr+1);   /* '?' 다음 위치부터 인자 복사 */
      *ptr = '\0';              /* filename용 URI는 '?' 앞까지만 남김 */

    }
    else
      strcpy(cgiargs, "");
      strcpy(filename, ".");
      strcat(filename, uri);
    return 0;
  }
}

void serve_static(int fd, char *filename, int filesize, int is_head)
{
    int srcfd;
    char *srcp;
    char filetype[MAXLINE];
    char buf[MAXBUF];

    /* 1) 응답 헤더 생성 */
    get_filetype(filename, filetype);
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);
    Rio_writen(fd, buf, strlen(buf));

    if (is_head) return; //11.11

    /* 2) 파일을 메모리에 매핑하여 전송 */
    srcfd = Open(filename, O_RDONLY, 0);
    // 11.9
    // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    srcp = (char *)malloc(filesize);
    Rio_readn(srcfd,srcp,filesize);
    Close(srcfd);
    Rio_writen(fd, srcp, filesize);
    // Munmap(srcp, filesize);
    free(srcp);
}

/* get_filetype - 파일명 확장자에 따라 Content-Type 문자열 결정 */
void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".mpg")) // 11.7 MPG 비디오 파일 추가 
        strcpy(filetype, "video/mpeg");
    else
        strcpy(filetype, "text/plain");
}

void serve_dynamic(int fd, char *filename, char *cgiargs)
{
    char buf[MAXLINE], *emptylist[] = { NULL };

    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    if (Fork() == 0) { /* 자식 */
        setenv("QUERY_STRING", cgiargs, 1);
        Dup2(fd, STDOUT_FILENO);          
        Execve(filename, emptylist, environ);
        perror("Execve error");

    }
    // Wait(NULL); 
}

// 11.8 
void sigchild_handler(int sig)
{
  /*
  waitpid(-1, ...): 모든 자식 프로세스를 대상으로
  WNOHANG: 종료된 자식만 회수, 없으면 즉시 리턴 (비블로킹)
  > 0: 성공적으로 자식 하나를 회수하면 루프 계속
  */
  while (waitpid(-1, NULL, WNOHANG) > 0)
    ;
}