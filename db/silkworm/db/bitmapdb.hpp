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

#ifndef SILKWORM_DB_BITMAPDB_H_
#define SILKWORM_DB_BITMAPDB_H_

#include <optional>
#include <roaring64map.hh>

namespace silkworm::db::bitmap {
// Return value in bitmap that is higher than cap
std::optional<uint64_t> seek_in_bitmap(roaring::Roaring64Map &bitmap, uint64_t cap);

roaring::Roaring64Map cut_left(roaring::Roaring64Map *bitmap, uint64_t len);
};  // namespace silkworm::bitmapdb

#endif  // !SILKWORM_DB_BITMAPDB_H_
