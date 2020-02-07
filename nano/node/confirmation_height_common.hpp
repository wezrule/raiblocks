#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/secure/blockstore.hpp>
#include <nano/secure/common.hpp>

#include <boost/circular_buffer.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace nano
{
namespace confirmation_height
{
	class callback_data final
	{
	public:
		callback_data (std::shared_ptr<nano::block> const & block_a, nano::block_sideband const & sideband_a) :
		block (block_a),
		sideband (sideband_a)
		{
		}

		std::shared_ptr<nano::block> block;
		nano::block_sideband sideband;
	};

	/** The maximum amount of blocks to iterate over while writing */
	uint64_t constexpr batch_write_size = 4096;
}
}