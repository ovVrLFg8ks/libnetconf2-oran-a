/**
 * @file session_mbedtls.c
 * @author Roman Janota <janota@cesnet.cz>
 * @brief libnetconf2 - wrapped MbedTLS function calls for TLS/asymmetric cryptography support
 *
 * This file is a wrapper for MbedTLS function calls. The implementation is done
 * in such a way that the original libnetconf2 code is not dependent on MbedTLS.
 * This file is included in the build process only if MbedTLS is being used.
 *
 * @copyright
 * Copyright (c) 2024 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "compat.h"
#include "config.h"
#include "log_p.h"
#include "session.h"
#include "session_p.h"
#include "session_wrapper.h"

#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pem.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crl.h>
#include <mbedtls/x509_crt.h>

/**
 * @brief Converts MbedTLS error code to a string and merges it with an arbitrary error message.
 *
 * @param[in] session NETCONF session.
 * @param[in] mbedtls_err_code MbedTLS error code.
 * @param[in] orig_err_msg_fmt Original error message format string.
 * @param[in] ... Additional arguments for the original error message.
 */
static void
nc_mbedtls_strerr(const struct nc_session *session, int mbedtls_err_code, const char *orig_err_msg_fmt, ...)
{
    va_list args;
    char *err_buf = NULL, *err_msg_fmt = NULL;
    size_t err_buf_len = 0, err_msg_fmt_len = 0;
    const char *high_err_str, *low_err_str;

    va_start(args, orig_err_msg_fmt);

    /* get the length of the error strings */
    high_err_str = mbedtls_high_level_strerr(mbedtls_err_code);
    low_err_str = mbedtls_low_level_strerr(mbedtls_err_code);
    if (high_err_str) {
        err_buf_len += strlen(high_err_str);
    }
    if (low_err_str) {
        err_buf_len += strlen(low_err_str);
    }
    if (!err_buf_len) {
        /* just print the original error message */
        nc_log_vprintf(session, NC_VERB_ERROR, orig_err_msg_fmt, args);
        goto cleanup;
    }

    if (high_err_str && low_err_str) {
        /* for a colon and 2 spaces */
        err_buf_len += 3;
    }

    /* allocate the mbedtls error buffer */
    err_buf = malloc(err_buf_len + 1);
    if (!err_buf) {
        /* just print the original error message */
        nc_log_vprintf(session, NC_VERB_ERROR, orig_err_msg_fmt, args);
        goto cleanup;
    }

    /* fill the error buffer and print it */
    if (high_err_str && low_err_str) {
        snprintf(err_buf, err_buf_len + 1, "%s : %s", high_err_str, low_err_str);
    } else if (high_err_str) {
        snprintf(err_buf, err_buf_len + 1, "%s", high_err_str);
    } else {
        snprintf(err_buf, err_buf_len + 1, "%s", low_err_str);
    }

    /* allocate the new error format string buffer, err_msg = "orig_err_msg (MbedTLS err)." */
    err_msg_fmt_len = strlen(orig_err_msg_fmt) + strlen(" (") + strlen(err_buf) + strlen(").");
    err_msg_fmt = malloc(err_msg_fmt_len + 1);
    if (!err_msg_fmt) {
        /* just print the original error message */
        nc_log_vprintf(session, NC_VERB_ERROR, orig_err_msg_fmt, args);
        goto cleanup;
    }

    /* fill the new error format string */
    snprintf(err_msg_fmt, err_msg_fmt_len + 1, "%s (%s).", orig_err_msg_fmt, err_buf);

    /* print the error message */
    nc_log_vprintf(session, NC_VERB_ERROR, err_msg_fmt, args);

cleanup:
    va_end(args);
    free(err_msg_fmt);
    free(err_buf);
}

/**
 * @brief Converts DN to a string.
 *
 * @param[in] dn Internal DN representation.
 * @return DN string on success, NULL of fail.
 */
static char *
nc_server_tls_dn2str(const mbedtls_x509_name *dn)
{
    char *str;
    size_t len = 64;
    int r;

    str = malloc(len);
    NC_CHECK_ERRMEM_RET(!str, NULL);

    while ((r = mbedtls_x509_dn_gets(str, len, dn)) == MBEDTLS_ERR_X509_BUFFER_TOO_SMALL) {
        len <<= 1;
        str = nc_realloc(str, len);
        NC_CHECK_ERRMEM_RET(!str, NULL);
    }
    if (r < 1) {
        free(str);
        nc_mbedtls_strerr(NULL, r, "Failed to convert DN to string");
        return NULL;
    }

    return str;
}

/**
 * @brief Create a new random number generator context.
 *
 * @param[out] ctr_drbg Random bit generator context.
 * @param[out] entropy Entropy context.
 * @return 0 on success, 1 on failure.
 */
static int
nc_tls_rng_new(mbedtls_ctr_drbg_context **ctr_drbg, mbedtls_entropy_context **entropy)
{
    int rc;

    *ctr_drbg = NULL;
    *entropy = NULL;

    *entropy = malloc(sizeof **entropy);
    NC_CHECK_ERRMEM_GOTO(!*entropy, , fail);
    *ctr_drbg = malloc(sizeof **ctr_drbg);
    NC_CHECK_ERRMEM_GOTO(!*ctr_drbg, , fail);

    mbedtls_entropy_init(*entropy);
    mbedtls_ctr_drbg_init(*ctr_drbg);

    rc = mbedtls_ctr_drbg_seed(*ctr_drbg, mbedtls_entropy_func, *entropy, NULL, 0);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Seeding ctr_drbg failed");
        goto fail;
    }

    return 0;

fail:
    mbedtls_ctr_drbg_free(*ctr_drbg);
    free(*ctr_drbg);
    if (*entropy) {
        mbedtls_entropy_free(*entropy);
        free(*entropy);
    }
    *ctr_drbg = NULL;
    *entropy = NULL;
    return 1;
}

/**
 * @brief Destroy the random number generator context.
 *
 * @param[in] ctr_drbg Random bit generator context.
 * @param[in] entropy Entropy context.
 */
static void
nc_tls_rng_destroy(mbedtls_ctr_drbg_context *ctr_drbg, mbedtls_entropy_context *entropy)
{
    mbedtls_ctr_drbg_free(ctr_drbg);
    free(ctr_drbg);
    if (entropy) {
        mbedtls_entropy_free(entropy);
        free(entropy);
    }
}

/**
 * @brief Get a string representation of the verification error.
 *
 * @param[in] err Verification error code.
 * @return String representation of the error. Caller is responsible for freeing it.
 */
static char *
nc_tls_get_verify_err_str(int err)
{
    int ret;
    char *err_buf = NULL;

    err_buf = malloc(256);
    NC_CHECK_ERRMEM_RET(!err_buf, NULL);

    ret = mbedtls_x509_crt_verify_info(err_buf, 256, "", err);
    if (ret < 0) {
        free(err_buf);
        return NULL;
    }

    /* strip the NL */
    err_buf[ret - 1] = '\0';

    return err_buf;
}

int
nc_tls_backend_init_wrap(void)
{
    int r;

    r = psa_crypto_init();

    if (r) {
        nc_mbedtls_strerr(NULL, r, "Failed to initialize PSA crypto");
        return -1;
    }

    return 0;
}

void
nc_tls_backend_destroy_wrap(void)
{
    mbedtls_psa_crypto_free();
}

void *
nc_tls_session_new_wrap(void *tls_cfg)
{
    int rc;
    mbedtls_ssl_context *session;

    session = malloc(sizeof *session);
    NC_CHECK_ERRMEM_RET(!session, NULL);

    mbedtls_ssl_init(session);

    rc = mbedtls_ssl_setup(session, tls_cfg);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Setting up TLS session failed");
        mbedtls_ssl_free(session);
        free(session);
        return NULL;
    }

    return session;
}

