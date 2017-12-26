/*
  DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
  Version 2, December 2004

  Copyright (C) 2014 Kenneth Ho <ken@fsfoundry.org>

  Everyone is permitted to copy and distribute verbatim or modified
  copies of this license document, and changing it is allowed as long
  as the name is changed.

  DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
  TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION

  0. You just DO WHAT THE FUCK YOU WANT TO.
*/
//#define MVCC11_NO_PTHREAD_SPINLOCK 1


#include <mvcc11/mvcc.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <cassert>

using namespace std;
using namespace chrono;
using namespace mvcc11;

namespace {
	auto hr_now() -> decltype(high_resolution_clock::now()) {
		return high_resolution_clock::now();
	}

	auto INIT = "init";
	auto OVERWRITTEN = "overwritten";
	auto UPDATED = "updated";
	auto DISTURBED = "disturbed";
}

template <class Mutex>
auto make_lock(Mutex& mtx) -> std::unique_lock<Mutex> {
	return std::unique_lock<Mutex>(mtx);
}

template <class Mutex, class Computation>
auto locked(Mutex& mtx, Computation comp) -> decltype(comp()) {
	auto lock = make_lock(mtx);
	return comp();
}

#define LOCKED(mtx)                                                     \
	if(bool locked_done_eiN8Aegu = false)                               \
{}																		\
  else                                                                  \
  for(auto mtx ## _lock_eiN8Aegu = make_lock(mtx);						\
  !locked_done_eiN8Aegu;												\
  locked_done_eiN8Aegu = true)

#define LOCKED_LOCK(mtx) mtx ## _lock_eiN8Aegu

static void test_null_snapshot_on_1st_reference() {
	mvcc<string> x;
	auto const snapshot = *x;
	assert(snapshot->version == 0);
	assert(snapshot->value.empty());
}

static void test_current_and_op_deref_yields_equivalent_results() {
	mvcc<string> x(INIT);
	auto const snapshot = x.current();

	assert(snapshot == x.current());
	assert(snapshot == *x);

	assert(snapshot != nullptr);
	assert(snapshot->version == 0);
	assert(snapshot->value == INIT);
}

static void test_snapshot_overwrite() {
	mvcc<string> x(INIT);
	auto const snapshot = x.overwrite(OVERWRITTEN);
	assert(snapshot != nullptr);
	assert(snapshot->version == 1);
	assert(snapshot->value == OVERWRITTEN);
}

static void test_snapshot_isolation() {
	mvcc<string> x(INIT);
	auto const snapshot1 = *x;
	assert(snapshot1 != nullptr);
	assert(snapshot1->version == 0);
	assert(snapshot1->value == INIT);

	auto const snapshot2 = x.overwrite(OVERWRITTEN);
	assert(snapshot2 != nullptr);
	assert(snapshot2->version == 1);
	assert(snapshot2->value == OVERWRITTEN);

	assert(snapshot1 != snapshot2);
	assert(snapshot1 != nullptr);
	assert(snapshot1->version == 0);
	assert(snapshot1->value == INIT);
}

// Ensuring different instance contain different snapshots.
static void test_instance() {
	auto const x = mvcc<string>("x").current();
	auto const y = mvcc<string>("y").overwrite("Y");

	assert(x->version == 0);
	assert(x->value == "x");

	assert(y->version == 1);
	assert(y->value == "Y");
}

static void test_snapshot_update() {
	mvcc<string> x(INIT);
	auto const init = *x;
	assert(init->version == 0);
	auto const updated = x.update([](size_t version, string const &value) {
		assert(version == 0);
		assert(value == INIT);
		return UPDATED;
	});
	assert(updated->version == 1);
	assert(updated->value == UPDATED);
	assert(init->value == INIT);

	x.update([](size_t version, string const &value) {
		assert(version == 1);
		assert(value == UPDATED);
		return UPDATED;
	});
}

// Updater wakes up disturber when ready, so that disturber
// could sneakily change the most recent version of snapshot
// and cause try_update_once() to fail
static void test_try_update_once_fails_with_disturber() {
	mutex mtx;
	auto updater_ready = false;
	auto disturber_ready = false;
	condition_variable cv;

	mvcc<string> x(INIT);
	assert(x.current()->version == 0);
	auto updater = async(launch::async, [&] {
		//assert(x.current()->version == 0);
		auto updated = x.try_update([&](size_t const version, string const &value) {
			//assert(version == 0);
			//assert(value == INIT);
			LOCKED(mtx) {
				updater_ready = true;
				cv.notify_one();
			};
			LOCKED(mtx) {
				cv.wait(LOCKED_LOCK(mtx), [&]() {
					return disturber_ready;
				});
			}

			return UPDATED;
		});

		//assert(updated == nullptr);
		return updated;
	});

	// disturber
	LOCKED(mtx) {
		cv.wait(LOCKED_LOCK(mtx), [&]() {
			return updater_ready;
		});
		x.overwrite(DISTURBED);
		assert(x.current()->version == 1);
		disturber_ready = true;
		cv.notify_one();
	}

	assert(updater.get() == nullptr);
	assert(x.current()->value == DISTURBED);
}

