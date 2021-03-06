#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h> // fcntl
#include <unistd.h> // close
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <limits.h> // [LONG|INT][MIN|MAX]
#include <errno.h>  // errno
#include <unistd.h>
#include "dbapi.h"

//"list "
#define LIST_LEN 5

typedef enum {
    S2ISUCCESS = 0,
    S2IOVERFLOW,
    S2IUNDERFLOW,
    S2IINCONVERTIBLE
} STR2INT_ERROR;

static int ignore_result(dbclient *client);

static int write_util(dbclient* client, int len, unsigned int delay) {
    int n, writed_len = 0;
    unsigned int now, timeout;
    struct timeval tv, tv2;

    gettimeofday(&tv2, NULL);
    timeout = (tv2.tv_sec*1000) + (tv2.tv_usec/1000) + delay;
    while(writed_len < len) {
        n = write(client->remote_fd, client->buf + writed_len, len - writed_len);
        if(n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                gettimeofday(&tv, NULL);
                now = (tv.tv_sec*1000) + (tv.tv_usec/1000);
                if(now > timeout) {
                    break;
                }

                usleep(50);
            }
        }

        writed_len += n;
    }

    if(writed_len != len) {
        return -1;
    }

    return 0;
}

//TODO better for code reused
static int create_client_fd(char* sock_path) {
    int len, remote_fd;
    struct sockaddr_un remote;
    struct timeval tv;

    tv.tv_sec = 1;
    tv.tv_usec = 0;

    if(-1 == (remote_fd = socket(PF_UNIX, SOCK_STREAM, 0))) {
        //perror("socket");
        return -1;
    }
    setsockopt(remote_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    remote.sun_family = AF_UNIX;
    strcpy(remote.sun_path, sock_path);
    len = strlen(remote.sun_path) + sizeof(remote.sun_family);
    if(-1 == connect(remote_fd, (struct sockaddr*)&remote, len)) {
        //perror("connect");
        close(remote_fd);
        return -1;
    }

    return remote_fd;
}

static void check_buf(dbclient* client, int len) {
    int clen = _max(len, BUF_MAX);

    if((NULL != client->buf) && (client->buf_max < clen)) {
        free(client->buf);
        client->buf = NULL;
    }

    if(NULL == client->buf) {
        client->buf = (char*)malloc(clen+1);
        client->buf_max = clen;
    }
}

static int setnonblock(int fd) {
    int flags;

    flags = fcntl(fd, F_GETFL);
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

static STR2INT_ERROR str2int(int *i, char *s, int base) {
  char *end;
  long  l;
  errno = 0;
  l = strtol(s, &end, base);

  if ((errno == ERANGE && l == LONG_MAX) || l > INT_MAX) {
    return S2IOVERFLOW;
  }
  if ((errno == ERANGE && l == LONG_MIN) || l < INT_MIN) {
    return S2IUNDERFLOW;
  }
  if (*s == '\0' || *end != '\0') {
    return S2IINCONVERTIBLE;
  }
  *i = l;
  return S2ISUCCESS;
}

int dbclient_start(dbclient* client) {
    char* socket_path = "/tmp/.skipd_server_sock";

    memset(client, 0, sizeof(dbclient));

    client->remote_fd = create_client_fd(socket_path);
    if(-1 == client->remote_fd) {
        return -1;
    }

    setnonblock(client->remote_fd);
    return 0;
}

int dbclient_bulk(dbclient* client, const char* command, const char* key, int nk, const char* value, int nv) {
    int n1,n2,nc;

    if(-1 == client->remote_fd) {
        return -1;
    }

    nc = strlen(command);
    if(nk == 0) {
        nk = strlen(key);
    }
    if(nv == 0) {
        nv = strlen(value);
    }

    if(!strcmp(command, "set") && nv == 0) {
        n1 = strlen("remove") + nk + 2;// remove key\n
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = sprintf(client->buf, "%s%07d remove %.*s\n", MAGIC, n1, nk, key);
    } else {
        n1 = nc + nk + nv + 3;// replace key value\n
        check_buf(client, n1 + HEADER_PREFIX);
        n2 = sprintf(client->buf, "%s%07d %s ", MAGIC, n1, command);

        memcpy(client->buf + n2, key, nk);
        client->buf[n2+nk] = ' ';
        n2 += nk + 1;

        memcpy(client->buf + n2, value, nv);
        client->buf[n2+nv] = '\n';
        n2 += nv + 1;

        client->buf[n2] = '\0';
    }

    if(0 == write_util(client, n1 + HEADER_PREFIX, 200)) {
        ignore_result(client);
        return 0;
    }

    return -1;
}

static int read_util(dbclient* client, int len, unsigned int delay) {
    int clen, n;
    unsigned int now, timeout;
    struct timeval tv,  tv2;

    check_buf(client, len);
    gettimeofday(&tv2, NULL);
    timeout = (tv2.tv_sec * 1000) + (tv2.tv_usec / 1000) + delay;
    client->buf_pos = 0;

    for(;;) {
        clen = len - client->buf_pos;
        n = recv(client->remote_fd, client->buf + client->buf_pos, clen, 0);
        if(n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                gettimeofday(&tv, NULL);
                now = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
                if(now > timeout) {
                    break;
                }

                usleep(50);
                continue;
            }
            //timeout
            return -2;
        } else if(n == 0) {
            //socket closed
            return -1;
        } else {
            client->buf_pos += n;
            if(client->buf_pos == len) {
                //read ok
                return 0;
            }
        }
    }

    //unkown error
    return -3;
}

static int ignore_result(dbclient *client) {
    int n1, n2;
    char* magic = MAGIC;

    do {
        n1 = read_util(client, HEADER_PREFIX, 110);
        if(n1 < 0) {
            return n1;
        }

        if(0 != memcmp(client->buf, magic, MAGIC_LEN)) {
            //message error
            return -3;
        }

        client->buf[HEADER_PREFIX-1] = '\0';
        if(S2ISUCCESS != str2int(&n2, client->buf+MAGIC_LEN, 10)) {
            //message error
            return -4;
        }

        n1 = read_util(client, n2, 110);
        if(n1 < 0) {
            return n1;
        }
    } while(0);

    return 0;
}

static int parse_list_result(dbclient *client, char* prefix, void* o, fn_db_parse fn) {
    int n1, n2;
    char *p1, *p2, *magic = MAGIC;

    for(;;) {
        n1 = read_util(client, HEADER_PREFIX, 110);
        if(n1 < 0) {
            return n1;
        }

        if(0 != memcmp(client->buf, magic, MAGIC_LEN)) {
            //message error
            return -3;
        }

        client->buf[HEADER_PREFIX-1] = '\0';
        if(S2ISUCCESS != str2int(&n2, client->buf+MAGIC_LEN, 10)) {
            //message error
            return -4;
        }

        n1 = read_util(client, n2, 510);
        if(n1 < 0) {
            return n1;
        }

        client->buf[n2] = '\0';

        if(NULL != strstr(client->buf, "__end__")) {
            break;
        }

        p1 = client->buf + LIST_LEN;
        p2 = strstr(p1, " ");
        *p2 = '\0';
        p2++;
        if(client->buf[n2-1] == '\n') {
            client->buf[n2-1] = '\0';
        }
        if(0 != (*fn)(client, o, prefix, p1, p2)) {
            break;
        }
    }

    return 0;
}

int dbclient_list(dbclient* client, char* prefix, void* o, fn_db_parse fn) {
    int n1, n2;

    if(-1 == client->remote_fd) {
        return -1;
    }

    n1 = strlen("list") + strlen(prefix) + 2;//list prefix\n
    check_buf(client, n1 + HEADER_PREFIX);
    n2 = sprintf(client->buf, "%s%07d list %s\n", MAGIC, n1, prefix);
    if(0 != write_util(client, n1 + HEADER_PREFIX, 100)) {
        return -1;
    }

    return parse_list_result(client, prefix, o, fn);
}

int dbclient_end(dbclient* client) {
    if(NULL != client->buf) {
        free(client->buf);
    }
    if((-1 != client->remote_fd) && (0 != client->remote_fd)) {
        close(client->remote_fd);
    }

    return 0;
}

static int myprint(dbclient* client, void* o, char* prefix, char* key, char* value) {
    printf("%s=%s\n", key, value);
    return 0;
}

static int print_json(dbclient* client, void* o, char* prefix, char* key, char* value) {
    printf("o['%s']='%s';\n", key, value);
    return 0;
}

int dbclient_json(dbclient* client, char* prefix) {
    printf("var %s=(function() ({\nvar o={};\n", prefix);
    dbclient_list(client, prefix, NULL, print_json);
    printf("return o;\n})();\n");
    return 0;
}

static void test_normal_1(void) {
    dbclient client;
    dbclient_start(&client);
    dbclient_bulk(&client, "set", "hello1", 0, "value-new-value", 0);
    dbclient_bulk(&client, "set", "hello11", 0, "value-new-value", 0);
    dbclient_bulk(&client, "set", "hello111", 0, "value-new-value", 0);
    dbclient_bulk(&client, "set", "hello1112", 0, "value-new-value", 0);
    dbclient_bulk(&client, "set", "hello13", 0, "value-new-value", 0);
    dbclient_bulk(&client, "set", "hello15", 0, "value-new-value", 0);
    dbclient_end(&client);
}

static void test_list_2(void) {
    dbclient client;
    dbclient_start(&client);
    //dbclient_list(&client, "hello1", NULL, myprint);
    dbclient_json(&client, "hello1");
    dbclient_end(&client);
}

static void test_loop_3(void) {
    int i;
    char buf1[512], buf2[512];

    dbclient client;
    dbclient_start(&client);

    for(i = 0; i < 5000; i++) {
        sprintf(buf1, "hello%d", i);
        sprintf(buf2, "new-value%d", i);
        dbclient_bulk(&client, "set", buf1, 0, buf2, 0);
        dbclient_bulk(&client, "set", buf1, 0, "", 0); //mark as remove
    }
    dbclient_end(&client);
}

static void test_multi_4(void) {
    dbclient client1;
    dbclient client2;
    dbclient_start(&client1);
    dbclient_start(&client2);

    dbclient_bulk(&client1, "set", "hello1", 0, "value-new-value", 0);
    dbclient_bulk(&client2, "set", "hello2", 0, "value-new-value", 0);

    dbclient_list(&client2, "hello", NULL, myprint);
    dbclient_bulk(&client1, "set", "hello3", 0, "value-new-value", 0);
    dbclient_list(&client1, "hello", NULL, myprint);

    dbclient_end(&client1);
    dbclient_end(&client2);
}

int main(int argc, char **argv)
{
    test_loop_3();
    test_multi_4();
    return 0;
}