void
nc_tls_session_destroy_wrap(void *tls_session)
{
    mbedtls_ssl_free(tls_session);
    free(tls_session);
}

void *
nc_tls_config_new_wrap(int UNUSED(side))
{
    mbedtls_ssl_config *tls_cfg;

    tls_cfg = malloc(sizeof *tls_cfg);
    NC_CHECK_ERRMEM_RET(!tls_cfg, NULL);

    mbedtls_ssl_config_init(tls_cfg);
    return tls_cfg;
}

void
nc_tls_config_destroy_wrap(void *tls_cfg)
{
    if (!tls_cfg) {
        return;
    }

    mbedtls_ssl_config_free(tls_cfg);
    free(tls_cfg);
}

void *
nc_tls_cert_new_wrap(void)
{
    mbedtls_x509_crt *cert;

    cert = malloc(sizeof *cert);
    NC_CHECK_ERRMEM_RET(!cert, NULL);

    mbedtls_x509_crt_init(cert);
    return cert;
}

void
nc_tls_cert_destroy_wrap(void *cert)
{
    mbedtls_x509_crt_free(cert);
    free(cert);
}

/**
 * @brief Create a new private key context.
 *
 * @return New private key context or NULL.
 */
static void *
nc_tls_pkey_new_wrap(void)
{
    mbedtls_pk_context *pkey;

    pkey = malloc(sizeof *pkey);
    NC_CHECK_ERRMEM_RET(!pkey, NULL);

    mbedtls_pk_init(pkey);
    return pkey;
}

void
nc_tls_privkey_destroy_wrap(void *pkey)
{
    mbedtls_pk_free(pkey);
    free(pkey);
}

void *
nc_tls_cert_store_new_wrap(void)
{
    /* certificate is the same as a certificate store in MbedTLS */
    return nc_tls_cert_new_wrap();
}

void
nc_tls_cert_store_destroy_wrap(void *cert_store)
{
    /* certificate is the same as a certificate store in MbedTLS */
    nc_tls_cert_destroy_wrap(cert_store);
}

void *
nc_tls_crl_store_new_wrap(void)
{
    mbedtls_x509_crl *crl;

    crl = malloc(sizeof *crl);
    NC_CHECK_ERRMEM_RET(!crl, NULL);

    mbedtls_x509_crl_init(crl);
    return crl;
}

void
nc_tls_crl_store_destroy_wrap(void *crl_store)
{
    mbedtls_x509_crl_free(crl_store);
    free(crl_store);
}

void *
nc_tls_pem_to_cert_wrap(const char *cert_data)
{
    int rc;
    mbedtls_x509_crt *cert;

    cert = nc_tls_cert_new_wrap();
    if (!cert) {
        return NULL;
    }

    rc = mbedtls_x509_crt_parse(cert, (const unsigned char *)cert_data, strlen(cert_data) + 1);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Parsing certificate data failed");
        nc_tls_cert_destroy_wrap(cert);
        return NULL;
    }

    return cert;
}

int
nc_tls_add_cert_to_store_wrap(void *cert, void *cert_store)
{
    mbedtls_x509_crt *iter;

    /* store is a linked list */
    iter = cert_store;
    while (iter->next) {
        iter = iter->next;
    }
    iter->next = cert;

    return 0;
}

void *
nc_tls_pem_to_privkey_wrap(const char *privkey_data)
{
    int rc = 0;
    mbedtls_pk_context *pkey = NULL;
    mbedtls_ctr_drbg_context *ctr_drbg = NULL;
    mbedtls_entropy_context *entropy = NULL;

    rc = nc_tls_rng_new(&ctr_drbg, &entropy);
    if (rc) {
        goto cleanup;
    }

    pkey = nc_tls_pkey_new_wrap();
    if (!pkey) {
        rc = 1;
        goto cleanup;
    }

    rc = mbedtls_pk_parse_key(pkey, (const unsigned char *)privkey_data, strlen(privkey_data) + 1, NULL, 0, mbedtls_ctr_drbg_random, ctr_drbg);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Parsing private key data failed");
        goto cleanup;
    }

cleanup:
    if (rc) {
        nc_tls_privkey_destroy_wrap(pkey);
        pkey = NULL;
    }
    nc_tls_rng_destroy(ctr_drbg, entropy);
    return pkey;
}

int
nc_server_tls_add_crl_to_store_wrap(const unsigned char *crl_data, size_t size, void *crl_store)
{
    int rc;

    /* try DER first */
    rc = mbedtls_x509_crl_parse_der(crl_store, crl_data, size);
    if (!rc) {
        /* success, it was DER */
        return 0;
    }

    /* DER failed, try PEM */
    rc = mbedtls_x509_crl_parse(crl_store, crl_data, size + 1);
    if (!rc) {
        /* success, it was PEM */
        return 0;
    }

    /* failed to parse it */
    ERR(NULL, "Reading downloaded CRL failed.");
    return 1;
}

int
nc_server_tls_set_tls_versions_wrap(void *tls_cfg, unsigned int tls_versions)
{
    if ((tls_versions & NC_TLS_VERSION_10) || ((tls_versions & NC_TLS_VERSION_11))) {
        /* skip TLS versions 1.0 and 1.1 */
        WRN(NULL, "mbedTLS does not support TLS1.0 and TLS1.1");
    }

    /* first set the minimum version */
    if (tls_versions & NC_TLS_VERSION_12) {
        mbedtls_ssl_conf_min_tls_version(tls_cfg, MBEDTLS_SSL_VERSION_TLS1_2);
    } else if (tls_versions & NC_TLS_VERSION_13) {
        mbedtls_ssl_conf_min_tls_version(tls_cfg, MBEDTLS_SSL_VERSION_TLS1_3);
    }

    /* then set the maximum version */
    if (tls_versions & NC_TLS_VERSION_13) {
        mbedtls_ssl_conf_max_tls_version(tls_cfg, MBEDTLS_SSL_VERSION_TLS1_3);
    } else if (tls_versions & NC_TLS_VERSION_12) {
        mbedtls_ssl_conf_max_tls_version(tls_cfg, MBEDTLS_SSL_VERSION_TLS1_2);
    }

    return 0;
}

/**
 * @brief Duplicates a certificate.
 *
 * @param[in] cert Certificate to duplicate.
 * @return Duplicated certificate or NULL.
 */
static mbedtls_x509_crt *
nc_tls_cert_dup(const mbedtls_x509_crt *cert)
{
    mbedtls_x509_crt *new_cert;

    new_cert = nc_tls_cert_new_wrap();
    if (!new_cert) {
        return NULL;
    }

    if (mbedtls_x509_crt_parse_der(new_cert, cert->raw.p, cert->raw.len)) {
        free(new_cert);
        return NULL;
    }

    return new_cert;
}

/**
 * @brief Duplicate a certificate and append it to a chain.
 *
 * @param[in] cert Certificate to duplicate and append.
 * @param[in,out] chain Chain to append the certificate to.
 * @return 0 on success, -1 on error.
 */
static int
nc_server_tls_append_cert_to_chain(mbedtls_x509_crt *cert, mbedtls_x509_crt **chain)
{
    mbedtls_x509_crt *iter, *copy;

    copy = nc_tls_cert_dup(cert);
    if (!copy) {
        return -1;
    }

    if (!*chain) {
        /* first in the list */
        *chain = copy;
    } else {
        /* find the last cert */
        iter = *chain;
        while (iter->next) {
            iter = iter->next;
        }
        iter->next = copy;
    }

    return 0;
}

