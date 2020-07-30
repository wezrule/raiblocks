#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/rocksdbconfig.hpp>
#include <nano/node/rocksdb/rocksdb.hpp>
#include <nano/node/rocksdb/rocksdb_iterator.hpp>
#include <nano/node/rocksdb/rocksdb_txn.hpp>

#include <boost/endian/conversion.hpp>
#include <boost/format.hpp>
#include <boost/polymorphic_cast.hpp>
#include <boost/property_tree/ptree.hpp>

#include <rocksdb/merge_operator.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/utilities/backupable_db.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>

namespace
{
class event_listener : public rocksdb::EventListener
{
public:
	event_listener (std::function<void(rocksdb::FlushJobInfo const &)> const & flush_completed_cb_a) :
	flush_completed_cb (flush_completed_cb_a)
	{
	}
	void OnFlushCompleted (rocksdb::DB * /* db_a */, rocksdb::FlushJobInfo const & flush_info_a) override
	{
		flush_completed_cb (flush_info_a);
	}

private:
	std::function<void(rocksdb::FlushJobInfo const &)> flush_completed_cb;
};

// This is a read-modify-write operator, to update a count atomically and more efficiently than 2 separate get/put operations
class uint64_merge_operator : public rocksdb::AssociativeMergeOperator
{
public:
	bool Merge (const rocksdb::Slice & key, const rocksdb::Slice * existing_value, const rocksdb::Slice & value, std::string * new_value, rocksdb::Logger * /*logger*/) const override
	{
		uint64_t existing = 0;
		if (existing_value)
		{
			existing = decode_integer (*existing_value);
		}

		uint64_t oper = decode_integer (value);
		// Values are in big endian, so first convert to native endian, perform calculation and convert back before storing it
		auto val = boost::endian::big_to_native (existing);
		if (oper == 0)
		{
			val -= 1;
		}
		else
		{
			val += boost::endian::big_to_native (oper);
		}

		boost::endian::native_to_big_inplace (val);

		new_value->clear ();
		new_value->append (const_cast<const char *> (reinterpret_cast<char *> (&val)), sizeof (val));
		return true;
	}

	const char * Name () const override
	{
		return "uint64_merge_operator";
	}

	bool AllowSingleOperand () const override
	{
		return false;
	}

private:
	// Takes the slice and decodes it into a uint64_t
	uint64_t decode_integer (rocksdb::Slice const & value) const
	{
		uint64_t result = 0;
		memcpy (&result, value.data (), sizeof (result));
		return result;
	}
};
}

namespace nano
{
template <>
void * rocksdb_val::data () const
{
	return (void *)value.data ();
}

template <>
size_t rocksdb_val::size () const
{
	return value.size ();
}

template <>
rocksdb_val::db_val (size_t size_a, void * data_a) :
value (static_cast<const char *> (data_a), size_a)
{
}

template <>
void rocksdb_val::convert_buffer_to_value ()
{
	value = rocksdb::Slice (reinterpret_cast<const char *> (buffer->data ()), buffer->size ());
}
}

nano::rocksdb_store::rocksdb_store (nano::logger_mt & logger_a, boost::filesystem::path const & path_a, nano::rocksdb_config const & rocksdb_config_a, bool open_read_only_a) :
logger (logger_a),
rocksdb_config (rocksdb_config_a)
{
	boost::system::error_code error_mkdir, error_chmod;
	boost::filesystem::create_directories (path_a, error_mkdir);
	nano::set_secure_perm_directory (path_a, error_chmod);
	error = static_cast<bool> (error_mkdir);

	if (!error)
	{
		auto small_table_options = get_small_table_options ();
		small_table_factory.reset (rocksdb::NewBlockBasedTableFactory (small_table_options));
		if (!open_read_only_a)
		{
			construct_column_family_mutexes ();
		}
		open (error, path_a, open_read_only_a);
	}
}

void nano::rocksdb_store::flush ()
{
	// The frontiers table is not used after all legacy blocks have been cemented
	if (num_frontiers_changes_since_last_flush > 0)
	{
		flush_tables ({ tables::frontiers });
	}
}

