libcache
========

An open-source clone of Apple's libcache system library.

Disclaimer: All public APIs (cache.h and cache_callbacks.h) are verified for
correctness by building and running the unit test suite against Apple's
libcache implementation. Private APIs (cache_private.h) are implemented on a
best-effort basis and are not guaranteed to be correct.

Core Features
-------------

* Lock Striping: Distributes elements across independent shards to minimize
  multi-threaded lock contention.
* Segmented LRU (SLRU) Eviction: Partitions cache queues into probationary and
  protected segments, ensuring recently and frequently accessed values are protected
  from being evicted by one-hit wonder scans.
* Kernel Eviction Hooks: Monitors low-priority GCD memory pressure queues to
  purge unretained nodes under resource strain.
* Retain/Release Lifecycles: Tracks unpinned elements by reference counts,
  locking retained entries from eviction.
* Deterministic Hashing: Provides built-in 64-bit FNV-1a hashing for strings,
  integers, and fixed byte arrays.

Compilation and Verification
----------------------------

    mkdir build && cd build
    cmake ..
    make
    export DYLD_LIBRARY_PATH="$(pwd):$DYLD_LIBRARY_PATH"
    ./cache_unit_tests
    ./cache_stress --threads 12 --duration 10 --keys 2048 --ballast 32 --zipf --mach-purgeable

API Usage Example
-----------------

    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>

    #include <cache.h>
    #include <cache_callbacks.h>

    static void value_retain_cb(void *value, void *user_data) {
        (void)user_data;
        printf("Value retained: %s\n", (char *)value);
    }

    static void value_release_cb(void *value, void *user_data) {
        (void)user_data;
        printf("Value released: %s\n", (char *)value);
        free(value);
    }

    int main(void) {
        cache_t *cache = NULL;
        cache_attributes_t attrs = {
            .version = CACHE_ATTRIBUTES_VERSION_2,
            .key_hash_cb = cache_key_hash_cb_cstring,
            .key_is_equal_cb = cache_key_is_equal_cb_cstring,
            .value_retain_cb = value_retain_cb,
            .value_release_cb = value_release_cb
        };

        if (cache_create("com.darwin.cache.image_store", &attrs, &cache) != 0) {
            return 1;
        }

        char *key = "user_avatar";
        char *value = strdup("/var/tmp/avatar.png");

        cache_set_and_retain(cache, key, value, 0);
        cache_release_value(cache, value);
        cache_destroy(cache);

        return 0;
    }

Compiling the Example
---------------------

To compile and run the example using local headers and `libcache.dylib` from the build directory:

    clang example.c -Iinclude -Lbuild -lcache -o example
    export DYLD_LIBRARY_PATH="build:$DYLD_LIBRARY_PATH"
    ./example

AI Disclosure
-------------

This library was developed with the assistance of Gemini 3.8 Flash. The AI model was
utilized throughout the engineering process, assisting with implementation, architecture
design (including lock striping and LRU queue management), refactoring, test suite
generation, and documentation. All documentation, implementations, and APIs were
verified through unit testing and manual review, and only public API documentation from
Apple was used in the validation process.

License
-------

Distributed under the BSD 3-Clause License.
Copyright (c) 2026 Brian McKenzie. All rights reserved.
