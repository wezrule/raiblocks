#include <nano/lib/stats.hpp>
#include <nano/node/confirmation_height_processor.hpp>
#include <nano/node/write_database_queue.hpp>

#include <nano/secure/ledger.hpp>

#include <numeric>

void nano::confirmation_height_processor::process_unbounded ()
{
	std::shared_ptr<conf_height_details_unbounded> receive_details;
	auto current = original_hash;
	orig_block_callbacks_required.clear ();

	std::vector<receive_source_pair_unbounded> receive_source_pairs;
	release_assert (receive_source_pairs.empty ());

	auto read_transaction (ledger.store.tx_begin_read ());

	do
	{
		if (!receive_source_pairs.empty ())
		{
			receive_details = receive_source_pairs.back ().receive_details;
			current = receive_source_pairs.back ().source_hash;
		}
		else
		{
			// If receive_details is set then this is the final iteration and we are back to the original chain.
			// We need to confirm any blocks below the original hash (incl self) and the first receive block
			// (if the original block is not already a receive)
			if (receive_details)
			{
				current = original_hash;
				receive_details = nullptr;
			}
		}

		nano::block_sideband sideband;
		auto block (ledger.store.block_get (read_transaction, current, &sideband));
		nano::account account (block->account ());
		if (account.is_zero ())
		{
			account = sideband.account;
		}

		auto block_height = sideband.height;
		nano::confirmation_height_info confirmation_height_info;
		release_assert (!ledger.store.confirmation_height_get (read_transaction, account, confirmation_height_info));
		auto confirmation_height = confirmation_height_info.height;
		auto iterated_height = confirmation_height;
		auto account_it = confirmed_iterated_pairs_unbounded.find (account);
		if (account_it != confirmed_iterated_pairs_unbounded.cend ())
		{
			if (account_it->second.confirmed_height > confirmation_height)
			{
				confirmation_height = account_it->second.confirmed_height;
				iterated_height = confirmation_height;
			}
			if (account_it->second.iterated_height > iterated_height)
			{
				iterated_height = account_it->second.iterated_height;
			}
		}

		auto count_before_receive = receive_source_pairs.size ();
		std::vector<callback_data> block_callbacks_required;
		auto already_traversed = iterated_height >= block_height; 
		if (!already_traversed)
		{
			collect_unconfirmed_receive_and_sources_for_account_unbounded (block_height, iterated_height, current, account, read_transaction, receive_source_pairs, block_callbacks_required);
		}

		// Exit early when the processor has been stopped, otherwise this function may take a
		// while (and hence keep the process running) if updating a long chain.
		if (stopped)
		{
			break;
		}

		// No longer need the read transaction
		read_transaction.reset ();

		// If this adds no more open or receive blocks, then we can now confirm this account as well as the linked open/receive block
		// Collect as pending any writes to the database and do them in bulk after a certain time.
		auto confirmed_receives_pending = (count_before_receive != receive_source_pairs.size ());
		if (!confirmed_receives_pending)
		{
			preparation_data_unbounded preparation_data{ block_height, confirmation_height, iterated_height, account_it, account, receive_details, already_traversed, current, block_callbacks_required };
			prepare_iterated_blocks_for_cementing_unbounded (preparation_data);

			if (!receive_source_pairs.empty ())
			{
				// Pop from the end
				receive_source_pairs.erase (receive_source_pairs.end () - 1);
			}
		}
		else if (block_height > iterated_height)
		{
			if (account_it != confirmed_iterated_pairs_unbounded.cend ())
			{
				account_it->second.iterated_height = block_height;
			}
			else
			{
				confirmed_iterated_pairs_unbounded.emplace (account, confirmed_iterated_pair_unbounded{ confirmation_height, block_height });
			}
		}

		auto max_write_size_reached = (pending_writes_unbounded.size () >= batch_write_size);
		// When there are a lot of pending confirmation height blocks, it is more efficient to
		// bulk some of them up to enable better write performance which becomes the bottleneck.
		auto min_time_exceeded = (timer.since_start () >= batch_separate_pending_min_time);
		auto finished_iterating = receive_source_pairs.empty ();
		auto no_pending = awaiting_processing_size ();
		auto should_output = finished_iterating && (no_pending || min_time_exceeded);

		if ((max_write_size_reached || should_output) && !pending_writes_unbounded.empty ())
		{
			if (write_database_queue.process (nano::writer::confirmation_height))
			{
				auto scoped_write_guard = write_database_queue.pop ();
				auto error = cement_blocks_unbounded ();
				// Don't set any more blocks as confirmed from the original hash if an inconsistency is found
				if (error)
				{
					break;
				}
			}
		}

		read_transaction.renew ();
	} while (!receive_source_pairs.empty () || current != original_hash);
}