/**
 * @brief Verify a certificate.
 *
 * @param[in] cb_data Callback data (session, opts, data for CTN).
 * @param[in] cert Certificate to verify.
 * @param[in] depth Certificate depth in the chain.
 * @param[in,out] flags Verification flags. Used to propagate errors.
 * @return 0 on success (verification result is based on the value of flags), non-zero on fatal-error.
 */
static int
nc_server_tls_verify_cb(void *cb_data, mbedtls_x509_crt *cert, int depth, uint32_t *flags)
{
    int ret = 0;
    struct nc_tls_verify_cb_data *data = cb_data;
    char *err;

    /* append to the chain we're building */
    ret = nc_server_tls_append_cert_to_chain(cert, (mbedtls_x509_crt **)&data->chain);
    if (ret) {
        nc_tls_cert_destroy_wrap(data->chain);
        return MBEDTLS_ERR_X509_ALLOC_FAILED;
    }

    if (!*flags) {
        /* in-built verification was successful */
        ret = nc_server_tls_verify_cert(cert, depth, 1, data);
    } else {
        /* in-built verification failed, but the client still may be authenticated if:
         * 1) the peer cert matches any configured end-entity cert
         * 2) the peer cert has a valid chain of trust to any configured certificate authority cert
         * otherwise just continue until we reach the peer cert (depth = 0)
         */
        if ((depth == 0) && (*flags == MBEDTLS_X509_BADCERT_NOT_TRUSTED)) {
            /* not trusted self-signed peer certificate, case 1) */
            ret = nc_server_tls_verify_cert(cert, depth, 0, data);
            if (!ret) {
                *flags &= ~MBEDTLS_X509_BADCERT_NOT_TRUSTED;
            }
        } else if (*flags == MBEDTLS_X509_BADCERT_MISSING) {
            /* full chain of trust is invalid, but it may be valid partially, case 2) */
            ret = nc_server_tls_verify_cert(cert, depth, 0, data);
            if (!ret) {
                *flags &= ~MBEDTLS_X509_BADCERT_MISSING;
            }
        } else {
            err = nc_tls_get_verify_err_str(*flags);
            ERR(data->session, "Cert verify: fail (%s).", err);
            free(err);
            ret = 1;
        }
    }

    if ((ret == -1) || (depth == 0)) {
        /* free the chain */
        nc_tls_cert_destroy_wrap(data->chain);
    }

    if (ret == -1) {
        /* fatal error */
        return MBEDTLS_ERR_X509_ALLOC_FAILED;
    } else if (!ret) {
        /* success */
        if ((depth == 0) && (!data->session->opts.server.client_cert)) {
            /* copy the client cert */
            data->session->opts.server.client_cert = nc_tls_cert_dup(cert);
            if (!data->session->opts.server.client_cert) {
                return MBEDTLS_ERR_X509_ALLOC_FAILED;
            }
        }
        return 0;
    } else {
        if (depth > 0) {
            /* chain verify failed, but peer cert can still match */
            return 0;
        } else {
            /* failed to verify peer cert, but return 0 so that we can propagate the error via the flags */
            if (!*flags) {
                *flags |= MBEDTLS_X509_BADCERT_OTHER;
            }
            return 0;
        }
    }
}

void
nc_server_tls_set_verify_wrap(void *tls_cfg, struct nc_tls_verify_cb_data *cb_data)
{
    mbedtls_ssl_conf_authmode(tls_cfg, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_verify(tls_cfg, nc_server_tls_verify_cb, cb_data);
}

void
nc_client_tls_set_verify_wrap(void *tls_cfg)
{
    mbedtls_ssl_conf_authmode(tls_cfg, MBEDTLS_SSL_VERIFY_REQUIRED);
}

char *
nc_server_tls_get_subject_wrap(void *cert)
{
    return nc_server_tls_dn2str(&(((mbedtls_x509_crt *)cert)->subject));
}

char *
nc_server_tls_get_issuer_wrap(void *cert)
{
    return nc_server_tls_dn2str(&(((mbedtls_x509_crt *)cert)->issuer));
}

void *
nc_tls_get_sans_wrap(void *cert)
{
    return &(((mbedtls_x509_crt *)cert)->subject_alt_names);
}

void
nc_tls_sans_destroy_wrap(void *UNUSED(sans))
{
    return;
}

int
nc_tls_get_num_sans_wrap(void *sans)
{
    mbedtls_x509_sequence *iter;
    int n = 0;

    /* sans are a linked list */
    iter = sans;
    while (iter) {
        ++n;
        iter = iter->next;
    }

    return n;
}

int
nc_tls_get_san_value_type_wrap(void *sans, int idx, char **san_value, NC_TLS_CTN_MAPTYPE *san_type)
{
    int i, rc, ret = 0;
    mbedtls_x509_sequence *iter;
    mbedtls_x509_subject_alternative_name san = {0};
    const mbedtls_x509_buf *ip;

    *san_value = NULL;
    *san_type = NC_TLS_CTN_UNKNOWN;

    /* find the SAN */
    iter = sans;
    for (i = 0; i < idx; i++) {
        iter = iter->next;
    }

    /* parse it */
    rc = mbedtls_x509_parse_subject_alt_name(&iter->buf, &san);
    if (rc && (rc != MBEDTLS_ERR_X509_FEATURE_UNAVAILABLE)) {
        return -1;
    }

    /* get its type and value */
    switch (san.type) {
    case MBEDTLS_X509_SAN_DNS_NAME:
        *san_type = NC_TLS_CTN_SAN_DNS_NAME;
        *san_value = strndup((const char *)san.san.unstructured_name.p, san.san.unstructured_name.len);
        NC_CHECK_ERRMEM_GOTO(!*san_value, ret = -1, cleanup);
        break;
    case MBEDTLS_X509_SAN_RFC822_NAME:
        *san_type = NC_TLS_CTN_SAN_RFC822_NAME;
        *san_value = strndup((const char *)san.san.unstructured_name.p, san.san.unstructured_name.len);
        NC_CHECK_ERRMEM_GOTO(!*san_value, ret = -1, cleanup);
        break;
    case MBEDTLS_X509_SAN_IP_ADDRESS:
        *san_type = NC_TLS_CTN_SAN_IP_ADDRESS;
        ip = &san.san.unstructured_name;
        if (ip->len == 4) {
            rc = asprintf(san_value, "%d.%d.%d.%d", ip->p[0], ip->p[1], ip->p[2], ip->p[3]) == -1;
            NC_CHECK_ERRMEM_GOTO(rc == -1, ret = -1, cleanup);
        } else if (ip->len == 16) {
            rc = asprintf(san_value, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                    ip->p[0], ip->p[1], ip->p[2], ip->p[3], ip->p[4], ip->p[5],
                    ip->p[6], ip->p[7], ip->p[8], ip->p[9], ip->p[10], ip->p[11],
                    ip->p[12], ip->p[13], ip->p[14], ip->p[15]);
            NC_CHECK_ERRMEM_GOTO(rc == -1, ret = -1, cleanup);
        } else {
            WRN(NULL, "SAN IP address in an unknown format (length is %d).", ip->len);
            ret = 1;
        }
        break;
    default:
        /* we dont care about other types */
        *san_type = NC_TLS_CTN_UNKNOWN;
        ret = 1;
        break;
    }

cleanup:
    mbedtls_x509_free_subject_alt_name(&san);
    return ret;
}

int
nc_tls_get_num_certs_wrap(void *chain)
{
    mbedtls_x509_crt *iter;
    int n = 0;

    /* chain is a linked list */
    iter = chain;
    while (iter) {
        ++n;
        iter = iter->next;
    }

    return n;
}

void
nc_tls_get_cert_wrap(void *chain, int idx, void **cert)
{
    int i;
    mbedtls_x509_crt *iter;

    iter = chain;
    for (i = 0; i < idx; i++) {
        iter = iter->next;
    }

    *cert = iter;
}

int
nc_server_tls_certs_match_wrap(void *cert1, void *cert2)
{
    mbedtls_x509_crt *c1 = cert1;
    mbedtls_x509_crt *c2 = cert2;

    if (!c1 || !c2) {
        return 0;
    }

    /* compare raw DER encoded data */
    if (!c1->raw.p || !c2->raw.p || (c1->raw.len != c2->raw.len) ||
            memcmp(c1->raw.p, c2->raw.p, c1->raw.len)) {
        return 0;
    }

    return 1;
}

int
nc_server_tls_md5_wrap(void *cert, unsigned char *buf)
{
    int rc;
    mbedtls_x509_crt *c = cert;

    rc = mbedtls_md5(c->raw.p, c->raw.len, buf);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Calculating MD5 digest failed");
        return 1;
    }

    return 0;
}

