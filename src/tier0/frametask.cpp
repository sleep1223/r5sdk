//=============================================================================//
//
// Purpose: 
// 
//-----------------------------------------------------------------------------
//
//=============================================================================//
#include "tier0/frametask.h"

//-----------------------------------------------------------------------------
// Purpose: run frame task and process queued calls
//-----------------------------------------------------------------------------
void CFrameTask::RunFrame()
{
    std::vector<std::function<void()>> readyTasks;

    {
        AUTO_LOCK(m_Mutex);

        for (auto it = m_QueuedTasks.begin(); it != m_QueuedTasks.end();)
        {
            if (it->m_nDelayedFrames == 0)
            {
                readyTasks.emplace_back(std::move(it->m_rFunctor));
                it = m_QueuedTasks.erase(it);
            }
            else
            {
                --it->m_nDelayedFrames;
                ++it;
            }
        }
    }

    for (const std::function<void()>& task : readyTasks)
        task();
}

//-----------------------------------------------------------------------------
// Purpose: is the task finished
// Output : true if finished, false otherwise
//-----------------------------------------------------------------------------
bool CFrameTask::IsFinished() const
{
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: adds function to list, to be called after 'i' frames.
// Input  : functor - 
//          frames - 
//-----------------------------------------------------------------------------
void CFrameTask::Dispatch(std::function<void()> functor, unsigned int frames)
{
    AUTO_LOCK(m_Mutex);
    m_QueuedTasks.emplace_back(frames, functor);
}

//-----------------------------------------------------------------------------
std::list<IFrameTask*> g_TaskQueueList;
CFrameTask g_TaskQueue;
