// Copyright 2019 DeepMind Technologies Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "reverb/cc/reverb_service_impl.h"

#include <algorithm>
#include <limits>
#include <list>
#include <memory>
#include <queue>
#include <vector>

#include "absl/base/thread_annotations.h"
#include "absl/flags/flag.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "reverb/cc/checkpointing/interface.h"
#include "reverb/cc/chunk_store.h"
#include "reverb/cc/platform/hash_map.h"
#include "reverb/cc/platform/hash_set.h"
#include "reverb/cc/platform/logging.h"
#include "reverb/cc/platform/status_macros.h"
#include "reverb/cc/platform/thread.h"
#include "reverb/cc/reverb_server_reactor.h"
#include "reverb/cc/reverb_service.grpc.pb.h"
#include "reverb/cc/reverb_service.pb.h"
#include "reverb/cc/sampler.h"
#include "reverb/cc/support/cleanup.h"
#include "reverb/cc/support/grpc_util.h"
#include "reverb/cc/support/trajectory_util.h"
#include "reverb/cc/support/uint128.h"
#include "reverb/cc/support/unbounded_queue.h"

ABSL_FLAG(size_t, reverb_callback_executor_num_threads, 32,
          "Number of threads in the callback executor thread pool.");

namespace deepmind {
namespace reverb {
namespace {

// Multiple `ChunkData` can be sent with the same `SampleStreamResponseCtx`. If
// the size of the message exceeds this value then the request is sent and the
// remaining chunks are sent with other messages.
static constexpr int64_t kMaxSampleResponseSizeBytes = 1 * 1024 * 1024;  // 1MB.

// How often to check whether callback execution finished before deleting
// reactor.
constexpr absl::Duration kCallbackWaitTime = absl::Milliseconds(1);

inline grpc::Status TableNotFound(absl::string_view name) {
  return grpc::Status(grpc::StatusCode::NOT_FOUND,
                      absl::StrCat("Priority table ", name, " was not found"));
}

inline grpc::Status Internal(const std::string& message) {
  return grpc::Status(grpc::StatusCode::INTERNAL, message);
}

}  // namespace

ReverbServiceImpl::ReverbServiceImpl(std::shared_ptr<Checkpointer> checkpointer)
    : checkpointer_(std::move(checkpointer)) {}

absl::Status ReverbServiceImpl::Create(
    std::vector<std::shared_ptr<Table>> tables,
    std::shared_ptr<Checkpointer> checkpointer,
    std::unique_ptr<ReverbServiceImpl>* service) {
  // Can't use make_unique because it can't see the Impl's private constructor.
  auto new_service = std::unique_ptr<ReverbServiceImpl>(
      new ReverbServiceImpl(std::move(checkpointer)));
  REVERB_RETURN_IF_ERROR(new_service->Initialize(std::move(tables)));
  std::swap(new_service, *service);
  return absl::OkStatus();
}

absl::Status ReverbServiceImpl::Create(
    std::vector<std::shared_ptr<Table>> tables,
    std::unique_ptr<ReverbServiceImpl>* service) {
  return Create(std::move(tables), /*checkpointer=*/nullptr, service);
}

absl::Status ReverbServiceImpl::Initialize(
    std::vector<std::shared_ptr<Table>> tables) {
  if (checkpointer_ != nullptr) {
    // We start by attempting to load the latest checkpoint from the root
    // directory.
    // In general we expect this to be nonempty (and thus succeed)
    // if this is a restart of a previously running job (e.g preemption).
    auto status = checkpointer_->LoadLatest(&chunk_store_, &tables);
    if (absl::IsNotFound(status)) {
      // No checkpoint was found in the root directory. If a fallback
      // checkpoint (path) has been configured then we attempt to load that
      // checkpoint instead.
      // Note that by first attempting to load from the root directory and
      // then only loading the fallback checkpoint iff the root directory is
      // empty we are effectively using the fallback checkpoint as a way to
      // initialise the service with a checkpoint generated by another
      // experiment.
      status = checkpointer_->LoadFallbackCheckpoint(&chunk_store_, &tables);
    }
    // If no checkpoint was found in neither the root directory nor a fallback
    // checkpoint was provided then proceed to initialise an empty service.
    // All other error types are unexpected and bubbled up to the caller.
    if (!status.ok() && !absl::IsNotFound(status)) {
      return status;
    }
  }

  for (auto& table : tables) {
    std::string name = table->name();
    tables_[name] = std::move(table);
  }

  auto executor = std::make_shared<TaskExecutor>(
      absl::GetFlag(FLAGS_reverb_callback_executor_num_threads),
      "TableCallbackExecutor");
  for (auto& table : tables_) {
    table.second->SetCallbackExecutor(executor);
  }

  tables_state_id_ = absl::MakeUint128(absl::Uniform<uint64_t>(rnd_),
                                       absl::Uniform<uint64_t>(rnd_));

  return absl::OkStatus();
}

grpc::ServerUnaryReactor* ReverbServiceImpl::Checkpoint(
    grpc::CallbackServerContext* context, const CheckpointRequest* request,
    CheckpointResponse* response) {
  grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
  if (checkpointer_ == nullptr) {
    reactor->Finish(
        grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                     "no Checkpointer configured for the replay service."));
    return reactor;
  }

