//
// ContextGroup.cpp
//
// LiquidPlayer project
// https://github.com/LiquidPlayer
//
// Created by Eric Lange
//
/*
 Copyright (c) 2016 - 2018 Eric Lange. All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 - Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 - Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
//#include "node/NodeInstance.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <android/log.h>
#include <exception>
#include <malloc.h>
#include <condition_variable>
#include "Common/ContextGroup.h"
#include "Common/JSValue.h"

static int pfd[2];
static pthread_t thr;
static const char *tag = "myapp";

static void *thread_func(void*)
{
    ssize_t rdsz;
    char buf[128];
    while((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0) {
        if(buf[rdsz - 1] == '\n') --rdsz;
        buf[rdsz] = 0;  /* add null-terminator */
        __android_log_write(ANDROID_LOG_DEBUG, tag, buf);
    }
    return 0;
}

int start_logger(const char *app_name)
{
    tag = app_name;

    /* make stdout line-buffered and stderr unbuffered */
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    /* create the pipe and redirect stdout and stderr */
    pipe(pfd);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);

    /* spawn the logging thread */
    if(pthread_create(&thr, 0, thread_func, 0) == -1)
        return -1;
    pthread_detach(thr);
    return 0;
}

class GenericAllocator : public ArrayBuffer::Allocator {
public:
    GenericAllocator() {}
    virtual ~GenericAllocator() {}
    virtual void* Allocate(size_t length) {
        unsigned char * mem =  (unsigned char *) malloc(length);
        memset(mem, 0, length);
        return (void*)mem;
    }
    virtual void* AllocateUninitialized(size_t length) {
        return malloc(length);
    }
    virtual void Free(void* data, size_t length) {
        free(data);
    }
};
static GenericAllocator s_allocator;

struct Runnable {
    jobject thiz;
    jobject runnable;
    JavaVM *jvm;
    std::function<void()> c_runnable;
};

class ContextGroupData {
public:
    ContextGroupData(std::shared_ptr<ContextGroup> cg) : m_context_group(cg) {}
    ~ContextGroupData() { m_context_group.reset(); }
    std::shared_ptr<ContextGroup> m_context_group;
};

void ContextGroup::StaticGCPrologueCallback(Isolate *isolate, GCType type, GCCallbackFlags flags)
{
    if (s_isolate_map.count(isolate)) {
        s_isolate_map[isolate]->GCPrologueCallback(type, flags);
    }
}

Platform *ContextGroup::s_platform = NULL;
int ContextGroup::s_init_count = 0;
std::mutex ContextGroup::s_mutex;
std::map<Isolate *, ContextGroup *> ContextGroup::s_isolate_map;

void ContextGroup::init_v8()
{
    s_mutex.lock();
    if (s_init_count++ == 0) {
        start_logger("LiquidCore");
        /* Add any required flags here.
        const char *flags = "--expose_gc";
        V8::SetFlagsFromString(flags, strlen(flags));
        */

        s_platform = platform::CreateDefaultPlatform(4);
        V8::InitializePlatform(s_platform);
        V8::Initialize();
    }

    s_mutex.unlock();
}

void ContextGroup::dispose_v8()
{
    s_mutex.lock();
    // FIXME: Once disposed, an attempt to re-init will crash
    // For now, init once and never dispose
    //--s_init_count;
    if (s_init_count == 0) {
        V8::Dispose();
        V8::ShutdownPlatform();
        delete s_platform;
        s_platform = nullptr;
    }
    s_mutex.unlock();
}

ContextGroup::ContextGroup()
{
    init_v8();
    m_create_params.array_buffer_allocator = &s_allocator;
    m_isolate = Isolate::New(m_create_params);
    m_manage_isolate = true;
    m_uv_loop = nullptr;
    m_thread_id = std::this_thread::get_id();
    m_async_handle = nullptr;
    m_isDefunct = false;

    s_isolate_map[m_isolate] = this;
    m_gc_callbacks.clear();
    m_isolate->AddGCPrologueCallback(StaticGCPrologueCallback);
}