int
nc_server_tls_sha1_wrap(void *cert, unsigned char *buf)
{
    int rc;
    mbedtls_x509_crt *c = cert;

    rc = mbedtls_sha1(c->raw.p, c->raw.len, buf);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Calculating SHA-1 digest failed");
        return 1;
    }

    return 0;
}

int
nc_server_tls_sha224_wrap(void *cert, unsigned char *buf)
{
    int rc;
    mbedtls_x509_crt *c = cert;

    rc = mbedtls_sha256(c->raw.p, c->raw.len, buf, 1);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Calculating SHA-224 digest failed");
        return 1;
    }

    return 0;
}

int
nc_server_tls_sha256_wrap(void *cert, unsigned char *buf)
{
    int rc;
    mbedtls_x509_crt *c = cert;

    rc = mbedtls_sha256(c->raw.p, c->raw.len, buf, 0);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Calculating SHA-256 digest failed");
        return 1;
    }

    return 0;
}

int
nc_server_tls_sha384_wrap(void *cert, unsigned char *buf)
{
    int rc;
    mbedtls_x509_crt *c = cert;

    rc = mbedtls_sha512(c->raw.p, c->raw.len, buf, 1);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Calculating SHA-384 digest failed");
        return 1;
    }

    return 0;
}

int
nc_server_tls_sha512_wrap(void *cert, unsigned char *buf)
{
    int rc;
    mbedtls_x509_crt *c = cert;

    rc = mbedtls_sha512(c->raw.p, c->raw.len, buf, 0);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Calculating SHA-512 digest failed");
        return 1;
    }

    return 0;
}

/**
 * @brief Callback for sending data.
 *
 * @param[in] ctx Socket.
 * @param[in] buf Data to send.
 * @param[in] len Length of the data.
 * @return Number of bytes sent or negative value on error.
 */
static int
nc_server_tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    int sock, ret;

    NC_CHECK_ARG_RET(NULL, ctx, MBEDTLS_ERR_NET_INVALID_CONTEXT);

    sock = *(int *)ctx;

    ret = send(sock, buf, len, MSG_NOSIGNAL);
    if (ret < 0) {
        if ((errno == EWOULDBLOCK) || (errno = EAGAIN) || (errno == EINTR)) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        } else if ((errno == EPIPE) || (errno == ECONNRESET)) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        } else {
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
    }

    return ret;
}

/**
 * @brief Callback for receiving data.
 *
 * @param[in] ctx Socket.
 * @param[out] buf Buffer to store the received data.
 * @param[in] len Length of the buffer.
 * @return Number of bytes received or negative value on error.
 */
static int
nc_server_tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    int sock, ret;

    NC_CHECK_ARG_RET(NULL, ctx, MBEDTLS_ERR_NET_INVALID_CONTEXT);

    sock = *(int *)ctx;

    ret = recv(sock, buf, len, 0);
    if (ret < 0) {
        if ((errno == EWOULDBLOCK) || (errno = EAGAIN) || (errno == EINTR)) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        } else if ((errno == EPIPE) || (errno == ECONNRESET)) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        } else {
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
    }

    return ret;
}

void
nc_tls_set_fd_wrap(void *tls_session, int sock, struct nc_tls_ctx *tls_ctx)
{
    /* mbedtls sets a pointer to the sock, which is stored in tls_ctx */
    *tls_ctx->sock = sock;
    mbedtls_ssl_set_bio(tls_session, tls_ctx->sock, nc_server_tls_send, nc_server_tls_recv, NULL);
}

int
nc_server_tls_handshake_step_wrap(void *tls_session)
{
    int rc = 0;

    rc = mbedtls_ssl_handshake(tls_session);
    if (!rc) {
        return 1;
    } else if ((rc == MBEDTLS_ERR_SSL_WANT_READ) || (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
        return 0;
    } else {
        return rc;
    }
}

int
nc_client_tls_handshake_step_wrap(void *tls_session, int sock)
{
    int rc = 0;
    struct pollfd pfd = {sock, 0, 0};

    rc = mbedtls_ssl_handshake(tls_session);
    if (!rc) {
        return 1;
    } else if ((rc == MBEDTLS_ERR_SSL_WANT_READ) || (rc == MBEDTLS_ERR_SSL_WANT_WRITE)) {
        /* check for EPIPE */
        if (poll(&pfd, 1, 0) < 0) {
            return -1;
        } else {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return -1;
            } else {
                return 0;
            }
        }
    } else {
        return rc;
    }
}

void
nc_tls_ctx_destroy_wrap(struct nc_tls_ctx *tls_ctx)
{
    nc_tls_rng_destroy(tls_ctx->ctr_drbg, tls_ctx->entropy);
    nc_tls_cert_destroy_wrap(tls_ctx->cert);
    nc_tls_privkey_destroy_wrap(tls_ctx->pkey);
    nc_tls_cert_store_destroy_wrap(tls_ctx->cert_store);
    nc_tls_crl_store_destroy_wrap(tls_ctx->crl_store);
    free(tls_ctx->sock);
}

void *
nc_tls_import_privkey_file_wrap(const char *privkey_path)
{
    int rc;
    mbedtls_pk_context *pkey;
    mbedtls_ctr_drbg_context *ctr_drbg;
    mbedtls_entropy_context *entropy;

    if (nc_tls_rng_new(&ctr_drbg, &entropy)) {
        return NULL;
    }

    pkey = nc_tls_pkey_new_wrap();
    if (!pkey) {
        nc_tls_rng_destroy(ctr_drbg, entropy);
        return NULL;
    }

    rc = mbedtls_pk_parse_keyfile(pkey, privkey_path, NULL, mbedtls_ctr_drbg_random, ctr_drbg);
    nc_tls_rng_destroy(ctr_drbg, entropy);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Parsing private key from file \"%s\" failed", privkey_path);
        nc_tls_privkey_destroy_wrap(pkey);
        return NULL;
    }
    return pkey;
}

int
nc_client_tls_load_cert_key_wrap(const char *cert_path, const char *key_path, void **cert, void **pkey)
{
    int ret = 0;
    mbedtls_x509_crt *c;
    mbedtls_pk_context *pk;

    NC_CHECK_ARG_RET(NULL, cert_path, key_path, cert, pkey, 1);

    c = nc_tls_cert_new_wrap();
    if (!c) {
        return 1;
    }

    ret = mbedtls_x509_crt_parse_file(c, cert_path);
    if (ret) {
        nc_mbedtls_strerr(NULL, ret, "Parsing certificate from file \"%s\" failed", cert_path);
        goto cleanup;
    }

    pk = nc_tls_import_privkey_file_wrap(key_path);
    if (!pk) {
        ret = 1;
        goto cleanup;
    }

    *cert = c;
    c = NULL;
    *pkey = pk;

cleanup:
    nc_tls_cert_destroy_wrap(c);
    return ret;
}

