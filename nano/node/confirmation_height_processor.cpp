#include <nano/lib/logger_mt.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/threading.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/confirmation_height_processor.hpp>
#include <nano/node/write_database_queue.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>

#include <cassert>
#include <numeric>

nano::confirmation_height_processor::confirmation_height_processor (nano::ledger & ledger_a, nano::write_database_queue & write_database_queue_a, std::chrono::milliseconds batch_separate_pending_min_time_a, nano::logger_mt & logger_a, confirmation_height_mode mode_a) :
ledger (ledger_a),
logger (logger_a),
write_database_queue (write_database_queue_a),
batch_separate_pending_min_time (batch_separate_pending_min_time_a),
process_mode (mode_a),
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
				accounts_confirmed_info_size = 0;
			}
			if (pending_writes_unbounded.empty ())
			{
				confirmed_iterated_pairs_unbounded.clear ();
				confirmed_iterated_pairs_unbounded_size = 0;
				implicit_receive_cemented_mapping_unbounded.clear ();
				implicit_receive_cemented_mapping_unbounded_size = 0;
			}

			if (pending_writes.empty () && pending_writes_unbounded.empty ())
			{
				timer.restart ();
			}

			set_next_hash ();

			const auto num_blocks_to_use_unbounded = batch_write_size;
			auto blocks_within_automatic_unbounded_selection = (ledger.cache.block_count < num_blocks_to_use_unbounded || ledger.cache.block_count - num_blocks_to_use_unbounded < ledger.cache.cemented_count);

			if (process_mode == confirmation_height_mode::unbounded || (process_mode == confirmation_height_mode::automatic && blocks_within_automatic_unbounded_selection))
			{
				process_unbounded ();
			}
			else
			{
				assert (process_mode == confirmation_height_mode::bounded || process_mode == confirmation_height_mode::automatic);
				process ();
			}

			lk.lock ();
		}
		else
		{
			// If there are blocks pending cementing, then make sure we flush out the remaining writes
			lk.unlock ();
			if (!pending_writes.empty ())
			{
				assert (pending_writes_unbounded.empty ());
				auto scoped_write_guard = write_database_queue.wait (nano::writer::confirmation_height);
				cement_blocks ();
				lk.lock ();
				original_hash.clear ();
			}
			else if (!pending_writes_unbounded.empty ())
			{
				assert (pending_writes.empty ());
				auto scoped_write_guard = write_database_queue.wait (nano::writer::confirmation_height);
				cement_blocks_unbounded ();
				lk.lock ();
				original_hash.clear ();
			}
			else
			{
				lk.lock ();
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

void nano::confirmation_height_processor::set_next_hash ()
{
	nano::lock_guard<std::mutex> guard (mutex);
	assert (!awaiting_processing.empty ());
	original_hash = *awaiting_processing.begin ();
	original_hashes_pending.insert (original_hash);
	awaiting_processing.erase (original_hash);
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
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (confirmation_height_processor & confirmation_height_processor_a, const std::string & name_a)
{
	auto composite = std::make_unique<container_info_composite> (name_a);

	size_t cemented_observers_count;
	size_t cemented_batch_finished_observer_count;
	{
		nano::lock_guard<std::mutex> guard (confirmation_height_processor_a.mutex);
		cemented_observers_count = confirmation_height_processor_a.cemented_observers.size ();
		cemented_batch_finished_observer_count = confirmation_height_processor_a.cemented_batch_finished_observers.size ();
	}

	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "cemented_observers", cemented_observers_count, sizeof (decltype (confirmation_height_processor_a.cemented_observers)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "cemented_batch_finished_observers", cemented_batch_finished_observer_count, sizeof (decltype (confirmation_height_processor_a.cemented_batch_finished_observers)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "awaiting_processing", confirmation_height_processor_a.awaiting_processing_size (), sizeof (decltype (confirmation_height_processor_a.awaiting_processing)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pending_writes", confirmation_height_processor_a.pending_writes_size, sizeof (decltype (confirmation_height_processor_a.pending_writes)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "accounts_confirmed_info", confirmation_height_processor_a.accounts_confirmed_info_size, sizeof (decltype (confirmation_height_processor_a.accounts_confirmed_info)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "confirmed_iterated_pairs_unbounded", confirmation_height_processor_a.confirmed_iterated_pairs_unbounded_size, sizeof (decltype (confirmation_height_processor_a.confirmed_iterated_pairs_unbounded)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pending_writes_unbounded", confirmation_height_processor_a.pending_writes_unbounded_size, sizeof (decltype (confirmation_height_processor_a.pending_writes_unbounded)::value_type) }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "implicit_receive_cemented_mapping_unbounded", confirmation_height_processor_a.implicit_receive_cemented_mapping_unbounded_size, sizeof (decltype (confirmation_height_processor_a.implicit_receive_cemented_mapping_unbounded)::value_type) }));
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

nano::confirmation_height_processor::callback_data::callback_data (std::shared_ptr<nano::block> const & block_a, nano::block_sideband const & sideband_a) :
block (block_a),
sideband (sideband_a)
{
}
