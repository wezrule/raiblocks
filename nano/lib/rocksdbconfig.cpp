#include <nano/lib/rocksdbconfig.hpp>
#include <nano/lib/tomlconfig.hpp>

nano::error nano::rocksdb_config::serialize_toml (nano::tomlconfig & toml) const
{
	toml.put ("enable", enable, "Whether to use the RocksDB backend for the ledger database.\ntype:bool");
	toml.put ("block_cache_multiplier", block_cache_multiplier, "This will .\ntype:uint64");
	toml.put ("io_threads", io_threads, "Number of threads to use with the background compaction and flushing. Number of hardware threads is recommended.\ntype:uint32");
	return toml.get_error ();
}

nano::error nano::rocksdb_config::deserialize_toml (nano::tomlconfig & toml)
{
	toml.get_optional<bool> ("enable", enable);
	toml.get_optional<uint64_t> ("block_cache_multiplier", block_cache_multiplier);
	toml.get_optional<unsigned> ("io_threads", io_threads);

	// Validate ranges
	if (io_threads == 0)
	{
		toml.get_error ().set ("io_threads must be non-zero");
	}
	if (block_cache_multiplier == 0)
	{
		toml.get_error ().set ("block_cache_multiplier must be non-zero");
	}

	return toml.get_error ();
}
