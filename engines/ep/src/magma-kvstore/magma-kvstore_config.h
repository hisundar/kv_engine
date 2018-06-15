/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#pragma once

#include "kvstore_config.h"

#include <string>

class Configuration;

// This class represents the MagmaKVStore specific configuration.
// MagmaKVStore uses this in place of the KVStoreConfig base class.
class MagmaKVStoreConfig : public KVStoreConfig {
public:
    // Initialize the object from the central EPEngine Configuration
    MagmaKVStoreConfig(Configuration& config, uint16_t shardid);

	uint64_t getMagmaMemQuota() {
		return magmaMemQuota;
	}
	bool isMagmaEnableDirectio() {
		return magmaEnableDirectio;
	}
	bool isMagmaKvSeparation() {
		return magmaKvSeparation;
	}
	int getMagmaLssCleanThreshold() {
		return magmaLssCleanThreshold;
	}
	int getMagmaLssCleanMax() {
		return magmaLssCleanMax;
	}
	int getMagmaDeltaChainLen() {
		return magmaDeltaChainLen;
	}
	int getMagmaBasePageItems() {
		return magmaBasePageItems;
	}
	int getMagmaLssNumSegments() {
		return magmaLssNumSegments;
	}
	int getMagmaSyncAt() {
		return magmaSyncAt;
	}
	bool isMagmaEnableUpsert() {
		return magmaEnableUpsert;
	}


private:
	// Magma Memory Quota
	size_t magmaMemQuota;

	// Magma Enable Direct I/O
	bool magmaEnableDirectio;

	// Magma Enable Key Value Separation
	bool magmaKvSeparation;

	// Magma LSS Clean Fragmentation
	size_t magmaLssCleanThreshold;

	// Magma LSS Clean Throtle
	size_t magmaLssCleanMax;

	// Magma delta chain len
	size_t magmaDeltaChainLen;

	// Magma base page len
	size_t magmaBasePageItems;

	// Magma LSS Num Segments
	size_t magmaLssNumSegments;

	// Magma Sync at ms
	size_t magmaSyncAt;

	// Magma enable upsert
	bool magmaEnableUpsert;

};
