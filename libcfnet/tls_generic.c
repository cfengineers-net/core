/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/


#include <cfnet.h>

#include <logging.h>                                            /* LogLevel */
#include <misc_lib.h>

/* TODO move crypto.h to libutils */
#include <crypto.h>                                    /* HavePublicKeyByIP */
#include <files_hashes.h>                              /* HashPubKey */

#include <assert.h>


/**
 * this is an always succeeding callback for SSL_CTX_set_cert_verify_callback().
 *
 * Verifying with a callback is the best way, *but* OpenSSL does not provide a
 * thread-safe way to passing a pointer with custom data (connection info). So
 * we always succeed verification here, and verify properly *after* the
 * handshake is complete.
 */
int TLSVerifyCallback(X509_STORE_CTX *ctx ARG_UNUSED,
                      void *arg ARG_UNUSED)
{
    return 1;
}

int TLSVerifyPeer(ConnectionInfo *conn_info, const char *remoteip, const char *username)
{
    int ret, retval;

    X509 *received_cert = SSL_get_peer_certificate(ConnectionInfoSSL(conn_info));
    if (received_cert == NULL)
    {
        Log(LOG_LEVEL_ERR,
            "No certificate presented by remote peer (openssl: %s)",
            ERR_reason_error_string(ERR_get_error()));
        retval = -1;
        goto ret1;
    }

    EVP_PKEY *received_pubkey = X509_get_pubkey(received_cert);
    if (received_pubkey == NULL)
    {
        Log(LOG_LEVEL_ERR, "X509_get_pubkey: %s",
            ERR_reason_error_string(ERR_get_error()));
        retval = -1;
        goto ret2;
    }
    if (EVP_PKEY_type(received_pubkey->type) != EVP_PKEY_RSA)
    {
        Log(LOG_LEVEL_ERR,
            "Received key of unknown type, only RSA currently supported!");
        retval = -1;
        goto ret3;
    }

    RSA *remote_key = EVP_PKEY_get1_RSA(received_pubkey);
    if (remote_key == NULL)
    {
        Log(LOG_LEVEL_ERR, "TLSVerifyPeer: EVP_PKEY_get1_RSA failed!");
        retval = -1;
        goto ret3;
    }

    Key *key = KeyNew(remote_key, CF_DEFAULT_DIGEST);
    ConnectionInfoSetKey(conn_info, key);

    /*
     * Compare the key received with the one stored.
     */
    RSA *expected_rsa_key = HavePublicKey(username, remoteip,
                                          KeyPrintableHash(key));
    if (expected_rsa_key == NULL)
    {
        Log(LOG_LEVEL_VERBOSE,
            "Public key for remote host not found in ppkeys");
        retval = 0;
        goto ret4;
    }

    EVP_PKEY *expected_pubkey = EVP_PKEY_new();
    if (expected_pubkey == NULL)
    {
        Log(LOG_LEVEL_ERR, "TLSVerifyPeer: EVP_PKEY_new allocation failed!");
        retval = -1;
        goto ret5;
    }

    ret = EVP_PKEY_set1_RSA(expected_pubkey, expected_rsa_key);
    if (ret == 0)
    {
        Log(LOG_LEVEL_ERR, "TLSVerifyPeer: EVP_PKEY_set1_RSA failed!");
        retval = -1;
        goto ret6;
    }

    ret = EVP_PKEY_cmp(received_pubkey, expected_pubkey);
    if (ret == 1)
    {
        Log(LOG_LEVEL_VERBOSE,
            "Received public key compares equal to the one we have stored");
        retval = 1;               /* TRUSTED KEY, equal to the expected one */
        goto ret6;
    }
    else if (ret == 0 || ret == -1)
    {
        Log(LOG_LEVEL_VERBOSE,
            "Public key for remote host compares different to the one in ppkeys");
        retval = 0;
        goto ret6;
    }
    else
    {
        const char *errmsg = ERR_reason_error_string(ERR_get_error());
        Log(LOG_LEVEL_ERR, "OpenSSL EVP_PKEY_cmp: %d %s",
            ret, errmsg ? errmsg : "");
        retval = -1;
        goto ret6;
    }

    UnexpectedError("Unreachable!");
    return 0;

  ret6:
    EVP_PKEY_free(expected_pubkey);
  ret5:
    RSA_free(expected_rsa_key);
  ret4:
    if (retval == -1)
    {
        /* We won't be needing conn_info->key */
        KeyDestroy(&key);
        ConnectionInfoSetKey(conn_info, NULL);
    }
  ret3:
    EVP_PKEY_free(received_pubkey);
  ret2:
    X509_free(received_cert);
  ret1:
    return retval;
}