rocksdb::ColumnFamilyOptions nano::rocksdb_store::get_cf_options (std::string const & cf_name_a) const
{
	std::shared_ptr<rocksdb::TableFactory> table_factory;

	auto base_memtable_size = 32; // MB
	rocksdb::ColumnFamilyOptions cf_options;
	if (cf_name_a == "unchecked")
	{
		auto unchecked_memtable_bytes = 1024ULL * 1024 * 32;
		table_factory.reset (rocksdb::NewBlockBasedTableFactory (get_active_table_options (32)));
		cf_options = get_common_cf_options (table_factory, unchecked_memtable_bytes);

		cf_options.memtable_prefix_bloom_size_ratio = 0.1; // create prefix bloom for memtable with the size of write_buffer_size * memtable_prefix_bloom_size_ratio
		cf_options.prefix_extractor.reset (rocksdb::NewFixedPrefixTransform (32)); // Hash prefix of unchecked root

		// Number of files in level 0 which triggers compaction. Size of L0 and L1 should be kept similar as this is the only compaction which is single threaded
		cf_options.level0_file_num_compaction_trigger = 2;

		// L1 size, compaction is triggered for L0 at this size (2 SST files in L1)
		cf_options.max_bytes_for_level_base = unchecked_memtable_bytes * 2;
		cf_options.max_bytes_for_level_multiplier = 10; // Default

		cf_options.target_file_size_base = unchecked_memtable_bytes;

		// Multiplier for each level
		cf_options.target_file_size_multiplier = 10;
		cf_options.write_buffer_size = unchecked_memtable_bytes;
	}
	else if (cf_name_a == "blocks")
	{
		table_factory.reset (rocksdb::NewBlockBasedTableFactory (get_active_table_options (64)));
		cf_options = get_common_cf_options (table_factory, 1024ULL * 1024 * base_memtable_size);
	}
	else if (cf_name_a == "confirmation_height")
	{
		// Entries will never be deleted so can make memtables a lot bigger
		table_factory.reset (rocksdb::NewBlockBasedTableFactory (get_active_table_options (16)));
		cf_options = get_common_cf_options (table_factory, 1024ULL * 1024 * base_memtable_size * 2);
	}
	else if (cf_name_a == "meta" || cf_name_a == "online_weight" || cf_name_a == "peers")
	{
		// Meta - It contains just version key
		// Online weight - Periodically deleted
		// Peers - Cleaned periodically, a lot of deletions. This is never read outside of initializing? Keep this small
		auto table_factory = small_table_factory;
		cf_options = get_small_cf_options (table_factory);
	}
	else if (cf_name_a == "cached_counts")
	{
		// Really small (key is state_blocks, value is uint64_t)
		auto table_factory = small_table_factory;
		cf_options = get_small_cf_options (table_factory);
		cf_options.merge_operator = std::make_shared<uint64_merge_operator> ();
	}
	else if (cf_name_a == "pending")
	{
		// pending, majority is unchanged, but most recently added is most likely to be removed tho.
		std::shared_ptr<rocksdb::TableFactory> table_factory (rocksdb::NewBlockBasedTableFactory (get_active_table_options (16)));
		cf_options = get_common_cf_options (table_factory, 1024ULL * 1024 * base_memtable_size);
	}
	else if (cf_name_a == "frontiers")
	{
		// Frontiers is only needed during bootstrap for legacy blocks
		std::shared_ptr<rocksdb::TableFactory> table_factory (rocksdb::NewBlockBasedTableFactory (get_active_table_options (16)));
		cf_options = get_common_cf_options (table_factory, 1024ULL * 1024 * base_memtable_size);
	}
	else if (cf_name_a == "accounts")
	{
		// accounts - It can have deletions from rollbacks
		std::shared_ptr<rocksdb::TableFactory> table_factory (rocksdb::NewBlockBasedTableFactory (get_active_table_options (32)));
		cf_options = get_common_cf_options (table_factory, 1024ULL * 1024 * base_memtable_size);
	}
	else if (cf_name_a == "vote")
	{
		// vote - No deletes it seems, only overwrites.
		std::shared_ptr<rocksdb::TableFactory> table_factory (rocksdb::NewBlockBasedTableFactory (get_active_table_options (32)));
		cf_options = get_common_cf_options (table_factory, 1024ULL * 1024 * base_memtable_size);
	}
	else if (cf_name_a == "default")
	{
		// Do nothing.
	}
	else
	{
		debug_assert (false);
	}

	return cf_options;
}

std::vector<rocksdb::ColumnFamilyDescriptor> nano::rocksdb_store::create_column_families ()
{
	std::initializer_list<std::string> names{ rocksdb::kDefaultColumnFamilyName.c_str (), "frontiers", "accounts", "blocks", "pending", "unchecked", "vote", "online_weight", "meta", "peers", "cached_counts", "confirmation_height" };
	std::vector<rocksdb::ColumnFamilyDescriptor> column_families;

	for (const auto & cf_name : names)
	{
		column_families.emplace_back (cf_name, get_cf_options (cf_name));
	}
	return column_families;
}

void nano::rocksdb_store::open (bool & error_a, boost::filesystem::path const & path_a, bool open_read_only_a)
{
	auto column_families = create_column_families ();
	auto options = get_db_options ();
	rocksdb::Status s;

	std::vector<rocksdb::ColumnFamilyHandle *> handles_l;
	if (open_read_only_a)
	{
		rocksdb::DB * db_l;
		s = rocksdb::DB::OpenForReadOnly (options, path_a.string (), column_families, &handles_l, &db_l);
		db.reset (db_l);
	}
	else
	{
		s = rocksdb::OptimisticTransactionDB::Open (options, path_a.string (), column_families, &handles_l, &optimistic_db);
		if (optimistic_db)
		{
			db.reset (optimistic_db);
		}
	}

	handles.resize (handles_l.size ());
	for (auto i = 0; i < handles_l.size (); ++i)
	{
		handles[i].reset (handles_l[i]);
	}

	// Assign handles to supplied
	error_a |= !s.ok ();

	if (!error_a)
	{
		auto transaction = tx_begin_read ();
		auto version_l = version_get (transaction);
		if (version_l > version)
		{
			error_a = true;
			logger.always_log (boost::str (boost::format ("The version of the ledger (%1%) is too high for this node") % version_l));
		}
	}
}

