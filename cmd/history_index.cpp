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

#include <CLI/CLI.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <roaring64map.hh>
#include <silkworm/common/log.hpp>
#include <silkworm/db/access_layer.hpp>
#include <silkworm/db/stages.hpp>
#include <silkworm/db/tables.hpp>
#include <silkworm/etl/collector.hpp>
#include <silkworm/db/bitmapdb.hpp>
#include <unordered_map>

using namespace silkworm;

constexpr size_t kBitmapBufferSizeLimit = 256 * kMebi;
auto last_chunk_index{Bytes(128, '\0')};

Bytes get_history_key(Bytes &changeset_key) {
    if (changeset_key.size() == kHashLength*2 + db::kIncarnationLength) {
        Bytes res(kHashLength*2, '\0');
        std::memcpy(&res[0], &changeset_key[0], kHashLength);
        std::memcpy(&res[kHashLength], &changeset_key[kHashLength+db::kIncarnationLength], kHashLength);
        return res;
    }

    if (changeset_key.size() == kHashLength + kAddressLength + db::kIncarnationLength) {
        Bytes res(kHashLength+kAddressLength, '\0');
        std::memcpy(&res[0], &changeset_key[0], kAddressLength);
        std::memcpy(&res[kAddressLength], &changeset_key[kAddressLength+db::kIncarnationLength], kHashLength);
        return res;
    }
    return changeset_key;
}
int main(int argc, char* argv[]) {
    namespace fs = boost::filesystem;

    CLI::App app{"Generates History Indexes"};

    std::string db_path{db::default_path()};
    bool full, storage;
    app.add_option("-d,--datadir", db_path, "Path to a database populated by Turbo-Geth", true)
        ->check(CLI::ExistingDirectory);

    app.add_flag("--full", full, "Start making history indexes from block 0");
    app.add_flag("--storage", storage, "Do history of storages");


    CLI11_PARSE(app, argc, argv);


    // Check data.mdb exists in provided directory
    boost::filesystem::path db_file{boost::filesystem::path(db_path) / boost::filesystem::path("data.mdb")};
    if (!boost::filesystem::exists(db_file)) {
        SILKWORM_LOG(LogError) << "Can't find a valid TG data file in " << db_path << std::endl;
        return -1;
    }
    fs::path datadir(db_path);
    fs::path etl_path(datadir.parent_path() / fs::path("etl-temp"));
    fs::create_directories(etl_path);
    etl::Collector collector(etl_path.string().c_str(), /* flush size */ 512 * kMebi);

    lmdb::DatabaseConfig db_config{db_path};
    db_config.set_readonly(false);
    std::shared_ptr<lmdb::Environment> env{lmdb::get_env(db_config)};
    std::unique_ptr<lmdb::Transaction> txn{env->begin_rw_transaction()};
    // We take data from header table and transform it and put it in blockhashes table
    lmdb::TableConfig changeset_config = storage ? db::table::kPlainStorageChangeSet : db::table::kPlainAccountChangeSet;
    lmdb::TableConfig index_config = storage ? db::table::kStorageHistory : db::table::kAccountHistory;
    const char * stage_key = storage ? db::stages::kStorageHistoryIndexKey : db::stages::kAccountHistoryIndexKey;

    auto changeset_table{txn->open(changeset_config)};
    auto target_table{txn->open(index_config)};
    std::unordered_map<Bytes, roaring::Roaring64Map> bitmaps;

    try {
        auto last_processed_block_number{db::stages::get_stage_progress(*txn, stage_key)};

        if (full) {
            last_processed_block_number = 0;
        }

        // Extract
        Bytes start(8, '\0');
        boost::endian::store_big_u64(&start[0], last_processed_block_number + 1);
        MDB_val mdb_key{db::to_mdb_val(start)};
        MDB_val mdb_data;
        if (storage) {
            SILKWORM_LOG(LogInfo) << "Started Storage Index Extraction" << std::endl;
        } else {
            SILKWORM_LOG(LogInfo) << "Started Account Index Extraction" << std::endl;
        }

        size_t allocated_space{0};
        uint64_t block_number;
        int rc{changeset_table->seek(&mdb_key, &mdb_data)};  // Sets cursor to nearest key greater equal than this
        while (!rc) {                                     /* Loop as long as we have no errors*/
            Bytes mdb_key_as_bytes{static_cast<uint8_t*>(mdb_key.mv_data), mdb_key.mv_size};
            auto composite_key{get_history_key(mdb_key_as_bytes)};
            if (bitmaps.find(composite_key) == bitmaps.end()) {
                bitmaps.insert({composite_key, roaring::Roaring64Map()});
            }
            block_number = boost::endian::load_big_u64(&composite_key[0]);
            bitmaps.at(composite_key).add(block_number);
            allocated_space += 8;
            if (64 * bitmaps.size() + allocated_space > kBitmapBufferSizeLimit) {
                for (auto& it: bitmaps) {
                    Bytes bitmap_bytes(it.second.getSizeInBytes(), '\0');
                    std::cout << it.second.minimum() << std::endl;
                    it.second.write((char *) &bitmap_bytes[0]);
                    etl::Entry entry{Bytes((uint8_t *)it.first.c_str(), it.first.size()), bitmap_bytes};
                    collector.collect(entry);
                }
                SILKWORM_LOG(LogInfo) << "Current Block: " << block_number << std::endl;
                bitmaps.clear();
                allocated_space = 0;
            }
            rc = changeset_table->get_next(&mdb_key, &mdb_data);
        }

        if (rc && rc != MDB_NOTFOUND) { /* MDB_NOTFOUND is not actually an error rather eof */
            lmdb::err_handler(rc);
        }

        for (auto& it: bitmaps) {
            Bytes bitmap_bytes(it.second.getSizeInBytes(), '\0');
            it.second.write((char *) &bitmap_bytes[0]);
            etl::Entry entry{Bytes((uint8_t *)it.first.c_str(), it.first.size()), bitmap_bytes};
            collector.collect(entry);
        }
        bitmaps.clear();
        allocated_space = 0;
        SILKWORM_LOG(LogInfo) << "Latest Block: " << block_number << std::endl;
        // Proceed only if we've done something
        if (collector.size()) {
            SILKWORM_LOG(LogInfo) << "Started Loading" << std::endl;

            /*
            * If we're on first sync then we shouldn't have any records in target
            * table. For this reason we can apply MDB_APPEND to load as
            * collector (with no transform) ensures collected entries
            * are already sorted. If instead target table contains already
            * some data the only option is to load in upsert mode as we
            * cannot guarantee keys are sorted amongst different calls
            * of this stage
            */
            size_t target_table_rcount{0};
            lmdb::err_handler(target_table->get_rcount(&target_table_rcount));
            unsigned int db_flags{target_table_rcount ? MDB_DUPSORT : (uint32_t)MDB_APPENDDUP};
            // Eventually load collected items with no transform (may throw)

            collector.load(target_table.get(), [](etl::Entry entry, lmdb::Table * history_index_table) {
                roaring::Roaring64Map bm;
                bm.readSafe((const char *)entry.value.data(), entry.value.size());
                last_chunk_index = last_chunk_index.substr(0, entry.key.size() + 8);
                boost::endian::store_big_u64(&last_chunk_index[entry.key.size()], (uint64_t)-1);
                auto previous_bitmap_bytes{history_index_table->get(last_chunk_index)};
                if (previous_bitmap_bytes->size() > 0) {
                    roaring::Roaring64Map previous_bitmap;
                    previous_bitmap.readSafe((const char *)previous_bitmap_bytes->data(), previous_bitmap_bytes->size());
                    bm |= previous_bitmap;
                }

                std::vector<etl::Entry> entries;
                while (bm.cardinality() > 0) {
                    std::cout << bm.maximum() << std::endl;
                    auto current_chunk{db::bitmap::cut_left(&bm, 1950)};
                    // make chunk index
                    auto chunk_index{Bytes(entry.key.size() + 8, '\0')};
                    std::memcpy(&chunk_index[0], &entry.key[0], entry.key.size());
                    uint64_t suffix{bm.cardinality() == 0? (uint64_t) -1 : current_chunk.maximum()};
                    boost::endian::store_big_u64(&chunk_index[entry.key.size()], suffix);
                    Bytes current_chunk_bytes(current_chunk.getSizeInBytes(), '\0');
                    current_chunk.write((char *) &current_chunk_bytes[0]);
                    auto entry{etl::Entry{chunk_index, current_chunk_bytes}};
                    entries.push_back(entry);
                }

                return entries;
            }, db_flags, /* log_every_percent = */ 10);

            // Update progress height with last processed block
            db::stages::set_stage_progress(*txn, stage_key, db::stages::get_stage_progress(*txn, db::stages::kExecutionKey));
            lmdb::err_handler(txn->commit());

        } else {
            SILKWORM_LOG(LogInfo) << "Nothing to process" << std::endl;
        }

        SILKWORM_LOG(LogInfo) << "All Done" << std::endl;
    } catch (const std::exception& ex) {
        SILKWORM_LOG(LogError) << ex.what() << std::endl;
        return -5;
    }
    return 0;
}
