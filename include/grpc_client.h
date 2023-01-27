// MIT License
// Copyright(c) 2022 Futurewei Cloud
//
//     Permission is hereby granted,
//     free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction,
//     including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons
//     to whom the Software is furnished to do so, subject to the following conditions:
//
//     The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
//     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//     FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
//     WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <iostream>
#include <mutex>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include "arionmaster.grpc.pb.h"
#include "segment_lock.h"
#include <sqlite_orm.h>
#include <concurrency/ConcurrentHashMap.h>
#include "bpf.h"
#include "libbpf.h"

using namespace arion::schema;
using grpc::Status;

class ArionMasterWatcherImpl final : public Watch::Service {
public:
    std::shared_ptr<grpc_impl::Channel> chan_;

    std::unique_ptr<Watch::Stub> stub_;

    explicit ArionMasterWatcherImpl() {}

    void RequestArionMaster(std::vector<ArionWingRequest *> *request_vector, grpc::CompletionQueue *cq);

    void ConnectToArionMaster();

    void RunClient(std::string ip, std::string port, std::string group, std::string neighbor_table, std::string security_group_rules_table);

    bool a = chan_ == nullptr;

    int fd_neighbor_ebpf_map = -1;

    int fd_security_group_ebpf_map = -1;

private:
    std::string server_address;

    std::string server_port;

    std::string group_id;

    std::string table_name_neighbor_ebpf_map;

    std::string table_name_sg_ebpf_map;

    // key std::string is '<vni>-<vpc_ip>', value is inserted version of this neighbor
    folly::ConcurrentHashMap<std::string, int> neighbor_task_map;

    // key std::string is 'securitygroupid', value is inserted version of this security group rule
    folly::ConcurrentHashMap<std::string, int> security_group_rule_task_map;
    // segment lock for neighbor key version control
    SegmentLock segment_lock;
};

struct AsyncClientCall {
    arion::schema::ArionWingResponse reply;
    grpc::ClientContext context;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncReaderWriter<ArionWingRequest, ArionWingResponse> > stream;
};
