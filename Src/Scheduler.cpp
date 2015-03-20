#include "Scheduler.h"
//#include "Assert.h"


//TODO: Split to files. One file - one class.

namespace MT
{
	static const size_t TASK_BUFFER_CAPACITY = 4096;

	ThreadContext::ThreadContext()
		: taskScheduler(nullptr)
		, hasNewTasksEvent(EventReset::AUTOMATIC, true)
		, state(ThreadState::ALIVE)
		, descBuffer(TASK_BUFFER_CAPACITY)
	{
	}

	ThreadContext::~ThreadContext()
	{
	}


	FiberContext::FiberContext()
		: taskStatus(FiberTaskStatus::UNKNOWN)
		, currentGroup(TaskGroup::GROUP_UNDEFINED)
		, childrenFibersCount(0)
		, parentFiber(nullptr)
		, threadContext(nullptr)
	{
	}

	void FiberContext::Reset()
	{
		ASSERT(childrenFibersCount.Get() == 0, "Can't release fiber with active children fibers");

		currentGroup = TaskGroup::GROUP_UNDEFINED;
		currentTask = TaskDesc();
		parentFiber = nullptr;
		threadContext = nullptr;
	}

	void FiberContext::WaitGroupAndYield(TaskGroup::Type group)
	{
		ASSERT(threadContext, "Sanity check failed!");
		ASSERT(threadContext->taskScheduler->IsWorkerThread(), "Can't use WaitGroupAndYield outside Task. Use TaskScheduler.WaitGroup() instead.");
		ASSERT(threadContext->thread.IsCurrentThread(), "Thread context sanity check failed");

		VERIFY(group != currentGroup, "Can't wait the same group. Deadlock detected!", return);
		VERIFY(group < TaskGroup::COUNT, "Invalid group!", return);

		ConcurrentQueueLIFO<FiberContext*> & groupQueue = threadContext->taskScheduler->waitTaskQueues[group];

		// Change status
		taskStatus = FiberTaskStatus::AWAITING_GROUP;

		// Add current fiber to awaiting queue
		groupQueue.Push(this);

		Fiber & schedulerFiber = threadContext->schedulerFiber;

		// Yielding, so reset thread context
		threadContext = nullptr;

		//switch to scheduler
		Fiber::SwitchTo(fiber, schedulerFiber);
	}

	void FiberContext::RunSubtasksAndYield(TaskGroup::Type taskGroup, fixed_array<TaskBucket>& buckets)
	{
		ASSERT(threadContext, "Sanity check failed!");
		ASSERT(taskGroup < TaskGroup::COUNT, "Sanity check failed!");
		ASSERT(threadContext->taskScheduler->IsWorkerThread(), "Can't use RunSubtasksAndYield outside Task. Use TaskScheduler.WaitGroup() instead.");
		ASSERT(threadContext->thread.IsCurrentThread(), "Thread context sanity check failed");

		// add to scheduler
		threadContext->taskScheduler->RunTasksImpl(taskGroup, buckets, this);

		//
		ASSERT(threadContext->thread.IsCurrentThread(), "Thread context sanity check failed");

		// Change status
		taskStatus = FiberTaskStatus::AWAITING_CHILD;

		Fiber & schedulerFiber = threadContext->schedulerFiber;

		// Yielding, so reset thread context
		threadContext = nullptr;

		//switch to scheduler
		Fiber::SwitchTo(fiber, schedulerFiber);
	}


	TaskScheduler::GroupStats::GroupStats()
	{
		inProgressTaskCount.Set(0);
		allDoneEvent.Create( EventReset::MANUAL, true );
	}

