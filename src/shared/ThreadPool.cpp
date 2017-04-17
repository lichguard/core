/*
 * Copyright (C) 2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "ThreadPool.h"
#include "Log.h"
#include <mysql.h>

ThreadPool::ThreadPool(int numThreads, ClearMode when, ErrorHandling mode) :
    m_errorHandling(mode), m_size(numThreads), m_clearMode(when), m_active(0)
{
    m_workers.reserve(m_size);
}

ThreadPool::~ThreadPool()
{
    std::unique_lock<std::shared_timed_mutex> lock(m_mutex);
    m_status = Status::TERMINATING;
    m_waitForWork.notify_all();
}

template void ThreadPool::start<ThreadPool::SingleQueue>();
template void ThreadPool::start<ThreadPool::MultiQueue>();
template void ThreadPool::start<ThreadPool::MySQL>();

std::future<void> ThreadPool::processWorkload()
{
    if (m_clearMode == ClearMode::AT_NEXT_WORKLOAD &&
            m_status == Status::READY && m_dirty)
        clearWorkload();
    if (m_status != Status::READY || m_workload.empty())
        return std::future<void>();
    m_result = std::promise<void>();
    m_dirty = true;
    m_active = m_size;
    m_index = 0;
    m_status = Status::PROCESSING;
    std::unique_lock<std::shared_timed_mutex> lock(m_mutex);
    for (int i = 0; i < m_size; i++)
        m_workers[i]->prepare();
    m_waitForWork.notify_all();
    return m_result.get_future();
}

std::future<void> ThreadPool::processWorkload(workload_t &workload)
{
    if (m_status != Status::READY)
        return std::future<void>();
    m_workload = workload;
    m_dirty = false;
    return processWorkload();
}

std::future<void> ThreadPool::processWorkload(workload_t &&workload)
{
    if (m_status != Status::READY)
        return std::future<void>();
    m_workload = std::move(workload);
    m_dirty = false;
    return processWorkload();
}

ThreadPool::Status ThreadPool::status()
{
    return m_status;
}

int ThreadPool::size()
{
    return m_size;
}

std::vector<std::exception_ptr> ThreadPool::taskErrors()
{
    return m_errors;
}

void ThreadPool::worker::waitForWork()
{
    std::shared_lock<std::shared_timed_mutex> lock(pool->m_mutex); //locked!
    while(!busy && pool->status() != Status::TERMINATING) //wait for work
        pool->m_waitForWork.wait(lock);
}

ThreadPool &ThreadPool::operator<<(std::function<void()> packaged_task)
{
    if (m_status == Status::PROCESSING || m_status == Status::ERROR)
        throw "Attempt to append a task to a load being processed!";
    if (m_clearMode == ClearMode::AT_NEXT_WORKLOAD && m_dirty)
        clearWorkload();
    m_workload.emplace_back(packaged_task);
    return *this;
}

void ThreadPool::clearWorkload()
{
    m_dirty = false;
    m_workload.clear();
}

ThreadPool::worker::worker(ThreadPool *pool, int id, ThreadPool::ErrorHandling mode) :
    id(id), errorHandling(mode), pool(pool), thread([this](){this->loop_wrapper();})
{
}

ThreadPool::worker::~worker()
{
    thread.join();
}

void ThreadPool::worker::loop_wrapper()
{
    if (pool->m_errorHandling == ErrorHandling::NONE)
        loop();
    else
    {
        std::exception_ptr err_p;
        try
        {
            loop();
        }
        catch (...)
        {

            err_p = std::current_exception();
        }

        if (pool->m_errorHandling == ErrorHandling::IGNORE)
        {
            if (pool->status() == Status::TERMINATING)
                return;
            loop_wrapper();
            return;
        }
        try{
            if (err_p)
            {
                pool->m_errors.push_back(err_p);
                std::rethrow_exception(err_p);
            }
        }
        catch (const std::exception &e)
        {
            sLog.outError("A ThreadPool task generated an exception: %s",e.what());
        }
        catch (const std::string &e)
        {
            sLog.outError("A ThreadPool task generated an exception: %s",e);
        }
        catch (...)
        {
            sLog.outError("A ThreadPool task generated an exception");
        }
        if (pool->m_errorHandling == ErrorHandling::TERMINATE)
            pool->m_status = Status::TERMINATING;
        if (pool->status() == Status::TERMINATING)
            return;
        loop_wrapper();
    }
}

void ThreadPool::worker::prepare()
{
    busy = true;
}

void ThreadPool::worker::loop()
{
    while(true)
    {
        waitForWork();
        if (pool->m_status == Status::TERMINATING)
            return;
        doWork();
        busy = false;
        int remaning = --(pool->m_active);
        if (!remaning)
        {
            if (pool->m_clearMode == ClearMode::UPPON_COMPLETION)
                pool->clearWorkload();
            if (pool->m_status == Status::ERROR)
            {
                pool->m_status = Status::READY;
                pool->m_result.set_exception(pool->m_errors.front());
            }
            else
            {
                pool->m_status = Status::READY;
                pool->m_result.set_value();
            }
        }
    }
}

ThreadPool::worker_mq::worker_mq(ThreadPool *pool, int id, ThreadPool::ErrorHandling mode) :
    worker(pool,id,mode)
{
}

void ThreadPool::worker_mq::doWork()
{
    while (it < pool->m_workload.end() && pool->m_status == Status::PROCESSING)
    {
        workload_t::iterator w = it;
        it += pool->m_size; //if it fails, we might want to skip this task.
        (*w)();
    }
}

void ThreadPool::worker_mq::prepare()
{
    it = pool->m_workload.begin() + id;
    worker::prepare();
}

ThreadPool::worker_sq::worker_sq(ThreadPool *pool, int id, ThreadPool::ErrorHandling mode) :
    worker(pool,id,mode)
{
}

void ThreadPool::worker_sq::doWork()
{
    int i = pool->m_index++;
    while (i < pool->m_workload.size() && pool->m_status == Status::PROCESSING)
    {
        pool->m_workload[i]();
        i = pool->m_index++;
    }
}

ThreadPool::worker_mysql::worker_mysql(ThreadPool *tp, int id, ThreadPool::ErrorHandling e):
    worker_sq(tp, id, e)
{
}

void ThreadPool::worker_mysql::doWork()
{
    mysql_thread_init();
    worker_sq::doWork();
    mysql_thread_end();
}
