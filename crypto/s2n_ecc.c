/*
 * Copyright 2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "crypto/s2n_ecc.h"

#include <openssl/ec.h>
#include <openssl/bn.h>
#include <openssl/ecdh.h>
#include <openssl/obj_mac.h>
#include <stdint.h>

#include "tls/s2n_tls_parameters.h"
#include "tls/s2n_kex.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_mem.h"

#define TLS_EC_CURVE_TYPE_NAMED 3

/* IANA values can be found here: https://tools.ietf.org/html/rfc8446#appendix-B.3.1.4 */
/* Share sizes are described here: https://tools.ietf.org/html/rfc8446#section-4.2.8.2
 * and include the extra "legacy_form" byte */
const struct s2n_ecc_named_curve s2n_ecc_supported_curves[2] = {
    {.iana_id = TLS_EC_CURVE_SECP_256_R1, .libcrypto_nid = NID_X9_62_prime256v1, .name = "secp256r1", .share_size = ( 32 * 2 ) + 1 },
    {.iana_id = TLS_EC_CURVE_SECP_384_R1, .libcrypto_nid = NID_secp384r1, .name= "secp384r1", .share_size = ( 48 * 2 ) + 1 },
};

static EC_KEY *s2n_ecc_generate_own_key(const struct s2n_ecc_named_curve *named_curve);
static EC_POINT *s2n_ecc_blob_to_point(struct s2n_blob *blob, const EC_KEY * ec_key);
static int s2n_ecc_calculate_point_length(const EC_POINT * point, const EC_GROUP * group, uint8_t * length);
static int s2n_ecc_write_point_data_snug(const EC_POINT * point, const EC_GROUP * group, struct s2n_blob *out);
static int s2n_ecc_write_point(const EC_POINT * point, const EC_GROUP * group, struct s2n_stuffer *out);
static int s2n_ecc_compute_shared_secret(EC_KEY * own_key, const EC_POINT * peer_public, struct s2n_blob *shared_secret);

int s2n_ecc_generate_ephemeral_key(struct s2n_ecc_params *server_ecc_params)
{
    notnull_check(server_ecc_params->negotiated_curve);
    server_ecc_params->ec_key = s2n_ecc_generate_own_key(server_ecc_params->negotiated_curve);
    S2N_ERROR_IF(server_ecc_params->ec_key == NULL, S2N_ERR_ECDHE_GEN_KEY);
    return 0;
}

int s2n_ecc_write_ecc_params(struct s2n_ecc_params *server_ecc_params, struct s2n_stuffer *out, struct s2n_blob *written)
{
    notnull_check(server_ecc_params);
    notnull_check(server_ecc_params->negotiated_curve);
    notnull_check(server_ecc_params->ec_key);
    notnull_check(out);
    notnull_check(written);

    int key_share_size = server_ecc_params->negotiated_curve->share_size;

    /* Remember where the written data starts */
    written->data = s2n_stuffer_raw_write(out, 0);
    notnull_check(written->data);

    GUARD(s2n_stuffer_write_uint8(out, TLS_EC_CURVE_TYPE_NAMED));
    GUARD(s2n_stuffer_write_uint16(out, server_ecc_params->negotiated_curve->iana_id));
    GUARD(s2n_stuffer_write_uint8(out, key_share_size));

    GUARD(s2n_ecc_write_ecc_params_point(server_ecc_params, out));

    /* key share + key share size (1) + iana (2) + curve type (1) */
    written->size = key_share_size + 4;

    return written->size;
}

int s2n_ecc_write_ecc_params_point(struct s2n_ecc_params *ecc_params, struct s2n_stuffer *out)
{
    notnull_check(ecc_params);
    notnull_check(ecc_params->ec_key);
    notnull_check(out);

    GUARD(s2n_ecc_write_point(
            EC_KEY_get0_public_key(ecc_params->ec_key),
            EC_KEY_get0_group(ecc_params->ec_key),
            out));

    return 0;
}

int s2n_ecc_read_ecc_params(struct s2n_stuffer *in, struct s2n_blob *data_to_verify, struct s2n_ecdhe_raw_server_params *raw_server_ecc_params)
{
    uint8_t curve_type;
    uint8_t point_length;

    /* Remember where we started reading the data */
    data_to_verify->data = s2n_stuffer_raw_read(in, 0);
    notnull_check(data_to_verify->data);

    /* Read the curve */
    GUARD(s2n_stuffer_read_uint8(in, &curve_type));
    S2N_ERROR_IF(curve_type != TLS_EC_CURVE_TYPE_NAMED, S2N_ERR_BAD_MESSAGE);
    raw_server_ecc_params->curve_blob.data = s2n_stuffer_raw_read(in, 2);
    notnull_check(raw_server_ecc_params->curve_blob.data);
    raw_server_ecc_params->curve_blob.size = 2;

    /* Read the point */
    GUARD(s2n_stuffer_read_uint8(in, &point_length));
    GUARD(s2n_ecc_read_ecc_params_point(in, &raw_server_ecc_params->point_blob, point_length));

    /* 1 byte for curve type, 2 for the curve data, 1 for the point length, and point_length for the point */
    data_to_verify->size = (1 + 2) + (1 + point_length);

    return 0;
}

