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
#pragma once

#ifndef MVCC11_CONTENSION_BACKOFF_SLEEP_MS
#	define MVCC11_CONTENSION_BACKOFF_SLEEP_MS 50
#endif // MVCC11_CONTENSION_BACKOFF_SLEEP_MS

#include <memory>

namespace mvcc11 {
	namespace smart_ptr {

		using std::shared_ptr;
		using std::make_shared;
		using std::atomic_load;
		using std::atomic_store;
		using std::atomic_compare_exchange_strong;

	} // namespace smart_ptr
} // namespace mvcc11

#include <utility>
#include <chrono>
#include <thread>

namespace mvcc11 {

	template <class ValueType>
	struct snapshot {
		typedef ValueType value_type;

		snapshot(size_t ver) : version(ver) {}

		template <class U>
		snapshot(size_t ver, U&& arg): version(ver), value(std::forward<U>(arg)) {}

		size_t version;
		value_type value;
	};

	template <class ValueType>
	class mvcc {
	public:
		typedef ValueType value_type;
		typedef snapshot<value_type> snapshot_type ;
		typedef smart_ptr::shared_ptr<snapshot_type> mutable_snapshot_ptr;
		typedef smart_ptr::shared_ptr<snapshot_type const> const_snapshot_ptr;

		mvcc() : mutable_current_(smart_ptr::make_shared<snapshot_type>(0)) {}
		mvcc(value_type const &value) : mutable_current_(smart_ptr::make_shared<snapshot_type>(0, value)) {}
		mvcc(value_type &&value) : mutable_current_(smart_ptr::make_shared<snapshot_type>(0, std::move(value))) {}
		mvcc(mvcc const &other) : mutable_current_(smart_ptr::atomic_load(other)) {}
		mvcc(mvcc &&other) : mutable_current_(smart_ptr::atomic_load(other)) {}

		mvcc& operator=(mvcc const &other);
		mvcc& operator=(mvcc &&other);

		const_snapshot_ptr current();
		const_snapshot_ptr operator*();
		const_snapshot_ptr operator->();

		const_snapshot_ptr overwrite(value_type const &value);
		const_snapshot_ptr overwrite(value_type &&value);

		template <class Updater>
		const_snapshot_ptr update(Updater updater);

		template <class Updater>
		const_snapshot_ptr try_update(Updater updater);

		template <class Updater, class Clock, class Duration>
		const_snapshot_ptr try_update_until(Updater updater, std::chrono::time_point<Clock, Duration> const &timeout_time);

		template <class Updater, class Rep, class Period>
		const_snapshot_ptr try_update_for(Updater updater, std::chrono::duration<Rep, Period> const &timeout_duration);

	private:
		template <class U>
		const_snapshot_ptr overwrite_impl(U &&value);

		template <class Updater>
		const_snapshot_ptr try_update_impl(Updater &updater);

		template <class Updater, class Clock, class Duration>
		const_snapshot_ptr try_update_until_impl(Updater &updater, std::chrono::time_point<Clock, Duration> const &timeout_time);

		mutable_snapshot_ptr mutable_current_;
	};

	template <class ValueType>
	auto mvcc<ValueType>::operator=(mvcc const &other) -> mvcc & {
		smart_ptr::atomic_store(&this->mutable_current_, smart_ptr::atomic_load(&other.mutable_current_));

		return *this;
	}

	template <class ValueType>
	auto mvcc<ValueType>::operator=(mvcc &&other) -> mvcc & {
		smart_ptr::atomic_store(&this->mutable_current_, smart_ptr::atomic_load(&other.mutable_current_));

		return *this;
	}

	template <class ValueType>
	auto mvcc<ValueType>::current() -> const_snapshot_ptr {
		return smart_ptr::atomic_load(&mutable_current_);
	}

	template <class ValueType>
	auto mvcc<ValueType>::operator*() -> const_snapshot_ptr {
		return this->current();
	}

	template <class ValueType>
	auto mvcc<ValueType>::operator->() -> const_snapshot_ptr {
		return this->current();
	}

	template <class ValueType>
	auto mvcc<ValueType>::overwrite(value_type const &value) -> const_snapshot_ptr {
		return this->overwrite_impl(value);
	}

	template <class ValueType>
	auto mvcc<ValueType>::overwrite(value_type &&value) -> const_snapshot_ptr
	{
		return this->overwrite_impl(std::move(value));
	}

	template <class ValueType>
	template <class U>
	auto mvcc<ValueType>::overwrite_impl(U &&value) -> const_snapshot_ptr {
		auto desired = smart_ptr::make_shared<snapshot_type>(0, std::forward<U>(value));

		while (true) {
			auto expected = smart_ptr::atomic_load(&mutable_current_);
			desired->version = expected->version + 1;

			auto const overwritten = smart_ptr::atomic_compare_exchange_strong(&mutable_current_, &expected, desired);

			if (overwritten) {
				return desired;
			}
		}
	}

	template <class ValueType>
	template <class Updater>
	auto mvcc<ValueType>::update(Updater updater) -> const_snapshot_ptr {
		while(true) {
			auto updated = this->try_update_impl(updater);
			if (updated != nullptr) {
				return updated;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(MVCC11_CONTENSION_BACKOFF_SLEEP_MS));
		}
	}

	template <class ValueType>
	template <class Updater>
	auto mvcc<ValueType>::try_update(Updater updater) -> const_snapshot_ptr {
		return this->try_update_impl(updater);
	}

	template <class ValueType>
	template <class Updater, class Clock, class Duration>
	auto mvcc<ValueType>::try_update_until(Updater updater, std::chrono::time_point<Clock, Duration> const &timeout_time) -> const_snapshot_ptr {
		return this->try_update_until_impl(updater, timeout_time);
	}

	template <class ValueType>
	template <class Updater, class Rep, class Period>
	auto mvcc<ValueType>::try_update_for(Updater updater, std::chrono::duration<Rep, Period> const &timeout_duration) -> const_snapshot_ptr {
		auto timeout_time = std::chrono::high_resolution_clock::now() + timeout_duration;
		return this->try_update_until_impl(updater, timeout_time);
	}

	template <class ValueType>
	template <class Updater>
	auto mvcc<ValueType>::try_update_impl(Updater &updater) -> const_snapshot_ptr {
		auto expected = smart_ptr::atomic_load(&mutable_current_);
		auto const const_expected_version = expected->version;
		auto const &const_expected_value = expected->value;

		auto desired = smart_ptr::make_shared<snapshot_type>(const_expected_version + 1, updater(const_expected_version, const_expected_value));

		auto const updated = smart_ptr::atomic_compare_exchange_strong(&mutable_current_, &expected, desired);

		if (updated) {
			return desired;
		}

		return nullptr;
	}

	template <class ValueType>
	template <class Updater, class Clock, class Duration>
	auto mvcc<ValueType>::try_update_until_impl(Updater &updater, std::chrono::time_point<Clock, Duration> const &timeout_time) -> const_snapshot_ptr {
		while (true) {
			auto updated = this->try_update_impl(updater);

			if (updated != nullptr) {
				return updated;
			}

			if (std::chrono::high_resolution_clock::now() > timeout_time) {
				return nullptr;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(MVCC11_CONTENSION_BACKOFF_SLEEP_MS));
		}
	}

} // namespace mvcc11