  std::vector<Table*> tables;
  for (auto& table : tables_) {
    tables.push_back(table.second.get());
  }

  auto status = checkpointer_->Save(std::move(tables), 1,
                                    response->mutable_checkpoint_path());
  reactor->Finish(ToGrpcStatus(status));
  REVERB_LOG_IF(REVERB_INFO, status.ok())
      << "Stored checkpoint to " << response->checkpoint_path();
  return reactor;
}

grpc::ServerBidiReactor<InsertStreamRequest, InsertStreamResponse>*
ReverbServiceImpl::InsertStream(grpc::CallbackServerContext* context) {
  struct InsertStreamResponseCtx {
    InsertStreamResponse payload;
  };

  class WorkerlessInsertReactor
      : public ReverbServerReactor<InsertStreamRequest, InsertStreamResponse,
                                   InsertStreamResponseCtx> {
   public:
    WorkerlessInsertReactor(ReverbServiceImpl* server)
        : ReverbServerReactor(),
          server_(server),
          insert_completed_(
              std::make_shared<Table::InsertCallback>([&](uint64_t key) {
                absl::MutexLock lock(&mu_);
                if (!read_in_flight_) {
                  read_in_flight_ = true;
                  StartRead(&request_);
                }
                if (!is_finished_) {
                  // The first element is the one in flight, modify not yet
                  // in flight response if possible.
                  if (responses_to_send_.size() < 2) {
                    responses_to_send_.emplace();
                  }
                  responses_to_send_.back().payload.add_keys(key);
                  if (responses_to_send_.size() == 1) {
                    MaybeSendNextResponse();
                  }
                }
              })),
          read_in_flight_(true) {
      MaybeStartRead();
    }

    ~WorkerlessInsertReactor() {
      // As callback references Reactor's memory make sure it can't be executed
      // anymore.
      std::weak_ptr<Table::InsertCallback> weak_ptr = insert_completed_;
      insert_completed_.reset();
      while (weak_ptr.lock()) {
        absl::SleepFor(kCallbackWaitTime);
      }
    }

    grpc::Status ProcessIncomingRequest(InsertStreamRequest* request) override
        ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
      read_in_flight_ = false;
      if (request->chunks_size() == 0 && request->items_size() == 0) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            absl::StrCat("ProcessIncomingRequest: Request lacks both chunks "
                         "and item.  Request: ",
                         request->ShortDebugString()));
      }
      if (auto status = SaveChunks(request); !status.ok()) {
        return status;
      }
      if (request->items_size() == 0) {
        // No item to add to the table - continue reading next requests.
        read_in_flight_ = true;
        StartRead(&request_);
        return grpc::Status::OK;
      }
      bool can_insert = true;
      for (auto& request_item : *request->mutable_items()) {
        Table::Item item;
        if (auto status = GetItemWithChunks(&item, &request_item);
            !status.ok()) {
          return status;
        }
        const auto& table_name = item.item.table();
        // Check that table name is valid.
        auto table = server_->TableByName(table_name);
        if (table == nullptr) {
          return TableNotFound(table_name);
        }
        if (auto status = table->InsertOrAssignAsync(
                std::move(item), &can_insert, insert_completed_);
            !status.ok()) {
          return ToGrpcStatus(status);
        }
      }
      if (auto status = ReleaseOutOfRangeChunks(request->keep_chunk_keys());
          !status.ok()) {
        return status;
      }
      if (can_insert) {
        // Insert didn't exceed table's buffer, we can continue reading next
        // requests.
        read_in_flight_ = true;
        StartRead(&request_);
      }
      return grpc::Status::OK;
    }

   private:
    grpc::Status SaveChunks(InsertStreamRequest* request) {
      for (auto& chunk : *request->mutable_chunks()) {
        ChunkStore::Key key = chunk.chunk_key();
        if (!chunks_.contains(key)) {
          chunks_[key] = std::make_shared<ChunkStore::Chunk>(std::move(chunk));
        }
      }

      return grpc::Status::OK;
    }

    grpc::Status GetItemWithChunks(
        Table::Item* item,
        PrioritizedItem* request_item) {
      for (ChunkStore::Key key :
           internal::GetChunkKeys(request_item->flat_trajectory())) {
        auto it = chunks_.find(key);
        if (it == chunks_.end()) {
          return Internal(
              absl::StrCat("Could not find sequence chunk ", key, "."));
        }
        item->chunks.push_back(it->second);
      }

      item->item = std::move(*request_item);

      return grpc::Status::OK;
    }

    grpc::Status ReleaseOutOfRangeChunks(absl::Span<const uint64_t> keep_keys) {
      for (auto it = chunks_.cbegin(); it != chunks_.cend();) {
        if (std::find(keep_keys.begin(), keep_keys.end(), it->first) ==
            keep_keys.end()) {
          chunks_.erase(it++);
        } else {
          ++it;
        }
      }
      if (chunks_.size() != keep_keys.size()) {
        return grpc::Status(
            grpc::StatusCode::FAILED_PRECONDITION,
            absl::StrCat("ReleaseOutOfRangeChunks: Kept less chunks than "
                         "expected.  chunks_.size() == ",
                         chunks_.size(),
                         " != keep_keys.size() == ", keep_keys.size()));
      }
      return grpc::Status::OK;
    }

    // Incoming messages are handled one at a time. That is StartRead is not
    // called until `request_` has been completely salvaged. Fields accessed
    // only by OnRead are thus thread safe and require no additional mutex to
    // control access.
    //
    // The following fields are ONLY accessed by OnRead (and subcalls):
    //  - chunks_

    // Chunks that may be referenced by items not yet received. The ChunkStore
    // itself only maintains weak pointers to the chunk so until an item that
    // references the chunk is created, this pointer is the only reference that
    // stops the chunk from being deallocated.
    internal::flat_hash_map<ChunkStore::Key, std::shared_ptr<ChunkStore::Chunk>>
        chunks_;

    // Used to lookup tables when inserting items.
    const ReverbServiceImpl* server_;

    // Callback called by the table when insert operation is completed.
    std::shared_ptr<Table::InsertCallback> insert_completed_;
    // Is there a GRPC read in flight.
    bool read_in_flight_ ABSL_GUARDED_BY(mu_);
  };

  return new WorkerlessInsertReactor(this);
}