int s2n_ecc_read_ecc_params_point(struct s2n_stuffer *in, struct s2n_blob *point_blob, int point_size)
{
    notnull_check(in);
    notnull_check(point_blob);
    gte_check(point_size, 0);

    /* Extract point from stuffer */
    point_blob->size = point_size;
    point_blob->data = s2n_stuffer_raw_read(in, point_size);
    notnull_check(point_blob->data);

    return 0;
}

int s2n_ecc_parse_ecc_params(struct s2n_ecc_params *server_ecc_params, struct s2n_ecdhe_raw_server_params *raw_server_ecc_params)
{
    /* Verify that the client supports the server curve */
    S2N_ERROR_IF(s2n_ecc_find_supported_curve(&raw_server_ecc_params->curve_blob, &server_ecc_params->negotiated_curve) != 0, S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    return s2n_ecc_parse_ecc_params_point(server_ecc_params, &raw_server_ecc_params->point_blob);
}

int s2n_ecc_parse_ecc_params_point(struct s2n_ecc_params *ecc_params, struct s2n_blob *point_blob)
{
    notnull_check(point_blob);
    notnull_check(ecc_params);
    notnull_check(ecc_params->negotiated_curve);

    /* Create a key to store the point */
    ecc_params->ec_key = EC_KEY_new_by_curve_name(ecc_params->negotiated_curve->libcrypto_nid);
    S2N_ERROR_IF(ecc_params->ec_key == NULL, S2N_ERR_ECDHE_UNSUPPORTED_CURVE);

    /* Parse and store the server public point */
    EC_POINT *point = s2n_ecc_blob_to_point(point_blob, ecc_params->ec_key);
    S2N_ERROR_IF(point == NULL, S2N_ERR_BAD_MESSAGE);

    /* Set the point as the public key */
    int success = EC_KEY_set_public_key(ecc_params->ec_key, point);
    EC_POINT_free(point);

    /* EC_KEY_set_public_key returns 1 on success, 0 on failure */
    S2N_ERROR_IF(success == 0, S2N_ERR_BAD_MESSAGE);

    return 0;
}

int s2n_ecc_compute_shared_secret_as_server(struct s2n_ecc_params *server_ecc_params, struct s2n_stuffer *Yc_in, struct s2n_blob *shared_key)
{
    uint8_t client_public_len;
    struct s2n_blob client_public_blob = {0};
    EC_POINT *client_public;
    int rc;

    GUARD(s2n_stuffer_read_uint8(Yc_in, &client_public_len));
    client_public_blob.size = client_public_len;
    client_public_blob.data = s2n_stuffer_raw_read(Yc_in, client_public_blob.size);
    notnull_check(client_public_blob.data);

    /* Parse the client public */
    client_public = s2n_ecc_blob_to_point(&client_public_blob, server_ecc_params->ec_key);
    S2N_ERROR_IF(client_public == NULL, S2N_ERR_BAD_MESSAGE);

    /* Compute the key and free the point */
    rc = s2n_ecc_compute_shared_secret(server_ecc_params->ec_key, client_public, shared_key);
    EC_POINT_free(client_public);
    return rc;
}

int s2n_ecc_compute_shared_secret_as_client(struct s2n_ecc_params *server_ecc_params, struct s2n_stuffer *Yc_out, struct s2n_blob *shared_key)
{
    EC_KEY *client_key;

    /* Generate the client key. Don't forget to free it. */
    notnull_check(server_ecc_params->negotiated_curve);
    client_key = s2n_ecc_generate_own_key(server_ecc_params->negotiated_curve);
    S2N_ERROR_IF(client_key == NULL, S2N_ERR_ECDHE_GEN_KEY);

    /* Compute the shared secret */
    if (s2n_ecc_compute_shared_secret(client_key, EC_KEY_get0_public_key(server_ecc_params->ec_key), shared_key) != 0) {
        EC_KEY_free(client_key);
        S2N_ERROR(S2N_ERR_ECDHE_SHARED_SECRET);
    }

    GUARD(s2n_stuffer_write_uint8(Yc_out, server_ecc_params->negotiated_curve->share_size));

    /* Write the client public to Yc */
    if (s2n_ecc_write_point(EC_KEY_get0_public_key(client_key), EC_KEY_get0_group(client_key), Yc_out) != 0) {
        EC_KEY_free(client_key);
        S2N_ERROR(S2N_ERR_ECDHE_SERIALIZING);
    }
    EC_KEY_free(client_key);

    return 0;
}

int s2n_ecc_params_free(struct s2n_ecc_params *server_ecc_params)
{
    if (server_ecc_params->ec_key != NULL) {
        EC_KEY_free(server_ecc_params->ec_key);
        server_ecc_params->ec_key = NULL;
    }
    return 0;
}

static EC_KEY *s2n_ecc_generate_own_key(const struct s2n_ecc_named_curve *named_curve)
{
    EC_KEY *key = EC_KEY_new_by_curve_name(named_curve->libcrypto_nid);
    if (key == NULL) {
        S2N_ERROR_PTR(S2N_ERR_ECDHE_GEN_KEY);
    }
    if (EC_KEY_generate_key(key) != 1) {
        EC_KEY_free(key);
        S2N_ERROR_PTR(S2N_ERR_ECDHE_GEN_KEY);
    }
    return key;
}

static EC_POINT *s2n_ecc_blob_to_point(struct s2n_blob *blob, const EC_KEY * ec_key)
{
    const EC_GROUP *group = EC_KEY_get0_group(ec_key);
    EC_POINT *point = EC_POINT_new(group);
    if (point == NULL) {
        S2N_ERROR_PTR(S2N_ERR_ECDHE_UNSUPPORTED_CURVE);
    }
    if (EC_POINT_oct2point(group, point, blob->data, blob->size, NULL) != 1) {
        EC_POINT_free(point);
        S2N_ERROR_PTR(S2N_ERR_BAD_MESSAGE);
    }
    return point;
}

static int s2n_ecc_calculate_point_length(const EC_POINT * point, const EC_GROUP * group, uint8_t * length)
{
    size_t ret = EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, NULL, 0, NULL);
    S2N_ERROR_IF(ret == 0, S2N_ERR_ECDHE_SERIALIZING);
    S2N_ERROR_IF(ret > UINT8_MAX, S2N_ERR_ECDHE_SERIALIZING);
    *length = (uint8_t) ret;
    return 0;
}