ContextGroup::ContextGroup(Isolate *isolate, uv_loop_t *uv_loop)
{
    m_isolate = isolate;
    m_manage_isolate = false;
    m_uv_loop = uv_loop;
    m_thread_id = std::this_thread::get_id();
    m_async_handle = nullptr;
    m_isDefunct = false;

    s_isolate_map[m_isolate] = this;
    m_gc_callbacks.clear();
    m_isolate->AddGCPrologueCallback(StaticGCPrologueCallback);
}

void ContextGroup::MarkZombie(std::shared_ptr<JSValue> obj)
{
    if ((void*)&*obj != this) {
        m_zombie_mutex.lock();
        m_value_zombies.push_back(obj);
        m_zombie_mutex.unlock();
    }
}

void ContextGroup::MarkZombie(std::shared_ptr<JSContext> obj)
{
    if ((void*)&*obj != this) {
        m_zombie_mutex.lock();
        m_context_zombies.push_back(obj);
        m_zombie_mutex.unlock();
    }
}

void ContextGroup::callback(uv_async_t* handle)
{
    ContextGroupData *data = reinterpret_cast<ContextGroupData*>(handle->data);
    std::shared_ptr<ContextGroup> group = data->m_context_group;
    delete data;

    // Since we are in the correct thread now, free the zombies!
    group->m_value_zombies.clear();
    group->m_context_zombies.clear();

    group->m_async_mutex.lock();
    struct Runnable *r = group->m_runnables.empty() ? nullptr : group->m_runnables.front();

    while (r) {
        group->m_async_mutex.unlock();

        if (r->c_runnable) {
            r->c_runnable();
        } else {
            JNIEnv *env;
            int getEnvStat = r->jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
            if (getEnvStat == JNI_EDETACHED) {
                r->jvm->AttachCurrentThread(&env, NULL);
            }

            jclass cls = env->GetObjectClass(r->thiz);
            jmethodID mid;
            do {
                mid = env->GetMethodID(cls,"inContextCallback","(Ljava/lang/Runnable;)V");
                if (!env->ExceptionCheck()) break;
                env->ExceptionClear();
                jclass super = env->GetSuperclass(cls);
                env->DeleteLocalRef(cls);
                if (super == NULL || env->ExceptionCheck()) {
                    if (super != NULL) env->DeleteLocalRef(super);
                    if (getEnvStat == JNI_EDETACHED) {
                        r->jvm->DetachCurrentThread();
                    }
                    __android_log_assert("FAIL", "ContextGroup::callback",
                        "Can't find the class to call back?");
                    break;
                }
                cls = super;
            } while (true);
            env->DeleteLocalRef(cls);

            env->CallVoidMethod(r->thiz, mid, r->runnable);

            env->DeleteGlobalRef(r->thiz);
            env->DeleteGlobalRef(r->runnable);

            if (getEnvStat == JNI_EDETACHED) {
                r->jvm->DetachCurrentThread();
            }
        }

        group->m_async_mutex.lock();

        group->m_runnables.erase(group->m_runnables.begin());
        delete r;

        r = group->m_runnables.empty() ? nullptr : group->m_runnables.front();
    }
    // Close the handle.  We will create a new one if we
    // need another.  This keeps the node process from staying alive
    // indefinitely
    uv_close((uv_handle_t*)handle, [](uv_handle_t *h){
        delete (uv_async_t*)h;
    });
    group->m_async_handle = nullptr;
    group->m_async_mutex.unlock();
}

void ContextGroup::RegisterGCCallback(void (*cb)(GCType, GCCallbackFlags, void*), void *data)
{
    auto gc = std::unique_ptr<struct GCCallback>(new struct GCCallback);
    gc->cb = cb;
    gc->data = data;
    m_gc_callbacks.push_back(std::move(gc));
}

