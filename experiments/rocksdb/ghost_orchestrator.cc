// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "experiments/rocksdb/ghost_orchestrator.h"

#include "absl/functional/bind_front.h"

namespace ghost_test {

void GhostOrchestrator::InitThreadPool() {
  // Initialize the thread pool.
  std::vector<ghost::GhostThread::KernelScheduler> kernel_schedulers;
  std::vector<std::function<void(uint32_t)>> thread_work;
  // Set up the load generator thread. The load generator thread runs in CFS.
  kernel_schedulers.push_back(ghost::GhostThread::KernelScheduler::kCfs);
  thread_work.push_back(
      absl::bind_front(&GhostOrchestrator::LoadGenerator, this));
  // Set up the worker threads. The worker threads run in ghOSt.
  kernel_schedulers.insert(kernel_schedulers.end(), options().num_workers,
                           ghost::GhostThread::KernelScheduler::kGhost);
  thread_work.insert(thread_work.end(), options().num_workers,
                     absl::bind_front(&GhostOrchestrator::Worker, this));
  // Checks.
  // Add 1 to account for the load generator thread.
  CHECK_EQ(kernel_schedulers.size(), total_threads());
  CHECK_EQ(kernel_schedulers.size(), thread_work.size());
  // Pass the scheduler types and the thread work to 'Init'.
  thread_pool().Init(kernel_schedulers, thread_work);
}

void GhostOrchestrator::InitPrioTable() {
  CHECK(UsesPrioTable());

  const std::vector<ghost::Gtid> gtids = thread_pool().GetGtids();
  // Add 1 to account for the load generator thread.
  CHECK_EQ(gtids.size(), total_threads());

  ghost::work_class wc;
  prio_table_helper_->GetWorkClass(kWorkClassIdentifier, wc);
  wc.id = kWorkClassIdentifier;
  wc.flags = WORK_CLASS_ONESHOT;
  wc.qos = options().ghost_qos;
  // Write '100' in for the deadline just in case we want to run the experiments
  // with the ghOSt EDF (Earliest-Deadline-First) scheduler.
  wc.exectime = 100;
  // 'period' is irrelevant because all threads scheduled by ghOSt are
  // one-shots.
  wc.period = 0;
  prio_table_helper_->SetWorkClass(kWorkClassIdentifier, wc);

  // Start at index 1 because the first thread is the load generator (SID 0),
  // which is scheduled by CFS (Linux Completely Fair Scheduler).
  for (size_t i = 1; i < gtids.size(); ++i) {
    ghost::sched_item si;
    prio_table_helper_->GetSchedItem(/*sid=*/i, si);
    si.sid = i;
    si.wcid = kWorkClassIdentifier;
    si.gpid = gtids[i].id();
    si.flags = 0;
    si.deadline = 0;
    prio_table_helper_->SetSchedItem(/*sid=*/i, si);
  }
}

GhostOrchestrator::GhostOrchestrator(Orchestrator::Options opts)
    // Add 1 to account for the load generator.
    : Orchestrator(opts, opts.num_workers + 1) {
  // We include a sched item for the load generator even though the load
  // generator is scheduled by CFS (Linux Completely Fair Scheduler) rather
  // than ghOSt. While the sched with SID 0 is unused, workers are able to
  // access their own sched item by passing their SID directly rather than
  // having to subtract 1 from their SID.
  if (UsesPrioTable()) {
    prio_table_helper_ = std::make_unique<PrioTableHelper>(
        /*num_sched_items=*/total_threads(), /*num_work_classes=*/1);
  } else {
    CHECK(UsesFutex());
    thread_wait_ = std::make_unique<ThreadWait>(/*num_threads=*/total_threads(),
                                                ThreadWait::WaitType::kFutex);
  }
  CHECK_EQ(options().worker_cpus.size(), 0);

  InitThreadPool();
  // This must be called after 'InitThreadPool' since it accesses the GTIDs of
  // the threads in the thread pool.
  if (UsesPrioTable()) {
    InitPrioTable();
  }

  threads_ready_.Notify();
}

void GhostOrchestrator::Terminate() {
  const absl::Duration runtime = absl::Now() - start();
  // Do this check after calculating 'runtime' to avoid inflating 'runtime'.
  CHECK_GT(start(), absl::UnixEpoch());

  // The load generator should exit first. If any worker were to exit before the
  // load generator, the load generator would trigger
  // `CHECK(prio_table_helper_->IsIdle(worker_sid))`.
  thread_pool().MarkExit(0);
  while (thread_pool().NumExited() < 1) {
  }

  for (size_t i = 1; i < thread_pool().NumThreads(); ++i) {
    thread_pool().MarkExit(i);
  }
  while (thread_pool().NumExited() < total_threads()) {
    // Makes ghOSt threads runnable so that they can exit.
    for (size_t i = 0; i < options().num_workers; ++i) {
      // We start at SID 1 (the first worker) since the load generator (SID 0)
      // is not scheduled by ghOSt and is always runnable.
      if (UsesPrioTable()) {
        prio_table_helper_->MarkRunnable(i + 1);
      } else {
        CHECK(UsesFutex());
        thread_wait_->MarkRunnable(i + 1);
      }
    }
  }
  thread_pool().Join();

  PrintResults(runtime);
}

bool GhostOrchestrator::SkipIdleWorker(uint32_t worker_sid) {
  if (UsesPrioTable()) {
    // This worker has finished its work but has not yet marked itself idle in
    // ghOSt. It is about to do so, so we cannot assign more work to it in the
    // meantime. If we did assign more work to the worker and then mark the
    // worker runnable, and then the worker marks itself idle, the worker will
    // never wake up and we will lose the worker for the remainder of the
    // experiment.
    return !prio_table_helper_->IsIdle(worker_sid);
  } else {
    CHECK(UsesFutex());
    return false;
  }
}

void GhostOrchestrator::GetIdleWorkerSIDs() {
  idle_sids_.clear();
  for (size_t i = 0; i < options().num_workers; ++i) {
    // Add 1 to skip the load generator thread, which has SID 0.
    uint32_t worker_sid = i + 1;
    if (worker_work()[worker_sid]->num_requests.load(
            std::memory_order_acquire) == 0 &&
        !SkipIdleWorker(worker_sid)) {
      idle_sids_.push_back(worker_sid);
    }
  }
}

void GhostOrchestrator::LoadGenerator(uint32_t sid) {
  if (!first_run().Triggered(sid)) {
    CHECK(first_run().Trigger(sid));
    CHECK_EQ(ghost::GhostHelper()->SchedSetAffinity(
                 ghost::Gtid::Current(),
                 ghost::MachineTopology()->ToCpuList(
                     std::vector<int>{options().load_generator_cpu})),
             0);
    // Use 'printf' instead of 'std::cout' so that the print contents do not get
    // interleaved with the dispatcher's and the workers' print contents.
    printf("Load generator (SID %u, TID: %ld, affined to CPU %u)\n", sid,
           syscall(SYS_gettid), options().load_generator_cpu);
    threads_ready_.WaitForNotification();
    set_start(absl::Now());
    network().Start();
  }

  GetIdleWorkerSIDs();
  uint32_t size = idle_sids_.size();
  for (uint32_t i = 0; i < size; ++i) {
    uint32_t worker_sid = idle_sids_.front();
    // We can do a relaxed load rather than an acquire load because
    // 'GetIdleWorkerSIDs' already did an acquire load for 'num_requests'.
    CHECK_EQ(
        worker_work()[worker_sid]->num_requests.load(std::memory_order_relaxed),
        0);

    // We assign a deadline to the worker just in case we want to run the
    // experiment with the ghOSt EDF (Earliest-Deadline-First) scheduler. The
    // deadline is not needed and is ignored for the centralized queuing
    // scheduler, the Shinjuku scheduler, and the Shenango scheduler.
    constexpr absl::Duration deadline = absl::Microseconds(100);

    worker_work()[worker_sid]->requests.clear();
    Request request;
    for (size_t i = 0; i < options().batch; ++i) {
      if (network().Poll(request)) {
        request.request_assigned = absl::Now();
        worker_work()[worker_sid]->requests.push_back(request);
      } else {
        // No more requests waiting in the ingress queue, so give the
        // requests we have so far to the worker.
        break;
      }
    }
    if (!worker_work()[worker_sid]->requests.empty()) {
      // Assign the batch of requests to the next worker
      idle_sids_.pop_front();
      CHECK_LE(worker_work()[worker_sid]->requests.size(), options().batch);
      worker_work()[worker_sid]->num_requests.store(
          worker_work()[worker_sid]->requests.size(),
          std::memory_order_release);

      if (UsesPrioTable()) {
        CHECK(prio_table_helper_->IsIdle(worker_sid));
        ghost::sched_item si;
        prio_table_helper_->GetSchedItem(worker_sid, si);
        si.deadline =
            PrioTableHelper::ToRawDeadline(ghost::MonotonicNow() + deadline);
        si.flags |= SCHED_ITEM_RUNNABLE;
        // All other flags were set in 'InitGhost' and do not need to be
        // changed.
        prio_table_helper_->SetSchedItem(worker_sid, si);
      } else {
        CHECK(UsesFutex());
        thread_wait_->MarkRunnable(worker_sid);
      }
    } else {
      // There is no work waiting in the ingress queue.
      break;
    }
  }
}

void GhostOrchestrator::Worker(uint32_t sid) {
  if (!first_run().Triggered(sid)) {
    CHECK(first_run().Trigger(sid));
    printf("Worker (SID %u, TID: %ld, not affined to any CPU)\n", sid,
           syscall(SYS_gettid));

    if (UsesFutex()) {
      thread_wait_->WaitUntilRunnable(sid);
    }
  }

  WorkerWork* work = worker_work()[sid].get();

  size_t num_requests = work->num_requests.load(std::memory_order_acquire);
  if (num_requests == 0) {
    // The worker might only be first scheduled when the process is exiting (so
    // the worker does not have any requests to schedule). This if block
    // captures that case.
    return;
  }
  CHECK_LE(num_requests, options().batch);
  CHECK_EQ(num_requests, work->requests.size());

  for (size_t i = 0; i < num_requests; ++i) {
    Request& request = work->requests[i];
    request.request_start = absl::Now();
    HandleRequest(request, gen()[sid]);
    request.request_finished = absl::Now();

    requests()[sid].push_back(request);
  }

  if (UsesPrioTable()) {
    // Set 'num_requests' to 0 before calling 'prio_table_helper_->MarkIdle'
    // since the worker could be descheduled by ghOSt at any time after calling
    // 'prio_table_helper_->MarkIdle' (or even before returning from
    // 'prio_table_helper_->MarkIdle'!). The load generator checks to make sure
    // that a worker with 'num_requests' set to 0 has also marked itself idle in
    // ghOSt before assigning more work to it and marking it runnable again. See
    // the comments above in 'LoadGenerator' for more details about the race
    // condition this prevents.
    work->num_requests.store(0, std::memory_order_release);
    prio_table_helper_->MarkIdle(sid);
    prio_table_helper_->WaitUntilRunnable(sid);
  } else {
    CHECK(UsesFutex());
    thread_wait_->MarkIdle(sid);
    // Do this after `MarkIdle`. If the worker did it before calling `MarkIdle`,
    // the dispatcher could assign work to this worker and then mark it
    // runnable. Then the worker could mark itself idle and go spin/sleep on
    // `WaitUntilRunnable`, causing the worker to do no work for the duration of
    // the experiment. Remember that `MarkIdle` does not make the worker
    // spin/sleep -- only `WaitUntilRunnable` does.
    work->num_requests.store(0, std::memory_order_release);
    thread_wait_->WaitUntilRunnable(sid);
  }
}

}  // namespace ghost_test