nano::write_transaction nano::rocksdb_store::tx_begin_write (std::vector<nano::tables> const & tables_requiring_locks_a, std::vector<nano::tables> const & tables_no_locks_a)
{
	std::unique_ptr<nano::write_rocksdb_txn> txn;
	release_assert (optimistic_db != nullptr);
	if (tables_requiring_locks_a.empty () && tables_no_locks_a.empty ())
	{
		// Use all tables if none are specified
		txn = std::make_unique<nano::write_rocksdb_txn> (optimistic_db, all_tables (), tables_no_locks_a, write_lock_mutexes);
	}
	else
	{
		txn = std::make_unique<nano::write_rocksdb_txn> (optimistic_db, tables_requiring_locks_a, tables_no_locks_a, write_lock_mutexes);
	}

	// Tables must be kept in alphabetical order. These can be used for mutex locking, so order is important to prevent deadlocking
	debug_assert (std::is_sorted (tables_requiring_locks_a.begin (), tables_requiring_locks_a.end ()));

	return nano::write_transaction{ std::move (txn) };
}

// From: https://github.com/facebook/rocksdb/wiki/Column-Families
// Every time a single Column Family is flushed, we create a new WAL (write-ahead log). All new writes to all Column Families go to the new WAL.
// However, we still can't delete the old WAL since it contains live data from other Column Families. We can delete the old WAL only when all Column Families have been flushed and all data contained in that WAL persists in sst files.
// Check when the last flush was done.
// The WAL file contains things from all membtables, it cannot be deleted if a column family record exists in there
void nano::rocksdb_store::flush_tables (std::vector<nano::tables> const & tables_a)
{
	rocksdb::FlushOptions flush_options;
	std::vector<rocksdb::ColumnFamilyHandle *> column_families;
	std::transform (tables_a.begin (), tables_a.end (), std::back_inserter (column_families), [this](auto table) {
		return table_to_column_family (table);
	});

	db->Flush (flush_options, column_families);
}

void nano::rocksdb_store::flush_table (nano::tables table_a)
{
	db->Flush (rocksdb::FlushOptions {}, table_to_column_family (table_a));
}

void nano::rocksdb_store::store_flush ()
{
	flush_tables (all_tables ());
}

nano::read_transaction nano::rocksdb_store::tx_begin_read ()
{
	return nano::read_transaction{ std::make_unique<nano::read_rocksdb_txn> (db.get ()) };
}

std::string nano::rocksdb_store::vendor_get () const
{
	return boost::str (boost::format ("RocksDB %1%.%2%.%3%") % ROCKSDB_MAJOR % ROCKSDB_MINOR % ROCKSDB_PATCH);
}

rocksdb::ColumnFamilyHandle * nano::rocksdb_store::table_to_column_family (tables table_a) const
{
	auto & handles_l = handles;
	auto get_handle = [&handles_l](const char * name) {
		auto iter = std::find_if (handles_l.begin (), handles_l.end (), [name](auto & handle) {
			return (handle->GetName () == name);
		});
		debug_assert (iter != handles_l.end ());
		return (*iter).get ();
	};

	switch (table_a)
	{
		case tables::frontiers:
			return get_handle ("frontiers");
		case tables::accounts:
			return get_handle ("accounts");
		case tables::blocks:
			return get_handle ("blocks");
		case tables::pending:
			return get_handle ("pending");
		case tables::unchecked:
			return get_handle ("unchecked");
		case tables::vote:
			return get_handle ("vote");
		case tables::online_weight:
			return get_handle ("online_weight");
		case tables::meta:
			return get_handle ("meta");
		case tables::peers:
			return get_handle ("peers");
		case tables::cached_counts:
			return get_handle ("cached_counts");
		case tables::confirmation_height:
			return get_handle ("confirmation_height");
		default:
			release_assert (false);
			return get_handle ("");
	}
}

bool nano::rocksdb_store::exists (nano::transaction const & transaction_a, tables table_a, nano::rocksdb_val const & key_a) const
{
	rocksdb::PinnableSlice slice;
	rocksdb::Status status;
	if (is_read (transaction_a))
	{
		// fill_cache?
		status = db->Get (snapshot_options (transaction_a), table_to_column_family (table_a), key_a, &slice);
	}
	else
	{
		rocksdb::ReadOptions options;
		options.fill_cache = false;
		status = tx (transaction_a)->Get (options, table_to_column_family (table_a), key_a, &slice);
	}

	return (status.ok ());
}

