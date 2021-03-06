#!/bin/bash

source ./test_helpers.sh

IPFS="../../main/ipfs --config /tmp/ipfs_1"

function pre {
	post
	eval "$IPFS" init;
	check_failure "pre" $?
}

function post {
	rm -Rf /tmp/ipfs_1;
	rm hello.txt;
}

function body {
	create_hello_world;
	eval "$IPFS" add hello.txt
	check_failure "add hello.txt" $?
	
	eval "$IPFS" cat QmYAXgX8ARiriupMQsbGXtKdDyGzWry1YV3sycKw1qqmgH
	check_failure "cat hello.txt" $?
}


