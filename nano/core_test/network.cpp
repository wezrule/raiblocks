#include <nano/core_test/testutil.hpp>
#include <nano/node/testing.hpp>
#include <nano/node/transport/udp.hpp>

#include <gtest/gtest.h>

#include <boost/iostreams/stream_buffer.hpp>
#include <boost/thread.hpp>

using namespace std::chrono_literals;

TEST (network, tcp_no_connect_excluded_peers)
{
	for (int i = 0; i < 50; ++i)
	{

	nano::system system (1);
	auto node0 (system.nodes[0]);
	ASSERT_EQ (0, node0->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, nano::get_available_port (), nano::unique_path (), system.alarm, system.logging, system.work));
	node1->start ();
	system.nodes.push_back (node1);
	auto endpoint1 (node1->network.endpoint ());
	auto endpoint1_tcp (nano::transport::map_endpoint_to_tcp (endpoint1));
	while (!node0->network.excluded_peers.check (endpoint1_tcp))
	{
		node0->network.excluded_peers.add (endpoint1_tcp, 1);
	}
	ASSERT_EQ (0, node0->stats.count (nano::stat::type::tcp, nano::stat::detail::tcp_excluded));
	node1->network.merge_peer (node0->network.endpoint ());
	ASSERT_TIMELY (5s, node0->stats.count (nano::stat::type::tcp, nano::stat::detail::tcp_excluded) >= 1);
	ASSERT_EQ (nullptr, node0->network.find_channel (endpoint1));

	// Should not actively reachout to excluded peers
	ASSERT_TRUE (node0->network.reachout (endpoint1, true));

	// Erasing from excluded peers should allow a connection
	node0->network.excluded_peers.remove (endpoint1_tcp);
	ASSERT_FALSE (node0->network.excluded_peers.check (endpoint1_tcp));

	// Manually cleanup previous attempt
	node1->network.cleanup (std::chrono::steady_clock::now ());
	node1->network.syn_cookies.purge (std::chrono::steady_clock::now ());

	// Ensure a successful connection
	ASSERT_EQ (0, node0->network.size ());
	std::cout << "Merge peer!!" << std::endl;
	node1->network.merge_peer (node0->network.endpoint ());
	ASSERT_TIMELY (5s, node0->network.size () == 1);
	}
}