	TaskScheduler::TaskScheduler()
		: roundRobinThreadIndex(0)
	{

		//query number of processor
		threadsCount = Max(Thread::GetNumberOfHardwareThreads() - 2, 1);

		if (threadsCount > MT_MAX_THREAD_COUNT)
		{
			threadsCount = MT_MAX_THREAD_COUNT;
		}

		// create fiber pool
		for (uint32 i = 0; i < MT_MAX_FIBERS_COUNT; i++)
		{
			FiberContext& context = fiberContext[i];
			context.fiber.Create(MT_FIBER_STACK_SIZE, FiberMain, &context);
			availableFibers.Push( &context );
		}

		// create worker thread pool
		for (uint32 i = 0; i < threadsCount; i++)
		{
			threadContext[i].taskScheduler = this;
			threadContext[i].thread.Start( MT_SCHEDULER_STACK_SIZE, ThreadMain, &threadContext[i] );
		}

	}

	TaskScheduler::~TaskScheduler()
	{
		for (uint32 i = 0; i < threadsCount; i++)
		{
			threadContext[i].state.Set(ThreadState::EXIT);
			threadContext[i].hasNewTasksEvent.Signal();
		}

		for (uint32 i = 0; i < threadsCount; i++)
		{
			threadContext[i].thread.Stop();
		}
	}

	FiberContext* TaskScheduler::RequestFiberContext(const GroupedTask& task)
	{
		FiberContext *fiberContext = nullptr;
		if (!availableFibers.TryPop(fiberContext))
		{
			ASSERT(false, "Fibers pool is empty");
		}

		fiberContext->currentTask = task.desc;
		fiberContext->currentGroup = task.group;
		fiberContext->parentFiber = task.parentFiber;
		return fiberContext;
	}

	void TaskScheduler::ReleaseFiberContext(FiberContext* fiberContext)
	{
		ASSERT(fiberContext != nullptr, "Can't release nullptr Fiber");
		fiberContext->Reset();
		availableFibers.Push(fiberContext);
	}


	void TaskScheduler::RestoreAwaitingTasks(TaskGroup::Type taskGroup)
	{
		ConcurrentQueueLIFO<FiberContext*> & groupQueue = waitTaskQueues[taskGroup];
		//TODO: move awaiting tasks into execution thread queues
	}