void nano::confirmation_height_processor::collect_unconfirmed_receive_and_sources_for_account_unbounded (uint64_t block_height_a, uint64_t confirmation_height_a, nano::block_hash const & hash_a, nano::account const & account_a, nano::read_transaction const & transaction_a, std::vector<receive_source_pair_unbounded> & receive_source_pairs_a, std::vector<callback_data> & block_callbacks_required)
{
	auto hash (hash_a);
	auto num_to_confirm = block_height_a - confirmation_height_a;

	// Store heights of blocks
	constexpr auto height_not_set = std::numeric_limits<uint64_t>::max ();
	auto next_height = height_not_set;

	// Handle any sends above a receive
	auto is_original_block = (hash == original_hash);
	bool hit_receive = false;
	while ((num_to_confirm > 0) && !hash.is_zero () && !stopped)
	{
		nano::block_sideband sideband;
		auto block (ledger.store.block_get (transaction_a, hash, &sideband));
		if (block)
		{
			auto source (block->source ());
			if (source.is_zero ())
			{
				source = block->link ();
			}

			if (!source.is_zero () && !ledger.is_epoch_link (source) && ledger.store.source_exists (transaction_a, source))
			{
				if (!hit_receive && !block_callbacks_required.empty ())
				{
					// Add the callbacks to the associated receive to retrieve later
					assert (!receive_source_pairs_a.empty ());
					auto & last_receive_details = receive_source_pairs_a.back ().receive_details;
					last_receive_details->send_callbacks_required.assign (block_callbacks_required.begin (), block_callbacks_required.end ());
					block_callbacks_required.clear ();
				}

				is_original_block = false;
				hit_receive = true;

				auto block_height = confirmation_height_a + num_to_confirm;
				receive_source_pairs_a.emplace_back (std::make_shared<conf_height_details_unbounded> (account_a, hash, block_height, 1, std::vector<callback_data>{ { block, sideband } }), source);
				next_height = block_height;
			}
			else if (is_original_block)
			{
				orig_block_callbacks_required.emplace_back (block, sideband);
			}
			else
			{
				if (!hit_receive)
				{
					// This block is cemented via a recieve, as opposed to below a receive being cemented
					block_callbacks_required.emplace_back (block, sideband);
				}
				else
				{
					// We have hit a receive before, add the block to it
					auto & last_receive_details = receive_source_pairs_a.back ().receive_details;
					++last_receive_details->num_blocks_confirmed;
					last_receive_details->block_callbacks_required.emplace_back (block, sideband);

					implicit_receive_cemented_mapping_unbounded[hash] = std::weak_ptr<conf_height_details_unbounded> (last_receive_details);
				}
			}

			hash = block->previous ();
		}

		--num_to_confirm;
	}
}