// Similar to `test_try_update_once_fails_with_disturber`, except
// using update() instead of try_update_once().
// Since update() loops until success, so its 2nd attempt will
// result in success.
static void test_update_succeeds_with_disturber() {
	auto updater_ready = false;
	auto disturber_ready = false;
	mutex mtx;
	condition_variable cv;

	atomic<size_t> update_attempts(0);
	mvcc<string> x(INIT);
	assert(x.current()->version == 0);
	auto updater = async(launch::async, [&] {
			auto updated = x.update([&](size_t, string const &value) {
				++update_attempts;
				if (update_attempts == 1) {
					//assert(value == INIT);
					//assert(x.current()->version == 0);
					LOCKED(mtx) {
						updater_ready = true;
						cv.notify_one();
					}

					LOCKED(mtx) {
						cv.wait(LOCKED_LOCK(mtx), [&]() {
							return disturber_ready;
						});
					}
				} else {
					//assert(x.current()->version == 1);
					//assert(value == DISTURBED);
				}

				return UPDATED;
			});
			//assert(x.current()->version == 2);
			//assert(updated->value == UPDATED);
			return updated;
	});

	// disturber
	LOCKED(mtx) {
		assert(x.current()->version == 0);
		cv.wait(LOCKED_LOCK(mtx), [&]() {
			return updater_ready;
		});
		x.overwrite(DISTURBED);
		assert(x.current()->version == 1);
		disturber_ready = true;
		cv.notify_one();
	}

	assert(updater.get() != nullptr);
	assert(x.current()->version == 2);
	assert(update_attempts == 2);
	assert(x.current()->value == UPDATED);
}

// Similar to `test_update_succeeds_with_disturber`, except
// using try_update_for() instead of update().
static void test_try_update_for_succeeds_with_disturber() {
	auto updater_ready = false;
	auto disturber_ready = false;
	mutex mtx;
	condition_variable cv;
	atomic<size_t> update_attempts(0);

	mvcc<string> x(INIT);

	auto updater = async(launch::async, [&] {
		auto updated = x.try_update_for([&](size_t, string const &value) {
			++update_attempts;
			if (update_attempts == 1) {
				//assert(value == INIT);
				//assert(x.current()->version == 0);
				LOCKED(mtx) {
					updater_ready = true;
					cv.notify_one();
				}
				LOCKED(mtx) {
					cv.wait(LOCKED_LOCK(mtx), [&]() {
						return disturber_ready;
					});
				}
			} else {
				//assert(x.current()->version != 0);
				//assert(value == DISTURBED);
			}

			return UPDATED;
		}, seconds(1));

		//assert(x.current()->version >= 2);
		//assert(updated->value == UPDATED);
		return updated;
	});

	// disturber
	LOCKED(mtx) {
		assert(x.current()->version == 0);
		cv.wait(LOCKED_LOCK(mtx), [&]() {
			return updater_ready;
		});
		x.overwrite(DISTURBED);
		assert(x.current()->version == 1);
		disturber_ready = true;
		cv.notify_one();
	}

	auto const updated = updater.get();
	assert(updated != nullptr);
	assert(updated->version >= 2);
	assert(updated->value == UPDATED);
	assert(x.current() == updated);
	assert(update_attempts == 2);
}

// try_update_for 1 second, while sleep for 100ms between
// udpate attempts so to give time for disturber to kick in
// and screw updater, and cause try_update_for to fail.
static void test_try_update_for_fails_with_disturber() {
	auto updater_ready = false;
	auto disturber_ready = false;
	mutex mtx;
	condition_variable cv;
	atomic<size_t> update_attempts(0);

	mvcc<string> x(INIT);
	auto updater = async(launch::async, [&] {
		auto updated = x.try_update_for([&](size_t, const string &value) {
			++update_attempts;
			if (update_attempts == 1) {
				LOCKED(mtx) {
					updater_ready = true;
					cv.notify_one();
				}

				LOCKED(mtx) {
					cv.wait(LOCKED_LOCK(mtx), [&]() {
						return disturber_ready;
					});
				}
			}

			this_thread::sleep_for(milliseconds(100));
			return UPDATED;
		}, seconds(1));

		return updated;
	});

	// disturber
	LOCKED(mtx) {
		cv.wait(LOCKED_LOCK(mtx), [&]() {
			return updater_ready;
		});
		x.overwrite(DISTURBED);
		disturber_ready = true;
		cv.notify_one();
	}
	auto const start = hr_now();
	do {
		x.overwrite(DISTURBED);
	} while (start + seconds(1) > hr_now());

	size_t const MINIMUM_NUMBER_OF_OVERWRITES = 100;
	auto const updated = updater.get();
	assert(updated == nullptr);
	auto const snapshot = x.current();
	assert(snapshot->version > MINIMUM_NUMBER_OF_OVERWRITES);
	assert(snapshot->value == DISTURBED);
}


struct Base {
	Base(int n ) : n(n) {} 
	int n;
};

struct Derived : Base {
	Derived(int n) : Base(n) {}
};

static void test_conversion_matching_type() {
	Base b(1);
	mvcc<Base> mb1(1);
	mvcc<Base> mb2(b);
	assert(mb2.current()->value.n == 1);
	b = 2;
	mb2.overwrite(b);
	assert(mb2.current()->value.n == 2);
	mb2.overwrite(3);
	assert(mb2.current()->value.n == 3);
	mb2 = mb1;
	assert(mb2.current()->value.n == 1);
	mb1.overwrite(4);
	assert(mb1.current()->value.n == 4);
	assert(mb2.current()->value.n == 1);
}

int _tmain(int argc, _TCHAR* argv[])
{
	test_null_snapshot_on_1st_reference();
	test_current_and_op_deref_yields_equivalent_results();
	test_snapshot_overwrite();
	test_snapshot_isolation();
	test_instance();
	test_snapshot_update();
	test_try_update_once_fails_with_disturber();
	test_update_succeeds_with_disturber();
	test_try_update_for_succeeds_with_disturber();
	test_try_update_for_fails_with_disturber();
	test_conversion_matching_type();
	return 0;
}