int nano::rocksdb_store::del (nano::write_transaction const & transaction_a, tables table_a, nano::rocksdb_val const & key_a)
{
	debug_assert (transaction_a.contains (table_a));
	// RocksDB does not report not_found status, it is a pre-condition that the key exists
	debug_assert (exists (transaction_a, table_a, key_a));

	// Removing an entry so counts may need adjusting
	if (is_caching_counts (table_a))
	{
		auto status = decrement (transaction_a, tables::cached_counts, rocksdb_val (rocksdb::Slice (table_to_column_family (table_a)->GetName ())));
		release_assert (status.ok ());
	}

	switch (table_a)
	{
	case nano::tables::frontiers:
		++num_frontiers_changes_since_last_flush;
		break;
	case nano::tables::unchecked:
		++num_unchecked_deletes_since_last_flush;
		if (num_unchecked_deletes_since_last_flush > 50000)
		{
			num_unchecked_deletes_since_last_flush = 0;
			flush_table (nano::tables::unchecked);
		}
		break;
	case nano::tables::blocks:
		++num_block_deletes_since_last_flush;
		if (num_block_deletes_since_last_flush > 25000)
		{
			flush_table (nano::tables::blocks);
			num_block_deletes_since_last_flush = 0;
		}
		break;
	case nano::tables::accounts:
		++num_account_deletes_since_last_flush;
		if (num_account_deletes_since_last_flush > 25000)
		{
			flush_table (nano::tables::accounts);
			num_account_deletes_since_last_flush = 0;
		}
		break;
	case nano::tables::pending:
		++num_pending_deletes_since_last_flush;
		if (num_pending_deletes_since_last_flush > 25000)
		{
			flush_table (nano::tables::pending);
			num_pending_deletes_since_last_flush = 0;
		}
		break;
	default:
		break;
	}

	return tx (transaction_a)->Delete (table_to_column_family (table_a), key_a).code ();
}

void nano::rocksdb_store::version_put (nano::write_transaction const & transaction_a, int version_a)
{
	debug_assert (transaction_a.contains (tables::meta));
	nano::uint256_union version_key (1);
	nano::uint256_union version_value (version_a);
	auto status (put (transaction_a, tables::meta, version_key, nano::rocksdb_val (version_value), nano::store_hint::none));
	release_assert (success (status));
}

rocksdb::Transaction * nano::rocksdb_store::tx (nano::transaction const & transaction_a) const
{
	debug_assert (!is_read (transaction_a));
	return static_cast<rocksdb::Transaction *> (transaction_a.get_handle ());
}

int nano::rocksdb_store::get (nano::transaction const & transaction_a, tables table_a, nano::rocksdb_val const & key_a, nano::rocksdb_val & value_a) const
{
	rocksdb::ReadOptions options;
	rocksdb::PinnableSlice slice;
	auto handle = table_to_column_family (table_a);
	rocksdb::Status status;
	if (is_read (transaction_a))
	{
		status = db->Get (snapshot_options (transaction_a), handle, key_a, &slice);
	}
	else
	{
		status = tx (transaction_a)->Get (options, handle, key_a, &slice);
	}

	if (status.ok ())
	{
		value_a.buffer = std::make_shared<std::vector<uint8_t>> (slice.size ());
		std::memcpy (value_a.buffer->data (), slice.data (), slice.size ());
		value_a.convert_buffer_to_value ();
	}
	return status.code ();
}

/** The column families which need to have their counts cached for later querying */
bool nano::rocksdb_store::is_caching_counts (nano::tables table_a) const
{
	switch (table_a)
	{
		case tables::blocks:
			return true;
		default:
			return false;
	}
}

rocksdb::Status nano::rocksdb_store::increment (nano::write_transaction const & transaction_a, tables table_a, nano::rocksdb_val const & key_a)
{
	release_assert (transaction_a.contains (table_a));
	auto txn = tx (transaction_a);
	return txn->Merge (table_to_column_family (table_a), key_a, nano::rocksdb_val (1));
}

rocksdb::Status nano::rocksdb_store::decrement (nano::write_transaction const & transaction_a, tables table_a, nano::rocksdb_val const & key_a)
{
	release_assert (transaction_a.contains (table_a));
	auto txn = tx (transaction_a);
	// This uses a special encoding to mean that 0 should substract 1
	return txn->Merge (table_to_column_family (table_a), key_a, nano::rocksdb_val (0));
}

int nano::rocksdb_store::put (nano::write_transaction const & transaction_a, tables table_a, nano::rocksdb_val const & key_a, nano::rocksdb_val const & value_a, nano::store_hint store_hint_a)
{
	debug_assert (transaction_a.contains (table_a));

	auto txn = tx (transaction_a);
	if (is_caching_counts (table_a))
	{
		if (store_hint_a == store_hint::key_not_exists || (store_hint_a == store_hint::none && !exists (transaction_a, table_a, key_a)))
		{
			// Adding a new entry so counts need adjusting
			auto status = increment (transaction_a, tables::cached_counts, rocksdb_val (rocksdb::Slice (table_to_column_family (table_a)->GetName ())));
			release_assert (status.ok ());
		}
		else
		{
			debug_assert (store_hint_a == store_hint::key_exists && exists (transaction_a, table_a, key_a));
		}
	}
	switch (table_a)
	{
	case nano::tables::frontiers:
		++num_frontiers_changes_since_last_flush;
		break;
	default:
		break;
	}

	return txn->Put (table_to_column_family (table_a), key_a, value_a).code ();
}

