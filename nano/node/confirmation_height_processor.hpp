#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/secure/blockstore.hpp>
#include <nano/secure/common.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace nano
{
class ledger;
class read_transaction;
class logger_mt;
class write_database_queue;

class confirmation_height_processor final
{
public: // TODO: Don't make it all public obviously..
	confirmation_height_processor (nano::ledger &, nano::write_database_queue &, std::chrono::milliseconds, nano::logger_mt &);
	~confirmation_height_processor ();
	void pause ();
	void unpause ();
	void stop ();
	void add (nano::block_hash const & hash_a);
	void run ();

	size_t pending_size ();
	bool is_processing_block (nano::block_hash const &);
	nano::block_hash current ();

class callback_data final
{
public:
	callback_data (std::shared_ptr<nano::block> const & block_a, nano::block_sideband const & sideband_a);
	std::shared_ptr<nano::block> block;
	nano::block_sideband sideband;
};

class top_hash final
{
public:
	nano::block_hash top;
	boost::optional<nano::block_hash> next;
};

class confirmed_info
{
public:
	confirmed_info (uint64_t confirmed_height_a, nano::block_hash const & iterated_frontier);
	uint64_t confirmed_height; // Can be determined from iterated_frontier though (-1)
	nano::block_hash iterated_frontier;
};

std::unordered_map<account, confirmed_info> confirmed;
std::atomic<uint64_t> confirmed_size{ 0 };

class conf_height_details final
{
public:
	conf_height_details (nano::account const &, nano::block_hash const &, uint64_t, nano::block_hash const &, boost::optional<nano::block_hash>);
	nano::account account;
	nano::block_hash hash;
	uint64_t num_blocks_confirmed;
	nano::block_hash top_level; // Used to be top_most_level
	boost::optional<nano::block_hash> next;
};

class preparation_data final
{
public:
	nano::transaction const & transaction;
	nano::block_hash const & top_most_send_block;
	bool already_cemented;
	boost::circular_buffer_space_optimized<nano::block_hash> & checkpoints;
	decltype (confirmed.begin ()) account_it;
	nano::confirmation_height_info const & confirmation_height_info;
	nano::account const & account;
	uint64_t num_contiguous_non_receive_blocks;
	nano::block_hash const & bottom_most;
	boost::optional<conf_height_details> & receive_details;
	boost::optional<top_hash> & next_in_receive_chain;
};

	std::mutex mutex;
	std::unordered_set<nano::block_hash> pending;
	std::vector<std::function<void(callback_data)>> cemented_observers;
	/** This is the last block popped off the confirmation height pending collection */
	nano::block_hash original_hash{ 0 };
	friend class confirmation_height_processor;
	friend class confirmation_height_pending_observer_callbacks_Test;
	friend class confirmation_height_dependent_election_Test;
	friend class confirmation_height_dependent_election_after_already_cemented_Test;

	void add_cemented_observer (std::function<void(callback_data)> const &);

	nano::ledger & ledger;
	nano::logger_mt & logger;
	nano::condition_variable condition;
	std::atomic<bool> stopped{ false };
	std::atomic<bool> paused{ false };
	nano::write_database_queue & write_database_queue;
	std::chrono::milliseconds batch_separate_pending_min_time;
	nano::timer<std::chrono::milliseconds> timer;

	/** The maximum amount of accounts to iterate over while writing */
	static uint64_t constexpr batch_block_write_size = 2; // batch_write_size = 2; // 2048;

	/** The maximum number of blocks to be read in while iterating over a long account chain */
	static uint64_t constexpr batch_read_size = 4096;

	class write_details final
	{
	public:
		write_details (nano::account const &, uint64_t, nano::block_hash const &);
		nano::account account;
		uint64_t num_blocks_confirmed;
		// This is the first block hash (boittom most) which is not cemented
		nano::block_hash start_hash;
	};

	class receive_source_pair final
	{
	public:
		receive_source_pair (conf_height_details const &, const nano::block_hash &);

		conf_height_details receive_details;
		nano::block_hash source_hash;
	};

	bool cement_blocks ();

	static uint32_t const max_accounts {2}; // or max blocks?

	std::deque<write_details> pending_writes;
	std::atomic<uint64_t> pending_writes_size{ 0 };
	static uint32_t constexpr pending_writes_max_size { max_accounts };


	std::thread thread;

	friend std::unique_ptr<container_info_component> collect_container_info (confirmation_height_processor &, const std::string &);

private:
	top_hash get_next_block (boost::optional<top_hash> const &, boost::circular_buffer_space_optimized<nano::block_hash> const &, boost::circular_buffer_space_optimized<receive_source_pair> const & receive_source_pairs, boost::optional<conf_height_details> &);
	nano::block_hash get_least_unconfirmed_hash_from_top_level (nano::transaction const &, nano::block_hash const &, nano::account const &, nano::confirmation_height_info const &, uint64_t);
	void notify_observers (std::vector<callback_data> const & cemented_blocks);
	void prepare_iterated_blocks_for_cementing (preparation_data const &);
	void set_next_hash ();
	void process ();
	bool iterate (nano::read_transaction const &, nano::block_hash const & start_hash_a, uint64_t &, boost::circular_buffer_space_optimized<nano::block_hash> &, nano::block_hash &, nano::block_hash const &, boost::circular_buffer_space_optimized<receive_source_pair> &, nano::account const &);
};

std::unique_ptr<container_info_component> collect_container_info (confirmation_height_processor &, const std::string &);
}
