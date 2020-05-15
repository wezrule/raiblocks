add_library (test_common
		testutil.hpp
		testutil.cpp)

target_compile_definitions(test_common
		PRIVATE
			-DTAG_VERSION_STRING=${TAG_VERSION_STRING}
			-DGIT_COMMIT_HASH=${GIT_COMMIT_HASH}
			-DBOOST_PROCESS_SUPPORTED=${BOOST_PROCESS_SUPPORTED}
			-DCI=${CI_TEST})

target_link_libraries (test_common node gtest Boost::log_setup Boost::log Boost::boost)