	bool TaskScheduler::ExecuteTask(ThreadContext& threadContext, FiberContext* fiberContext)
	{
		ASSERT(threadContext.thread.IsCurrentThread(), "Thread context sanity check failed");

		for(int iteration = 0;;iteration++)
		{
			ASSERT(fiberContext, "Invalid fiber context");
			ASSERT(fiberContext->currentTask.IsValid(), "Invalid task");
			ASSERT(fiberContext->currentGroup < TaskGroup::COUNT, "Invalid task group");

			// Set actual thread context to fiber
			fiberContext->threadContext = &threadContext;

			// Update task status
			fiberContext->taskStatus = FiberTaskStatus::RUNNED;

			ASSERT(fiberContext->threadContext->thread.IsCurrentThread(), "Thread context sanity check failed");

			// Run current task code
			Fiber::SwitchTo(threadContext.schedulerFiber, fiberContext->fiber);

			// If task was done
			if (fiberContext->taskStatus == FiberTaskStatus::FINISHED)
			{
				TaskGroup::Type taskGroup = fiberContext->currentGroup;
				ASSERT(taskGroup < TaskGroup::COUNT, "Invalid group.");

				// Update group status
				int groupTaskCount = threadContext.taskScheduler->groupStats[taskGroup].inProgressTaskCount.Dec();
				ASSERT(groupTaskCount >= 0, "Sanity check failed!");
				if (groupTaskCount == 0)
				{
					// Restore awaiting tasks
					threadContext.taskScheduler->RestoreAwaitingTasks(taskGroup);
					threadContext.taskScheduler->groupStats[taskGroup].allDoneEvent.Signal();
				}

				// Update total task count
				groupTaskCount = threadContext.taskScheduler->allGroupStats.inProgressTaskCount.Dec();
				ASSERT(groupTaskCount >= 0, "Sanity check failed!");
				if (groupTaskCount == 0)
				{
					// Notify all tasks in all group finished
					threadContext.taskScheduler->allGroupStats.allDoneEvent.Signal();
				}

				FiberContext* parentFiberContext = fiberContext->parentFiber;

				// Release fiber context
				if (iteration > 0)
				{
					threadContext.taskScheduler->ReleaseFiberContext(fiberContext);
				}

				// 
				if (parentFiberContext != nullptr)
				{
					int childrenFibersCount = parentFiberContext->childrenFibersCount.Dec();
					ASSERT(childrenFibersCount >= 0, "Sanity check failed!");

					if (childrenFibersCount == 0)
					{
						// This is a last subtask. Restore parent task
#if FIBER_DEBUG
						int fiberUsageCounter = parentFiberContext->fiber.GetUsageCounter();
						ASSERT(fiberUsageCounter == 0, "Parent fiber in invalid state");
#endif

						ASSERT(threadContext.thread.IsCurrentThread(), "Thread context sanity check failed");
						ASSERT(parentFiberContext->threadContext == nullptr, "Inactive parent should not have a valid thread context");

						// WARNING!! Thread context can changed here! Set actual current thread context.
						parentFiberContext->threadContext = &threadContext;

						ASSERT(parentFiberContext->threadContext->thread.IsCurrentThread(), "Thread context sanity check failed");

						// Change current fiber to parent fiber
						fiberContext = parentFiberContext;

						// Continue execution

					} else
					{
						// Other subtasks still exist.
						// Exiting and destroy current task context.
						return true;
					}
				} else
				{
					// Task is finished and no parent task.
					// Exiting and destroy context.
					return true;
				}
			} else if (fiberContext->taskStatus == FiberTaskStatus::AWAITING_GROUP)
			{
				// Task was yielded, due to awaiting another task group
				// Exiting and keep context
				return false;
			}
			else if (fiberContext->taskStatus == FiberTaskStatus::AWAITING_CHILD)
			{
				// Task was yielded, due to subtask spawn
				// Exiting and keep context
				return false;
			}
			else if (fiberContext->taskStatus == FiberTaskStatus::RUNNED)
			{
				ASSERT(false, "Incorrect task status")
			}
			else
			{
				ASSERT(false, "State is not supperted. Undefined behaviour!")
			}
		} // infinite loop
	}


	void TaskScheduler::FiberMain(void* userData)
	{
		FiberContext& fiberContext = *(FiberContext*)(userData);

		for(;;)
		{
			ASSERT(fiberContext.currentTask.IsValid(), "Invalid task in fiber context");
			ASSERT(fiberContext.currentGroup < TaskGroup::COUNT, "Invalid task group");
			ASSERT(fiberContext.threadContext, "Invalid thread context");
			ASSERT(fiberContext.threadContext->thread.IsCurrentThread(), "Thread context sanity check failed");

			fiberContext.currentTask.taskFunc( fiberContext, fiberContext.currentTask.userData );

			fiberContext.taskStatus = FiberTaskStatus::FINISHED;

			Fiber::SwitchTo(fiberContext.fiber, fiberContext.threadContext->schedulerFiber);
		}

	}