bool nano::rocksdb_store::not_found (int status) const
{
	return (status_code_not_found () == status);
}

bool nano::rocksdb_store::success (int status) const
{
	return (static_cast<int> (rocksdb::Status::Code::kOk) == status);
}

int nano::rocksdb_store::status_code_not_found () const
{
	return static_cast<int> (rocksdb::Status::Code::kNotFound);
}

uint64_t nano::rocksdb_store::count (nano::transaction const & transaction_a, rocksdb::ColumnFamilyHandle * handle) const
{
	uint64_t count = 0;
	nano::rocksdb_val val;
	auto const & key = handle->GetName ();
	auto status = get (transaction_a, tables::cached_counts, nano::rocksdb_val (key.size (), (void *)key.data ()), val);
	if (success (status))
	{
		count = static_cast<uint64_t> (val);
	}

	release_assert (success (status) || not_found (status));
	return count;
}

size_t nano::rocksdb_store::count (nano::transaction const & transaction_a, tables table_a) const
{
	size_t sum = 0;
	// Some column families are small enough (except unchecked) that they can just be iterated, rather than doing extra io caching counts
	if (table_a == tables::peers)
	{
		for (auto i (peers_begin (transaction_a)), n (peers_end ()); i != n; ++i)
		{
			++sum;
		}
	}
	else if (table_a == tables::online_weight)
	{
		for (auto i (online_weight_begin (transaction_a)), n (online_weight_end ()); i != n; ++i)
		{
			++sum;
		}
	}
	// This is only an estimation.
	else if (table_a == tables::unchecked)
	{
		db->GetIntProperty (table_to_column_family (table_a), "rocksdb.estimate-num-keys", &sum);
	}
	// This should only be used in tests
	else if (table_a == tables::accounts)
	{
		debug_assert (network_constants ().is_test_network ());
		for (auto i (latest_begin (transaction_a)), n (latest_end ()); i != n; ++i)
		{
			++sum;
		}
	}
	else
	{
		debug_assert (is_caching_counts (table_a));
		return count (transaction_a, table_to_column_family (table_a));
	}

	return sum;
}

std::vector<nano::unchecked_info> nano::rocksdb_store::unchecked_get (nano::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	auto cf = table_to_column_family (tables::unchecked);

	std::unique_ptr<rocksdb::Iterator> iter;
	if (is_read (transaction_a))
	{
		iter.reset (db->NewIterator (snapshot_options (transaction_a), cf));
	}
	else
	{
		rocksdb::ReadOptions ropts;
		ropts.fill_cache = false;
		iter.reset (tx (transaction_a)->GetIterator (ropts, cf));
	}

	// Uses prefix extraction
	std::vector<nano::unchecked_info> result;

	auto prefix = nano::rocksdb_val (hash_a);
	for (iter->Seek (prefix); iter->Valid () && iter->key ().starts_with (prefix); iter->Next ())
	{
		auto unchecked_info = static_cast<nano::unchecked_info> (nano::rocksdb_val (iter->value ()));
		result.push_back (unchecked_info);
	}
	return result;
}

// From https://rocksdb.org/blog/2018/11/21/delete-range.html
// TODO: Note that memtable range tombstones are fragmented every read; for now this is acceptable, since we expect there to be relatively few range tombstones in memtables (and users can enforce this by keeping track of the number of memtable range deletions and manually flushing after it passes a threshold)
// How to track amount of deletions in memtable????

int nano::rocksdb_store::drop (nano::write_transaction const & transaction_a, tables table_a)
{
	debug_assert (transaction_a.contains (table_a));
	auto col = table_to_column_family (table_a);

	int status = static_cast<int> (rocksdb::Status::Code::kOk);
	if (is_caching_counts (table_a))
	{
		// Reset counter to 0
		status = put (transaction_a, tables::cached_counts, nano::rocksdb_val (rocksdb::Slice (col->GetName ())), nano::rocksdb_val (uint64_t{ 0 }), nano::store_hint::none);
	}

	if (success (status))
	{
		// Dropping/creating families like in node::ongoing_peer_clear can cause write stalls, just delete them manually.
		if (table_a == tables::peers)
		{
			int status = 0;
			for (auto i = peers_begin (transaction_a), n = peers_end (); i != n; ++i)
			{
				status = del (transaction_a, tables::peers, nano::rocksdb_val (i->first));
				release_assert (success (status));
			}
			return status;
		}
		else
		{
			return clear (col);
		}
	}
	return status;
}

