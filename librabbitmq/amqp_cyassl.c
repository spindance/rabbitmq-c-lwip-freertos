/* vim:set ft=c ts=2 sw=2 sts=2 et cindent: */
/*
 * Copyright 2012-2013 Michael Steinert
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "amqp_ssl_socket.h"
#include "amqp_private.h"
#include "lwip/sockets.h"
#include <cyassl/ssl.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>

#ifndef AMQP_USE_UNTESTED_SSL_BACKEND
# error This SSL backend is alpha quality and likely contains errors.\
  -DAMQP_USE_UNTESTED_SSL_BACKEND to use this backend
#endif

struct amqp_ssl_socket_t {
  const struct amqp_socket_class_t *klass;
  CYASSL_CTX *ctx;
  CYASSL *ssl;
  int sockfd;
  char *buffer;
  size_t length;
  int last_error;
};

CYASSL_CTX *amqp_ssl_socket_get_cyassl_ctx(amqp_socket_t *base)
{
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;
  return self->ctx;
}

CYASSL *amqp_ssl_socket_get_cyassl_session_object(amqp_socket_t *base)
{
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;
  return self->ssl;
}

static inline ssize_t amqp_ssl_socket_are_we_open(struct amqp_ssl_socket_t *self)
{
  ssize_t res = AMQP_STATUS_INVALID_PARAMETER;
  if ((self) && (self->sockfd >= 0) && (self->ssl)) {
    res = AMQP_STATUS_OK;
  }
  return res;
}

static ssize_t
amqp_ssl_socket_send_inner(void *base, const void *buf, size_t len, int flags)
{
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;

  ssize_t res = amqp_ssl_socket_are_we_open(self);
  if (AMQP_STATUS_OK != res) {
    return res;
  }

  const char *buf_left = buf;
  ssize_t len_left = len;

#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif

  uint64_t startTimeNs = amqp_get_monotonic_timestamp();

start:
  RABBIT_INFO("send_inner: base=%08x, buf=%08x, len=%u flags=0x%08x", (uint32_t)base, (uint32_t)buf, len, flags);
  res = CyaSSL_send(self->ssl, buf_left, len_left, flags);
  uint64_t endTimeNs = amqp_get_monotonic_timestamp();
  RABBIT_INFO("send_inner: base=%08x, CyaSSL_send res=%d", (uint32_t)base, res);
  if (endTimeNs-startTimeNs > 7ULL*1000*1000*1000) {
    RABBIT_INFO( "send_inner-time %u sec", (uint32_t)((endTimeNs-startTimeNs)/1000/1000/1000));
  }

  uint32_t sendTimeMs = (uint32_t)((endTimeNs-startTimeNs)/1000/1000);

  if (res < 0) {
    self->last_error = CyaSSL_get_error(self->ssl,res);
  }

  if ((res>=0) && (res == len_left)) {
      self->last_error = 0;
      res = AMQP_STATUS_OK;

  } else if (sendTimeMs>1000) {
    logInfo("rabbit send to time=%ums res=%d last_error=%d len_left=%u/%u", sendTimeMs, res, self->last_error, len_left, len);
    res = AMQP_STATUS_SOCKET_ERROR;

  } else if (res < 0) {

    if (SSL_ERROR_WANT_WRITE == self->last_error) {
      logInfo("rabbit want_write time=%ums res=%d last_error=%d len_left=%u/%u", sendTimeMs, res, self->last_error, len_left, len);
      goto start;

    } else if (EINTR == self->last_error) {
      logInfo("rabbit EINTR time=%ums res=%d last_error=%d len_left=%u/%u", sendTimeMs, res, self->last_error, len_left, len);
      goto start;

    } else {
      logInfo("rabbit CyaSSL_send time=%ums res=%d last_error=%d len_left=%u/%u", sendTimeMs, res, self->last_error, len_left, len);
      res = AMQP_STATUS_SOCKET_ERROR;
    }

  } else {
    buf_left += res;
    len_left -= res;
    goto start;
  }

  RABBIT_INFO("send_inner: base=%08x, return res=%d", (uint32_t)base, (int)res);
  return res;
}

static ssize_t
amqp_ssl_socket_send(void *base, const void *buf, size_t len)
{
  return amqp_ssl_socket_send_inner(base, buf, len, 0);
}

static ssize_t
amqp_ssl_socket_writev(void *base, struct iovec *iov, int iovcnt)
{
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;
  ssize_t ret;

  int i;
  for (i = 0; i < iovcnt - 1; ++i) {
    ret = amqp_ssl_socket_send_inner(self, iov[i].iov_base, iov[i].iov_len, MSG_MORE);
    if (ret != AMQP_STATUS_OK) {
      goto exit;
    }
  }
  ret = amqp_ssl_socket_send_inner(self, iov[i].iov_base, iov[i].iov_len, 0);

exit:
  return ret;
}

static ssize_t
amqp_ssl_socket_recv(void *base, void *buf, size_t len, int flags)
{
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;

  ssize_t res = amqp_ssl_socket_are_we_open(self);
  if (AMQP_STATUS_OK != res) {
    return res;
  }


start:
  RABBIT_INFO("socket_recv: base=%08x, buf=%08x, len=%u flags=0x%08x", (uint32_t)base, (uint32_t)buf, len, flags);
  res = CyaSSL_recv(self->ssl, buf, len, flags);
  RABBIT_INFO("socket_recv: base=%08x, CyaSSL_recv ret=%d", (uint32_t)base, res);

  if (0 > res) {
    self->last_error = CyaSSL_get_error(self->ssl,res);
    if (EINTR == self->last_error) {
      goto start;
    } else {
      res = AMQP_STATUS_SOCKET_ERROR;
    }
  } else if (0 == res) {
    res = AMQP_STATUS_CONNECTION_CLOSED;
  }

  return res;
}

static int
amqp_ssl_socket_get_sockfd(void *base)
{
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;
  return self->sockfd;
}

static int
amqp_ssl_socket_close(void *base)
{
  RABBIT_INFO("socket_close: base=%08x", (uint32_t)base);
  int status = -1;
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;
  if (self) {
    if (self->sockfd >= 0) {
      status = amqp_os_socket_close(self->sockfd);
      if (status) {
        logError("amqp_os_socket_close=%d",status);
      }
      // ALII-4689 only close a socket once.
      self->sockfd = -1;
    }
    if (self->ssl) {
      CyaSSL_free(self->ssl);
      // ALII-4689 only free a connection CYASSL object once
      self->ssl = NULL;
    }
  }
  return status;
}

static void amqp_ssl_socket_delete(void *base)
{
  RABBIT_INFO("socket_delete: base=%08x", (uint32_t)base);
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;

  if (self) {
    amqp_ssl_socket_close(self);

#ifndef CONFIG_APP_CLOUD_MESSAGING_ENA
    if (self->ctx) {
      CyaSSL_CTX_free(self->ctx);
    }
#endif
    free(self->buffer);
    free(self);
  }
}

static int
amqp_ssl_socket_error(void *base)
{
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;

  if (self) {
    return self->last_error;
  } else {
    return AMQP_STATUS_INVALID_PARAMETER;
  }
}

#ifndef CONFIG_RABBITMQ_TINY_EMBEDDED_ENA

char *
amqp_ssl_error_string(AMQP_UNUSED int err)
{
  return strdup("A ssl socket error occurred.");
}

#else

char *
amqp_ssl_error_string(AMQP_UNUSED int err)
{
  return "A ssl socket error occurred.";
}

#endif

static int
amqp_ssl_socket_open(void *base, const char *host, int port, struct timeval *timeout)
{
  RABBIT_INFO("socket_open: base=%08x host=%s port=%d timeout=%d.%06d", (uint32_t)base, host, port, timeout->tv_sec, timeout->tv_usec);
  struct amqp_ssl_socket_t *self = (struct amqp_ssl_socket_t *)base;

  if (NULL == self) {
    return AMQP_STATUS_INVALID_PARAMETER;
  }

  self->last_error = AMQP_STATUS_OK;

  if (NULL == self->ctx) {
    self->last_error = AMQP_STATUS_INVALID_PARAMETER;
    return self->last_error;
  }

  RABBIT_INFO("Calling CyaSSL_new\n");
  self->ssl = CyaSSL_new(self->ctx);
  if (NULL == self->ssl) {
    self->last_error = AMQP_STATUS_SSL_ERROR;
    return self->last_error;
  }

  RABBIT_INFO("Calling amqp_open_socket_noblock");
  self->sockfd = amqp_open_socket_noblock(host, port, timeout);
  if (0 > self->sockfd) {
    self->last_error = - self->sockfd;
    return AMQP_STATUS_SOCKET_ERROR;;
  }
  CyaSSL_set_fd(self->ssl, self->sockfd);

  RABBIT_INFO("Calling CyaSSL_connect");
  int status = CyaSSL_connect(self->ssl);
  logDebug("%d=CyaSSL_connect",status);
  if (SSL_SUCCESS != status) {
    logOffNominal("CyaSSL_connect failed = %d", status);
    self->last_error = AMQP_STATUS_SSL_ERROR;
    return self->last_error;
  }
  return AMQP_STATUS_OK;
}

static const struct amqp_socket_class_t amqp_ssl_socket_class = {
  amqp_ssl_socket_writev, /* writev */
  amqp_ssl_socket_send, /* send */
  amqp_ssl_socket_recv, /* recv */
  amqp_ssl_socket_open, /* open */
  amqp_ssl_socket_close, /* close */
  amqp_ssl_socket_get_sockfd, /* get_sockfd */
  amqp_ssl_socket_delete /* delete */
};

