#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h> 
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h> 
#include <sys/epoll.h>
#include <regex.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <pthread.h>

#define LOG_LINE() printf("%s %d\n", __func__, __LINE__);

void failed(const char* error_info)
{
    fprintf(stderr, "%s\n", error_info ? error_info : "failed");
    exit(1);
}

void assure(bool condition, const char* error_info)
{
    if (!condition)
    {
        failed(error_info ? error_info : "error condition");
        exit(1);
    }
}

int regex_match(const char *string, const char *pattern, regmatch_t* result, int result_max_size)
{
    int    status;
    regex_t    regex;
    int cflag=(result==NULL?REG_NOSUB|REG_EXTENDED:REG_EXTENDED);
    if (regcomp(&regex, pattern, cflag) != 0)
    {
        fprintf(stderr, "cannot compile regex pattern %s\n", pattern);
        return -1;
    }
    status = regexec(&regex, string, result_max_size, result, 0);
    regfree(&regex);
    if (status != 0)
    {
        fprintf(stderr, "cannot match %s for %s\n", string, pattern);
        return -1;
    }
    return 0;
}

char* regex_extrct(const char *string, const char *pattern)
{
    regmatch_t result[2];
    if (regex_match(string, pattern, result, 2)<0)
    {
        return NULL;
    }
    assure(result[1].rm_so!=-1 && result[1].rm_eo!=-1, "error in regex_extrct");
    int length=result[1].rm_eo-result[1].rm_so;
    char* str=malloc(length+1);
    memcpy(str, string+result[1].rm_so, length);
    str[length] ='\0';
    return str;
}

