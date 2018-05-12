//*************************************************************************
// Authors: Oleksiy Kebkal                                                *
//*************************************************************************

// ISO C headers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>          /* waitpid() */
#include <sys/un.h>
#include <sys/wait.h>           /* waitpid() */

#include <stream.h>
#include <error.h>

struct private_data_t
{
    // Code of last error.
    int error;
    // Last error operation.
    const char* error_op;
    
    // socket handle.
    int fd;
    // socket parameters
    struct sockaddr_in saun;
};

static int stream_open_connect(sdm_stream_t *stream)
{
    struct private_data_t *pdata = stream->pdata;

    if ((pdata->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        pdata->error = errno;
        pdata->error_op = "socket creation error";
        return SDM_ERROR_STREAM;
    }

    /* TODO: add retry count here */
    if (connect(pdata->fd, (struct sockaddr *)&pdata->saun, sizeof(pdata->saun)) == -1) {
        close(pdata->fd);
        pdata->error = errno;
        pdata->error_op = "connecting socket";
        return SDM_ERROR_STREAM;
    }
    return SDM_ERROR_NONE;
}

static int stream_open_listen(sdm_stream_t *stream)
{
    struct private_data_t *pdata = stream->pdata;
    int wait_conn_fd = -1;
    int opt;

    if ((wait_conn_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        pdata->error = errno;
        pdata->error_op = "socket creation error";
        goto stream_listen_error;
    }

    opt = 1;
    if (setsockopt(wait_conn_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt)) < 0) {
        pdata->error = errno;
        pdata->error_op = "setting socket parameters";
        goto stream_listen_error;
    }
  
    if (bind(wait_conn_fd,(struct sockaddr *)&pdata->saun, sizeof(pdata->saun)) < 0) {
        pdata->error = errno;
        pdata->error_op = "binding socket";
        goto stream_listen_error;
    }

    if (listen(wait_conn_fd, 1) == -1) {
        pdata->error = errno;
        pdata->error_op = "listening socket";
        goto stream_listen_error;
    }
  
    pdata->fd = accept(wait_conn_fd, NULL, NULL);
    if (pdata->fd < 0) {
        pdata->error = errno;
        pdata->error_op = "accepting socket connection";
        goto stream_listen_error;
    }
  return SDM_ERROR_NONE;

stream_listen_error:
  if (wait_conn_fd >= 0) {
      close(wait_conn_fd);
  }
  return SDM_ERROR_STREAM;

}

static int stream_open(sdm_stream_t *stream)
{
    struct private_data_t *pdata = stream->pdata;
    int rv = SDM_ERROR_STREAM, port;
    char *args;
    char *socket_type, *ip_s, *port_s;
    
    /* args: tcp:[connect|listen]:<ip>:<port> */
    if (stream->args == NULL) {
        pdata->error = EINVAL;
        pdata->error_op = "tcp arguments undefined";
        return rv;
    }
    args = strdup(stream->args);
    socket_type = strtok(args, ":");
    ip_s = strtok(NULL, ":");
    port_s = strtok(NULL, ":");
    if (!socket_type || !ip_s || !port_s) {
        pdata->error = EINVAL;
        pdata->error_op = "arguments parsing error";
        goto stream_open_finish;
    }
    port = atoi(port_s);
    
    pdata->saun.sin_family = AF_INET;
    pdata->saun.sin_addr.s_addr = inet_addr(ip_s);
    pdata->saun.sin_port = htons(port);

    if (strcmp(socket_type, "connect") == 0) {
        rv = stream_open_connect(stream);
    } else if (strcmp(socket_type, "listen") == 0) {
        rv = stream_open_listen(stream);
    } else {
        pdata->error = EINVAL;
        pdata->error_op = "connection type undefiend";
    }
  stream_open_finish:
    free(args);
    return rv;
}

static int stream_close(sdm_stream_t *stream)
{
    struct private_data_t *pdata = stream->pdata;

    if (pdata->fd >= 0) {
        close(pdata->fd);
    }
    
    return SDM_ERROR_NONE;
}

static void stream_free(sdm_stream_t *stream)
{
    free(stream->pdata);
}

static int stream_read(const sdm_stream_t *stream, int16_t* samples, unsigned sample_count)
{
    struct private_data_t *pdata = stream->pdata;
    int rv, offset = 0;
    int requested_length = 2 * sample_count;
    
    if (stream->direction == STREAM_OUTPUT) {
        return SDM_ERROR_STREAM;
    }
    do {
        rv = read(pdata->fd, &((char*)samples)[offset], requested_length - offset);
        if (rv > 0) {
            offset += rv;
        } else {
            if (rv < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }
    } while (offset < requested_length);

    if (rv == 0) {
        return offset / 2;
    } else if (offset == requested_length) {
        return sample_count;
    } else {
        return SDM_ERROR_STREAM;
    }
}

static int stream_write(sdm_stream_t *stream, void* samples, unsigned sample_count)
{
    struct private_data_t *pdata = stream->pdata;
    int rv, offset = 0;
    int requested_length = 2 * sample_count;
    
    if (stream->direction == STREAM_INPUT) {
        return SDM_ERROR_STREAM;
    }
    do {
        rv = write(pdata->fd, &((char*)samples)[offset], requested_length - offset);
        if (rv > 0) {
            offset += rv;
        } else {
            if (rv < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            break;
        }
    } while (offset < requested_length);
  
    if (offset == requested_length) {
        return sample_count;
    } else {
        return SDM_ERROR_STREAM;
    }
}

static const char* stream_get_error(sdm_stream_t *stream)
{
    struct private_data_t *pdata = stream->pdata;
    return strerror(pdata->error);
}

static const char* stream_get_error_op(sdm_stream_t *stream)
{
    struct private_data_t *pdata = stream->pdata;
    return pdata->error_op;
}

static int stream_count(sdm_stream_t* stream)
{
    if (stream->direction == STREAM_OUTPUT) {
        return 0;
    }
    struct stat st;
    stat(stream->args, &st);
    return st.st_size / 2;
}

int sdm_stream_tcp_new(sdm_stream_t *stream)
{
    stream->pdata = calloc(1, sizeof(struct private_data_t));
    stream->open = stream_open;
    stream->close = stream_close;
    stream->free = stream_free;
    stream->read = stream_read;
    stream->write = stream_write;
    stream->get_error = stream_get_error;
    stream->get_error_op = stream_get_error_op;
    stream->count = stream_count;
    strcpy(stream->name, "TCP");
    
    return SDM_ERROR_NONE;
}