int
nc_client_tls_load_trusted_certs_wrap(void *cert_store, const char *file_path, const char *dir_path)
{
    int rc;

    if (file_path && ((rc = mbedtls_x509_crt_parse_file(cert_store, file_path)) < 0)) {
        nc_mbedtls_strerr(NULL, rc, "Loading CA certificate from file \"%s\" failed", file_path);
        return 1;
    }

    if (dir_path && ((rc = mbedtls_x509_crt_parse_path(cert_store, dir_path)) < 0)) {
        nc_mbedtls_strerr(NULL, rc, "Loading CA certificate from directory \"%s\" failed", dir_path);
        return 1;
    }

    return 0;
}

int
nc_client_tls_set_hostname_wrap(void *tls_session, const char *hostname)
{
    int rc;

    rc = mbedtls_ssl_set_hostname(tls_session, hostname);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Setting hostname failed");
        return 1;
    }

    return 0;
}

int
nc_tls_init_ctx_wrap(void *cert, void *pkey, void *cert_store, void *crl_store, struct nc_tls_ctx *tls_ctx)
{
    /* setup rng */
    if (nc_tls_rng_new(&tls_ctx->ctr_drbg, &tls_ctx->entropy)) {
        return 1;
    }

    /* fill the context */
    tls_ctx->sock = malloc(sizeof *tls_ctx->sock);
    NC_CHECK_ERRMEM_RET(!tls_ctx->sock, 1);
    tls_ctx->cert = cert;
    tls_ctx->pkey = pkey;
    tls_ctx->cert_store = cert_store;
    tls_ctx->crl_store = crl_store;
    return 0;
}

int
nc_tls_setup_config_from_ctx_wrap(struct nc_tls_ctx *tls_ctx, int side, void *tls_cfg)
{
    int rc;

    /* set default config data */
    if (side == NC_SERVER) {
        rc = mbedtls_ssl_config_defaults(tls_cfg, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    } else {
        rc = mbedtls_ssl_config_defaults(tls_cfg, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Setting default TLS config failed");
        return 1;
    }

    /* set config's rng */
    mbedtls_ssl_conf_rng(tls_cfg, mbedtls_ctr_drbg_random, tls_ctx->ctr_drbg);
    /* set config's cert and key */
    mbedtls_ssl_conf_own_cert(tls_cfg, tls_ctx->cert, tls_ctx->pkey);
    /* set config's CA and CRL cert store */
    mbedtls_ssl_conf_ca_chain(tls_cfg, tls_ctx->cert_store, tls_ctx->crl_store);
    return 0;
}

uint32_t
nc_tls_get_verify_result_wrap(void *tls_session)
{
    return mbedtls_ssl_get_verify_result(tls_session);
}

char *
nc_tls_verify_error_string_wrap(uint32_t err_code)
{
    return nc_tls_get_verify_err_str(err_code);
}

void
nc_client_tls_print_connect_err_wrap(int connect_ret, const char *peername, void *UNUSED(tls_session))
{
    if (peername) {
        nc_mbedtls_strerr(NULL, connect_ret, "TLS connect to host \"%s\" failed", peername);
    } else {
        nc_mbedtls_strerr(NULL, connect_ret, "TLS connect to an unknown host failed");
    }
}

void
nc_server_tls_print_accept_err_wrap(int accept_ret, void *UNUSED(tls_session))
{
    nc_mbedtls_strerr(NULL, accept_ret, "TLS accept failed");
}

int
nc_tls_is_der_subpubkey_wrap(unsigned char *der, long len)
{
    int ret;
    mbedtls_pk_context *pkey;

    pkey = nc_tls_pkey_new_wrap();
    if (!pkey) {
        return -1;
    }

    ret = mbedtls_pk_parse_subpubkey(&der, der + len, pkey);
    nc_tls_privkey_destroy_wrap(pkey);

    return !ret;
}

int
nc_base64_decode_wrap(const char *base64, unsigned char **bin)
{
    size_t size;
    int rc;

    /* get the size of the decoded data */
    rc = mbedtls_base64_decode(NULL, 0, &size, (const unsigned char *)base64, strlen(base64));
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        nc_mbedtls_strerr(NULL, rc, "Base64 decoding failed");
        return -1;
    }

    *bin = malloc(size);
    NC_CHECK_ERRMEM_RET(!*bin, -1);

    /* decode */
    rc = mbedtls_base64_decode(*bin, size, &size, (const unsigned char *)base64, strlen(base64));
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Base64 decoding failed");
        free(*bin);
        *bin = NULL;
        return -1;
    }

    return size;
}

int
nc_base64_encode_wrap(const unsigned char *bin, size_t len, char **base64)
{
    size_t size;
    int rc;

    rc = mbedtls_base64_encode(NULL, 0, &size, bin, len);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        nc_mbedtls_strerr(NULL, rc, "Base64 encoding failed");
        return -1;
    }

    *base64 = malloc(size);
    NC_CHECK_ERRMEM_RET(!*base64, -1);

    rc = mbedtls_base64_encode((unsigned char *)*base64, size, &size, bin, len);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Base64 encoding failed");
        free(*base64);
        *base64 = NULL;
        return -1;
    }

    return 0;
}

int
nc_tls_read_wrap(struct nc_session *session, unsigned char *buf, size_t size)
{
    int rc;
    mbedtls_ssl_context *tls_session = session->ti.tls.session;

    rc = mbedtls_ssl_read(tls_session, buf, size);
    if (rc <= 0) {
        switch (rc) {
        case MBEDTLS_ERR_SSL_WANT_READ:
        case MBEDTLS_ERR_SSL_WANT_WRITE:
            rc = 0;
            break;
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
            nc_mbedtls_strerr(session, rc, "Communication socket unexpectedly closed");
            session->status = NC_STATUS_INVALID;
            session->term_reason = NC_SESSION_TERM_DROPPED;
            rc = -1;
            break;
        default:
            nc_mbedtls_strerr(session, rc, "TLS communication error occurred");
            session->status = NC_STATUS_INVALID;
            session->term_reason = NC_SESSION_TERM_OTHER;
            rc = -1;
            break;
        }
    }

    return rc;
}

int
nc_tls_write_wrap(struct nc_session *session, const unsigned char *buf, size_t size)
{
    int rc = 0;
    mbedtls_ssl_context *tls_session = session->ti.tls.session;

    rc = mbedtls_ssl_write(tls_session, buf, size);
    if (rc < 0) {
        switch (rc) {
        case MBEDTLS_ERR_SSL_WANT_READ:
        case MBEDTLS_ERR_SSL_WANT_WRITE:
            rc = 0;
            break;
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
            nc_mbedtls_strerr(session, rc, "TLS connection was properly closed.");
            rc = -1;
            break;
        default:
            nc_mbedtls_strerr(session, rc, "TLS communication error occurred");
            rc = -1;
            break;
        }
    }

    return rc;
}

int
nc_tls_get_num_pending_bytes_wrap(void *tls_session)
{
    return mbedtls_ssl_get_bytes_avail(tls_session);
}

int
nc_tls_get_fd_wrap(const struct nc_session *session)
{
    return session->ti.tls.ctx.sock ? *session->ti.tls.ctx.sock : -1;
}

void
nc_tls_close_notify_wrap(void *tls_session)
{
    int rc;

    while ((rc = mbedtls_ssl_close_notify(tls_session))) {
        if ((rc != MBEDTLS_ERR_SSL_WANT_READ) && (rc != MBEDTLS_ERR_SSL_WANT_WRITE)) {
            /* some error occurred */
            nc_mbedtls_strerr(NULL, rc, "Sending TLS close notify failed");
            return;
        }
    }
}