/* Generate in-memory self-signed cert valid from now to 50 years later. */
X509 *TLSGenerateCertFromPrivKey(RSA *privkey)
{
    int ret;
    X509 *x509 = X509_new();
    if (x509 == NULL)
    {
        Log(LOG_LEVEL_ERR, "X509_new: %s",
            ERR_reason_error_string(ERR_get_error()));
        goto err1;
    }

    ASN1_TIME *t1 = X509_gmtime_adj(X509_get_notBefore(x509), 0);
    ASN1_TIME *t2 = X509_gmtime_adj(X509_get_notAfter(x509), 60*60*24*365*50);
    if (t1 == NULL || t2 == NULL)
    {
        Log(LOG_LEVEL_ERR, "X509_gmtime_adj: %s",
            ERR_reason_error_string(ERR_get_error()));
        goto err2;
    }

    EVP_PKEY *pkey = EVP_PKEY_new();
    if (pkey == NULL)
    {
        Log(LOG_LEVEL_ERR, "EVP_PKEY_new: %s",
            ERR_reason_error_string(ERR_get_error()));
        goto err2;
    }

    ret = EVP_PKEY_set1_RSA(pkey, privkey);
    if (ret != 1)
    {
        Log(LOG_LEVEL_ERR, "EVP_PKEY_set1_RSA: %s",
            ERR_reason_error_string(ERR_get_error()));
        goto err3;
    }

    X509_NAME *name = X509_get_subject_name(x509);
    if (name == NULL)
    {
        Log(LOG_LEVEL_ERR, "X509_get_subject_name: %s",
            ERR_reason_error_string(ERR_get_error()));
        goto err3;
    }

    ret = 0;
    ret += X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                      (const char *) "a",
                                      -1, -1, 0);
    ret += X509_set_issuer_name(x509, name);
    ret += X509_set_pubkey(x509, pkey);
    if (ret < 3)
    {
        Log(LOG_LEVEL_ERR, "Failed to set certificate details: %s",
            ERR_reason_error_string(ERR_get_error()));
        goto err3;
    }

    const EVP_MD *md = EVP_get_digestbyname("sha384");
    if (md == NULL)
    {
        Log(LOG_LEVEL_ERR, "OpenSSL: Uknown digest algorithm %s",
            "sha384");
        goto err3;
    }

    /* Not really needed since the other side does not
       verify the signature. */
    ret = X509_sign(x509, pkey, md);
    /* X509_sign obscurely returns the length of the signature... */
    if (ret == 0)
    {
        Log(LOG_LEVEL_ERR, "X509_sign: %s",
            ERR_reason_error_string(ERR_get_error()));
        goto err3;
    }

    EVP_PKEY_free(pkey);

    assert (x509 != NULL);
    return x509;


  err3:
    EVP_PKEY_free(pkey);
  err2:
    X509_free(x509);
  err1:
    return NULL;
}