void nano::confirmation_height_processor::prepare_iterated_blocks_for_cementing_unbounded (preparation_data_unbounded & preparation_data_unbounded_a)
{
	auto receive_details = preparation_data_unbounded_a.receive_details;
	auto block_height = preparation_data_unbounded_a.block_height;
	if (block_height > preparation_data_unbounded_a.confirmation_height)
	{
		// Check whether the previous block has been seen. If so, the rest of sends below have already been seen so don't count them
		if (preparation_data_unbounded_a.account_it != confirmed_iterated_pairs_unbounded.cend ())
		{
			preparation_data_unbounded_a.account_it->second.confirmed_height = block_height;
			if (block_height > preparation_data_unbounded_a.iterated_height)
			{
				preparation_data_unbounded_a.account_it->second.iterated_height = block_height;
			}
		}
		else
		{
			confirmed_iterated_pairs_unbounded.emplace (preparation_data_unbounded_a.account, confirmed_iterated_pair_unbounded{ block_height, block_height });
		}

		auto num_blocks_confirmed = block_height - preparation_data_unbounded_a.confirmation_height;
		auto block_callbacks_required = preparation_data_unbounded_a.block_callbacks_required;
		if (block_callbacks_required.empty ())
		{
			if (!receive_details)
			{
				block_callbacks_required = orig_block_callbacks_required;
			}
			else
			{
				assert (receive_details);

				if (preparation_data_unbounded_a.already_traversed && receive_details->send_callbacks_required.empty ())
				{
					// We are confirming a block which has already been traversed and found no associated receive details for it.
					auto & above_receive_details_w = implicit_receive_cemented_mapping_unbounded[preparation_data_unbounded_a.current];
					assert (!above_receive_details_w.expired ());
					auto above_receive_details = above_receive_details_w.lock ();

					// Excess to discard
					auto num_blocks_already_confirmed = above_receive_details->num_blocks_confirmed - (above_receive_details->height - preparation_data_unbounded_a.confirmation_height);

					auto start_it = above_receive_details->block_callbacks_required.begin () + above_receive_details->block_callbacks_required.size () - (num_blocks_already_confirmed + num_blocks_confirmed);
					auto end_it = above_receive_details->block_callbacks_required.begin () + above_receive_details->block_callbacks_required.size () - (num_blocks_already_confirmed);

					block_callbacks_required.assign (start_it, end_it);
				}
				else
				{
					block_callbacks_required = receive_details->send_callbacks_required;				
				}

				auto num_to_remove = block_callbacks_required.size () - num_blocks_confirmed;
				block_callbacks_required.erase (block_callbacks_required.begin () + block_callbacks_required.size () - num_to_remove, block_callbacks_required.end ());
				receive_details->send_callbacks_required.clear ();
			}
		}

		pending_writes_unbounded.emplace_back (preparation_data_unbounded_a.account, preparation_data_unbounded_a.current, block_height, num_blocks_confirmed, block_callbacks_required);
	}

	if (receive_details)
	{
		// Check whether the previous block has been seen. If so, the rest of sends below have already been seen so don't count them
		auto const & receive_account = receive_details->account;
		auto receive_account_it = confirmed_iterated_pairs_unbounded.find (receive_account);
		if (receive_account_it != confirmed_iterated_pairs_unbounded.cend ())
		{
			// Get current height
			auto current_height = receive_account_it->second.confirmed_height;
			receive_account_it->second.confirmed_height = receive_details->height;
			auto const orig_num_blocks_confirmed = receive_details->num_blocks_confirmed;
			receive_details->num_blocks_confirmed = receive_details->height - current_height;

			// Get the difference and remove the callbacks
			auto block_callbacks_to_remove = orig_num_blocks_confirmed - receive_details->num_blocks_confirmed;
			receive_details->block_callbacks_required.erase (receive_details->block_callbacks_required.begin () + receive_details->block_callbacks_required.size () - block_callbacks_to_remove, receive_details->block_callbacks_required.end ());
			assert (receive_details->block_callbacks_required.size () == receive_details->num_blocks_confirmed);
		}
		else
		{
			confirmed_iterated_pairs_unbounded.emplace (receive_account, confirmed_iterated_pair_unbounded{ receive_details->height, receive_details->height });
		}

		pending_writes_unbounded.push_back (*receive_details);
	}
}

