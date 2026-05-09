// Ranvier Core - request_timeout helper unit tests
//
// Static coverage of the request_timeout_error exception class exposed by
// src/request_timeout.hpp. The templated with_request_timeout() helper itself
// requires a running Seastar reactor and is exercised by integration tests.

#include "request_timeout.hpp"

#include <gtest/gtest.h>

#include <exception>
#include <string>

#include <seastar/core/timed_out_error.hh>

using namespace ranvier;

TEST(RequestTimeoutErrorTest, WhatReturnsLabel) {
    request_timeout_error err("tokenize");
    EXPECT_STREQ(err.what(), "tokenize");
}

TEST(RequestTimeoutErrorTest, LabelAccessorMatchesWhat) {
    request_timeout_error err("stream_chunk");
    EXPECT_STREQ(err.label(), "stream_chunk");
    EXPECT_STREQ(err.label(), err.what());
}

TEST(RequestTimeoutErrorTest, IsCatchableAsSeastarTimedOutError) {
    // Existing handlers across the codebase do
    //   catch (const seastar::timed_out_error&)
    // The new tagged type must continue to match those handlers without any
    // call-site change.
    bool caught = false;
    try {
        throw request_timeout_error("tokenize_thread_pool");
    } catch (const seastar::timed_out_error& e) {
        caught = true;
        EXPECT_STREQ(e.what(), "tokenize_thread_pool");
    }
    EXPECT_TRUE(caught);
}

TEST(RequestTimeoutErrorTest, IsCatchableAsStdException) {
    bool caught = false;
    try {
        throw request_timeout_error("backend_connect");
    } catch (const std::exception& e) {
        caught = true;
        EXPECT_STREQ(e.what(), "backend_connect");
    }
    EXPECT_TRUE(caught);
}

TEST(RequestTimeoutErrorTest, LabelDistinguishesPhases) {
    // Code that wants to react differently per phase can downcast and read
    // label(); confirm two different labels round-trip distinctly.
    request_timeout_error tokenize_err("tokenize");
    request_timeout_error stream_err("stream_chunk");
    EXPECT_STRNE(tokenize_err.label(), stream_err.label());
}
