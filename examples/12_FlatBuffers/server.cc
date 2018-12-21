/* Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <chrono>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <thread>

#include "nvrpc/executor.h"
#include "nvrpc/life_cycle_unary_fb.h"
#include "nvrpc/server.h"
#include "nvrpc/service.h"
#include "tensorrt/playground/core/pool.h"
#include "tensorrt/playground/core/resources.h"
#include "tensorrt/playground/core/thread_pool.h"

#include "example.grpc.fb.h"
#include "example_generated.h"

using yais::AsyncRPC;
using yais::AsyncService;
using yais::BaseContext;
using yais::Executor;
using yais::Resources;
using yais::Server;
using yais::ThreadPool;

using yais::LifeCycleUnaryFB;

template<class Request, class Response, class Resources>
using Context = BaseContext<LifeCycleUnaryFB<Request, Response>, Resources>;

using Request = flatbuffers::grpc::Message<HelloRequest>;
using Response = flatbuffers::grpc::Message<HelloReply>;

// CLI Options
DEFINE_int32(thread_count, 1, "Size of thread pool");

// Define the resources your RPC will need to execute
// ==================================================
// In this case, all simple::Inference::Compute RPCs share a threadpool in which they will
// queue up some work on.  This essentially means, after the message as been received and
// processed, the actual work for the RPC is pushed to a worker pool outside the scope of
// the transaction processing system (TPS).  This is essentially async computing, we have
// decoupled the transaction from the workers executing the implementation.  The TPS can
// continue to queue work, while the workers process the load.
struct SimpleResources : public Resources
{
    SimpleResources(int numThreadsInPool = 3) : m_ThreadPool(numThreadsInPool) {}

    ThreadPool& GetThreadPool()
    {
        return m_ThreadPool;
    }

  private:
    ThreadPool m_ThreadPool;
};

// Contexts hold the state and provide the definition of the work to be performed by the RPC.
// This is where you define what gets executed for a given RPC.
// Incoming Message = simple::Input (RequestType)
// Outgoing Message = simple::Output (ResponseType)
class SimpleContext final : public Context<Request, Response, SimpleResources>
{
    void ExecuteRPC(Request& input, Response& output) final override
    {
        flatbuffers::grpc::MessageBuilder mb_;

        // We call GetRoot to "parse" the message. Verification is already
        // performed by default. See the notes below for more details.
        const HelloRequest* request = input.GetRoot();

        // Fields are retrieved as usual with FlatBuffers
        const std::string& name = request->name()->str();

        // `flatbuffers::grpc::MessageBuilder` is a `FlatBufferBuilder` with a
        // special allocator for efficient gRPC buffer transfer, but otherwise
        // usage is the same as usual.
        auto msg_offset = mb_.CreateString("Hello, " + name);
        auto hello_offset = CreateHelloReply(mb_, msg_offset);
        mb_.Finish(hello_offset);

        // The `ReleaseMessage<T>()` function detaches the message from the
        // builder, so we can transfer the resopnse to gRPC while simultaneously
        // detaching that memory buffer from the builer.
        output = mb_.ReleaseMessage<HelloReply>();
        CHECK(output.Verify());
        this->FinishResponse();
    }
};

int main(int argc, char* argv[])
{
    FLAGS_alsologtostderr = 1; // Log to console

    ::google::InitGoogleLogging("flatbuffer service");
    ::google::ParseCommandLineFlags(&argc, &argv, true);

    // A server will bind an IP:PORT to listen on
    Server server("0.0.0.0:50051");

    // A server can host multiple services
    auto simpleInference = server.RegisterAsyncService<Greeter>();

    auto rpcCompute =
        simpleInference->RegisterRPC<SimpleContext>(&Greeter::AsyncService::RequestSayHello);

    auto rpcResources = std::make_shared<SimpleResources>();
    auto executor = server.RegisterExecutor(new Executor(1));
    executor->RegisterContexts(rpcCompute, rpcResources, 10);

    LOG(INFO) << "Running Server";
    server.Run(std::chrono::milliseconds(2000), [] {
        // This is a timeout loop executed every 2seconds
        // Run() with no arguments will run an empty timeout loop every 5 seconds.
        // RunAsync() will return immediately, its your responsibility to ensure the
        // server doesn't go out of scope or a Shutdown will be triggered on your services.
    });
}