	void TaskScheduler::ThreadMain( void* userData )
	{
		ThreadContext& context = *(ThreadContext*)(userData);
		ASSERT(context.taskScheduler, "Task scheduler must be not null!");
		context.schedulerFiber.CreateFromThread(context.thread);

		while(context.state.Get() != ThreadState::EXIT)
		{
			GroupedTask task;
			if (context.queue.TryPop(task))
			{
				// There is a new task

				FiberContext* fiberContext = context.taskScheduler->RequestFiberContext(task);
				ASSERT(fiberContext, "Can't get execution context from pool");
				ASSERT(fiberContext->currentTask.IsValid(), "Sanity check failed");

				// prevent invalid fiber resume from child tasks, before ExecuteTask is done
				fiberContext->childrenFibersCount.Inc();

				for(;;)
				{
					bool canDropContext = ExecuteTask(context, fiberContext);

					// Can drop fiber context - task is finished
					if (canDropContext || fiberContext->taskStatus == FiberTaskStatus::FINISHED)
					{
						int childrenFibersCount = fiberContext->childrenFibersCount.Dec();
						ASSERT( childrenFibersCount == 0, "Sanity check failed");

						context.taskScheduler->ReleaseFiberContext(fiberContext);
						fiberContext = nullptr;

						break;
					}

					// If subtasks still exist, drop current task execution. task will be resumed when last subtask finished
					if (fiberContext->childrenFibersCount.Get() > 1)
					{
						break;
					}

					// If task is in await state drop execution. task will be resumed when RestoreAwaitingTasks called
					if (fiberContext->taskStatus == FiberTaskStatus::AWAITING_GROUP)
					{
						break;
					}

					// No subtasks here and status is not finished, this mean all subtasks already finished before parent return from ExecuteTask
					// Continue current task execution
				}

				// Release children guard if need
				if (fiberContext)
				{
					int childrenFibersCount = fiberContext->childrenFibersCount.Dec();
					ASSERT( childrenFibersCount >= 0, "Sanity check failed");
					fiberContext = nullptr;
				}

			} else
			{
				// Queue is empty
				// TODO: can try to steal tasks from other threads
				context.hasNewTasksEvent.Wait(2000);
			}

		} // main thread loop

	}


	void TaskScheduler::RunTasksImpl(TaskGroup::Type taskGroup, fixed_array<TaskBucket>& buckets, FiberContext * parentFiber)
	{
		ASSERT(taskGroup < TaskGroup::COUNT, "Invalid group.");

		size_t count = 0;
		for (size_t i = 0; i < buckets.size(); ++i)
		{
			count += buckets[i].count;
		}

		if (parentFiber)
		{
			parentFiber->childrenFibersCount.Add((uint32)count);
		}

		for (size_t i = 0; i < buckets.size(); ++i)
		{
			int bucketIndex = roundRobinThreadIndex.Inc() % threadsCount;
			ThreadContext & context = threadContext[bucketIndex];

			TaskBucket& bucket = buckets[i];

			allGroupStats.allDoneEvent.Reset();
			allGroupStats.inProgressTaskCount.Add((uint32)bucket.count);

			groupStats[taskGroup].allDoneEvent.Reset();
			groupStats[taskGroup].inProgressTaskCount.Add((uint32)bucket.count);

			for (size_t taskIndex = 0; taskIndex < bucket.count; taskIndex++)
			{
				bucket.tasks[taskIndex].parentFiber = parentFiber;
			}

			context.queue.PushRange(bucket.tasks, bucket.count);

			context.hasNewTasksEvent.Signal();

		}
	}

	bool TaskScheduler::WaitGroup(TaskGroup::Type group, uint32 milliseconds)
	{
		VERIFY(IsWorkerThread() == false, "Can't use WaitGroup inside Task. Use FiberContext.WaitGroupAndYield() instead.", return false);

		return groupStats[group].allDoneEvent.Wait(milliseconds);
	}

	bool TaskScheduler::WaitAll(uint32 milliseconds)
	{
		VERIFY(IsWorkerThread() == false, "Can't use WaitAll inside Task.", return false);

		return allGroupStats.allDoneEvent.Wait(milliseconds);
	}

	bool TaskScheduler::IsEmpty()
	{
		for (uint32 i = 0; i < MT_MAX_THREAD_COUNT; i++)
		{
			if (!threadContext[i].queue.IsEmpty())
			{
				return false;
			}
		}
		return true;
	}

	uint32 TaskScheduler::GetWorkerCount() const
	{
		return threadsCount;
	}

	bool TaskScheduler::IsWorkerThread() const
	{
		for (uint32 i = 0; i < MT_MAX_THREAD_COUNT; i++)
		{
			if (threadContext[i].thread.IsCurrentThread())
			{
				return true;
			}
		}
		return false;
	}

}
