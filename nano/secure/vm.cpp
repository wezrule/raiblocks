#include <boost/multiprecision/cpp_int.hpp>

#include <string_view>
#include <nano/secure/vm.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/blockstore.hpp>

// 256 bit word stack machine
//using uint256_t = boost::multiprecision::uint256_t;

// Constant is implied by default
std::string_view reserved_keywords[] = { "class", "uint128", "bool", "address", "public", "constant", "return" };

// address is typedef to uint256

// NVM Nano virtual machine
// Is a statically typed quasi-non-turning complete contract oriented programming language.

#include <stack>
#include <vector>

namespace nano
{
nano::uint256_t execute_nano_bytecode (std::vector<uint8_t> const & contract_code, std::vector<uint8_t> const & data_payload /*, nano::ledger const & ledger*/)
{
	std::vector<nano::uint256_t> stack;
	uint32_t pc = 0; // Program counter
	auto code_length = contract_code.size ();
	nano::uint256_t result{ 0 };
	const auto max_stack_depth = 1024;
	bool jumped;

	//auto txn = ledger.store.tx_begin_write ();

	while (true)
	{
		jumped = false;
		auto opcode_l = static_cast<opcode> (contract_code[pc]);
		switch (opcode_l)
		{
			case push1:
				assert (stack.size () < max_stack_depth);
				stack.push_back (contract_code[++pc]);
				break;
			case push4:
			{
				std::vector<uint8_t> push_bytes;
				assert (stack.size () + 3 < max_stack_depth);
				for (auto i = 0; i < 4; ++i)
				{
					push_bytes.push_back (contract_code[++pc]);
				}
				
				nano::uint256_t push;
				import_bits (push, push_bytes.begin (), push_bytes.end ()); // set return result
				stack.push_back (push);
				break;
			}
			case pop:
				assert (!stack.empty ());
				stack.erase (stack.rbegin ().base () - 1); // erase last
				break;
			case add:
			{
				assert (stack.size () > 1);
				auto top = stack.back ();
				stack.pop_back ();
				stack.back () += top;
				break;
			}
			case sub:
			{
				assert (stack.size () > 1);
				auto top = stack.back ();
				stack.pop_back ();
				stack.back () -= top;
				break;
			}
			case dup1:
				// duplicate top stack item
				assert (stack.size () < max_stack_depth);
				stack.push_back (stack.back ());
				break;
			case dup2_:
				// duplicate 2nd stack item
				assert (stack.size () < max_stack_depth);
				assert (stack.size () > 1);
				stack.push_back (stack[stack.size () - 2]);
				break;
			case dup3:
				// duplicate 3rd stack item
				assert (stack.size () < max_stack_depth);
				assert (stack.size () > 2);
				stack.push_back (stack[stack.size () - 3]);
				break;
			case dup4:
				// duplicate 4th stack item
				assert (stack.size () < max_stack_depth);
				assert (stack.size () > 3);
				stack.push_back (stack[stack.size () - 4]);
				break;
			case lt:
			{
				assert (stack.size () > 1);
				auto top = stack.back ();
				stack.pop_back ();
				stack.back () = top < stack.back () ? 1 : 0;
				break;
			}
			case eq:
			{
				assert (stack.size () > 1);
				auto top = stack.back ();
				stack.pop_back ();
				stack.back () = top == stack.back () ? 1 : 0;
				break;
			}
			case jump:
			{
				assert (stack.size () > 1);
				auto pos = stack.back ();
				stack.pop_back ();
				pc = boost::lexical_cast<uint32_t> (pos);
				jumped = true;
				break;
			}
			case jumpi:
			{
				// Conditional jump
				assert (stack.size () > 1);
				auto pos = stack.back ();
				stack.pop_back ();
				auto cond = stack.back ();
				stack.pop_back ();
				if (cond.convert_to<bool> ())
				{
					pc = boost::lexical_cast<uint32_t> (pos);		
					jumped = true;
				}
				break;
			}

			case calldataload:
			{
				// Get part of the data payload with offset and size into the array. This is not similar to Ethereum which uses 32 bytes always.
				assert (stack.size () > 1);
				auto size = stack.back ().convert_to<uint64_t> ();			
				stack.pop_back ();
				auto offset = stack.back ().convert_to<uint64_t> ();
				import_bits (stack.back (), data_payload.begin () + offset, data_payload.begin () + offset + size);
				break;
			}

			case calldatasize:
				stack.push_back (data_payload.size ());
				break;

			case revert:
				// This should revert any changes made due to an error in the contract (insufficient work?)
				// txn.revert (); // revert
				assert (false); // Not implemented yet
				break;
			case send_:
				assert (false);
				break;

			case ret:
			{
				// Return is 256, the last 2 items on the stack are copied to return "register"
				assert (stack.size () > 0);
				result = stack.back ();
				stack.pop_back ();			
				break;
			}
		}

		if (!jumped) // TODO: also !reverted?
		{
			++pc;
		}

		if (pc >= code_length)
		{
			assert (stack.empty ());
			break;
		}
	}
	return result;
}
}