grpc::ServerBidiReactor<InitializeConnectionRequest,
                        InitializeConnectionResponse>*
ReverbServiceImpl::InitializeConnection(grpc::CallbackServerContext* context) {
  class Reactor : public grpc::ServerBidiReactor<InitializeConnectionRequest,
                                                 InitializeConnectionResponse> {
   public:
    Reactor(grpc::CallbackServerContext* context, ReverbServiceImpl* server)
        : server_(server) {
      if (!IsLocalhostOrInProcess(context->peer())) {
        Finish(grpc::Status::OK);
        return;
      }

      StartRead(&request_);
    }

    void OnReadDone(bool ok) override {
      if (!ok) {
        Finish(Internal("Failed to read from stream"));
        return;
      }

      if (request_.pid() != getpid()) {
        // A response without an address signal that the client and server are
        // not part of the same process.
        response_.set_address(0);
        StartWrite(&response_);
        return;
      }

      if (table_ptr_ == nullptr) {
        auto table = server_->TableByName(request_.table_name());
        if (table == nullptr) {
          Finish(TableNotFound(request_.table_name()));
          return;
        }

        // Allocate a new shared pointer on the heap and transmit its memory
        // address.
        // The client will dereference and assume ownership of the object before
        // sending its response. For simplicity, the client will copy the
        // shared_ptr so the server is always responsible for cleaning up the
        // heap allocated object.
        table_ptr_ = new std::shared_ptr<Table>(table);

        response_.set_address(reinterpret_cast<int64_t>(table_ptr_));
        StartWrite(&response_);
        return;
      }

      if (!request_.ownership_transferred()) {
        Finish(Internal("Received unexpected request"));
      }

      Finish(grpc::Status::OK);
    }

    void OnWriteDone(bool ok) override {
      if (!ok) {
        Finish(Internal("Failed to write to stream"));
        return;
      }

      // If the address was not set then the client was not running in the same
      // process. No further actions are required so we close down the stream.
      if (response_.address() == 0) {
        Finish(grpc::Status::OK);
        return;
      }

      // Wait for the response from the client confirming that the shared_ptr
      // was copied.
      request_.Clear();
      StartRead(&request_);
    }

    void OnDone() override {
      if (table_ptr_ != nullptr) {
        delete table_ptr_;
      }
      delete this;
    }

   private:
    ReverbServiceImpl* server_;
    InitializeConnectionRequest request_;
    InitializeConnectionResponse response_;
    std::shared_ptr<Table>* table_ptr_ = nullptr;
  };

  return new Reactor(context, this);
}