static const char *TLSPrimarySSLError(int code)
{
    switch (code)
    {
    case SSL_ERROR_NONE:
        return "TLSGetSSLErrorString: No SSL error!";
    case SSL_ERROR_ZERO_RETURN:
        return "TLS session has been terminated (SSL_ERROR_ZERO_RETURN)";
    case SSL_ERROR_WANT_READ:
        return "SSL_ERROR_WANT_READ";
    case SSL_ERROR_WANT_WRITE:
        return "SSL_ERROR_WANT_WRITE";
    case SSL_ERROR_WANT_CONNECT:
        return "SSL_ERROR_WANT_CONNECT";
    case SSL_ERROR_WANT_ACCEPT:
        return "SSL_ERROR_WANT_ACCEPT";
    case SSL_ERROR_WANT_X509_LOOKUP:
        return "SSL_ERROR_WANT_X509_LOOKUP";
    case SSL_ERROR_SYSCALL:
        return "SSL_ERROR_SYSCALL";
    case SSL_ERROR_SSL:
        return "SSL_ERROR_SSL";
    }
    return "Unknown OpenSSL error code!";
}

/* TODO ERR_get_error is only meaningful for some error codes, so check
 * and return empty string otherwise. */
static const char *TLSSecondarySSLError(int code ARG_UNUSED)
{
    return ERR_reason_error_string(ERR_get_error());
}

void TLSLogError(SSL *ssl, LogLevel level, const char *prepend, int code)
{
    assert(prepend != NULL);

    const char *err2 = TLSSecondarySSLError(code);

    Log(level, "%s: (%d %s) %s",
        prepend, code,
        TLSPrimarySSLError(SSL_get_error(ssl, code)),
        err2 == NULL ? "" : err2);
}

int TLSSend(SSL *ssl, const char *buffer, int length)
{
    assert(length >= 0);

    if (length == 0)
    {
        UnexpectedError("TLSSend: Zero length buffer!");
        return 0;
    }

    int sent = SSL_write(ssl, buffer, length);
    if (sent == 0)
    {
        if ((SSL_get_shutdown(ssl) & SSL_RECEIVED_SHUTDOWN) != 0)
        {
            Log(LOG_LEVEL_VERBOSE, "Remote peer terminated TLS session");
            return 0;
        }
        else
        {
            TLSLogError(ssl, LOG_LEVEL_ERR,
                        "TLS session abruptly closed. SSL_write", sent);
            return 0;
        }
    }
    if (sent < 0)
    {
        TLSLogError(ssl, LOG_LEVEL_ERR, "SSL_write", sent);
        return -1;
    }

    return sent;
}

int TLSRecv(SSL *ssl, char *buffer, int length)
{
    assert(length > 0);

    int received = SSL_read(ssl, buffer, length);
    if (received == 0)
    {
        if ((SSL_get_shutdown(ssl) & SSL_RECEIVED_SHUTDOWN) != 0)
        {
            Log(LOG_LEVEL_VERBOSE, "Remote peer terminated TLS session");
            return 0;
        }
        else
        {
            TLSLogError(ssl, LOG_LEVEL_ERR,
                        "TLS session abruptly closed. SSL_read", received);
            return 0;
        }
    }
    if (received < 0)
    {
        TLSLogError(ssl, LOG_LEVEL_ERR, "SSL_read", received);
        return -1;
    }

    return received;
}

/* TODO: replace with a function taking a Buffer ! */
ssize_t TLSRecvLine(SSL *ssl, char *buf, size_t buf_size)
{
    size_t got = 0;
    buf[0] = '\0';
    buf_size -= 1; /* Reserve one byte for terminating '\0' */

    /* Repeat until we receive end of line or run out of space. */
    while (got < buf_size)
    {
        char *tail = buf + got;
        int ret = TLSRecv(ssl, tail, buf_size - got);
        if (ret <= 0)
        {
            Log(LOG_LEVEL_ERR,
                "Connection was hung up while receiving line: %s",
                buf);
            return -1;
        }
        tail[ret] = '\0'; /* Tell strchr where to stop. */
        char *nl = strchr(tail, '\n');
        if (nl)
        {
            got = nl - buf;
            assert(got <= buf_size);
            *nl = '\0'; /* Truncate line at first '\n' */
            return (ssize_t) got;
        }
        got += ret;
    }

    Log(LOG_LEVEL_ERR, "No new line found and the buffer is already full");
    return -1;
}
