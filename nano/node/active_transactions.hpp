#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/node/confirmation_solicitor.hpp>
#include <nano/node/election.hpp>
#include <nano/node/gap_cache.hpp>
#include <nano/node/repcrawler.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/blockstore.hpp>
#include <nano/secure/common.hpp>

#include <boost/circular_buffer.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/thread/thread.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace mi = boost::multi_index;

namespace nano
{
class node;
class block;
class block_sideband;
class election;
class vote;
class transaction;
class confirmation_height_processor;

class conflict_info final
{
public:
	nano::qualified_root root;
	uint64_t difficulty;
	uint64_t adjusted_difficulty;
	std::shared_ptr<nano::election> election;
};

class cementable_account final
{
public:
	cementable_account (nano::account const & account_a, size_t blocks_uncemented_a);
	nano::account account;
	uint64_t blocks_uncemented{ 0 };
};

class election_timepoint final
{
public:
	std::chrono::steady_clock::time_point time;
	nano::qualified_root root;
};

class inactive_cache_information final
{
public:
	std::chrono::steady_clock::time_point arrival;
	nano::block_hash hash;
	std::vector<nano::account> voters;
	bool bootstrap_started{ false };
	bool confirmed{ false }; // Did item reach votes quorum? (minimum config value)
};

// Core class for determining consensus
// Holds all active blocks i.e. recently added blocks that need confirmation
class active_transactions final
{
	friend class nano::election;

	// clang-format off
	class tag_account {};
	class tag_difficulty {};
	class tag_root {};
	class tag_sequence {};
	class tag_uncemented {};
	class tag_arrival {};
	class tag_hash {};
	// clang-format on

public:
	explicit active_transactions (nano::node &, nano::confirmation_height_processor &);
	~active_transactions ();
	// Start an election for a block
	// Call action with confirmed block, may be different than what we started with
	// clang-format off
	std::pair<std::shared_ptr<nano::election>, bool> insert (std::shared_ptr<nano::block>, std::function<void(std::shared_ptr<nano::block>)> const & = [](std::shared_ptr<nano::block>) {});
	// clang-format on
	// Distinguishes replay votes, cannot be determined if the block is not in any election
	nano::vote_code vote (std::shared_ptr<nano::vote>);
	// Is the root of this block in the roots container
	bool active (nano::block const &);
	bool active (nano::qualified_root const &);
	std::shared_ptr<nano::election> election (nano::qualified_root const &) const;
	void update_difficulty (std::shared_ptr<nano::block>);
	void adjust_difficulty (nano::block_hash const &);
	void update_active_difficulty (nano::unique_lock<std::mutex> &);
	uint64_t active_difficulty ();
	uint64_t limited_active_difficulty ();
	std::deque<std::shared_ptr<nano::block>> list_blocks ();
	void erase (nano::block const &);
	bool empty ();
	size_t size ();
	void stop ();
	bool publish (std::shared_ptr<nano::block> block_a);
	boost::optional<nano::election_status_type> confirm_block (nano::transaction const &, std::shared_ptr<nano::block>);
	void block_cemented_callback (std::shared_ptr<nano::block> const & block_a);
	void block_already_cemented_callback (nano::block_hash const &);
	// clang-format off
	boost::multi_index_container<nano::conflict_info,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<nano::conflict_info, nano::qualified_root, &nano::conflict_info::root>>,
		mi::ordered_non_unique<mi::tag<tag_difficulty>,
			mi::member<nano::conflict_info, uint64_t, &nano::conflict_info::adjusted_difficulty>,
			std::greater<uint64_t>>>>
	roots;
	// clang-format on
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::election>> blocks;
	std::deque<nano::election_status> list_cemented ();
	std::deque<nano::election_status> recently_cemented;

