/*
   Copyright 2021 The Silkworm Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "bitmapdb.hpp"

namespace silkworm::db::bitmap {

std::optional<uint64_t> seek_in_bitmap(roaring::Roaring64Map &bitmap, uint64_t cap) {
    for (auto it = bitmap.begin(); it != bitmap.end(); ++it) {
        if (*it > cap) return *it;
    }
    return std::nullopt;
}

roaring::Roaring64Map cut_left(roaring::Roaring64Map *bm, uint64_t size_limit) {
    if (bm->cardinality() == 0) return roaring::Roaring64Map();

    if (bm->getSizeInBytes() <= size_limit) {
        roaring::Roaring64Map res;
        // Add range
        uint64_t range_array[bm->maximum() - bm->minimum() + 1];
        for (size_t i = bm->minimum(); i <= bm->maximum()+1; i++) {
            range_array[i-bm->minimum()] = i;
        }
        res.addMany(bm->maximum() - bm->minimum() + 1, range_array);
        res &= *bm;
        res.runOptimize();
        return res;
    }
    auto from{bm->minimum()};
    auto min_max{bm->maximum() - bm->minimum()};

    // We look for the cutting point
	uint64_t to = 0;
    uint64_t j = min_max;
	while (true) {
		uint64_t h = (to+j) >> 1;
		roaring::Roaring64Map current_bm;
        // add the range
        uint64_t range_array[to - from + 1];
        for (size_t i = from; i <= to+1; i++) {
            range_array[i-from] = i;
        }
        current_bm.addMany(from - to + 1, range_array);
		current_bm &= *bm;
		current_bm.runOptimize();    
		if (current_bm.getSizeInBytes() <= size_limit) {
			to = h + 1;
		} else {
			j = h;
		}
        if (to > j) {
            // remove range
            for (uint64_t i = from; i <= to+1; i++) {
                bm->remove(i);
            }
            return current_bm;
        }
	}
}
};  // namespace silkworm::db::bitmap