amqp_socket_t *
amqp_ssl_socket_new(amqp_connection_state_t state)
{
  struct amqp_ssl_socket_t *self = calloc(1, sizeof(*self));
  assert(self);

#ifdef CONFIG_APP_CLOUD_MESSAGING_ENA
  self->ctx = CYASSL_SINGLE_GLOBAL_CONTEXT();
#else
  CyaSSL_Init();
  self->ctx = CyaSSL_CTX_new(CyaTLSv1_2_client_method());
#endif
  assert(self->ctx);
  self->klass = &amqp_ssl_socket_class;
  self->sockfd = -1;

  amqp_set_socket(state, (amqp_socket_t *)self);

  return (amqp_socket_t *)self;
}

#if defined(CONFIG_RABBITMQ_USE_CYASSL_BUFFER) && CONFIG_RABBITMQ_USE_CYASSL_BUFFER
int
amqp_ssl_socket_set_cacert_buffer(amqp_socket_t *base,
                           const char *cacert,
                           size_t certSize,
                           int type)
{
  int status;
  struct amqp_ssl_socket_t *self;
  if (base->klass != &amqp_ssl_socket_class) {
    amqp_abort("<%p> is not of type amqp_ssl_socket_t", base);
  }
  self = (struct amqp_ssl_socket_t *)base;
  status = CyaSSL_CTX_load_verify_buffer(self->ctx, (const unsigned char*)cacert, certSize, type);
  if (SSL_SUCCESS != status) {
    return -1;
  }
  return 0;
}
#endif