static int s2n_ecc_write_point_data_snug(const EC_POINT * point, const EC_GROUP * group, struct s2n_blob *out)
{
    size_t ret = EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, out->data, out->size, NULL);
    S2N_ERROR_IF(ret != out->size, S2N_ERR_ECDHE_SERIALIZING);
    return 0;
}

static int s2n_ecc_write_point(const EC_POINT * point, const EC_GROUP * group, struct s2n_stuffer *out)
{
    uint8_t point_len;
    struct s2n_blob point_blob = {0};

    GUARD(s2n_ecc_calculate_point_length(point, group, &point_len));

    point_blob.data = s2n_stuffer_raw_write(out, point_len);
    point_blob.size = point_len;
    notnull_check(point_blob.data);

    GUARD(s2n_ecc_write_point_data_snug(point, group, &point_blob));

    return 0;
}

static int s2n_ecc_compute_shared_secret(EC_KEY * own_key, const EC_POINT * peer_public, struct s2n_blob *shared_secret)
{
    int field_degree;
    int shared_secret_size;

    field_degree = EC_GROUP_get_degree(EC_KEY_get0_group(own_key));
    S2N_ERROR_IF(field_degree <= 0, S2N_ERR_ECDHE_SHARED_SECRET);

    shared_secret_size = (field_degree + 7) / 8;
    GUARD(s2n_alloc(shared_secret, shared_secret_size));

    if (ECDH_compute_key(shared_secret->data, shared_secret_size, peer_public, own_key, NULL) != shared_secret_size) {
        GUARD(s2n_free(shared_secret));
        S2N_ERROR(S2N_ERR_ECDHE_SHARED_SECRET);
    }

    return 0;
}

int s2n_ecc_find_supported_curve(struct s2n_blob *iana_ids, const struct s2n_ecc_named_curve **found)
{
    struct s2n_stuffer iana_ids_in = {0};

    GUARD(s2n_stuffer_init(&iana_ids_in, iana_ids));
    GUARD(s2n_stuffer_write(&iana_ids_in, iana_ids));
    for (int i = 0; i < S2N_ECC_SUPPORTED_CURVES_COUNT; i++) {
        const struct s2n_ecc_named_curve *supported_curve = &s2n_ecc_supported_curves[i];
        for (int j = 0; j < iana_ids->size / 2; j++) {
            uint16_t iana_id;
            GUARD(s2n_stuffer_read_uint16(&iana_ids_in, &iana_id));

            if (supported_curve->iana_id == iana_id) {
                *found = supported_curve;
                return 0;
            }
        }
        GUARD(s2n_stuffer_reread(&iana_ids_in));
    }

    /* Nothing found */
    S2N_ERROR(S2N_ERR_ECDHE_UNSUPPORTED_CURVE);
}