/*
 * Returns true if there was an error in finding one of the blocks to write a confirmation height for, false otherwise
 */
bool nano::confirmation_height_processor::cement_blocks_unbounded ()
{
	auto total_pending_write_block_count = std::accumulate (pending_writes_unbounded.cbegin (), pending_writes_unbounded.cend (), uint64_t (0), [](uint64_t total, conf_height_details_unbounded const & receive_details_unbounded_a) {
		return total += receive_details_unbounded_a.num_blocks_confirmed;
	});

	auto transaction (ledger.store.tx_begin_write ({}, { nano::tables::confirmation_height }));
	while (!pending_writes_unbounded.empty ())
	{
		auto & pending = pending_writes_unbounded.front ();
		nano::confirmation_height_info confirmation_height_info;
		auto error = ledger.store.confirmation_height_get (transaction, pending.account, confirmation_height_info);
		release_assert (!error);
		auto confirmation_height = confirmation_height_info.height;
		if (pending.height > confirmation_height)
		{
#ifndef NDEBUG
			// Do more thorough checking in Debug mode, indicates programming error.
			nano::block_sideband sideband;
			auto block = ledger.store.block_get (transaction, pending.hash, &sideband);
			static nano::network_constants network_constants;
			assert (network_constants.is_test_network () || block != nullptr);
			assert (network_constants.is_test_network () || sideband.height == pending.height);
#else
			auto block = ledger.store.block_get (transaction, pending.hash);
#endif
			// Check that the block still exists as there may have been changes outside this processor.
			if (!block)
			{
				logger.always_log ("Failed to write confirmation height for: ", pending.hash.to_string ());
				ledger.stats.inc (nano::stat::type::confirmation_height, nano::stat::detail::invalid_block);
				pending_writes_unbounded.clear ();
				total_pending_write_block_count = 0;
				return true;
			}

			ledger.stats.add (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in, pending.height - confirmation_height);
			assert (pending.num_blocks_confirmed == pending.height - confirmation_height);
			confirmation_height = pending.height;
			ledger.cache.cemented_count += pending.num_blocks_confirmed;
			ledger.store.confirmation_height_put (transaction, pending.account, { confirmation_height, pending.hash });

			transaction.commit ();
			// Reverse it so that the callbacks start from the lowest point and move upwards
			std::reverse (pending.block_callbacks_required.begin (), pending.block_callbacks_required.end ());
			notify_observers (pending.block_callbacks_required);
			transaction.renew ();
		}
		total_pending_write_block_count -= pending.num_blocks_confirmed;
		pending_writes_unbounded.erase (pending_writes_unbounded.begin ());
	}
	assert (total_pending_write_block_count == 0);
	assert (pending_writes_unbounded.empty ());
	return false;
}

nano::confirmation_height_processor::conf_height_details_unbounded::conf_height_details_unbounded (nano::account const & account_a, nano::block_hash const & hash_a, uint64_t height_a, uint64_t num_blocks_confirmed_a, std::vector<callback_data> const & block_callbacks_required_a) :
account (account_a),
hash (hash_a),
height (height_a),
num_blocks_confirmed (num_blocks_confirmed_a),
block_callbacks_required (block_callbacks_required_a)
{
}

nano::confirmation_height_processor::receive_source_pair_unbounded::receive_source_pair_unbounded (std::shared_ptr<conf_height_details_unbounded> const & receive_details_a, const block_hash & source_a) :
receive_details (receive_details_a),
source_hash (source_a)
{
}

nano::confirmation_height_processor::confirmed_iterated_pair_unbounded::confirmed_iterated_pair_unbounded (uint64_t confirmed_height_a, uint64_t iterated_height_a) :
confirmed_height (confirmed_height_a),
iterated_height (iterated_height_a)
{
}
