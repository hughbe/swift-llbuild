//===-- LaneBasedExecutionQueue.cpp ---------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildExecutionQueue.h"
#include "llbuild/BuildSystem/CommandResult.h"

#include "llbuild/Basic/LLVM.h"
#include "llbuild/Basic/PlatformUtility.h"
#include "llbuild/Basic/RedirectedProcess.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <unordered_set>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::buildsystem;

namespace std {
  template<> struct hash<llvm::StringRef> {
    size_t operator()(const StringRef& value) const {
      return size_t(hash_value(value));
    }
  };
}

namespace {

struct LaneBasedExecutionQueueJobContext {
  QueueJob& job;
};

/// Build execution queue.
//
// FIXME: Consider trying to share this with the Ninja implementation.
class LaneBasedExecutionQueue : public BuildExecutionQueue {
  /// The number of lanes the queue was configured with.
  unsigned numLanes;

  /// A thread for each lane.
  std::vector<std::unique_ptr<std::thread>> lanes;

  /// The ready queue of jobs to execute.
  std::deque<QueueJob> readyJobs;
  std::mutex readyJobsMutex;
  std::condition_variable readyJobsCondition;
  bool cancelled { false };
  bool shutdown { false };
  
  /// The set of spawned processes to terminate if we get cancelled.
  std::unordered_set<sys::RedirectedProcess> spawnedProcesses;
  std::mutex spawnedProcessesMutex;

  /// Management of cancellation and SIGKILL escalation
  std::unique_ptr<std::thread> killAfterTimeoutThread = nullptr;
  std::condition_variable queueCompleteCondition;
  std::mutex queueCompleteMutex;
  bool queueComplete { false };

  /// The base environment.
  const char* const* environment;
  
  void executeLane(unsigned laneNumber) {
    // Set the thread name, if available.
#if defined(__APPLE__)
    pthread_setname_np(
        (llvm::Twine("org.swift.llbuild Lane-") +
         llvm::Twine(laneNumber)).str().c_str());
#elif defined(__linux__)
    pthread_setname_np(
        pthread_self(),
        (llvm::Twine("org.swift.llbuild Lane-") +
         llvm::Twine(laneNumber)).str().c_str());
#endif

    // Set the QoS class, if available.
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
    
    // Execute items from the queue until shutdown.
    while (true) {
      // Take a job from the ready queue.
      QueueJob job{};
      {
        std::unique_lock<std::mutex> lock(readyJobsMutex);

        // While the queue is empty, wait for an item.
        while (!shutdown && readyJobs.empty()) {
          readyJobsCondition.wait(lock);
        }
        if (shutdown && readyJobs.empty())
          return;

        // Take an item according to the chosen policy.
        job = readyJobs.front();
        readyJobs.pop_front();
      }

      // If we got an empty job, the queue is shutting down.
      if (!job.getForCommand())
        break;

      // Process the job.
      LaneBasedExecutionQueueJobContext context{ job };
      getDelegate().commandJobStarted(job.getForCommand());
      job.execute(reinterpret_cast<QueueJobContext*>(&context));
      getDelegate().commandJobFinished(job.getForCommand());
    }
  }

  void killAfterTimeout() {
    std::unique_lock<std::mutex> lock(queueCompleteMutex);

    if (!queueComplete) {
      // Shorten timeout if in testing context
      if (getenv("LLBUILD_TEST") != nullptr) {
        queueCompleteCondition.wait_for(lock, std::chrono::milliseconds(1000));
      } else {
        queueCompleteCondition.wait_for(lock, std::chrono::seconds(10));
      }

      sendSignalToProcesses(sys::RedirectedProcess::sigkill());
    }
  }

  void sendSignalToProcesses(int signal) {
    std::unique_lock<std::mutex> lock(spawnedProcessesMutex);

    for (sys::RedirectedProcess process: spawnedProcesses) {
      // We are killing the whole process group here, this depends on us
      // spawning each process in its own group earlier.
      process.kill(signal);
    }
  }

public:
  LaneBasedExecutionQueue(BuildExecutionQueueDelegate& delegate,
                          unsigned numLanes,
                          const char* const* environment)
      : BuildExecutionQueue(delegate), numLanes(numLanes),
        environment(environment)
  {
    for (unsigned i = 0; i != numLanes; ++i) {
      lanes.push_back(std::unique_ptr<std::thread>(
                          new std::thread(
                              &LaneBasedExecutionQueue::executeLane, this, i)));
    }
  }
  
