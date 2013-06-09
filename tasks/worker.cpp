/*
 * Copyright (c) 2013 Andreas Pohl <apohl79 at gmail.com>
 *
 * This file is part of libtasks.
 * 
 * libtasks is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * libtasks is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with libtasks.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tasks/worker.h>
#include <tasks/dispatcher.h>
#include <tasks/task.h>
#include <tasks/io_task.h>
#include <tasks/timer_task.h>
#include <chrono>

namespace tasks {

	worker::worker(int id) : m_id(id), m_thread(&worker::run, this) {
		m_term.store(false);
		// Initialize and add the threads async watcher
		ev_async_init(&m_signal_watcher, ev_async_callback);
		m_signal_watcher.data = new task_func_queue;
		ev_async_start(ev_default_loop(0), &m_signal_watcher);
	}

	worker::~worker() {
		tdbg(get_string() << ": dtor" << std::endl);
		m_term.store(true);
		m_thread.join();
		task_func_queue* tfq = (task_func_queue*) m_signal_watcher.data;
		delete tfq;
	}

	void worker::promote_leader() {
		// Multiple IO events could be fired by one event loop iteration. As we promote the next leader
		// after the 
		std::shared_ptr<worker> w = dispatcher::get_instance()->get_free_worker();
		if (nullptr != w) {
			// If we find a free worker, we promote it to the next leader. This thread stays leader
			// otherwise.
			m_leader.store(false);
			w->set_event_loop(std::move(m_loop));
		}
	}
	
	void worker::run() {
		// Wait for a short while before entering the loop to allow the dispatcher
		// to finish its initialization.
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		while (!m_term) {
			// Wait until promoted to the leader thread
			if (!m_leader) {
				tdbg(get_string() << ": waiting..." << std::endl);
				std::unique_lock<std::mutex> lock(m_work_mutex);
				// use wait_for to detect shutdowns
				while (m_work_cond.wait_for(lock, std::chrono::milliseconds(100)) == std::cv_status::timeout
					   && !m_leader
					   && !m_term) {}
			}

			// Became leader, so execute the event loop
			while (m_leader && !m_term) {
				tdbg(get_string() << ": running event loop" << std::endl);
				ev_loop(m_loop->loop, EVLOOP_ONESHOT);
				// Check if events got fired
				if (!m_io_events_queue.empty() || !m_timer_events_queue.empty()) {
					// Now promote the next leader and call the event handlers
					promote_leader();

					// IO
					while (!m_io_events_queue.empty()) {
						io_event event = m_io_events_queue.front();
						if (event.task->handle_event(this, event.revents)) {
							// On success we activate the watcher again. 
							event.task->start_watcher(this);
						} else {
							if (event.task->delete_after_error()) {
								delete event.task;
							}
						}
						m_io_events_queue.pop();
					}
					
					// TIMERS
					while (!m_timer_events_queue.empty()) {
						timer_task* task = m_timer_events_queue.front();
						if (task->handle_event(this, 0)) {
							if (task->get_repeat() > 0) {
								// On success we activate the watcher again.
								task->start_watcher(this);
							} else {
								// Non repeatable tasks will be deleted here.
								delete task;
							}
						} else {
							if (task->delete_after_error()) {
								delete task;
							}
						}
						m_timer_events_queue.pop();
					}
				}
			}

			if (!m_term) {
				// Add this worker as available worker
				dispatcher::get_instance()->add_free_worker(get_id());
			} else {
				// Shutdown, the leader terminates the loop
				if (m_leader) {
					// FIXME: Iterate over all watchers and delete registered tasks.
					ev_unloop (m_loop->loop, EVUNLOOP_ALL);
				}
			}
		}
	}

	void worker::handle_io_event(ev_io* watcher, int revents) {
		io_task* task = (tasks::io_task*) watcher->data;
		assert(nullptr != task);
		task->stop_watcher(this);
		io_event event = {task, revents};
		m_io_events_queue.push(event);
	}

	void worker::handle_timer_event(ev_timer* watcher) {
		timer_task* task = (tasks::timer_task*) watcher->data;
		assert(nullptr != task);
		task->stop_watcher(this);
		m_timer_events_queue.push(task);
	}

} // tasks