grpc::ServerUnaryReactor* ReverbServiceImpl::MutatePriorities(
    grpc::CallbackServerContext* context,
    const MutatePrioritiesRequest* request,
    MutatePrioritiesResponse* response) {
  grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
  std::shared_ptr<Table> table = TableByName(request->table());
  if (table == nullptr) {
    reactor->Finish(TableNotFound(request->table()));
    return reactor;
  }

  auto status = table->MutateItems(
      std::vector<KeyWithPriority>(request->updates().begin(),
                                   request->updates().end()),
      request->delete_keys());
  reactor->Finish(ToGrpcStatus(status));
  return reactor;
}

grpc::ServerUnaryReactor* ReverbServiceImpl::Reset(
    grpc::CallbackServerContext* context, const ResetRequest* request,
    ResetResponse* response) {
  grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
  std::shared_ptr<Table> table = TableByName(request->table());
  if (table == nullptr) {
    reactor->Finish(TableNotFound(request->table()));
    return reactor;
  }
  auto status = table->Reset();
  reactor->Finish(ToGrpcStatus(status));
  return reactor;
}

grpc::ServerBidiReactor<SampleStreamRequest, SampleStreamResponse>*
ReverbServiceImpl::SampleStream(grpc::CallbackServerContext* context) {
  struct SampleStreamResponseCtx {
    SampleStreamResponseCtx() {}
    SampleStreamResponseCtx(const SampleStreamResponseCtx&) = delete;
    SampleStreamResponseCtx& operator=(const SampleStreamResponseCtx&) = delete;
    SampleStreamResponseCtx(SampleStreamResponseCtx&& response) = default;
    SampleStreamResponseCtx& operator=(SampleStreamResponseCtx&& response) =
        default;

    ~SampleStreamResponseCtx() {
      // SampleStreamResponseCtx does not own immutable parts of the payload.
      // We need to make sure not to destroy them while destructing the payload.
      for (auto& entry : *payload.mutable_entries()) {
        if (entry.info().has_item()) {
          auto* item = entry.mutable_info()->mutable_item();
          item->/*unsafe_arena_*/release_inserted_at();
          item->/*unsafe_arena_*/release_flat_trajectory();
        }
        while (entry.data_size() != 0) {
          entry.mutable_data()->UnsafeArenaReleaseLast();
        }
      }
    }

    void AddTableItem(std::shared_ptr<TableItem> item) {
      table_items.push_back(std::move(item));
    }

    SampleStreamResponse payload;
    std::vector<std::shared_ptr<TableItem>> table_items;
  };

  // Maximal number of queued SampleStreamResponse-messages waiting to be send
  // to the client. When this limit is reached enqueuing of sampling requests on
  // the target table is paused. The limit is in place to cap reactor's memory
  // usage.
  static constexpr int kMaxQueuedResponses = 3;

  class WorkerlessSampleReactor
      : public ReverbServerReactor<SampleStreamRequest, SampleStreamResponse,
                                   SampleStreamResponseCtx> {
   public:
    using SamplingCallback = std::function<void(Table::SampleRequest*)>;

    WorkerlessSampleReactor(ReverbServiceImpl* server)
        : ReverbServerReactor(),
          server_(server),
          sampling_done_(std::make_shared<SamplingCallback>(
              [&](Table::SampleRequest* sample) {
                {
                  absl::MutexLock lock(&mu_);
                  waiting_for_enqueued_sample_ = false;
                  if (!sample->status.ok()) {
                    if (!is_finished_) {
                      SetReactorAsFinished(ToGrpcStatus(sample->status));
                    }
                    return;
                  }
                  task_info_.fetched_samples += sample->samples.size();
                  bool already_writing = !responses_to_send_.empty();
                  for (Table::SampledItem& sample : sample->samples) {
                    ProcessSample(&sample, already_writing);
                  }
                  if (!already_writing) {
                    MaybeSendNextResponse();
                  }
                  const int next_batch_size = task_info_.NextSampleSize();
                  if (next_batch_size != 0) {
                    MaybeStartSampling();
                    return;
                  }
                }
                // Current request is finalized, ask for another one.
                MaybeStartRead();
              })),
          waiting_for_enqueued_sample_(false) {
      MaybeStartRead();
    }

    ~WorkerlessSampleReactor() {
      // As callback references Reactor's memory make sure it can't be executed
      // anymore.
      std::weak_ptr<SamplingCallback> weak_ptr = sampling_done_;
      sampling_done_.reset();
      while (weak_ptr.lock()) {
        absl::SleepFor(kCallbackWaitTime);
      }
    }

    void OnWriteDone(bool ok) override {
      ReverbServerReactor::OnWriteDone(ok);
      absl::MutexLock lock(&mu_);
      MaybeStartSampling();
    }

    grpc::Status ProcessIncomingRequest(SampleStreamRequest* request) override
        ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
      if (request->num_samples() <= 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            absl::StrCat("`num_samples` must be > 0 (got",
                                         request->num_samples(), ")."));
      }
      if (request->flexible_batch_size() <= 0 &&
          request->flexible_batch_size() != Sampler::kAutoSelectValue) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            absl::StrCat("`flexible_batch_size` must be > 0 or ",
                         Sampler::kAutoSelectValue, " (for auto tuning). Got",
                         request->flexible_batch_size(), "."));
      }
      if (request->has_rate_limiter_timeout() &&
          request->rate_limiter_timeout().milliseconds() > 0) {
        task_info_.timeout =
            absl::Milliseconds(request->rate_limiter_timeout().milliseconds());
      } else {
        task_info_.timeout = absl::InfiniteDuration();
      }

      task_info_.table = server_->TableByName(request->table());
      if (task_info_.table == nullptr) {
        return TableNotFound(request->table());
      }
      task_info_.flexible_batch_size =
          request->flexible_batch_size() == Sampler::kAutoSelectValue
              ? task_info_.table->DefaultFlexibleBatchSize()
              : request->flexible_batch_size();
      task_info_.fetched_samples = 0;
      task_info_.requested_samples = request->num_samples();
      MaybeStartSampling();
      return grpc::Status::OK;
    }

   private:
    void MaybeStartSampling() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
      const int next_batch_size = task_info_.NextSampleSize();
      if (next_batch_size == 0) {
        // Current request has been fully processed.
        return;
      }
      if (waiting_for_enqueued_sample_) {
        // There is already an inflight sample request.
        return;
      }
      if (responses_to_send_.size() >= kMaxQueuedResponses) {
        // There are too many pending responses to send to the client.
        return;
      }
      waiting_for_enqueued_sample_ = true;
      task_info_.table->EnqueSampleRequest(next_batch_size, sampling_done_,
                                           task_info_.timeout);
    }

    void ProcessSample(Table::SampledItem* sample, bool write_in_flight)
        ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
      if (responses_to_send_.empty() ||
          (responses_to_send_.size() == 1 && write_in_flight) ||
          current_response_size_bytes_ > kMaxSampleResponseSizeBytes) {
        // We need a new response as there is no previous one / is already in
        // flight or too big.
        responses_to_send_.emplace();
        current_response_size_bytes_ = 0;
      }
      SampleStreamResponseCtx* response = &responses_to_send_.back();
      auto* entry = response->payload.add_entries();
      for (int i = 0; i < sample->ref->chunks.size(); i++) {
        entry->set_end_of_sequence(i + 1 == sample->ref->chunks.size());
        // Attach the info to the first message.
        if (i == 0) {
          auto* item = entry->mutable_info()->mutable_item();
          auto& sample_item = sample->ref->item;
          item->set_key(sample_item.key());
          item->set_table(sample_item.table());
          item->set_priority(sample->priority);
          item->set_times_sampled(sample->times_sampled);
          // ~SampleStreamResponseCtx releases these fields from the proto
          // upon destruction of the item.
          item->/*unsafe_arena_*/set_allocated_inserted_at(
              sample_item.mutable_inserted_at());
          item->/*unsafe_arena_*/set_allocated_flat_trajectory(
              sample_item.mutable_flat_trajectory());
          entry->mutable_info()->set_probability(sample->probability);
          entry->mutable_info()->set_table_size(sample->table_size);
          entry->mutable_info()->set_rate_limited(sample->rate_limited);
        }
        ChunkData* chunk =
            const_cast<ChunkData*>(&sample->ref->chunks[i]->data());
        current_response_size_bytes_ += chunk->ByteSizeLong();
        entry->mutable_data()->UnsafeArenaAddAllocated(chunk);
        if (i < sample->ref->chunks.size() - 1 &&
            current_response_size_bytes_ > kMaxSampleResponseSizeBytes) {
          // Current response is too big, start a new one.
          responses_to_send_.emplace();
          current_response_size_bytes_ = 0;
          response = &responses_to_send_.back();
          entry = response->payload.add_entries();
        }
      }
      // Reference sample only in the last response containing it, so it is
      // released when fully sent to the client.
      response->AddTableItem(sample->ref);
    }

    // Used to lookup tables when inserting items.
    const ReverbServiceImpl* server_;

    // Context of the current sample request.
    SampleTaskInfo task_info_ ABSL_GUARDED_BY(mu_);

    // Callback called by the table worker when current sampling batch is done.
    std::shared_ptr<SamplingCallback> sampling_done_;

    // Size (measured in bytes occupied by items' chunks) of the response
    // currently being constructed.
    int64_t current_response_size_bytes_ ABSL_GUARDED_BY(mu_);

    // True if the reactor is awaiting the result of a sampling request already
    // enqueued in the target table.
    bool waiting_for_enqueued_sample_ ABSL_GUARDED_BY(mu_);
  };

  return new WorkerlessSampleReactor(this);
}