  virtual ~LaneBasedExecutionQueue() {
    // Shut down the lanes.
    {
      std::unique_lock<std::mutex> lock(readyJobsMutex);
      shutdown = true;
      readyJobsCondition.notify_all();
    }

    for (unsigned i = 0; i != numLanes; ++i) {
      lanes[i]->join();
    }

    if (killAfterTimeoutThread) {
      {
        std::unique_lock<std::mutex> lock(queueCompleteMutex);
        queueComplete = true;
        queueCompleteCondition.notify_all();
      }
      killAfterTimeoutThread->join();
    }
  }

  virtual void addJob(QueueJob job) override {
    std::lock_guard<std::mutex> guard(readyJobsMutex);
    readyJobs.push_back(job);
    readyJobsCondition.notify_one();
  }

  virtual void cancelAllJobs() override {
    {
      std::lock_guard<std::mutex> lock(readyJobsMutex);
      std::lock_guard<std::mutex> guard(spawnedProcessesMutex);
      if (cancelled) return;
      cancelled = true;
      readyJobsCondition.notify_all();
    }

    sendSignalToProcesses(SIGINT);
    killAfterTimeoutThread = llvm::make_unique<std::thread>(
        &LaneBasedExecutionQueue::killAfterTimeout, this);
  }

  virtual CommandResult
  executeProcess(QueueJobContext* opaqueContext,
                 ArrayRef<StringRef> commandLine,
                 ArrayRef<std::pair<StringRef,
                                    StringRef>> environment,
                 bool inheritEnvironment) override {
    {
      std::unique_lock<std::mutex> lock(readyJobsMutex);
      // Do not execute new processes anymore after cancellation.
      if (cancelled) {
        return CommandResult::Cancelled;
      }
    }

    // Assign a process handle, which just needs to be unique for as long as we
    // are communicating with the delegate.
    struct BuildExecutionQueueDelegate::ProcessHandle handle;
    handle.id = reinterpret_cast<uintptr_t>(&handle);

    // Whether or not we are capturing output.
    const bool shouldCaptureOutput = true;

    LaneBasedExecutionQueueJobContext& context =
      *reinterpret_cast<LaneBasedExecutionQueueJobContext*>(opaqueContext);
    getDelegate().commandProcessStarted(context.job.getForCommand(), handle);

    // Form the complete C string command line.
    std::vector<std::string> argsStorage(
        commandLine.begin(), commandLine.end());
    std::vector<const char*> args(argsStorage.size() + 1);
    for (size_t i = 0; i != argsStorage.size(); ++i) {
      args[i] = argsStorage[i].c_str();
    }
    args[argsStorage.size()] = nullptr;

    // Form the complete environment.
    std::vector<std::string> envStorage;
    std::vector<const char*> env;
    const char* const* envp = nullptr;

    // If no additional environment is supplied, use the base environment.
    if (environment.empty()) {
      envp = this->environment;
    } else {
      // Inherit the base environment, if desired.
      if (inheritEnvironment) {
        std::unordered_set<StringRef> overriddenKeys{};
        // Compute the set of strings which are overridden in the process
        // environment.
        for (const auto& entry: environment) {
          overriddenKeys.insert(entry.first);
        }

        // Form the complete environment by adding the base key value pairs
        // which are not overridden, then adding all of the overridden ones.
        for (const char* const* p = this->environment; *p != nullptr; ++p) {
          // Find the key.
          auto key = StringRef(*p).split('=').first;
          if (!overriddenKeys.count(key)) {
            env.emplace_back(*p);
          }
        }
      }

      // Add the requested environment.
      for (const auto& entry: environment) {
        SmallString<256> assignment;
        assignment += entry.first;
        assignment += '=';
        assignment += entry.second;
        assignment += '\0';
        envStorage.emplace_back(assignment.str());
      }
      // We must do this in a second pass, once the entries are guaranteed not
      // to move.
      for (const auto& entry: envStorage) {
        env.emplace_back(entry.c_str());
      }
      env.emplace_back(nullptr);
      envp = env.data();
    }

    // Resolve the executable path, if necessary.
    //
    // FIXME: This should be cached.
    if (!llvm::sys::path::is_absolute(args[0])) {
      auto res = llvm::sys::findProgramByName(args[0]);
      if (!res.getError()) {
        argsStorage[0] = *res;
        args[0] = argsStorage[0].c_str();
      }
    }
      
    // Spawn the command.
    pid_t pid = -1;
    bool wasCancelled;
    {
      // We need to hold the spawn processes lock when we spawn, to ensure that
      // we don't create a process in between when we are cancelled.
      std::lock_guard<std::mutex> guard(spawnedProcessesMutex);
      wasCancelled = cancelled;
      
      // If we have been cancelled since we started, do nothing.
      if (!wasCancelled) {
        if (posix_spawn(&pid, args[0], /*file_actions=*/&fileActions,
                        /*attrp=*/&attributes, const_cast<char**>(args.data()),
                        const_cast<char* const*>(envp)) != 0) {
          getDelegate().commandProcessHadError(
              context.job.getForCommand(), handle,
              Twine("unable to spawn process (") + strerror(errno) + ")");
          getDelegate().commandProcessFinished(context.job.getForCommand(), handle,
                                               CommandResult::Failed, -1);
          pid = -1;
        } else {
          spawnedProcesses.insert(pid);
        }
      }
    }

    posix_spawn_file_actions_destroy(&fileActions);
    posix_spawnattr_destroy(&attributes);
    
    // If we failed to launch a process, clean up and abort.
    if (pid == -1) {
      if (shouldCaptureOutput) {
        ::close(outputPipe[1]);
        ::close(outputPipe[0]);
      }
      return wasCancelled ? CommandResult::Cancelled : CommandResult::Failed;
    }

    // Read the command output, if capturing.
    if (shouldCaptureOutput) {
      // Close the write end of the output pipe.
      ::close(outputPipe[1]);

      // Read all the data from the output pipe.
      while (true) {
        char buf[4096];
        ssize_t numBytes = read(outputPipe[0], buf, sizeof(buf));
        if (numBytes < 0) {
          getDelegate().commandProcessHadError(
              context.job.getForCommand(), handle,
              Twine("unable to read process output (") + strerror(errno) + ")");
          break;
        }

        if (numBytes == 0)
          break;

        // Notify the client of the output.
        getDelegate().commandProcessHadOutput(
            context.job.getForCommand(), handle,
            StringRef(buf, numBytes));
      }

      // Close the read end of the pipe.
      ::close(outputPipe[0]);
    }

    // Wait for the command to complete.
    int status;
    bool waitResult = process.waitForCompletion(&status);

    // Update the set of spawned processes.
    {
        std::lock_guard<std::mutex> guard(spawnedProcessesMutex);
        spawnedProcesses.erase(process);
    }

    if (!waitResult) {
      getDelegate().commandProcessHadError(
          context.job.getForCommand(), handle,
          Twine("unable to wait for process (") + strerror(errno) + ")");
      getDelegate().commandProcessFinished(context.job.getForCommand(), handle,
                                           CommandResult::Failed, -1);
      return CommandResult::Failed;
    }
    
    // Notify of the process completion.
    bool cancelled = sys::RedirectedProcess::isProcessCancelledStatus(status);
    CommandResult commandResult = cancelled ? CommandResult::Cancelled : (status == 0) ? CommandResult::Succeeded : CommandResult::Failed;
    getDelegate().commandProcessFinished(context.job.getForCommand(), handle,
                                         commandResult, status);
    return commandResult;
  }
};

}

#if !defined(_WIN32)
extern "C" {
  extern char **environ;
}
#endif

BuildExecutionQueue*
llbuild::buildsystem::createLaneBasedExecutionQueue(
    BuildExecutionQueueDelegate& delegate, int numLanes,
    const char* const* environment) {
  if (!environment) {
    environment = const_cast<const char* const*>(environ);
  }
  return new LaneBasedExecutionQueue(delegate, numLanes, environment);
}