void *
nc_tls_import_cert_file_wrap(const char *cert_path)
{
    int rc;
    mbedtls_x509_crt *c;

    c = nc_tls_cert_new_wrap();
    if (!c) {
        return NULL;
    }

    rc = mbedtls_x509_crt_parse_file(c, cert_path);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Parsing certificate from file \"%s\" failed", cert_path);
        nc_tls_cert_destroy_wrap(c);
        return NULL;
    }

    return c;
}

char *
nc_tls_export_privkey_pem_wrap(void *pkey)
{
    int rc;
    char *pem;
    size_t size = 128;

    pem = malloc(size);
    NC_CHECK_ERRMEM_RET(!pem, NULL);

    while ((rc = mbedtls_pk_write_key_pem(pkey, (unsigned char *)pem, size)) == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        size <<= 1;
        pem = nc_realloc(pem, size);
        NC_CHECK_ERRMEM_RET(!pem, NULL);
    }
    if (rc < 0) {
        nc_mbedtls_strerr(NULL, rc, "Exporting private key to PEM format failed");
        free(pem);
        return NULL;
    }

    return pem;
}

char *
nc_tls_export_cert_pem_wrap(void *cert)
{
    char *b64 = NULL, *pem = NULL;

    /* encode the certificate */
    if (nc_base64_encode_wrap(((mbedtls_x509_crt *)cert)->raw.p, ((mbedtls_x509_crt *)cert)->raw.len, &b64)) {
        goto cleanup;
    }

    if (asprintf(&pem, "-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n", b64) == -1) {
        ERRMEM;
        pem = NULL;
        goto cleanup;
    }

cleanup:
    free(b64);
    return pem;
}

char *
nc_tls_export_pubkey_pem_wrap(void *pkey)
{
    int rc;
    char *pem;
    size_t size = 128;

    pem = malloc(size);
    NC_CHECK_ERRMEM_RET(!pem, NULL);

    while ((rc = mbedtls_pk_write_pubkey_pem(pkey, (unsigned char *)pem, size)) == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        size <<= 1;
        pem = nc_realloc(pem, size);
        NC_CHECK_ERRMEM_RET(!pem, NULL);
    }
    if (rc < 0) {
        nc_mbedtls_strerr(NULL, rc, "Exporting public key to PEM format failed");
        free(pem);
        return NULL;
    }

    return pem;
}

int
nc_tls_privkey_is_rsa_wrap(void *pkey)
{
    return mbedtls_pk_get_type(pkey) == MBEDTLS_PK_RSA;
}

int
nc_tls_get_rsa_pubkey_params_wrap(void *pkey, void **e, void **n)
{
    int rc;
    mbedtls_mpi *exp = NULL, *mod = NULL;

    exp = malloc(sizeof *exp);
    mod = malloc(sizeof *mod);
    if (!exp || !mod) {
        ERRMEM;
        goto fail;
    }
    mbedtls_mpi_init(exp);
    mbedtls_mpi_init(mod);

    if ((rc = mbedtls_rsa_export(mbedtls_pk_rsa(*(mbedtls_pk_context *)pkey), mod, NULL, NULL, NULL, exp))) {
        nc_mbedtls_strerr(NULL, rc, "Failed to export RSA public key parameters");
        goto fail;
    }

    *e = exp;
    *n = mod;
    return 0;

fail:
    mbedtls_mpi_free(exp);
    mbedtls_mpi_free(mod);
    free(exp);
    free(mod);
    return 1;
}

void
nc_tls_destroy_mpi_wrap(void *mpi)
{
    mbedtls_mpi_free(mpi);
    free(mpi);
}

int
nc_tls_privkey_is_ec_wrap(void *pkey)
{
    return mbedtls_pk_get_type(pkey) == MBEDTLS_PK_ECKEY;
}

char *
nc_tls_get_ec_group_wrap(void *pkey)
{
    const mbedtls_ecp_curve_info *curve_info;
    mbedtls_ecp_group_id group_id;
    mbedtls_ecp_keypair *ec;

    /* get the group ID from the EC key */
    ec = mbedtls_pk_ec(*(mbedtls_pk_context *)pkey);
    group_id = ec->private_grp.id;

    /* get the group name based on the id */
    curve_info = mbedtls_ecp_curve_info_from_grp_id(group_id);
    return strdup(curve_info->name);
}

int
nc_tls_get_ec_pubkey_params_wrap(void *pkey, void **q, void **q_grp)
{
    int ret;
    mbedtls_ecp_group *grp = NULL;
    mbedtls_ecp_point *p = NULL;
    mbedtls_mpi *d = NULL;

    /* init group, mpi and point */
    grp = malloc(sizeof *grp);
    d = malloc(sizeof *d);
    p = malloc(sizeof *p);
    if (!grp || !p || !d) {
        ERRMEM;
        ret = 1;
        goto cleanup;
    }
    mbedtls_ecp_group_init(grp);
    mbedtls_mpi_init(d);
    mbedtls_ecp_point_init(p);

    /* get the group and public key */
    ret = mbedtls_ecp_export(mbedtls_pk_ec(*(mbedtls_pk_context *)pkey), grp, d, p);
    if (ret) {
        nc_mbedtls_strerr(NULL, ret, "Failed to export EC private key parameters");
        ret = 1;
        goto cleanup;
    }

    *q_grp = grp;
    grp = NULL;
    *q = p;
    p = NULL;

cleanup:
    mbedtls_ecp_group_free(grp);
    free(grp);
    mbedtls_mpi_free(d);
    free(d);
    mbedtls_ecp_point_free(p);
    free(p);
    return ret;
}

int
nc_tls_ec_point_to_bin_wrap(void *q, void *q_grp, unsigned char **bin, int *bin_len)
{
    int rc;
    unsigned char *buf;
    size_t buf_len = 32, out_len;

    buf = malloc(buf_len);
    NC_CHECK_ERRMEM_RET(!buf, 1);

    while ((rc = (mbedtls_ecp_point_write_binary(q_grp, q, MBEDTLS_ECP_PF_COMPRESSED, &out_len, buf, buf_len)))) {
        if (rc != MBEDTLS_ERR_ECP_BUFFER_TOO_SMALL) {
            break;
        }
        buf_len <<= 1;
        buf = nc_realloc(buf, buf_len);
        NC_CHECK_ERRMEM_RET(!buf, 1);
    }
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Failed to write EC public key binary");
        free(buf);
        return 1;
    }

    *bin = buf;
    *bin_len = out_len;
    return 0;
}

void
nc_tls_ec_point_destroy_wrap(void *p)
{
    mbedtls_ecp_point_free(p);
    free(p);
}

void
nc_tls_ec_group_destroy_wrap(void *grp)
{
    mbedtls_ecp_group_free(grp);
    free(grp);
}

int
nc_tls_mpi2bin_wrap(void *mpi, unsigned char **bin, int *bin_len)
{
    int rc;
    unsigned char *buf;
    int buf_len;

    buf_len = mbedtls_mpi_size(mpi);
    buf = malloc(buf_len);
    NC_CHECK_ERRMEM_RET(!buf, 1);

    rc = mbedtls_mpi_write_binary(mpi, buf, buf_len);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Failed to convert MPI to binary");
        free(buf);
        return 1;
    }

    *bin = buf;
    *bin_len = buf_len;
    return 0;
}

void *
nc_tls_import_pubkey_file_wrap(const char *pubkey_path)
{
    int rc = 0;
    mbedtls_pk_context *pk = NULL;

    pk = nc_tls_pkey_new_wrap();
    if (!pk) {
        return NULL;
    }

    rc = mbedtls_pk_parse_public_keyfile(pk, pubkey_path);
    if (rc) {
        nc_mbedtls_strerr(NULL, rc, "Parsing public key from file \"%s\" failed", pubkey_path);
        nc_tls_privkey_destroy_wrap(pk);
        return NULL;
    }

    return pk;
}

