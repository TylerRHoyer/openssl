/*
 * Dumps master keys for OpenSSL clients to file. The format is documented at
 * https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/Key_Log_Format
 *
 * Copyright (C) 2014 Peter Wu <peter@lekensteyn.nl>
 * Licensed under the terms of GPLv3 (or any later version) at your choice.
 *
 * Usage:
 *  cc sslkeylog.c -shared -o libsslkeylog.so -fPIC -ldl
 *  SSLKEYLOGFILE=premaster.txt LD_PRELOAD=./libsslkeylog.so openssl ...
 */
#define _GNU_SOURCE /* for RTLD_NEXT */
#include <dlfcn.h>
#include <openssl/objects.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define PREFIX      "CLIENT_RANDOM "
#define PREFIX_LEN  (sizeof(PREFIX) - 1)
#define FIRSTLINE   "# SSL key logfile generated by sslkeylog.c\n"
#define FIRSTLINE_LEN (sizeof(FIRSTLINE) - 1)

static int keylog_file_fd = -1;

static inline void put_hex(char *buffer, int pos, char c)
{
    unsigned char c1 = ((unsigned char) c) >> 4;
    unsigned char c2 = c & 0xF;
    buffer[pos] = c1 < 10 ? '0' + c1 : 'A' + c1 - 10;
    buffer[pos+1] = c2 < 10 ? '0' + c2 : 'A' + c2 - 10;
}

static void dump_to_fd(int fd, unsigned char *client_random,
        unsigned char *master_key, int master_key_length)
{
    int pos, i;
    char line[PREFIX_LEN + 2 * SSL3_RANDOM_SIZE + 1 +
              2 * SSL_MAX_MASTER_KEY_LENGTH + 1];

    memcpy(line, PREFIX, PREFIX_LEN);
    pos = PREFIX_LEN;
    /* Client Random for SSLv3/TLS */
    for (i = 0; i < SSL3_RANDOM_SIZE; i++) {
        put_hex(line, pos, client_random[i]);
        pos += 2;
    }
    line[pos++] = ' ';
    /* Master Secret (size is at most SSL_MAX_MASTER_KEY_LENGTH) */
    for (i = 0; i < master_key_length; i++) {
        put_hex(line, pos, master_key[i]);
        pos += 2;
    }
    line[pos++] = '\n';
    /* Write at once rather than using buffered I/O. Perhaps there is concurrent
     * write access so do not write hex values one by one. */
    write(fd, line, pos);
}

static void init_keylog_file(void)
{
    if (keylog_file_fd >= 0)
        return;

    const char *filename = getenv("SSLKEYLOGFILE");
    if (filename) {
        keylog_file_fd = open(filename, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (keylog_file_fd >= 0 && lseek(keylog_file_fd, 0, SEEK_END) == 0) {
            /* file is opened successfully and there is no data (pos == 0) */
            write(keylog_file_fd, FIRSTLINE, FIRSTLINE_LEN);
        }
    }
}

typedef struct ssl_tap_state {
    int master_key_length;
    unsigned char master_key[SSL_MAX_MASTER_KEY_LENGTH];

} ssl_tap_state_t;

static void copy_master_secret(const SSL_SESSION *session,
        unsigned char *master_key_out, int *keylen_out)
{
    if (session->master_key_length > 0) {
        *keylen_out = session->master_key_length;
        memcpy(master_key_out, session->master_key,
                session->master_key_length);
    }
}

static void copy_client_random(const SSL *ssl, unsigned char *client_random)
{
    if (ssl->s3) {
        memcpy(client_random, ssl->s3->client_random, SSL3_RANDOM_SIZE);
    }
}

/* Copies SSL state for later comparison in tap_ssl_key. */
static void ssl_tap_state_init(ssl_tap_state_t *state, const SSL *ssl)
{
    const SSL_SESSION *session = ssl->session;

    memset(state, 0, sizeof(ssl_tap_state_t));
    if (session) {
        copy_master_secret(session, state->master_key, &state->master_key_length);
    }
}

#define SSL_TAP_STATE(state, ssl) \
    ssl_tap_state_t state; \
    ssl_tap_state_init(&state, ssl)

static void tap_ssl_key(const SSL *ssl, ssl_tap_state_t *state)
{
    const SSL_SESSION *session = ssl->session;
    unsigned char client_random[SSL3_RANDOM_SIZE];
    unsigned char master_key[SSL_MAX_MASTER_KEY_LENGTH];
    int master_key_length = 0;

    if (session) {
        copy_master_secret(session, master_key, &master_key_length);
        /* Assume we have a client random if the master key is set. */
        if (master_key_length > 0) {
            copy_client_random(ssl, client_random);
        }
    }

    /* Write the logfile when the master key is available for SSLv3/TLSv1. */
    if (master_key_length > 0) {
        /* Skip writing keys if it did not change. */
        if (state->master_key_length == master_key_length &&
            memcmp(state->master_key, master_key, master_key_length) == 0) {
            return;
        }

        init_keylog_file();
        if (keylog_file_fd >= 0) {
            dump_to_fd(keylog_file_fd, client_random, master_key,
                    master_key_length);
        }
    }
}