#if !defined(NO_FILESYSTEM) && !defined(NO_CERTS)
int
amqp_ssl_socket_set_cacert(amqp_socket_t *base,
                           const char *cacert)
{
  int status;
  struct amqp_ssl_socket_t *self;
  if (base->klass != &amqp_ssl_socket_class) {
    amqp_abort("<%p> is not of type amqp_ssl_socket_t", base);
  }
  self = (struct amqp_ssl_socket_t *)base;
  status = CyaSSL_CTX_load_verify_locations(self->ctx, cacert, NULL);
  if (SSL_SUCCESS != status) {
    return -1;
  }
  return 0;
}
#endif

#if defined(CONFIG_RABBITMQ_USE_CYASSL_BUFFER) && CONFIG_RABBITMQ_USE_CYASSL_BUFFER
int
amqp_ssl_socket_set_key_buffer(amqp_socket_t *base,
                                   const char *cert,
                                   const size_t certSize,
                                   const char *key,
                                   const size_t keySize,
                                   const int keyType)
{
  int status;
  struct amqp_ssl_socket_t *self;
  if (base->klass != &amqp_ssl_socket_class) {
    amqp_abort("<%p> is not of type amqp_ssl_socket_t", base);
  }
  self = (struct amqp_ssl_socket_t *)base;
  status = CyaSSL_CTX_use_PrivateKey_buffer(
               self->ctx,
               (const unsigned char*)key,
               keySize,
               keyType);
  if (SSL_SUCCESS != status) {
    return -1;
  }

  status = CyaSSL_CTX_use_certificate_chain_buffer(self->ctx, (const unsigned char*)cert, certSize);
  if (SSL_SUCCESS != status) {
    return -1;
  }

  return 0;
}
#endif


#if !defined(NO_FILESYSTEM) && !defined(NO_CERTS)
int
amqp_ssl_socket_set_key(amqp_socket_t *base,
                        const char *cert,
                        const char *key)
{
  int status;
  struct amqp_ssl_socket_t *self;
  if (base->klass != &amqp_ssl_socket_class) {
    amqp_abort("<%p> is not of type amqp_ssl_socket_t", base);
  }
  self = (struct amqp_ssl_socket_t *)base;
  status = CyaSSL_CTX_use_PrivateKey_file(self->ctx, key,
                                          SSL_FILETYPE_PEM);
  if (SSL_SUCCESS != status) {
    return -1;
  }

  status = CyaSSL_CTX_use_certificate_chain_file(self->ctx, cert);
  if (SSL_SUCCESS != status) {
    return -1;
  }

  return 0;
}
#endif

void
amqp_ssl_socket_set_verify(AMQP_UNUSED amqp_socket_t *base,
                           AMQP_UNUSED amqp_boolean_t verify)
{
  /* noop for CyaSSL */
  logFatal("Not Implemented.",0);

}

void
amqp_set_initialize_ssl_library(AMQP_UNUSED amqp_boolean_t do_initialize)
{
	  logFatal("Not Implemented.",0);
}
