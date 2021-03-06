#!/usr/bin/env python

"""
Evict the specified key from memory.

Copyright (c) 2017 Couchbase, Inc
"""

import mc_bin_client
import sys

HOST = '127.0.0.1'
PORT = 12000

if len(sys.argv) < 5:
    msg = ('Usage: {} <user> <password> <bucket> <vbid> <key>'.format(
        sys.argv[0]))
    print >> sys.stderr, msg
    sys.exit(1)

client = mc_bin_client.MemcachedClient(host=HOST, port=PORT)
client.sasl_auth_plain(user=sys.argv[1], password=sys.argv[2])
client.bucket_select(sys.argv[3])
client.vbucketId = int(sys.argv[4])

key = sys.argv[5]
print client.evict_key(key)
