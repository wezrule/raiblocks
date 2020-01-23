#include <nano/lib/logger_mt.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/confirmation_height_processor.hpp>
#include <nano/node/write_database_queue.hpp>
#include <nano/secure/blockstore.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>

#include <boost/optional.hpp>

#include <cassert>
#include <numeric>

nano::confirmation_height_processor::confirmation_height_processor (nano::ledger & ledger_a, nano::write_database_queue & write_database_queue_a, std::chrono::milliseconds batch_separate_pending_min_time_a, nano::logger_mt & logger_a) :
ledger (ledger_a),
logger (logger_a),
write_database_queue (write_database_queue_a),
batch_separate_pending_min_time (batch_separate_pending_min_time_a),
thread ([this]() {
	nano::thread_role::set (nano::thread_role::name::confirmation_height_processing);
	this->run ();
})
{
}

nano::confirmation_height_processor::~confirmation_height_processor ()
{
	stop ();
}

void nano::confirmation_height_processor::stop ()
{
	stopped = true;
	condition.notify_one ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::confirmation_height_processor::run ()
{
	nano::unique_lock<std::mutex> lk (mutex);
	while (!stopped)
	{
		if (!paused && !awaiting_processing.empty ())
		{
			lk.unlock ();
			if (pending_writes.empty ())
			{
				// Separate blocks which are pending confirmation height can be batched by a minimum processing time (to improve lmdb disk write performance),
				// so make sure the slate is clean when a new batch is starting.
				accounts_confirmed_info.clear ();
				timer.restart ();
			}
			set_next_hash ();
			process ();
			lk.lock ();
		}
		else
		{
			// If there are blocks pending cementing, then make sure we flush out the remaining writes
			if (!pending_writes.empty ())
			{
				lk.unlock ();
				auto scoped_write_guard = write_database_queue.wait (nano::writer::confirmation_height);
				cement_blocks ();
				lk.lock ();
				original_hash.clear ();
			}
			else
			{
				original_hash.clear ();
				condition.wait (lk);
			}
		}
	}
}

// Pausing only affects processing new blocks, not the current one being processed. Currently only used in tests
void nano::confirmation_height_processor::pause ()
{
	paused = true;
}

void nano::confirmation_height_processor::unpause ()
{
	paused = false;
	condition.notify_one ();
}

void nano::confirmation_height_processor::add (nano::block_hash const & hash_a)
{
	{
		nano::lock_guard<std::mutex> lk (mutex);
		awaiting_processing.insert (hash_a);
	}
	condition.notify_one ();
}

// The next block hash to iterate over, the priority is as follows:
// 1 - The next block in the account chain for the last processed receive (if there is any)
// 2 - The next receive block which is closest to genesis
// 3 - The last checkpoint hit.
// 4 - The hash that was passed in originally. Either all checkpoints were exhausted (this can happen when there are many accounts to genesis)
//     or all other blocks have been processed.
nano::confirmation_height_processor::top_hash nano::confirmation_height_processor::get_next_block (boost::optional<top_hash> const & next_in_receive_chain_a, boost::circular_buffer_space_optimized<nano::block_hash> const & checkpoints_a, boost::circular_buffer_space_optimized<receive_source_pair> const & receive_source_pairs, boost::optional<conf_height_details> & receive_details_a)
{
	top_hash next;
	if (next_in_receive_chain_a.is_initialized ())
	{
		next = *next_in_receive_chain_a;
	}
	else if (!receive_source_pairs.empty ())
	{
		auto next_receive_source_pair = receive_source_pairs.back ();
		receive_details_a = next_receive_source_pair.receive_details;
		next = { next_receive_source_pair.source_hash, receive_details_a->next };
	}
	else if (!checkpoints_a.empty ())
	{
		next = { checkpoints_a.back (), boost::none };
	}
	else
	{
		next = { original_hash, boost::none };
	}

	return next;
}

nano::block_hash nano::confirmation_height_processor::get_least_unconfirmed_hash_from_top_level (nano::transaction const & transaction_a, nano::block_hash const & hash_a, nano::account const & account_a, nano::confirmation_height_info const & confirmation_height_info_a, uint64_t block_height_a)
{
	nano::block_hash least_unconfirmed_hash = hash_a;
	bool already_cemented = false;
	nano::block_sideband sideband;
	if (confirmation_height_info_a.height != 0)
	{
		if (block_height_a > confirmation_height_info_a.height)
		{
			auto block_l = ledger.store.block_get (transaction_a, confirmation_height_info_a.frontier, &sideband);
			least_unconfirmed_hash = sideband.successor;
		}
	}
	else
	{
		// No blocks have been confirmed, so the first block will be the open block
		nano::account_info account_info;
		ledger.store.account_get (transaction_a, account_a, account_info);
		least_unconfirmed_hash = account_info.open_block;
	}
	return least_unconfirmed_hash;
}

void nano::confirmation_height_processor::set_next_hash ()
{
	nano::lock_guard<std::mutex> guard (mutex);
	assert (!awaiting_processing.empty ());
	original_hash = *awaiting_processing.begin ();

	if (original_hash.to_string () == "66B75FBA2E8EAC47B53910A9F725ACD74EB21F84D9C7171490F7224BF47BA213")
	{
		// Breakpoint!
		std::cout << "hit nano_1m1ag8pgrnd4ofe36c9kkrtnrreewxjfagkfc6hrak3kez7abtbj6gi7ii6x" << std::endl;
	}

	//std::cout << "Original hash: " << original_hash.to_string () << std::endl;
	original_hashes_pending.insert (original_hash);
	awaiting_processing.erase (original_hash);
}

void nano::confirmation_height_processor::process ()
{
	nano::block_sideband sideband;
	nano::confirmation_height_info confirmation_height_info;
	auto transaction (ledger.store.tx_begin_read ());

	boost::optional<top_hash> next_in_receive_chain;
	boost::circular_buffer_space_optimized<nano::block_hash> checkpoints{ max_items };
	boost::circular_buffer_space_optimized<receive_source_pair> receive_source_pairs{ max_items };
	nano::block_hash current;
	do
	{
		boost::optional<conf_height_details> receive_details;
		auto hash_to_process = get_next_block (next_in_receive_chain, checkpoints, receive_source_pairs, receive_details);
		current = hash_to_process.top;

		if (current.to_string () == "66B75FBA2E8EAC47B53910A9F725ACD74EB21F84D9C7171490F7224BF47BA213")
		{
			bool cheese = false;
		}

		auto top_level_hash = current;
		nano::account account (ledger.store.block_account (transaction, current));
		release_assert (!ledger.store.confirmation_height_get (transaction, account, confirmation_height_info));

		// Checks if we have encountered this account before but not commited changes yet, if so then update the cached confirmation height
		auto account_it = accounts_confirmed_info.find (account);
		if (account_it != accounts_confirmed_info.cend () && account_it->second.confirmed_height > confirmation_height_info.height)
		{
			confirmation_height_info.height = account_it->second.confirmed_height;
			confirmation_height_info.frontier = account_it->second.iterated_frontier;
		}

		auto block_height (ledger.store.block_account_height (transaction, current));
		bool already_cemented = confirmation_height_info.height >= block_height;
		// If we are not already at the bottom of the account chain (1 above cemented frontier) then find it

		if (current.to_string () == "66B75FBA2E8EAC47B53910A9F725ACD74EB21F84D9C7171490F7224BF47BA213")
		{
			bool cheese = false;
		}

		if (!already_cemented && !hash_to_process.next.is_initialized () && block_height - confirmation_height_info.height > 1)
		{
			current = get_least_unconfirmed_hash_from_top_level (transaction, current, account, confirmation_height_info, block_height);
		}

		if (current.to_string () == "562EA861A93BE5534DB0DEF4C8DA2E7CEC0CA47B2BEA2C2C55E11FC254522969")
		{
			bool cheese = false;
		}

		uint64_t num_contiguous_non_receive_blocks = 0;
		auto top_most_non_receive_block_hash = current;

		if (top_most_non_receive_block_hash.is_zero ())
		{
			bool poop = false;
		}

		bool hit_receive = false;
		if (!already_cemented)
		{
			hit_receive = iterate (transaction, current, num_contiguous_non_receive_blocks, checkpoints, top_most_non_receive_block_hash, top_level_hash, receive_source_pairs, account);
		}

		if (top_most_non_receive_block_hash.is_zero ())
		{
			bool wazzza = false;
		}

		// Exit early when the processor has been stopped, otherwise this function may take a
		// while (and hence keep the process running) if updating a long chain.
		if (stopped)
		{
			break;
		}

		// next_in_receive_chain can be modified when writing, so need to cache it here before resetting
		auto is_set = next_in_receive_chain.is_initialized ();
		next_in_receive_chain = boost::none;

		// Need to also handle the case where we are hitting receives where the sends below should be confirmed
		if (!hit_receive || (receive_source_pairs.size () == 1 && top_most_non_receive_block_hash != current))
		{
			preparation_data preparation_data{ transaction, top_most_non_receive_block_hash, already_cemented, checkpoints, account_it, confirmation_height_info, account, num_contiguous_non_receive_blocks, current, receive_details, next_in_receive_chain };
			prepare_iterated_blocks_for_cementing (preparation_data);

			// If used the top level, don't pop off the receive source pair because it wasn't used
			if (!is_set && !receive_source_pairs.empty ())
			{
				receive_source_pairs.pop_back ();
			}

			// Accumulate the number of blocks
			auto total_pending_write_block_count = std::accumulate (pending_writes.cbegin (), pending_writes.cend (), uint64_t (0), [](uint64_t total, auto const & write_details_a) {
				return total += write_details_a.num_blocks_confirmed;
			});

			auto max_batch_write_size_reached = (total_pending_write_block_count >= batch_write_size);
			// When there are a lot of pending confirmation height blocks, it is more efficient to
			// bulk some of them up to enable better write performance which becomes the bottleneck.
			auto min_time_exceeded = (timer.since_start () >= batch_separate_pending_min_time);
			auto finished_iterating = current == original_hash;
			auto non_awaiting_processing = awaiting_processing_size () == 0;
			auto should_output = finished_iterating && (non_awaiting_processing || min_time_exceeded);
			auto force_write = pending_writes.size () >= pending_writes_max_size || accounts_confirmed_info.size () >= pending_writes_max_size;

			if (((max_batch_write_size_reached || should_output) && !pending_writes.empty ()) || force_write)
			{
				//std::cout << "WRITING" << std::endl;

				bool error = false;
				// If nothing is currently using the database write lock then write the cemented pending blocks otherwise continue iterating
				if (write_database_queue.process (nano::writer::confirmation_height))
				{
					auto scoped_write_guard = write_database_queue.pop ();
					error = cement_blocks ();
				}
				else if (force_write)
				{
					auto scoped_write_guard = write_database_queue.wait (nano::writer::confirmation_height);
					error = cement_blocks ();
				}
				// Don't set any more cemented blocks from the original hash if an inconsistency is found
				if (error)
				{
					checkpoints.clear ();
					break;
				}
			}
		}

		transaction.refresh ();
	} while (!receive_source_pairs.empty () || current != original_hash);

	assert (checkpoints.empty ());
}

bool nano::confirmation_height_processor::iterate (nano::read_transaction const & transaction_a, nano::block_hash const & start_hash_a, uint64_t & num_contiguous_non_receive_blocks_a, boost::circular_buffer_space_optimized<nano::block_hash> & checkpoints_a, nano::block_hash & top_most_non_receive_block_hash_a, nano::block_hash const & top_level_hash_a, boost::circular_buffer_space_optimized<receive_source_pair> & receive_source_pairs_a, nano::account const & account_a)
{
	uint64_t num_contiguous_non_receive_blocks_above_first_receive = 0;
	bool reached_target = false;
	bool hit_receive = false;
	auto hash = start_hash_a;
	nano::block_sideband sideband;
	while (!hash.is_zero () && !reached_target && !stopped)
	{
		// Keep iterating going up until we either reach the desired block or a receive the
		auto block = ledger.store.block_get (transaction_a, hash, &sideband);
		auto source (block->source ());
		if (source.is_zero ())
		{
			source = block->link ();
		}

		if (!source.is_zero () && !ledger.is_epoch_link (source) && ledger.store.source_exists (transaction_a, source))
		{
			// This is a second receive block hit in this chain
			if (hit_receive)
			{
				reached_target = true;
			}
			else
			{
				hit_receive = true;
				// Set the number of blocks to 1 initially, this will be updated for all blocks above this receive, until the next receive is reached
				receive_source_pairs_a.push_back (receive_source_pair{ conf_height_details{ account_a, hash, 1, top_level_hash_a, boost::none }, source });
				// Store a checkpoint every max_items so that we can always traverse a long number of accounts to genesis
				if (receive_source_pairs_a.size () % max_items == 0)
				{
					checkpoints_a.push_back (top_level_hash_a);
				}
				hash = sideband.successor;
			}
		}
		else
		{
			if (hit_receive)
			{
				++num_contiguous_non_receive_blocks_above_first_receive;
			}
			else
			{
				// Found a send/change/epoch block which isn't the desired top level
				++num_contiguous_non_receive_blocks_a;
				top_most_non_receive_block_hash_a = hash;
				if (hash == top_level_hash_a)
				{
					reached_target = true;
				}
			}

			hash = sideband.successor;

			if (hash.to_string () == "562EA861A93BE5534DB0DEF4C8DA2E7CEC0CA47B2BEA2C2C55E11FC254522969")
			{
				bool cheese = false;
			}
		}

		// We could be traversing a very large account so we don't want to open read transactions for too long.
		if ((num_contiguous_non_receive_blocks_a > 0) && num_contiguous_non_receive_blocks_a % batch_read_size == 0)
		{
			transaction_a.refresh ();
		}
	}

	// Any blocks above the first receive hit (before the next one) can be implicitly cemented when that receive is cemented.
	if (num_contiguous_non_receive_blocks_above_first_receive > 0)
	{
		assert (hit_receive);
		auto next{ !sideband.successor.is_zero () ? boost::optional<nano::block_hash> (sideband.successor) : boost::none };
		auto last_receive_block_details = receive_source_pairs_a.back ().receive_details;
		last_receive_block_details.num_blocks_confirmed = num_contiguous_non_receive_blocks_above_first_receive + 1;

		// TODO:
		/*nano::block_sideband sideband1;
		auto block = ledger.store.block_get (transaction_a, last_receive_block_details.hash, &sideband1);
		if (sideband1.height != last_receive_block_details. receive_account_it->second.confirmed_height)
		{
			bool cheese = false;
		}*/


		last_receive_block_details.next = next;
	}

	return hit_receive;
}

// Once the path to genesis have been iterated to, we can begin to cement the lowest blocks in the accounts. This sets up
// the non-receive blocks which have been iterated for an account, and the associated receive block.
void nano::confirmation_height_processor::prepare_iterated_blocks_for_cementing (preparation_data const & preparation_data_a)
{
	if (!preparation_data_a.already_cemented)
	{
		// Add the non-receive blocks iterated for this account
		nano::block_sideband sideband;
		auto block = ledger.store.block_get (preparation_data_a.transaction, preparation_data_a.top_most_non_receive_block_hash, &sideband);
		if (!block)
		{
			std::cout << "Block not found: " << preparation_data_a.top_most_non_receive_block_hash.to_string () << std::endl;
		}
		//TODO: Use: auto block_height = (ledger.store.block_account_height (preparation_data_a.transaction, preparation_data_a.top_most_non_receive_block_hash));
		auto block_height = sideband.height;

		if (block_height > preparation_data_a.confirmation_height_info.height)
		{
			confirmed_info confirmed_info_l{ block_height, preparation_data_a.top_most_non_receive_block_hash };
			if (preparation_data_a.account_it != accounts_confirmed_info.cend ())
			{
				preparation_data_a.account_it->second = confirmed_info_l;
			}
			else
			{
				accounts_confirmed_info.emplace (preparation_data_a.account, confirmed_info_l);
				accounts_confirmed_info_size = accounts_confirmed_info.size ();
			}

			preparation_data_a.checkpoints.erase (std::remove (preparation_data_a.checkpoints.begin (), preparation_data_a.checkpoints.end (), preparation_data_a.top_most_non_receive_block_hash), preparation_data_a.checkpoints.end ());
			pending_writes.emplace_back (preparation_data_a.account, preparation_data_a.num_contiguous_non_receive_blocks, preparation_data_a.bottom_most);
			++pending_writes_size;
		}
	}

	// Add the receive block and all non-receive blocks above that one
	auto & receive_details = preparation_data_a.receive_details;
	if (receive_details)
	{
		auto receive_account_it = accounts_confirmed_info.find (receive_details->account);
		if (receive_account_it != accounts_confirmed_info.cend ())
		{
			auto current_height = receive_account_it->second.confirmed_height;
//			receive_details->num_blocks_confirmed = receive_details->num_blocks_confirmed;

			// TODO:
			nano::block_sideband sideband1;
			auto block = ledger.store.block_get (preparation_data_a.transaction, receive_details->hash, &sideband1);
			if (sideband1.height != current_height + receive_details->num_blocks_confirmed)
			{
				bool cheese = false;
			}

			
			receive_account_it->second.confirmed_height = current_height + receive_details->num_blocks_confirmed;
			receive_account_it->second.iterated_frontier = receive_details->hash;
		}
		else
		{
			nano::block_sideband sideband;
			auto block = ledger.store.block_get (preparation_data_a.transaction, receive_details->hash, &sideband);
			assert (block);
			accounts_confirmed_info.emplace (receive_details->account, confirmed_info{ sideband.height, receive_details->hash });
			accounts_confirmed_info_size = accounts_confirmed_info.size ();
		}

		preparation_data_a.next_in_receive_chain = top_hash{ receive_details->top_level, receive_details->next };

		if (receive_details->hash == receive_details->top_level)
		{
			preparation_data_a.checkpoints.erase (std::remove (preparation_data_a.checkpoints.begin (), preparation_data_a.checkpoints.end (), receive_details->hash), preparation_data_a.checkpoints.end ());
		}

		pending_writes.emplace_back (receive_details->account, receive_details->num_blocks_confirmed, receive_details->hash);
		++pending_writes_size;
	}
}

bool nano::confirmation_height_processor::cement_blocks ()
{
	// Will contain all blocks that have been cemented (bounded by batch_write_size)
	// and will get run through the cemented observer callback
	std::vector<callback_data> cemented_blocks;
	{
		// This only writes to the confirmation_height table and is the only place to do so in a single process
		auto transaction (ledger.store.tx_begin_write ({}, { nano::tables::confirmation_height }));

		// Cement all pending entries, each entry is specific to an account and contains the least amount
		// of blocks to retain consistent cementing across all account chains to genesis.
		while (!pending_writes.empty ())
		{
			const auto & pending = pending_writes.front ();

			auto write_confirmation_height = [& account = pending.account, &ledger = ledger, &transaction](uint64_t num_blocks_cemented, uint64_t confirmation_height, nano::block_hash const & confirmed_frontier) {
				ledger.store.confirmation_height_put (transaction, account, nano::confirmation_height_info{ confirmation_height, confirmed_frontier });
				ledger.cache.cemented_count += num_blocks_cemented;
				ledger.stats.add (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in, num_blocks_cemented);

				/*
				nano::confirmation_height_info confirmation_height_info_l;
				ledger.store.confirmation_height_get (transaction, account, confirmation_height_info_l);

				auto cemented_count = 0;
				for (auto i (ledger.store.confirmation_height_begin (transaction)), n (ledger.store.confirmation_height_end ()); i != n; ++i)
				{
					cemented_count += i->second.height;
				}
				*/
				// TODO: Something has gone wrong
				//auto count = ledger.cache.cemented_count.load ();
//				auto count1 = cemented_count;// ledger.conf cemented confirmation_height_count (transaction);
				//if (count != cemented_count)
				//{
					//int cheese = ledger.cache.cemented_count;
					//std::cout << "pew pew" << std::endl;
				//}
			};

			nano::block_sideband sideband;
			auto block = ledger.store.block_get (transaction, pending.start_hash, &sideband);
			auto confirmation_height = sideband.height + pending.num_blocks_confirmed - 1;
			auto start_height = sideband.height;

			//std::cout << "PENDING start: " << pending.start_hash.to_string () << std::endl;

			// TODO: Remove after
			/*nano::confirmation_height_info confirmation_height_info;
			auto error = ledger.store.confirmation_height_get (transaction, pending.account, confirmation_height_info);
			release_assert (!error);
			if (confirmation_height != confirmation_height_info.height)
			{
				std::

				release_assert (false);
			}
			*/




			// The highest block which is currently cemented
			auto new_cemented_frontier = pending.start_hash;

			auto total_blocks_cemented = 0;
			auto num_blocks_iterated = 0;

			// Cementing starts from the bottom of the chain and works upwards. This is because chains can have effectively
			// an infinite number of send/change blocks in a row. We don't want to hold the write transaction open for too long.
			for (; pending.num_blocks_confirmed - num_blocks_iterated != 0; ++num_blocks_iterated)
			{
				if (!block)
				{
					logger.always_log ("Failed to write confirmation height for: ", new_cemented_frontier.to_string ());
					ledger.stats.inc (nano::stat::type::confirmation_height, nano::stat::detail::invalid_block);
					pending_writes.clear ();
					pending_writes_size = 0;
					return true;
				}

				//std::cout << "Cement block: " << block->hash ().to_string () << std::endl;
				cemented_blocks.emplace_back (block, sideband);

				// We have likely hit a long chain, flush these callbacks and continue
				if (cemented_blocks.size () == batch_write_size)
				{
					auto num_blocks_cemented = num_blocks_iterated - total_blocks_cemented + 1;
					total_blocks_cemented += num_blocks_cemented;
					write_confirmation_height (num_blocks_cemented, start_height + total_blocks_cemented - 1, new_cemented_frontier);
					transaction.commit ();
					notify_observers (cemented_blocks);
					cemented_blocks.clear ();
					transaction.renew ();
				}

				// Get the next block in the chain until we have reached the final desired one
				auto last_iteration = (pending.num_blocks_confirmed - num_blocks_iterated) == 1;
				if (!last_iteration)
				{
					new_cemented_frontier = sideband.successor;
					block = ledger.store.block_get (transaction, new_cemented_frontier, &sideband);
				}
			}

			auto num_blocks_cemented = pending.num_blocks_confirmed - total_blocks_cemented;
			write_confirmation_height (num_blocks_cemented, confirmation_height, new_cemented_frontier);

			auto it = accounts_confirmed_info.find (pending.account);
			if (it == accounts_confirmed_info.cend ())
			{
				std::cout << "Pending Account " << pending.account.to_account () << std::endl;
			}
			assert (it != accounts_confirmed_info.cend ());
			if (it != accounts_confirmed_info.cend () && it->second.confirmed_height == confirmation_height)
			{
				accounts_confirmed_info.erase (pending.account);
				accounts_confirmed_info_size = accounts_confirmed_info.size ();
			}
			pending_writes.pop_front ();
			--pending_writes_size;
		}
	}

	notify_observers (cemented_blocks);

	assert (pending_writes.empty ());
	assert (pending_writes_size == 0);
	nano::lock_guard<std::mutex> guard (mutex);
	original_hashes_pending.clear ();
	return false;
}

void nano::confirmation_height_processor::add_cemented_observer (std::function<void(callback_data)> const & callback_a)
{
	nano::lock_guard<std::mutex> guard (mutex);
	cemented_observers.push_back (callback_a);
}

void nano::confirmation_height_processor::add_cemented_batch_finished_observer (std::function<void()> const & callback_a)
{
	nano::lock_guard<std::mutex> guard (mutex);
	cemented_batch_finished_observers.push_back (callback_a);
}

void nano::confirmation_height_processor::notify_observers (std::vector<callback_data> const & cemented_blocks)
{
	for (auto const & block_callback_data : cemented_blocks)
	{
		for (auto const & observer : cemented_observers)
		{
			observer (block_callback_data);
		}
	}

	if (!cemented_blocks.empty ())
	{
		for (auto const & observer : cemented_batch_finished_observers)
		{
			observer ();
		}
	}

	if ((ledger.cache.cemented_count - 1) != ledger.stats.count (nano::stat::type::observer, nano::stat::detail::all, nano::stat::dir::out))
	{
		std::cout << "cheesey poofs" << std::endl;
	}
}

nano::confirmation_height_processor::conf_height_details::conf_height_details (nano::account const & account_a, nano::block_hash const & hash_a, uint64_t num_blocks_confirmed_a, nano::block_hash const & top_level_a, boost::optional<nano::block_hash> next_a) :
hash (hash_a),
num_blocks_confirmed (num_blocks_confirmed_a),
top_level (top_level_a),
account (account_a),
next (next_a)
{
}

nano::confirmation_height_processor::write_details::write_details (nano::account const & account_a, uint64_t num_blocks_confirmed_a, nano::block_hash const & start_hash_a) :
num_blocks_confirmed (num_blocks_confirmed_a),
start_hash (start_hash_a),
account (account_a)
{
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (confirmation_height_processor & confirmation_height_processor_a, const std::string & name_a)
{
	auto composite = std::make_unique<container_info_composite> (name_a);

	size_t cemented_observers_count;
	{
		nano::lock_guard<std::mutex> guard (confirmation_height_processor_a.mutex);
		cemented_observers_count = confirmation_height_processor_a.cemented_observers.size ();
	}

	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "cemented_observers", cemented_observers_count, sizeof (decltype (confirmation_height_processor_a.cemented_observers)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pending_writes", confirmation_height_processor_a.pending_writes_size, sizeof (decltype (confirmation_height_processor_a.pending_writes)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "accounts_confirmed_info", confirmation_height_processor_a.accounts_confirmed_info_size, sizeof (decltype (confirmation_height_processor_a.accounts_confirmed_info)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "awaiting_processing", confirmation_height_processor_a.awaiting_processing_size (), sizeof (decltype (confirmation_height_processor_a.awaiting_processing)::value_type) }));
	return composite;
}

size_t nano::confirmation_height_processor::awaiting_processing_size ()
{
	nano::lock_guard<std::mutex> guard (mutex);
	return awaiting_processing.size ();
}

bool nano::confirmation_height_processor::is_processing_block (nano::block_hash const & hash_a)
{
	nano::lock_guard<std::mutex> guard (mutex);
	return original_hashes_pending.find (hash_a) != original_hashes_pending.cend () || awaiting_processing.find (hash_a) != awaiting_processing.cend ();
}

nano::block_hash nano::confirmation_height_processor::current ()
{
	nano::lock_guard<std::mutex> lk (mutex);
	return original_hash;
}

nano::confirmation_height_processor::receive_source_pair::receive_source_pair (confirmation_height_processor::conf_height_details const & receive_details_a, const block_hash & source_a) :
receive_details (receive_details_a),
source_hash (source_a)
{
}

nano::confirmation_height_processor::confirmed_info::confirmed_info (uint64_t confirmed_height_a, nano::block_hash const & iterated_frontier_a) :
confirmed_height (confirmed_height_a),
iterated_frontier (iterated_frontier_a)
{
}

nano::confirmation_height_processor::callback_data::callback_data (std::shared_ptr<nano::block> const & block_a, nano::block_sideband const & sideband_a) :
block (block_a),
sideband (sideband_a)
{
}
