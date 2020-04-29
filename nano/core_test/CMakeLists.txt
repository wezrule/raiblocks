add_executable (core_test
	core_test_main.cc
	testutil.hpp
	network.cpp)

target_compile_definitions(core_test
		PRIVATE
			-DTAG_VERSION_STRING=${TAG_VERSION_STRING}
			-DGIT_COMMIT_HASH=${GIT_COMMIT_HASH}
			-DBOOST_PROCESS_SUPPORTED=${BOOST_PROCESS_SUPPORTED}
			-DCI=${CI_TEST})

target_link_libraries (core_test node secure gtest libminiupnpc-static Boost::log_setup Boost::log Boost::boost)