int nano::rocksdb_store::clear (rocksdb::ColumnFamilyHandle * column_family)
{
	// Dropping completely removes the column
	auto name = column_family->GetName ();
	auto status = db->DropColumnFamily (column_family);
	release_assert (status.ok ());

	// Need to add it back as we just want to clear the contents
	auto handle_it = std::find_if (handles.begin (), handles.end (), [column_family](auto & handle) {
		return handle.get () == column_family;
	});
	debug_assert (handle_it != handles.cend ());
	status = db->CreateColumnFamily (get_cf_options (name), name, &column_family);
	release_assert (status.ok ());
	handle_it->reset (column_family);
	return status.code ();
}

void nano::rocksdb_store::construct_column_family_mutexes ()
{
	for (auto table : all_tables ())
	{
		write_lock_mutexes.emplace (std::piecewise_construct, std::forward_as_tuple (table), std::forward_as_tuple ());
	}
}

rocksdb::Options nano::rocksdb_store::get_db_options ()
{
	rocksdb::Options db_options;
	db_options.create_if_missing = true;
	db_options.create_missing_column_families = true;
	//db_options.bloom_locality = 1; // TODO: What number to set this to?

	// Sets the compaction priority
	db_options.compaction_pri = rocksdb::CompactionPri::kMinOverlappingRatio;

	// Start aggressively flushing WAL files when they reach over 1GB
	db_options.max_total_wal_size = 1 * 1024 * 1024 * 1024LL;

	// Optimize RocksDB. This is the easiest way to get RocksDB to perform well
	db_options.IncreaseParallelism (rocksdb_config.io_threads);
	db_options.OptimizeLevelStyleCompaction ();

	// Adds a separate write queue for memtable/WAL
	db_options.enable_pipelined_write = true;

	// These options are for speeding up the initial DB open:
	// Default is 16, this can allow faster startup times for SSDs by allowings more files to be read in parallel.
	db_options.max_file_opening_threads = -1;
	// db_options.skip_checking_sst_file_sizes_on_db_open=true;
	// The MANIFEST file contains a history of all file operations since the last time the DB was opened and is replayed during DB open.
	// Default is 1GB, lowering this to avoid replaying for too long (100MB), TODO: not entirely sure why it's needed!!
	db_options.max_manifest_file_size = 100 * 1024 * 1024ULL;

	db_options.allow_concurrent_memtable_write = false; //Need to be disabled if using a plaintable block factory

	// Total size of memtables across column families. This can be used to manage the total memory used by memtables.
	db_options.db_write_buffer_size = 0; // rocksdb_config.total_memtable_size * 1024 * 1024ULL;

	auto event_listener_l = new event_listener ([this](rocksdb::FlushJobInfo const & flush_job_info_a) { this->on_flush (flush_job_info_a); });
	db_options.listeners.emplace_back (event_listener_l);

	// https://github.com/facebook/rocksdb/issues/3760
	db_options.max_successive_merges = 1000;
	return db_options;
}

void nano::rocksdb_store::on_flush (rocksdb::FlushJobInfo const & flush_job_info_a)
{
	// Reset appropriate counters
	if (flush_job_info_a.cf_name == "frontiers")
	{
		num_frontiers_changes_since_last_flush = 0;
	}
	else if (flush_job_info_a.cf_name == "unchecked")
	{
		//std::cout << "During flush" << num_unchecked_deletes_since_last_flush << std::endl;
		num_unchecked_deletes_since_last_flush = 0;
	}
	else if (flush_job_info_a.cf_name == "blocks")
	{
		num_block_deletes_since_last_flush = 0;
	}
	else if (flush_job_info_a.cf_name == "pending")
	{
		num_pending_deletes_since_last_flush = 0;
	}
	else if (flush_job_info_a.cf_name == "accounts")
	{
		num_account_deletes_since_last_flush = 0;
	}
}

rocksdb::BlockBasedTableOptions nano::rocksdb_store::get_active_table_options (int lru_size) const
{
	rocksdb::BlockBasedTableOptions table_options;

	// Improve point lookup performance be using the data block hash index (uses about 5% more space).
	// https://rocksdb.org/blog/2018/08/23/data-block-hash-index.html
	table_options.data_block_index_type = rocksdb::BlockBasedTableOptions::DataBlockIndexType::kDataBlockBinaryAndHash;
	table_options.data_block_hash_table_util_ratio = 0.75;

	// Block cache for reads
	table_options.block_cache = rocksdb::NewLRUCache (1024ULL * 1024 * lru_size);

	// Bloom filter to help with point reads. 10bits gives 1% false positive rate.
	table_options.filter_policy.reset (rocksdb::NewBloomFilterPolicy (10, false));

	// Increasing block_size decreases memory usage and space amplification, but increases read amplification.
	table_options.block_size = 16 * 1024ULL;

	// Whether index and filter blocks are stored in block_cache. These settings should be synced. TODO: https://github.com/facebook/rocksdb/wiki/RocksDB-FAQ says keep them in memory with cache_index_and_filter_blocks as false
	//	table_options.cache_index_and_filter_blocks = true; // default of false uses more memory I think, but better read performance
	table_options.pin_l0_filter_and_index_blocks_in_cache = table_options.cache_index_and_filter_blocks;

	return table_options;
}