	void add_recently_cemented (nano::election_status const &);
	void add_recently_confirmed (nano::qualified_root const &);
	void add_inactive_votes_cache (nano::block_hash const &, nano::account const &);
	nano::inactive_cache_information find_inactive_votes_cache (nano::block_hash const &);
	void erase_inactive_votes_cache (nano::block_hash const &);
	nano::confirmation_height_processor & confirmation_height_processor;
	nano::node & node;
	mutable std::mutex mutex;
	boost::circular_buffer<double> multipliers_cb;
	uint64_t trended_active_difficulty;
	size_t priority_cementable_frontiers_size ();
	size_t priority_wallet_cementable_frontiers_size ();
	boost::circular_buffer<double> difficulty_trend ();
	size_t inactive_votes_cache_size ();
	size_t election_winner_details_size ();
	void add_election_winner_details (nano::block_hash const &, std::shared_ptr<nano::election> const &);
	nano::confirmation_solicitor solicitor;

private:
	std::mutex election_winner_details_mutex;
	std::unordered_map<nano::block_hash, std::shared_ptr<nano::election>> election_winner_details;

	// Call action with confirmed block, may be different than what we started with
	// clang-format off
	std::pair<std::shared_ptr<nano::election>, bool> insert_impl (std::shared_ptr<nano::block>, std::function<void(std::shared_ptr<nano::block>)> const & = [](std::shared_ptr<nano::block>) {});
	// clang-format on
	void request_loop ();
	void search_frontiers (nano::transaction const &);
	void request_confirm (nano::unique_lock<std::mutex> &);
	nano::account next_frontier_account{ 0 };
	std::chrono::steady_clock::time_point next_frontier_check{ std::chrono::steady_clock::now () };
	nano::condition_variable condition;
	bool started{ false };
	std::atomic<bool> stopped{ false };

	// Maximum time an election can be kept active if it is extending the container
	std::chrono::seconds const election_time_to_live;

	// clang-format off
	boost::multi_index_container<nano::qualified_root,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequence>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::identity<nano::qualified_root>>>>
	recently_confirmed;
	using prioritize_num_uncemented = boost::multi_index_container<nano::cementable_account,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_account>,
			mi::member<nano::cementable_account, nano::account, &nano::cementable_account::account>>,
		mi::ordered_non_unique<mi::tag<tag_uncemented>,
			mi::member<nano::cementable_account, uint64_t, &nano::cementable_account::blocks_uncemented>,
			std::greater<uint64_t>>>>;
	// clang-format on
	prioritize_num_uncemented priority_wallet_cementable_frontiers;
	prioritize_num_uncemented priority_cementable_frontiers;
	void prioritize_frontiers_for_confirmation (nano::transaction const &, std::chrono::milliseconds, std::chrono::milliseconds);
	std::unordered_set<nano::wallet_id> wallet_ids_already_iterated;
	std::unordered_map<nano::wallet_id, nano::account> next_wallet_id_accounts;
	bool skip_wallets{ false };
	void prioritize_account_for_confirmation (prioritize_num_uncemented &, size_t &, nano::account const &, nano::account_info const &, uint64_t);
	static size_t constexpr max_priority_cementable_frontiers{ 100000 };
	static size_t constexpr confirmed_frontiers_max_pending_cut_off{ 1000 };
	// clang-format off
	using ordered_cache = boost::multi_index_container<nano::inactive_cache_information,
	mi::indexed_by<
		mi::ordered_non_unique<mi::tag<tag_arrival>,
			mi::member<nano::inactive_cache_information, std::chrono::steady_clock::time_point, &nano::inactive_cache_information::arrival>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<nano::inactive_cache_information, nano::block_hash, &nano::inactive_cache_information::hash>>>>;
	ordered_cache inactive_votes_cache;
	// clang-format on
	bool inactive_votes_bootstrap_check (std::vector<nano::account> const &, nano::block_hash const &, bool &);
	boost::thread thread;

	friend class confirmation_height_prioritize_frontiers_Test;
	friend class confirmation_height_prioritize_frontiers_overwrite_Test;
	friend class active_transactions_confirmation_consistency_Test;
	friend std::unique_ptr<container_info_component> collect_container_info (active_transactions &, const std::string &);
};

std::unique_ptr<container_info_component> collect_container_info (active_transactions & active_transactions, const std::string & name);
}
