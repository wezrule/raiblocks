#pragma once

#include <nano/lib/locks.hpp>
#include <nano/secure/common.hpp>

#include <functional>
#include <thread>

namespace nano
{
class epochs;
class logger_mt;
class node_config;
class signature_checker;

class state_block_signature_verification
{
public:
	state_block_signature_verification (nano::signature_checker &, nano::epochs &, nano::node_config &, nano::logger_mt &, uint64_t);
	~state_block_signature_verification ();
	void add (nano::unchecked_info const & info_a);
	size_t size ();
	void stop ();
	bool is_active ();

	std::function<void(unchecked_info_mic &, std::vector<int> const &, std::vector<nano::block_hash> const &, std::vector<nano::signature> const &)> blocks_verified_callback;
	std::function<void()> transition_inactive_callback;

private:
	nano::signature_checker & signature_checker;
	nano::epochs & epochs;
	nano::node_config & node_config;
	nano::logger_mt & logger;

	std::mutex mutex;
	bool stopped{ false };
	bool active{ false };

	unchecked_info_mic state_blocks;
	nano::condition_variable condition;
	std::thread thread;

	void run (uint64_t block_processor_verification_size);
	unchecked_info_mic setup_items (size_t);
	void verify_state_blocks (unchecked_info_mic &);
};

std::unique_ptr<nano::container_info_component> collect_container_info (state_block_signature_verification & state_block_signature_verification, const std::string & name);
}