rocksdb::BlockBasedTableOptions nano::rocksdb_store::get_small_table_options () const
{
	rocksdb::BlockBasedTableOptions table_options;
	// Improve point lookup performance be using the data block hash index (uses about 5% more space).
	// https://rocksdb.org/blog/2018/08/23/data-block-hash-index.html
	table_options.data_block_index_type = rocksdb::BlockBasedTableOptions::DataBlockIndexType::kDataBlockBinaryAndHash;
	table_options.data_block_hash_table_util_ratio = 0.75;
	table_options.block_size = 1024ULL;
	return table_options;
}

rocksdb::ColumnFamilyOptions nano::rocksdb_store::get_small_cf_options (std::shared_ptr<rocksdb::TableFactory> const & table_factory_a) const
{
	rocksdb::ColumnFamilyOptions cf_options;
	cf_options.table_factory = table_factory_a;

	// Number of files in level 0 which triggers compaction. Size of L0 and L1 should be kept similar as this is the only compaction which is single threaded
	cf_options.level0_file_num_compaction_trigger = 1;

	auto const memtable_size_bytes = 10000;

	// L1 size, compaction is triggered for L0 at this size (1 SST file in L1)
	cf_options.max_bytes_for_level_base = memtable_size_bytes; // 1KB

	// Files older than this (1 day) will be scheduled for compaction when there is no other background work
	cf_options.ttl = 1 * 24 * 60 * 60;

	// Multiplier for each level
	cf_options.target_file_size_multiplier = 10;

	// Size of level 1 sst files. Recommended setting by RocksDB docs to have 10 files in level1.
	cf_options.target_file_size_base = memtable_size_bytes;

	// Size of each memtable
	cf_options.write_buffer_size = memtable_size_bytes;

	return cf_options;
}

rocksdb::ColumnFamilyOptions nano::rocksdb_store::get_common_cf_options (std::shared_ptr<rocksdb::TableFactory> const & table_factory_a, unsigned long long memtable_size_bytes) const
{
	rocksdb::ColumnFamilyOptions cf_options;
	cf_options.table_factory = table_factory_a;

	// Use hash to find data instead of binary search
	//	cf_options.memtable_factory.reset (rocksdb::NewHashSkipListRepFactory ());

	// Number of files in level 0 which triggers compaction. Size of L0 and L1 should be kept similar as this is the only compaction which is single threaded
	cf_options.level0_file_num_compaction_trigger = 4;

	// L1 size, compaction is triggered for L0 at this size (4 SST files in L1)
	cf_options.max_bytes_for_level_base = memtable_size_bytes * 4;

	// Files older than this (1 day) will be scheduled for compaction when there is no other background work. This can lead to more writes however.
	cf_options.ttl = 1 * 24 * 60 * 60;

	// Multiplier for each level
	cf_options.target_file_size_multiplier = 10; // .target_file_size_multiplier = 10;

	// This is per file. Size of level 1 sst files. Recommended setting by RocksDB docs to have 10 files in level1.
	cf_options.target_file_size_base = memtable_size_bytes; // cf_options.max_bytes_for_level_base / 10;

	// Size of each memtable
	cf_options.write_buffer_size = memtable_size_bytes;

	// Size target of levels are changed dynamically based on size of the last level
	cf_options.level_compaction_dynamic_level_bytes = true;

	// Number of memtables to keep in memory (1 active, rest inactive/immutable)
	cf_options.max_write_buffer_number = 2; // rocksdb_config.num_memtables;


	// cf_options.memtable_huge_page_size // Make it a config option!!
	// cf_options.optimize_filters_for_hits

//	cf_options.min_wmin_write_buffer_number_to_merge = 2; //min_write_buffer_number_to_merge

	// cf_options.max_write_buffer_size_to_maintain;

	return cf_options;
}

std::vector<nano::tables> nano::rocksdb_store::all_tables () const
{
	return std::vector<nano::tables>{ tables::accounts, tables::blocks, tables::cached_counts, tables::confirmation_height, tables::frontiers, tables::meta, tables::online_weight, tables::peers, tables::pending, tables::unchecked, tables::vote };
}

