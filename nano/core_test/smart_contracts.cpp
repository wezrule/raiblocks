#include <nano/node/election.hpp>
#include <nano/node/testing.hpp>
#include <nano/test_common/testutil.hpp>

#include <nano/secure/vm.hpp>

#include <gtest/gtest.h>

#include <boost/format.hpp>

using namespace std::chrono_literals;

TEST (smart_contract, simple_add)
{
	// This would declare a smart contract similar to the follows (it demonstrates a function call with local variables, addition and returning) using nanolang.
	auto contract_source = R"(
	pragma nanolet 0.1

	// Implicit work value
	function add () view public {
	  uint128 amount = 1;
	  uint128 amount2 = 2;
	  uint128 add = amount + amount2;
	  return add;
	}
	)";

	// Nano bytecode for this
	std::vector<uint8_t> total_contract = {
	
		push1, 0x4,
		calldatasize,
		lt, // 0x3
		push1, 0x16, // PC for revert, this checks there's at least 4 bytes for the function signature
		jumpi,

		// Try to find which function we called
		push1, 0x0,   // offset
		push1, 0x4,  // size (num bytes)
		calldataload, // Gets the function address
		push4, 0xF0, 0xCA, 0x88, 0x3F, // Hardcoded function signature (which the user has picked) for one of the functions
		eq,
		push1, 0x16, // Jump to the function above, if that's indeed what we called... 
		jumpi,

		// Try another function (if there was one) else continue to the revert..
		// error
		revert, // 0x15
	
		// Function add which has signature (0xF0CA883F) which adds 2 numbers together. The signature is currently hardcoded but will be determanistically found.
		push1, 0x01, // 0x16
		push1, 0x02,
		dup2_,
		dup2_,
		add,
		dup1,
		ret,
		pop,
		pop,
		pop
	};

	std::vector<uint8_t> data_payload_call_add = {0xF0, 0xCA, 0x88, 0x3F};
	auto result = nano::execute_nano_bytecode (total_contract, data_payload_call_add);
	ASSERT_EQ (3, result);

	// Could in theory send nano to the smart contract too. It would need a withdraw function from owner though
//	nano::system system (1);
//	auto node = system.nodes.front ();
	//	nano::keypair contract_key; // This would be calculated by doing blake2b (this_address || block_height)?
	//	auto contract_state_create = std::make_shared<nano::state_block> (nano::genesis_account, nano::genesis_hash, nano::genesis_account, 0, contract_key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (nano::genesis_hash), total_contract);
//	node->ledger.process (node->store.tx_begin_write (), *contract_state_create);

//	auto contract_call = std::make_shared<nano::state_block> (nano::genesis_account, nano::genesis_hash, nano::genesis_account, 0, contract_key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, *system.work.generate (contract_state_create->hash ()), data_payload_call_add);
//	node->ledger.process (node->store.tx_begin_write (), *contract_state_create);
}
