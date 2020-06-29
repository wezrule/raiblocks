#pragma once

#include "nano/lib/numbers.hpp"

namespace nano
{
class ledger;
nano::uint256_t execute_nano_bytecode (std::vector<uint8_t> const & contract_code, std::vector<uint8_t> const & data_payload /*, nano::ledger const */);
}

// Do not have more than 256, all opcodes should be 1 byte
enum opcode
{
	add = 0x20, // add this value to the top of the stack
	sub = 0x21, // substract this value with that at the top of the stack
	push1 = 0x60, // push 1 byte onto the stack
	push4 = 0x61, // push 4 bytes onto the stack
	pop = 0x80, // pop word off the stack
	dup1 = 0x81, // duplicate word top of the stack
	dup2_ = 0x82, // duplicate 2nd item from stack (Windows doesn't like dup2!!)
	dup3 = 0x83, // duplicate 3rd item from stack
	dup4 = 0x84, // duplicate 4th item from stack
	ret = 0x85, // return value to caller of smart contract
	jump = 0x86, // jump to instruction
	jumpi = 0x87, // Condition jump to instruction
	calldataload, // Load the data payload with offset -> size values on the stack
	calldatasize, // The number of bytes in the data payload (minimum 4 for the function signature)
	lt, // Compares the top 2 items on the stack and add 1 if less than, 0 otherwise
	eq, // Compares the top 2 items on the stack for equality
	revert, // Revert all changes made
	send_, // Send funds to a address
	pull, // Pull funds from an address
	// .... TODO
};