/**
 * @brief Parse the CRL distribution points X509v3 extension and obtain the URIs.
 *
 * @param[in,out] p Pointer to the DER encoded extension. When the function gets called, this should
 * point to the first byte in the value of CRLDistributionPoints.
 * @param[in] len Length of the CRLDistributionPoints ASN.1 encoded value.
 * @param[out] uris Array of URIs found in the extension.
 * @param[out] uri_count Number of URIs found in the extension.
 * @return 0 on success, non-zero on error.
 */
static int
nc_server_tls_parse_crl_dist_points(unsigned char **p, size_t len, char ***uris, int *uri_count)
{
    char **tmp_uris, *uri;
    int ret = 0;
    size_t name_len;
    unsigned char *end_crl_dist_points, *end_general_names, *end_names, tag, tag_class, tag_value;

    /*
     * parsing the value of CRLDistributionPoints
     *
     * CRLDistributionPoints ::= SEQUENCE SIZE (1..MAX) OF DistributionPoint
     */
    end_crl_dist_points = *p + len;
    while (*p < end_crl_dist_points) {
        /*
         * DistributionPoint ::= SEQUENCE {
         *      distributionPoint       [0]     DistributionPointName OPTIONAL,
         *      reasons                 [1]     ReasonFlags OPTIONAL,
         *      cRLIssuer               [2]     GeneralNames OPTIONAL }
         */
        ret = mbedtls_asn1_get_tag(p, end_crl_dist_points, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
        if (ret) {
            nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
            goto cleanup;
        }
        if (!len) {
            /* empty sequence */
            continue;
        }

        /* parse distributionPoint */
        ret = mbedtls_asn1_get_tag(p, end_crl_dist_points, &len, MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 0);
        if (!ret) {
            /*
             * DistributionPointName ::= CHOICE {
             *      fullName                [0]     GeneralNames,
             *      nameRelativeToCRLIssuer [1]     RelativeDistinguishedName }
             */
            ret = mbedtls_asn1_get_tag(p, end_crl_dist_points, &len, MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 0);
            if (ret) {
                if ((ret == MBEDTLS_ERR_ASN1_UNEXPECTED_TAG) && (**p == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED | 1))) {
                    /* it's nameRelativeToCRLIssuer, but we don't support it */
                    ERR(NULL, "Failed to parse CRL distribution points extension (nameRelativeToCRLIssuer not yet supported).");
                    goto cleanup;
                } else {
                    nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
                    goto cleanup;
                }
            }

            /* GeneralNames ::= SEQUENCE SIZE (1..MAX) OF GeneralName */
            end_general_names = *p + len;
            ret = mbedtls_asn1_get_tag(p, end_general_names, &name_len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
            if (ret) {
                nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
                goto cleanup;
            }

            end_names = *p + name_len;
            while (*p < end_names) {
                tag = **p;
                tag_class = tag & MBEDTLS_ASN1_TAG_CLASS_MASK;
                tag_value = tag & MBEDTLS_ASN1_TAG_VALUE_MASK;
                /*
                 * read the GeneralName tag and length
                 *
                 * GeneralName ::= CHOICE {
                 *      otherName                 [0]  AnotherName,
                 *      rfc822Name                [1]  IA5String,
                 *      dNSName                   [2]  IA5String,
                 *      x400Address               [3]  ORAddress,
                 *      directoryName             [4]  Name,
                 *      ediPartyName              [5]  EDIPartyName,
                 *      uniformResourceIdentifier [6]  IA5String,
                 *      iPAddress                 [7]  OCTET STRING,
                 *      registeredID              [8]  OBJECT IDENTIFIER }
                 */
                ret = mbedtls_asn1_get_tag(p, end_names, &name_len, MBEDTLS_ASN1_CONTEXT_SPECIFIC | (tag & MBEDTLS_ASN1_CONSTRUCTED) | tag_value);
                if (ret) {
                    nc_mbedtls_strerr(NULL, ret, "Failed to parse GeneralName in CRL distribution points extension");
                    goto cleanup;
                }

                /* uniformResourceIdentifier [6] IA5String */
                if ((tag_class == MBEDTLS_ASN1_CONTEXT_SPECIFIC) &&
                        (tag_value == MBEDTLS_X509_SAN_UNIFORM_RESOURCE_IDENTIFIER)) {
                    uri = strndup((char *)*p, name_len);
                    if (!uri) {
                        ERRMEM;
                        ret = 1;
                        goto cleanup;
                    }

                    tmp_uris = realloc(*uris, (*uri_count + 1) * sizeof **uris);
                    if (!tmp_uris) {
                        ERRMEM;
                        free(uri);
                        ret = 1;
                        goto cleanup;
                    }
                    *uris = tmp_uris;

                    (*uris)[*uri_count] = uri;
                    ++(*uri_count);
                }

                /* Move to the next GeneralName */
                *p += name_len;
            }

        } else if (ret != MBEDTLS_ERR_ASN1_UNEXPECTED_TAG) {
            /* failed to parse it, but not because it's optional */
            nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
            goto cleanup;
        }
    }

cleanup:
    return ret;
}

int
nc_server_tls_get_crl_distpoint_uris_wrap(void *leaf_cert, void *cert_store, char ***uris, int *uri_count)
{
    int ret = 0, is_critical = 0, cert_count, i;
    mbedtls_x509_crt *cert;
    unsigned char *p, *end_v3_ext, *end_ext, *end_ext_octet;
    size_t len;
    mbedtls_x509_buf ext_oid = {0};

    NC_CHECK_ARG_RET(NULL, cert_store, uris, uri_count, 1);

    *uris = NULL;
    *uri_count = 0;

    /* get the number of certs in the store */
    cert = cert_store;
    cert_count = 0;
    while (cert) {
        ++cert_count;
        cert = cert->next;
    }

    /* iterate over all the certs */
    for (i = -1; i < cert_count; i++) {
        if (i == -1) {
            cert = leaf_cert;
        } else if (i == 0) {
            cert = cert_store;
        } else {
            cert = cert->next;
        }

        if (!cert->v3_ext.len) {
            /* no extensions, skip this cert */
            continue;
        }

        /* go over all the extensions and try to find the CRL distribution points */
        p = cert->v3_ext.p;
        end_v3_ext = p + cert->v3_ext.len;

        /*
         * Extensions  ::=  SEQUENCE SIZE (1..MAX) OF Extension
         */
        ret = mbedtls_asn1_get_tag(&p, end_v3_ext, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
        if (ret) {
            nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
            goto cleanup;
        }

        while (p < end_v3_ext) {
            /*
             * Extension  ::=  SEQUENCE  {
             *      extnID      OBJECT IDENTIFIER,
             *      critical    BOOLEAN DEFAULT FALSE,
             *      extnValue   OCTET STRING  }
             */
            ret = mbedtls_asn1_get_tag(&p, end_v3_ext, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
            if (ret) {
                nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
                goto cleanup;
            }

            end_ext = p + len;

            /* parse extnID */
            ret = mbedtls_asn1_get_tag(&p, end_ext, &ext_oid.len, MBEDTLS_ASN1_OID);
            if (ret) {
                nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
                goto cleanup;
            }
            ext_oid.tag = MBEDTLS_ASN1_OID;
            ext_oid.p = p;

            if (memcmp(ext_oid.p, MBEDTLS_OID_CRL_DISTRIBUTION_POINTS, ext_oid.len)) {
                /* not the extension we are looking for */
                p = end_ext;
                continue;
            }

            p += ext_oid.len;

            /* parse optional critical */
            ret = mbedtls_asn1_get_bool(&p, end_ext, &is_critical);
            if (ret && (ret != MBEDTLS_ERR_ASN1_UNEXPECTED_TAG)) {
                nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
                goto cleanup;
            }

            /* parse extnValue */
            ret = mbedtls_asn1_get_tag(&p, end_ext, &len, MBEDTLS_ASN1_OCTET_STRING);
            if (ret) {
                nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
                goto cleanup;
            }

            end_ext_octet = p + len;

            /*
             * parse extnValue, that is CRLDistributionPoints
             *
             * CRLDistributionPoints ::= SEQUENCE SIZE (1..MAX) OF DistributionPoint
             */
            ret = mbedtls_asn1_get_tag(&p, end_ext_octet, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
            if (ret) {
                nc_mbedtls_strerr(NULL, ret, "Failed to parse CRL distribution points extension");
                goto cleanup;
            }
            if (p + len != end_ext_octet) {
                /* length mismatch */
                ERR(NULL, "Failed to parse CRL distribution points extension (length mismatch).");
                goto cleanup;
            } else if (!len) {
                /* empty sequence, but size is 1..max */
                ERR(NULL, "Failed to parse CRL distribution points extension (empty sequence).");
                goto cleanup;
            }

            /* parse the distribution points and obtain the uris */
            ret = nc_server_tls_parse_crl_dist_points(&p, len, uris, uri_count);
            if (ret) {
                goto cleanup;
            }
        }
    }

cleanup:
    return ret;
}

int
nc_tls_process_cipher_suite_wrap(const char *cipher, char **out)
{
    const char *begin, *ptr;

    /* check if it's a TLS 1.3 cipher suite */
    if (!strcmp(cipher, "tls-aes-256-gcm-sha384") || !strcmp(cipher, "tls-aes-128-gcm-sha256") ||
            !strcmp(cipher, "tls-chacha20-poly1305-sha256") || !strcmp(cipher, "tls-aes-128-ccm-sha256") ||
            !strcmp(cipher, "tls-aes-128-ccm-8-sha256")) {
        /* + 3 because mbedtls has "TLS1-3" prefix for 1.3 suites */
        *out = malloc(strlen(cipher) + 3 + 1);
        NC_CHECK_ERRMEM_RET(!*out, 1);
        sprintf(*out, "TLS1-3");
        begin = cipher + 4;
    } else {
        *out = malloc(strlen(cipher) + 1);
        NC_CHECK_ERRMEM_RET(!*out, 1);
        begin = cipher;
    }

    /* convert to uppercase */
    for (ptr = begin; *ptr; ptr++) {
        (*out)[ptr - begin] = toupper(*ptr);
    }

    (*out)[ptr - begin] = '\0';
    return 0;
}

int
nc_tls_append_cipher_suite_wrap(struct nc_server_tls_opts *opts, const char *cipher_suite)
{
    int cipher_id;

    cipher_id = mbedtls_ssl_get_ciphersuite_id(cipher_suite);
    if (!cipher_id) {
        return 1;
    }

    /* append the cipher suite to a zero terminated array */
    if (!opts->ciphers) {
        /* first entry, account for terminating 0 */
        opts->ciphers = malloc(2 * sizeof *opts->ciphers);
        NC_CHECK_ERRMEM_RET(!opts->ciphers, 1);
        ((int *)opts->ciphers)[0] = cipher_id;
        opts->cipher_count = 1;
    } else {
        /* +2 because of terminating 0 */
        opts->ciphers = nc_realloc(opts->ciphers, (opts->cipher_count + 2) * sizeof *opts->ciphers);
        NC_CHECK_ERRMEM_RET(!opts->ciphers, 1);
        ((int *)opts->ciphers)[opts->cipher_count] = cipher_id;
        opts->cipher_count++;
    }

    /* terminate the array */
    ((int *)opts->ciphers)[opts->cipher_count] = 0;
    return 0;
}

void
nc_server_tls_set_cipher_suites_wrap(void *tls_cfg, void *cipher_suites)
{
    mbedtls_ssl_conf_ciphersuites(tls_cfg, cipher_suites);
}

time_t
nc_tls_get_cert_exp_time_wrap(void *cert)
{
    struct tm t = {0};
    mbedtls_x509_time *valid_to;

    valid_to = &((mbedtls_x509_crt *)cert)->valid_to;

    t.tm_sec = valid_to->sec;
    t.tm_min = valid_to->min;
    t.tm_hour = valid_to->hour;

    t.tm_mday = valid_to->day;
    t.tm_mon = valid_to->mon - 1;
    t.tm_year = valid_to->year - 1900;

    /* let system figure out the DST */
    t.tm_isdst = -1;

    return timegm(&t);
}

/**
 * @brief Convert the MbedTLS key export type to a label for the keylog file.
 *
 * @param[in] type MbedTLS key export type.
 * @return Label for the keylog file or NULL if the type is not supported.
 */
static const char *
nc_tls_keylog_type2label(mbedtls_ssl_key_export_type type)
{
    switch (type) {
    case MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET:
        return "CLIENT_RANDOM";
#ifdef MBEDTLS_SSL_PROTO_TLS1_3
    case MBEDTLS_SSL_KEY_EXPORT_TLS1_3_CLIENT_HANDSHAKE_TRAFFIC_SECRET:
        return "CLIENT_HANDSHAKE_TRAFFIC_SECRET";
    case MBEDTLS_SSL_KEY_EXPORT_TLS1_3_SERVER_HANDSHAKE_TRAFFIC_SECRET:
        return "SERVER_HANDSHAKE_TRAFFIC_SECRET";
    case MBEDTLS_SSL_KEY_EXPORT_TLS1_3_CLIENT_APPLICATION_TRAFFIC_SECRET:
        return "CLIENT_TRAFFIC_SECRET_0";
    case MBEDTLS_SSL_KEY_EXPORT_TLS1_3_SERVER_APPLICATION_TRAFFIC_SECRET:
        return "SERVER_TRAFFIC_SECRET_0";
#endif
    default:
        return NULL;
    }
}

/**
 * @brief Callback for writing a line in the keylog file.
 */
static void
nc_tls_keylog_write_line(void *UNUSED(p_expkey), mbedtls_ssl_key_export_type type, const unsigned char *secret,
        size_t secret_len, const unsigned char client_random[32],
        const unsigned char UNUSED(server_random[32]), mbedtls_tls_prf_types UNUSED(tls_prf_type))
{
    size_t linelen, len = 0, i, client_random_len;
    char buf[256];
    const char *label;

    if (!server_opts.tls_keylog_file) {
        return;
    }

    label = nc_tls_keylog_type2label(type);
    if (!label) {
        /* type not supported */
        return;
    }

    /* <Label> <space> 0x<ClientRandom> <space> 0x<Secret> */
    linelen = strlen(label) + 1 + 2 * 32 + 1 + 2 * secret_len + 1;
    if (linelen > sizeof(buf)) {
        /* sanity check, should not happen since the max len should be 196 bytes */
        return;
    }

    /* write the label */
    len += sprintf(buf + len, "%s ", label);

    /* write the client random */
    client_random_len = 32;
    for (i = 0; i < client_random_len; i++) {
        len += sprintf(buf + len, "%02x", client_random[i]);
    }
    len += sprintf(buf + len, " ");

    /* write the secret */
    for (i = 0; i < secret_len; i++) {
        len += sprintf(buf + len, "%02x", secret[i]);
    }

    len += sprintf(buf + len, "\n");
    buf[len] = '\0';

    if (len != linelen) {
        return;
    }

    fputs(buf, server_opts.tls_keylog_file);
    fflush(server_opts.tls_keylog_file);
}

void
nc_tls_keylog_session_wrap(void *session)
{
    mbedtls_ssl_set_export_keys_cb(session, nc_tls_keylog_write_line, NULL);
}