void ContextGroup::UnregisterGCCallback(void (*cb)(GCType, GCCallbackFlags, void*), void *data)
{
    auto it = m_gc_callbacks.begin();

    while (it != m_gc_callbacks.end()) {
        const auto& item = *it;
        ++it;
        if (item->cb == cb && item->data == data) {
            m_gc_callbacks.remove(item);
        }
    }
}

void ContextGroup::GCPrologueCallback(GCType type, GCCallbackFlags flags)
{
    auto it = m_gc_callbacks.begin();

    while (it != m_gc_callbacks.end()) {
        const auto& item = *it;
        ++it;
        item->cb(type, flags, item->data);
    }
}

void ContextGroup::Manage(std::shared_ptr<JSValue> obj)
{
    m_managedValues.push_back(std::move(obj));
}

void ContextGroup::Manage(std::shared_ptr<JSContext> obj)
{
    m_managedContexts.push_back(std::move(obj));
}

void ContextGroup::Dispose()
{
    if (!m_isDefunct) {
        m_isolate->RemoveGCPrologueCallback(StaticGCPrologueCallback);

        m_scheduling_mutex.lock();

        for (auto it = m_managedValues.begin(); it != m_managedValues.end(); ++it) {
            std::shared_ptr<JSValue> valid = (*it).lock();
            if (valid) {
                valid->Dispose();
            }
        }
        for (auto it = m_managedContexts.begin(); it != m_managedContexts.end(); ++it) {
            std::shared_ptr<JSContext> valid = (*it).lock();
            if (valid) {
                valid->Dispose();
            }
        }
        m_isDefunct = true;
        m_managedValues.clear();
        m_managedContexts.clear();
        m_scheduling_mutex.unlock();
        m_value_zombies.clear();
        m_context_zombies.clear();

        s_isolate_map.erase(m_isolate);
        if (m_manage_isolate) {
            m_isolate->Dispose();
        } else {
            dispose_v8();
        }
    }
}

ContextGroup::~ContextGroup()
{
    Dispose();
}

void ContextGroup::sync(std::function<void()> runnable)
{
    m_scheduling_mutex.lock();
    if (Loop() && std::this_thread::get_id() != Thread()) {
        m_scheduling_mutex.unlock();

        std::condition_variable cv;
        bool signaled = false;

        struct Runnable *r = new struct Runnable;
        r->thiz = nullptr;
        r->runnable = nullptr;
        r->jvm = nullptr;
        r->c_runnable = [&]() {
            runnable();
            {
                std::lock_guard<std::mutex> lk(m_async_mutex);
                signaled = true;
            }
            cv.notify_one();
        };

        std::unique_lock<std::mutex> lk(m_async_mutex);
        m_runnables.push_back(r);

        if (!m_async_handle) {
            m_async_handle = new uv_async_t();
            m_async_handle->data = new ContextGroupData(shared_from_this());
            uv_async_init(Loop(), m_async_handle, ContextGroup::callback);
            uv_async_send(m_async_handle);
        }

        cv.wait(lk, [&]{return signaled;});
        lk.unlock();
    } else {
        m_scheduling_mutex.unlock();
        runnable();
    }
}

void ContextGroup::schedule_java_runnable(JNIEnv *env, jobject thiz, jobject runnable)
{
    m_async_mutex.lock();

    struct Runnable *r = new struct Runnable;
    r->thiz = env->NewGlobalRef(thiz);
    r->runnable = env->NewGlobalRef(runnable);
    r->c_runnable = nullptr;
    env->GetJavaVM(&r->jvm);
    m_runnables.push_back(r);

    if (!m_async_handle) {
        m_async_handle = new uv_async_t();
        m_async_handle->data = new ContextGroupData(shared_from_this());
        uv_async_init(Loop(), m_async_handle, ContextGroup::callback);
        uv_async_send(m_async_handle);
    }
    m_async_mutex.unlock();
}