char* file_read_all(const char* path)
{
    FILE* file=fopen(path, "rb");
    if (file==NULL)
    {
        fprintf(stderr, "cannot open file %s\n", path);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(size+1);

    size_t result = fread(buffer, 1, size, file);
    if (result != size)
    {
        fputs("error in fread", stderr);
        free(buffer);
        buffer=NULL;
    }
    buffer[size]='\0';
    return buffer;
}

bool check_is_path_exist(const char* path)
{
    struct stat path_stat;
    return !stat(path, &path_stat);
}

bool check_is_regular_file(const char* path)
{
    struct stat path_stat;
    stat(path, &path_stat);
    return S_ISREG(path_stat.st_mode);
}

bool check_is_directory(const char* path)
{
    struct stat path_stat;
    stat(path, &path_stat);
    return S_ISDIR(path_stat.st_mode);
}

char* header_append(char* header, const char* content)
{
    int header_length=strlen(header);
    int content_length=strlen(content);
    assure(strcmp(header+header_length-4, "\r\n\r\n")==0, "error header end tag");
    assure(strcmp(content+content_length-2, "\r\n")==0, "error content end tag");
    char* result=(char*)malloc(header_length+content_length+1);
    memcpy(result, header, header_length-2);
    memcpy(result+header_length-2, content, content_length);
    memcpy(result+header_length+content_length-2, "\r\n", 5);
    return result;
}

int recv_request(int socket_fd, char **header, int* header_length, char** body, int* body_length)
{
    int buffer_length=128;
    char* buffer=(char*)malloc(buffer_length);
    int ack = 0;
    while (1)
    {
        int n = recv(socket_fd, buffer+ack, buffer_length-ack, 0);
        if (n < 0)
        {
            perror("erorr in read");
            exit(1);
        }
        ack += n;
        if (ack >= buffer_length)
        {// expansion
            buffer_length*=2;
            buffer=(char*)realloc(buffer, buffer_length);
        }
        // memcpy(dest + ack, buffer, n); 
        regmatch_t result;
        if (regex_match(buffer, "\r\n\r\n", &result, 1)!=-1)
        {// found header end tag \r\n\r\n 
            *header_length=result.rm_eo;
            *header=(char*)malloc(*header_length+1);
            memcpy(*header, buffer, *header_length);
            (*header)[*header_length]='\0';

            //assume body is received, to do
            *body_length=ack-*header_length;
            *body=(char*)malloc(*body_length+1);
            memcpy(*body, buffer+*header_length, *body_length);
            (*body)[*body_length]='\0';
            break;
        }
    }
    free(buffer);
    return ack;
}

const char error_400[]=
"HTTP/1.1 400 Invalid\r\n"
"Content-Type: text/html\r\n\r\n"
"<html><body>Bad Request<br>"
"</body></html>";

const char header_200[]=
"HTTP/1.1 200 OK\r\n\r\n";

int send_reponse(int socket_fd, const char* string1, const char* string2)
{
    if (string1!=NULL)
    {
        if (send(socket_fd, string1, strlen(string1), 0)<0)
        {
            fprintf(stderr, "error in send response \n");
            return -1;
        }
    }
    if (string2!=NULL)
    {
        if (send(socket_fd, string2, strlen(string2), 0)<0)
        {
            fprintf(stderr, "error in send response \n");
            return -1;
        }
    }
    return 0;
}

bool regex_check_equal(const char* string, const char* pattern, const char* expectation)
{
    regmatch_t result;
    if (regex_match(string, pattern, &result, 1)<0)
    {
        return false;
    }
    if (strncmp(string+result.rm_so, expectation, strlen(expectation))!=0)
    {
        return false;
    }
    return true;
}

bool check_http_version(const char* header)
{
    return regex_check_equal(header, "HTTP/1\\.1", "HTTP/1.1");
}

bool check_method(const char* header, const char* expectation)
{
    return regex_check_equal(header, "^\\w+", expectation);
}

char* html_construct(const char* header, const char* body)
{
    const char* template=""
        "<!DOCTYPE html>"
        "<html>"
        "<head>%s</head>"
        "<body>%s</body>"
        "</html>";
    int length=strlen(header)+strlen(body)+strlen(template);
    char* html=(char*)malloc(length);
    sprintf(html, template, header, body);
    return html;
}


char* html_header_construct(const char* title)
{
    const char* template=""
        "<title>%s</title>"
        "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />";
    int length=strlen(title)+strlen(template);
    char* html_header=(char*)malloc(length);
    sprintf(html_header, template, title);
    return html_header;
}

char* html_body_append(char* body, const char* content)
{
    int body_length=strlen(body);
    int content_length=strlen(content);
    char* result=(char*)malloc(body_length+content_length+1);
    result[0]='\0';
    strcat(result, body);
    strcat(result, content);
    return result;
}

char* convert_to_root_path(const char* path)
{
    const char* root_prefix_path="root";
    char* new_path=(char*)malloc(strlen(root_prefix_path)+strlen(path)+1);
    memcpy(new_path, root_prefix_path, strlen(root_prefix_path)+1);
    strcat(new_path, path);
    return new_path;
}

void *accept_request(void *arg)
{
    int client_socket_fd = *(int *)arg;
    char *header=NULL;
    int header_length=0;
    char *body=NULL;
    int body_length=0;
    printf("start recv -- ");
    if (recv_request(client_socket_fd, &header, &header_length, &body, &body_length) < 0)
    {
        fprintf(stderr, "error in recv request");
        exit(1);
    }
    printf("end recv length: %d + %d = %d\n", header_length, body_length, header_length+body_length);
    printf("header:\n %*s\n", header_length, header);
    printf("body:\n %*s\n", body_length, body);
    assure(check_http_version(header), "error http version");
    if (check_method(header, "GET"))
    {
        char* relative_path=regex_extrct(header, "^\\w+ (/\\S*)");
        assure(relative_path!=NULL, "error GET URL");
        char* root_path=convert_to_root_path(relative_path);
        printf("relative_path %s\n", relative_path);
        printf("root_path %s\n", root_path);
        if (!check_is_path_exist(root_path))
        {
            printf("error file path %s", root_path);
            send_reponse(client_socket_fd, error_400, NULL);
        }
        else
        {
            if (check_is_regular_file(root_path))
            {
                char* file=file_read_all(root_path);
                if (file==NULL)
                {
                    send_reponse(client_socket_fd, error_400, NULL);
                }
                else
                {
                    send_reponse(client_socket_fd, header_200, file);
                }
                free(file);
            }
            else if (check_is_directory(root_path))
            {
                DIR* dir=opendir(root_path);
                assure(dir!=NULL, "error dir path");
                struct dirent* ent=NULL;
                char* html_body=(char*)malloc(1);
                html_body[0]='\0';

                char buffer[1024];
                sprintf(buffer, "<a href=\"%s\" style=\"color:red;\">%s</a><br/>", relative_path, ".");
                char* new_body=html_body_append(html_body, buffer);
                free(html_body);
                html_body=new_body;

                char* last=strrchr(relative_path, '/');
                if (last==relative_path)
                {
                    sprintf(buffer, "<a href=\"%s\" style=\"color:red;\">%s</a><br/>", "/", "..");
                    char* new_body=html_body_append(html_body, buffer);
                    free(html_body);
                    html_body=new_body;
                }
                else
                {
                    char temp=*last;
                    *last='\0';
                    sprintf(buffer, "<a href=\"%s\" style=\"color:red;\">%s</a><br/>", relative_path, "..");
                    *last=temp;
                    char* new_body=html_body_append(html_body, buffer);
                    free(html_body);
                    html_body=new_body;
                }
                while ((ent = readdir(dir)) != NULL)
                {
                    if (strcmp(ent->d_name, ".")!=0&&strcmp(ent->d_name, "..")!=0)
                    {
                        char path_buffer[1024]="";
                        strcat(path_buffer, relative_path);
                        if (strcmp(relative_path, "/")!=0)
                        {
                            strcat(path_buffer, "/");
                        }
                        strcat(path_buffer, ent->d_name);
                        if (ent->d_type==DT_DIR)
                        {
                            sprintf(buffer, "<a href=\"%s\" style=\"color:red;\">%s</a><br/>", path_buffer, ent->d_name);
                        }
                        else
                        {
                            sprintf(buffer, "<a href=\"%s\">%s</a><br/>", path_buffer, ent->d_name);
                        }

                        char* new_body=html_body_append(html_body, buffer);
                        free(html_body);
                        html_body=new_body;
                        printf("%s\n", ent->d_name);
                    }
                }
                char* html_header=html_header_construct(root_path);
                char* html=html_construct(html_header, html_body);
                send_reponse(client_socket_fd, header_200, html);
                puts(html);
                free(html_body);
                free(html_header);
                free(html);
            }
            else
            {
                send_reponse(client_socket_fd, error_400, NULL);
            }
        }
        free(root_path);
        free(relative_path);
    }
    else
    {
        send_reponse(client_socket_fd, error_400, NULL);
    }
    free(header);
    free(body);
    close(client_socket_fd);
    return NULL;
}

int main(int argc, char *argv[])
{
    int server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_fd == -1)
    {
        perror("error socket fd");
        exit(1);
    }
    int listen_port = 8000;
    struct sockaddr_in server_socket_addr;
    bzero(&server_socket_addr, sizeof(server_socket_addr));
    server_socket_addr.sin_family = AF_INET;
    server_socket_addr.sin_port = htons(listen_port);
    if (bind(server_socket_fd, (struct sockaddr *)&server_socket_addr, sizeof(server_socket_addr)) < 0)
    {
        perror("error in bind socket");
        exit(-1);
    }
    if (listen(server_socket_fd, 1) < 0)
    {
        perror("error in listen");
        exit(1);
    }
    printf("listen on %d:\n", listen_port);

    struct sockaddr_in client_socket_addr;
    socklen_t client_socket_length = sizeof(client_socket_addr);

    int flag = fcntl(server_socket_fd, F_GETFL);
    fcntl(server_socket_fd, F_SETFL, flag | O_NONBLOCK);
    int client_socket_fd;
    while (1)
    {
        client_socket_fd = accept(server_socket_fd, (struct sockaddr *)&client_socket_addr, &client_socket_length);
        if (client_socket_fd < 0)
        {
            if (errno == EWOULDBLOCK)
            {
                // puts("client is not ready");
            }
            else
            {
                perror("error client socket fd");
                exit(1);
            }
        }
        else
        {
            printf("accept socket %s %u\n", inet_ntoa(client_socket_addr.sin_addr), ntohs(client_socket_addr.sin_port));

            pthread_t client_thread;
            if (pthread_create(&client_thread, NULL, accept_request, &client_socket_fd))
            {
                perror("error in create thread");
                exit(1);
            }
        }
        sleep(2);
    }
    close(server_socket_fd);
    return 0;
}