std::shared_ptr<Table> ReverbServiceImpl::TableByName(
    absl::string_view name) const {
  auto it = tables_.find(name);
  if (it == tables_.end()) return nullptr;
  return it->second;
}

void ReverbServiceImpl::Close() {
  for (auto& table : tables_) {
    table.second->Close();
  }
}

std::string ReverbServiceImpl::DebugString() const {
  std::string str = "ReverbServiceAsync(tables=[";
  for (auto iter = tables_.cbegin(); iter != tables_.cend(); ++iter) {
    if (iter != tables_.cbegin()) {
      absl::StrAppend(&str, ", ");
    }
    absl::StrAppend(&str, iter->second->DebugString());
  }
  absl::StrAppend(&str, "], checkpointer=",
                  (checkpointer_ ? checkpointer_->DebugString() : "nullptr"),
                  ")");
  return str;
}

grpc::ServerUnaryReactor* ReverbServiceImpl::ServerInfo(
    grpc::CallbackServerContext* context, const ServerInfoRequest* request,
    ServerInfoResponse* response) {
  grpc::ServerUnaryReactor* reactor = context->DefaultReactor();
  for (const auto& iter : tables_) {
    *response->add_table_info() = iter.second->info();
  }
  *response->mutable_tables_state_id() = Uint128ToMessage(tables_state_id_);
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

internal::flat_hash_map<std::string, std::shared_ptr<Table>>
ReverbServiceImpl::tables() const {
  return tables_;
}

}  // namespace reverb
}  // namespace deepmind
