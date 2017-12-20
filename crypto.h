/*
Serval DNA internal cryptographic operations
Copyright 2013 Serval Project Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef __SERVAL_DNA__CRYPTO_H
#define __SERVAL_DNA__CRYPTO_H

#include "serval_types.h"

#define SIGNATURE_BYTES crypto_sign_BYTES

struct subscriber;

int crypto_isvalid_keypair(const sign_private_t *private_key, const sign_public_t *public_key);
int crypto_verify_message(struct subscriber *subscriber, unsigned char *message, size_t *message_len);
int crypto_sign_to_sid(const sign_public_t *public_key, sid_t *sid);
int crypto_ismatching_sign_sid(const sign_public_t *public_key, const sid_t *sid);
int crypto_seed_keypair(sign_keypair_t *key, const char *fmt, ...);

#endif