bool nano::rocksdb_store::copy_db (boost::filesystem::path const & destination_path)
{
	std::unique_ptr<rocksdb::BackupEngine> backup_engine;
	{
		rocksdb::BackupEngine * backup_engine_raw;
		rocksdb::BackupableDBOptions backup_options (destination_path.string ());
		// Use incremental backups (default)
		backup_options.share_table_files = true;

		// Increase number of threads used for copying
		backup_options.max_background_operations = std::thread::hardware_concurrency ();
		auto status = rocksdb::BackupEngine::Open (rocksdb::Env::Default (), backup_options, &backup_engine_raw);
		backup_engine.reset (backup_engine_raw);
		if (!status.ok ())
		{
			return false;
		}
	}

	auto status = backup_engine->CreateNewBackup (db.get ());
	if (!status.ok ())
	{
		return false;
	}

	std::vector<rocksdb::BackupInfo> backup_infos;
	backup_engine->GetBackupInfo (&backup_infos);

	for (auto const & backup_info : backup_infos)
	{
		status = backup_engine->VerifyBackup (backup_info.backup_id);
		if (!status.ok ())
		{
			return false;
		}
	}

	rocksdb::BackupEngineReadOnly * backup_engine_read;
	status = rocksdb::BackupEngineReadOnly::Open (rocksdb::Env::Default (), rocksdb::BackupableDBOptions (destination_path.string ()), &backup_engine_read);
	if (!status.ok ())
	{
		delete backup_engine_read;
		return false;
	}

	// First remove all files (not directories) in the destination
	for (boost::filesystem::directory_iterator end_dir_it, it (destination_path); it != end_dir_it; ++it)
	{
		auto path = it->path ();
		if (boost::filesystem::is_regular_file (path))
		{
			boost::filesystem::remove (it->path ());
		}
	}

	// Now generate the relevant files from the backup
	status = backup_engine->RestoreDBFromLatestBackup (destination_path.string (), destination_path.string ());
	delete backup_engine_read;

	// Open it so that it flushes all WAL files
	if (status.ok ())
	{
		nano::rocksdb_store rocksdb_store (logger, destination_path.string (), rocksdb_config, false);
		return !rocksdb_store.init_error ();
	}
	return false;
}

void nano::rocksdb_store::rebuild_db (nano::write_transaction const & transaction_a)
{
	release_assert (false && "Not available for RocksDB");
}

bool nano::rocksdb_store::init_error () const
{
	return error;
}

void nano::rocksdb_store::serialize_memory_stats (boost::property_tree::ptree & json)
{
	uint64_t out;

	// Approximate size of active and unflushed immutable memtables (bytes).
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kCurSizeAllMemTables, &out);
	json.put ("rocksdb.cur-size-all-mem-tables", out);

	// Approximate size of active, unflushed immutable, and pinned immutable memtables (bytes).
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kSizeAllMemTables, &out);
	json.put ("rocksdb.size-all-mem-tables", out);

	// Estimated memory used for reading SST tables, excluding memory used in block cache (e.g. filter and index blocks).
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kEstimateTableReadersMem, &out);
	json.put ("rocksdb.estimate-table-readers-mem", out);

	//  An estimate of the amount of live data in bytes.
	uint64_t int_num;
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kEstimateLiveDataSize, &out);
	json.put ("rocksdb.estimate-live-data-size", out);

	//  Returns 1 if at least one compaction is pending; otherwise, returns 0.
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kCompactionPending, &out);
	json.put ("rocksdb.compaction-pending", out);

	// Estimated number of total keys in the active and unflushed immutable memtables and storage.
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kEstimateNumKeys, &out);
	json.put ("rocksdb.estimate-num-keys", out);

	// Estimated total number of bytes compaction needs to rewrite to get all levels down
	// to under target size. Not valid for other compactions than level-based.
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kEstimatePendingCompactionBytes, &out);
	json.put ("rocksdb.estimate-pending-compaction-bytes", out);

	//  Total size (bytes) of all SST files.
	//  WARNING: may slow down online queries if there are too many files.
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kTotalSstFilesSize, &out);
	json.put ("rocksdb.total-sst-files-size", out);

	// Block cache capacity.
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kBlockCacheCapacity, &out);
	json.put ("rocksdb.block-cache-capacity", out);

	// Memory size for the entries residing in block cache.
	db->GetAggregatedIntProperty (rocksdb::DB::Properties::kBlockCacheUsage, &out);
	json.put ("rocksdb.block-cache-usage", out);
}

// Explicitly instantiate
template class nano::block_store_partial<rocksdb::Slice, nano::rocksdb_store>;

// TODO: Seems to have worse performance??

/*
			unchecked table
			rocksdb::PlainTableOptions table_options;
			table_options.bloom_bits_per_key = 10;
			table_options.full_scan_mode = false;
			table_options.store_index_in_file = true;
			table_options.encoding_type = rocksdb::EncodingType::kPrefix; // TODO: Check this
			table_options.hash_table_ratio = 0.75;

			//	table_options.index_type = rocksdb::BlockBasedTableOptions::kHashSearch; // Only for block based ones

			table_options.user_key_len = sizeof (nano::unchecked_key); // Should be 64; // Size of unchecked_key
			table_factory.reset (rocksdb::NewPlainTableFactory (table_options));
			cf_options = get_cf_options (table_factory);
			cf_options.ttl = 0; // Not supported with plain table
			cf_options.memtable_prefix_bloom_size_ratio = 0.25;
			cf_options.prefix_extractor.reset (rocksdb::NewFixedPrefixTransform (32)); // unchecked root
			*/

// cf_options.optimize_filters_for_hits = true; // TODO: Use this when the key will exist more than not exist
