// Copyright 2023 The TensorStore Authors
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

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "absl/log/absl_log.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "tensorstore/context.h"
#include "tensorstore/data_type.h"
#include "tensorstore/internal/data_copy_concurrency_resource.h"
#include "tensorstore/internal/digest/sha256.h"
#include "tensorstore/internal/http/curl_transport.h"
#include "tensorstore/internal/http/http_request.h"
#include "tensorstore/internal/http/http_response.h"
#include "tensorstore/internal/http/http_transport.h"
#include "tensorstore/internal/intrusive_ptr.h"
#include "tensorstore/internal/json_binding/json_binding.h"
#include "tensorstore/internal/metrics/counter.h"
#include "tensorstore/internal/metrics/histogram.h"
#include "tensorstore/internal/retry.h"
#include "tensorstore/internal/schedule_at.h"
#include "tensorstore/internal/source_location.h"
#include "tensorstore/internal/uri_utils.h"
#include "tensorstore/kvstore/byte_range.h"
#include "tensorstore/kvstore/driver.h"
#include "tensorstore/kvstore/gcs/validate.h"
#include "tensorstore/kvstore/gcs_http/rate_limiter.h"
#include "tensorstore/kvstore/generation.h"
#include "tensorstore/kvstore/key_range.h"
#include "tensorstore/kvstore/operations.h"
#include "tensorstore/kvstore/read_result.h"
#include "tensorstore/kvstore/registry.h"
#include "tensorstore/kvstore/s3/s3_credential_provider.h"
#include "tensorstore/kvstore/s3/s3_metadata.h"
#include "tensorstore/kvstore/s3/s3_request_builder.h"
#include "tensorstore/kvstore/s3/s3_resource.h"
#include "tensorstore/kvstore/s3/s3_uri_utils.h"
#include "tensorstore/kvstore/s3/validate.h"
#include "tensorstore/kvstore/spec.h"
#include "tensorstore/kvstore/url_registry.h"
#include "tensorstore/util/execution/any_receiver.h"
#include "tensorstore/util/execution/execution.h"
#include "tensorstore/util/executor.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/garbage_collection/fwd.h"
#include "tensorstore/util/quote_string.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"
#include "tensorstore/util/str_cat.h"

// specializations
#include "tensorstore/internal/cache_key/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/internal/json_binding/std_array.h"  // IWYU pragma: keep
#include "tensorstore/internal/json_binding/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/serialization/fwd.h"  // IWYU pragma: keep
#include "tensorstore/serialization/std_optional.h"  // IWYU pragma: keep
#include "tensorstore/util/garbage_collection/std_optional.h"  // IWYU pragma: keep

#ifndef TENSORSTORE_INTERNAL_S3_LOG_REQUESTS
#define TENSORSTORE_INTERNAL_S3_LOG_REQUESTS 0
#endif

#ifndef TENSORSTORE_INTERNAL_S3_LOG_RESPONSES
#define TENSORSTORE_INTERNAL_S3_LOG_RESPONSES 0
#endif

using ::tensorstore::internal::DataCopyConcurrencyResource;
using ::tensorstore::internal::IntrusivePtr;
using ::tensorstore::internal::ScheduleAt;
using ::tensorstore::internal::SHA256Digester;
using ::tensorstore::internal_http::HttpRequest;
using ::tensorstore::internal_http::HttpResponse;
using ::tensorstore::internal_http::HttpTransport;
using ::tensorstore::internal_kvstore_gcs_http::RateLimiter;
using ::tensorstore::internal_kvstore_gcs_http::RateLimiterNode;
using ::tensorstore::internal_kvstore_s3::CredentialProvider;
using ::tensorstore::internal_kvstore_s3::FindTag;
using ::tensorstore::internal_kvstore_s3::GetS3CredentialProvider;
using ::tensorstore::internal_kvstore_s3::GetTag;
using ::tensorstore::internal_kvstore_s3::IsValidBucketName;
using ::tensorstore::internal_kvstore_s3::IsValidObjectName;
using ::tensorstore::internal_kvstore_s3::IsValidStorageGeneration;
using ::tensorstore::internal_kvstore_s3::S3ConcurrencyResource;
using ::tensorstore::internal_kvstore_s3::S3Credentials;
using ::tensorstore::internal_kvstore_s3::S3RateLimiterResource;
using ::tensorstore::internal_kvstore_s3::S3RequestBuilder;
using ::tensorstore::internal_kvstore_s3::S3RequestRetries;
using ::tensorstore::internal_kvstore_s3::S3UriEncode;
using ::tensorstore::internal_kvstore_s3::S3UriObjectKeyEncode;
using ::tensorstore::internal_kvstore_s3::StorageGenerationFromHeaders;
using ::tensorstore::internal_kvstore_s3::TagAndPosition;
using ::tensorstore::internal_storage_gcs::IsRetriable;
using ::tensorstore::kvstore::Key;
using ::tensorstore::kvstore::ListOptions;

namespace {}  // namespace

namespace tensorstore {
namespace {
namespace jb = tensorstore::internal_json_binding;

auto& s3_bytes_read = internal_metrics::Counter<int64_t>::New(
    "/tensorstore/kvstore/s3/bytes_read",
    "Bytes read by the s3 kvstore driver");

auto& s3_bytes_written = internal_metrics::Counter<int64_t>::New(
    "/tensorstore/kvstore/s3/bytes_written",
    "Bytes written by the s3 kvstore driver");

auto& s3_retries = internal_metrics::Counter<int64_t>::New(
    "/tensorstore/kvstore/s3/retries",
    "Count of all retried S3 requests (read/write/delete)");

auto& s3_read = internal_metrics::Counter<int64_t>::New(
    "/tensorstore/kvstore/s3/read", "S3 driver kvstore::Read calls");

auto& s3_read_latency_ms =
    internal_metrics::Histogram<internal_metrics::DefaultBucketer>::New(
        "/tensorstore/kvstore/s3/read_latency_ms",
        "S3 driver kvstore::Read latency (ms)");

auto& s3_write = internal_metrics::Counter<int64_t>::New(
    "/tensorstore/kvstore/s3/write", "S3 driver kvstore::Write calls");

auto& s3_write_latency_ms =
    internal_metrics::Histogram<internal_metrics::DefaultBucketer>::New(
        "/tensorstore/kvstore/s3/write_latency_ms",
        "S3 driver kvstore::Write latency (ms)");

auto& s3_delete_range = internal_metrics::Counter<int64_t>::New(
    "/tensorstore/kvstore/s3/delete_range",
    "S3 driver kvstore::DeleteRange calls");

auto& s3_list = internal_metrics::Counter<int64_t>::New(
    "/tensorstore/kvstore/s3/list", "S3 driver kvstore::List calls");

/// S3 strings
static constexpr char kUriScheme[] = "s3";
static constexpr char kDotAmazonAwsDotCom[] = ".amazonaws.com";
static constexpr char kAmzBucketRegionHeader[] = "x-amz-bucket-region";

/// sha256 hash of an empty string
static constexpr char kEmptySha256[] =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

/// An empty etag which should not collide with an actual payload hash
static constexpr char kEmptyEtag[] = "\"\"";

/// Adds the generation header to the provided builder.
bool AddGenerationHeader(S3RequestBuilder* builder, std::string_view header,
                         const StorageGeneration& gen) {
  if (StorageGeneration::IsUnknown(gen)) {
    // Unconditional.
    return false;
  }

  // If no generation is provided, we still need to provide an empty etag
  auto etag = StorageGeneration::IsNoValue(gen)
                  ? kEmptyEtag
                  : StorageGeneration::DecodeString(gen);

  builder->AddHeader(absl::StrCat(header, ": ", etag));
  return true;
}

std::string payload_sha256(const absl::Cord& cord = absl::Cord()) {
  SHA256Digester sha256;
  sha256.Write(cord);
  auto digest = sha256.Digest();
  auto digest_sv = std::string_view(reinterpret_cast<const char*>(&digest[0]),
                                    digest.size());

  return absl::BytesToHexString(digest_sv);
}

struct S3KeyValueStoreSpecData {
  std::string bucket;
  bool requester_pays;
  std::optional<std::string> endpoint;
  std::optional<std::string> host;
  std::string profile;
  std::string aws_region;

  Context::Resource<S3ConcurrencyResource> request_concurrency;
  std::optional<Context::Resource<S3RateLimiterResource>> rate_limiter;
  Context::Resource<S3RequestRetries> retries;
  Context::Resource<DataCopyConcurrencyResource> data_copy_concurrency;

  constexpr static auto ApplyMembers = [](auto& x, auto f) {
    return f(x.bucket, x.request_concurrency, x.rate_limiter, x.requester_pays,
             x.endpoint, x.host, x.profile, x.retries, x.data_copy_concurrency);
  };

  constexpr static auto default_json_binder = jb::Object(
      // Bucket is specified in the `spec` since it identifies the resource
      // being accessed.
      jb::Member("bucket",
                 jb::Projection<&S3KeyValueStoreSpecData::bucket>(jb::Validate(
                     [](const auto& options, const std::string* x) {
                       if (!IsValidBucketName(*x)) {
                         return absl::InvalidArgumentError(tensorstore::StrCat(
                             "Invalid S3 bucket name: ", QuoteString(*x)));
                       }
                       return absl::OkStatus();
                     }))),

      jb::Member("requester_pays",
                 jb::Projection<&S3KeyValueStoreSpecData::requester_pays>(
                     jb::DefaultValue([](auto* v) { *v = false; }))),
      jb::Member("host", jb::Projection<&S3KeyValueStoreSpecData::host>()),
      jb::Member("endpoint",
                 jb::Projection<&S3KeyValueStoreSpecData::endpoint>()),
      // TODO: Move to s3_credentials resource.
      jb::Member("profile", jb::Projection<&S3KeyValueStoreSpecData::profile>(
                                jb::DefaultValue<jb::kAlwaysIncludeDefaults>(
                                    [](auto* v) { *v = "default"; }))),
      jb::OptionalMember(
          "aws_region", jb::Projection<&S3KeyValueStoreSpecData::aws_region>()),
      jb::Member(
          S3ConcurrencyResource::id,
          jb::Projection<&S3KeyValueStoreSpecData::request_concurrency>()),
      jb::Member(S3RateLimiterResource::id,
                 jb::Projection<&S3KeyValueStoreSpecData::rate_limiter>()),
      jb::Member(S3RequestRetries::id,
                 jb::Projection<&S3KeyValueStoreSpecData::retries>()),
      jb::Member(DataCopyConcurrencyResource::id,
                 jb::Projection<
                     &S3KeyValueStoreSpecData::data_copy_concurrency>()) /**/
  );
};

std::string GetS3Url(std::string_view bucket, std::string_view path) {
  return tensorstore::StrCat(kUriScheme, "://", bucket, "/", S3UriEncode(path));
}

class S3KeyValueStoreSpec
    : public internal_kvstore::RegisteredDriverSpec<S3KeyValueStoreSpec,
                                                    S3KeyValueStoreSpecData> {
 public:
  static constexpr char id[] = "s3";

  Future<kvstore::DriverPtr> DoOpen() const override;
  Result<std::string> ToUrl(std::string_view path) const override {
    return GetS3Url(data_.bucket, path);
  }
};

class S3KeyValueStore
    : public internal_kvstore::RegisteredDriver<S3KeyValueStore,
                                                S3KeyValueStoreSpec> {
 public:
  const std::string& endpoint() const { return endpoint_; }
  bool IsAwsEndpoint() const {
    return absl::EndsWith(endpoint_, kDotAmazonAwsDotCom);
  }

  Future<ReadResult> Read(Key key, ReadOptions options) override;

  Future<TimestampedStorageGeneration> Write(Key key,
                                             std::optional<Value> value,
                                             WriteOptions options) override;

  void ListImpl(ListOptions options,
                AnyFlowReceiver<absl::Status, Key> receiver) override;

  Future<const void> DeleteRange(KeyRange range) override;

  absl::Status GetBoundSpecData(SpecData& spec) const {
    spec = spec_;
    return absl::OkStatus();
  }

  const Executor& executor() const {
    return spec_.data_copy_concurrency->executor;
  }

  RateLimiter& read_rate_limiter() {
    if (spec_.rate_limiter.has_value()) {
      return *(spec_.rate_limiter.value()->read_limiter);
    }
    return no_rate_limiter_;
  }
  RateLimiter& write_rate_limiter() {
    if (spec_.rate_limiter.has_value()) {
      return *(spec_.rate_limiter.value()->write_limiter);
    }
    return no_rate_limiter_;
  }

  RateLimiter& admission_queue() { return *spec_.request_concurrency->queue; }

  Result<std::optional<S3Credentials>> GetCredentials() {
    if (!credential_provider_) {
      auto result = GetS3CredentialProvider(spec_.profile, transport_);
      if (!result.ok() && absl::IsNotFound(result.status())) {
        credential_provider_ = nullptr;
      } else {
        TENSORSTORE_RETURN_IF_ERROR(result);
        credential_provider_ = std::move(*result);
      }
    }
    if (!*credential_provider_) return std::nullopt;
    auto credential_result_ = (*credential_provider_)->GetCredentials();
    if (!credential_result_.ok() &&
        absl::IsNotFound(credential_result_.status())) {
      return std::nullopt;
    }
    return credential_result_;
  }

  // Apply default backoff/retry logic to the task.
  // Returns whether the task will be retried. On false, max retries have
  // been met or exceeded.  On true, `task->Retry()` will be scheduled to run
  // after a suitable backoff period.
  template <typename Task>
  absl::Status BackoffForAttemptAsync(
      absl::Status status, int attempt, Task* task,
      SourceLocation loc = ::tensorstore::SourceLocation::current()) {
    if (attempt >= spec_.retries->max_retries) {
      return MaybeAnnotateStatus(
          status,
          tensorstore::StrCat("All ", attempt, " retry attempts failed"),
          absl::StatusCode::kAborted, loc);
    }

    // https://cloud.google.com/storage/docs/retry-strategy#exponential-backoff
    s3_retries.Increment();
    auto delay = internal::BackoffForAttempt(
        attempt, spec_.retries->initial_delay, spec_.retries->max_delay,
        /*jitter=*/std::min(absl::Seconds(1), spec_.retries->initial_delay));
    ScheduleAt(absl::Now() + delay,
               WithExecutor(executor(), [task = IntrusivePtr<Task>(task)] {
                 task->Retry();
               }));

    return absl::OkStatus();
  }

  std::shared_ptr<HttpTransport> transport_;
  internal_kvstore_gcs_http::NoRateLimiter no_rate_limiter_;
  // TODO: Clarify use of endpoint_ and host_.
  std::string endpoint_;  // endpoint url
  std::string host_;
  SpecData spec_;

  absl::Mutex credential_provider_mutex_;
  std::optional<std::shared_ptr<CredentialProvider>> credential_provider_;
  std::string aws_region_;
};

Future<kvstore::DriverPtr> S3KeyValueStoreSpec::DoOpen() const {
  auto driver = internal::MakeIntrusivePtr<S3KeyValueStore>();
  driver->spec_ = data_;
  driver->transport_ = internal_http::GetDefaultHttpTransport();

  if (data_.endpoint.has_value()) {
    auto endpoint = data_.endpoint.value();
    auto parsed = internal::ParseGenericUri(endpoint);
    if (parsed.scheme != "http" && parsed.scheme != "https") {
      return absl::InvalidArgumentError(
          tensorstore::StrCat("Endpoint ", endpoint, " has invalid schema ",
                              parsed.scheme, ". Should be http(s)."));
    }
    if (!parsed.query.empty()) {
      return absl::InvalidArgumentError(
          tensorstore::StrCat("Query in endpoint unsupported ", endpoint));
    }
    if (!parsed.fragment.empty()) {
      return absl::InvalidArgumentError(
          tensorstore::StrCat("Fragment in endpoint unsupported ", endpoint));
    }

    driver->aws_region_ = data_.aws_region;
    driver->endpoint_ = endpoint;

    if (data_.host.has_value()) {
      driver->host_ = data_.host.value();
    } else {
      auto parsed = internal::ParseGenericUri(driver->endpoint_);
      size_t end_of_host = parsed.authority_and_path.find('/');
      driver->host_ = parsed.authority_and_path.substr(0, end_of_host);
    }
  } else if (!data_.aws_region.empty()) {
    // AWS Region
    driver->aws_region_ = data_.aws_region;
    driver->host_ = driver->endpoint_ =
        tensorstore::StrCat("https://", data_.bucket, ".s3.",
                            driver->aws_region_, kDotAmazonAwsDotCom);
  } else {
    // TODO: Rework this to happen on the first Read/Write/Lisst call.

    // Assume AWS
    // Make global request to get bucket region from response headers,
    // then create region specific endpoint
    auto url = tensorstore::StrCat("https://", data_.bucket, ".s3",
                                   kDotAmazonAwsDotCom);
    auto request =
        internal_http::HttpRequestBuilder("HEAD", url).BuildRequest();
    auto future = driver->transport_->IssueRequest(request, {});
    if (!future.status().ok()) return future.status();
    auto& headers = future.value().headers;
    if (auto it = headers.find(kAmzBucketRegionHeader); it != headers.end()) {
      driver->aws_region_ = it->second;
      driver->host_ = driver->endpoint_ =
          tensorstore::StrCat("https://", data_.bucket, ".s3.",
                              driver->aws_region_, kDotAmazonAwsDotCom);
    } else {
      return absl::FailedPreconditionError(
          tensorstore::StrCat("bucket ", data_.bucket, " does not exist"));
    }
  }

  ABSL_LOG(INFO) << "S3 driver using endpoint [" << driver->endpoint_ << "]";

  // NOTE: Remove temporary logging use of experimental feature.
  if (data_.rate_limiter.has_value()) {
    ABSL_LOG(INFO) << "Using experimental_s3_rate_limiter";
  }
  return driver;
}

/// A ReadTask is a function object used to satisfy a
/// S3KeyValueStore::Read request.
struct ReadTask : public RateLimiterNode,
                  public internal::AtomicReferenceCount<ReadTask> {
  IntrusivePtr<S3KeyValueStore> owner;
  std::string read_url;
  kvstore::ReadOptions options;
  Promise<kvstore::ReadResult> promise;

  int attempt_ = 0;
  absl::Time start_time_;

  ReadTask(IntrusivePtr<S3KeyValueStore> owner, std::string read_url,
           kvstore::ReadOptions options, Promise<kvstore::ReadResult> promise)
      : owner(std::move(owner)),
        read_url(std::move(read_url)),
        options(std::move(options)),
        promise(std::move(promise)) {}

  ~ReadTask() { owner->admission_queue().Finish(this); }

  static void Start(void* task) {
    auto* self = reinterpret_cast<ReadTask*>(task);
    self->owner->read_rate_limiter().Finish(self);
    self->owner->admission_queue().Admit(self, &ReadTask::Admit);
  }

  static void Admit(void* task) {
    auto* self = reinterpret_cast<ReadTask*>(task);
    self->owner->executor()(
        [state = IntrusivePtr<ReadTask>(self, internal::adopt_object_ref)] {
          state->Retry();
        });
  }

  void Retry() {
    if (!promise.result_needed()) {
      return;
    }

    auto maybe_credentials = owner->GetCredentials();
    if (!maybe_credentials.ok()) {
      promise.SetResult(maybe_credentials.status());
      return;
    }

    auto request_builder = S3RequestBuilder("GET", read_url);

    AddGenerationHeader(&request_builder, "if-none-match",
                        options.if_not_equal);
    AddGenerationHeader(&request_builder, "if-match", options.if_equal);

    S3Credentials credentials;

    if (maybe_credentials.value().has_value()) {
      credentials = std::move(*maybe_credentials.value());
    }

    start_time_ = absl::Now();
    auto request =
        request_builder.EnableAcceptEncoding()
            .MaybeAddRequesterPayer(owner->spec_.requester_pays)
            .MaybeAddRangeHeader(options.byte_range)
            .BuildRequest(owner->host_, credentials, owner->aws_region_,
                          kEmptySha256, start_time_);

    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_REQUESTS)
        << "ReadTask: " << request;
    auto future = owner->transport_->IssueRequest(request, {});
    future.ExecuteWhenReady([self = IntrusivePtr<ReadTask>(this)](
                                ReadyFuture<HttpResponse> response) {
      self->OnResponse(response.result());
    });
  }

  void OnResponse(const Result<HttpResponse>& response) {
    if (!promise.result_needed()) {
      return;
    }
    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_RESPONSES && response.ok())
        << "ReadTask " << *response;

    absl::Status status = [&]() -> absl::Status {
      if (!response.ok()) return response.status();
      switch (response.value().status_code) {
        // Special status codes handled outside the retry loop.
        case 412:
        case 404:
        case 304:
          return absl::OkStatus();
      }
      return HttpResponseCodeToStatus(response.value());
    }();

    if (!status.ok() && IsRetriable(status)) {
      status = owner->BackoffForAttemptAsync(status, attempt_++, this);
      if (status.ok()) {
        return;
      }
    }
    if (!status.ok()) {
      promise.SetResult(status);
    } else {
      promise.SetResult(FinishResponse(response.value()));
    }
  }

  Result<kvstore::ReadResult> FinishResponse(const HttpResponse& httpresponse) {
    s3_bytes_read.IncrementBy(httpresponse.payload.size());
    auto latency = absl::Now() - start_time_;
    s3_read_latency_ms.Observe(absl::ToInt64Milliseconds(latency));

    // Parse `Date` header from response to correctly handle cached responses.
    // The GCS servers always send a `date` header.
    kvstore::ReadResult read_result;
    read_result.stamp.time = start_time_;

    switch (httpresponse.status_code) {
      case 204:
      case 404:
        // Object not found.
        read_result.stamp.generation = StorageGeneration::NoValue();
        read_result.state = kvstore::ReadResult::kMissing;
        return read_result;
      case 412:
        // "Failed precondition": indicates the ifGenerationMatch condition
        // did not hold.
        // NOTE: This is returned even when the object does not exist.
        read_result.stamp.generation = StorageGeneration::Unknown();
        return read_result;
      case 304:
        // "Not modified": indicates that the ifGenerationNotMatch condition
        // did not hold.
        read_result.stamp.generation = options.if_not_equal;
        return read_result;
    }

    size_t payload_size = httpresponse.payload.size();
    if (httpresponse.status_code != 206) {
      // This may or may not have been a range request; attempt to validate.
      TENSORSTORE_ASSIGN_OR_RETURN(auto byte_range,
                                   options.byte_range.Validate(payload_size));
      read_result.state = kvstore::ReadResult::kValue;
      read_result.value =
          internal::GetSubCord(httpresponse.payload, byte_range);
    } else {
      // Server should return a parseable content-range header.
      TENSORSTORE_ASSIGN_OR_RETURN(auto content_range_tuple,
                                   ParseContentRangeHeader(httpresponse));

      if (auto request_size = options.byte_range.size();
          (options.byte_range.inclusive_min != -1 &&
           options.byte_range.inclusive_min !=
               std::get<0>(content_range_tuple)) ||
          (request_size != -1 && request_size != payload_size)) {
        // Return an error when the response does not start at the requested
        // offset of when the response is smaller than the desired size.
        return absl::OutOfRangeError(tensorstore::StrCat(
            "Requested byte range ", options.byte_range,
            " was not satisfied by S3 response of size ", payload_size));
      }
      // assert(payload_size == std::get<2>(content_range_tuple));
      read_result.state = kvstore::ReadResult::kValue;
      read_result.value = httpresponse.payload;
    }

    TENSORSTORE_ASSIGN_OR_RETURN(
        read_result.stamp.generation,
        StorageGenerationFromHeaders(httpresponse.headers));
    return read_result;
  }
};

Future<kvstore::ReadResult> S3KeyValueStore::Read(Key key,
                                                  ReadOptions options) {
  s3_read.Increment();
  if (!IsValidObjectName(key)) {
    return absl::InvalidArgumentError("Invalid S3 object name");
  }
  if (!IsValidStorageGeneration(options.if_equal) ||
      !IsValidStorageGeneration(options.if_not_equal)) {
    return absl::InvalidArgumentError("Malformed StorageGeneration");
  }

  auto encoded_object_name = S3UriObjectKeyEncode(key);
  std::string resource =
      tensorstore::StrCat(endpoint_, "/", encoded_object_name);

  auto op = PromiseFuturePair<ReadResult>::Make();
  auto state = internal::MakeIntrusivePtr<ReadTask>(
      internal::IntrusivePtr<S3KeyValueStore>(this), std::move(resource),
      std::move(options), std::move(op.promise));

  intrusive_ptr_increment(state.get());  // adopted by ReadTask::Start.
  read_rate_limiter().Admit(state.get(), &ReadTask::Start);
  return std::move(op.future);
}

/// A WriteTask is a function object used to satisfy a
/// S3KeyValueStore::Write request.
struct WriteTask : public RateLimiterNode,
                   public internal::AtomicReferenceCount<WriteTask> {
  IntrusivePtr<S3KeyValueStore> owner;
  std::string encoded_object_name;
  absl::Cord value;
  kvstore::WriteOptions options;
  Promise<TimestampedStorageGeneration> promise;

  S3Credentials credentials_;
  std::string upload_url_;
  int attempt_ = 0;
  absl::Time start_time_;

  WriteTask(IntrusivePtr<S3KeyValueStore> owner,
            std::string encoded_object_name, absl::Cord value,
            kvstore::WriteOptions options,
            Promise<TimestampedStorageGeneration> promise)
      : owner(std::move(owner)),
        encoded_object_name(std::move(encoded_object_name)),
        value(std::move(value)),
        options(std::move(options)),
        promise(std::move(promise)) {}

  ~WriteTask() { owner->admission_queue().Finish(this); }

  static void Start(void* task) {
    auto* self = reinterpret_cast<WriteTask*>(task);
    self->owner->write_rate_limiter().Finish(self);
    self->owner->admission_queue().Admit(self, &WriteTask::Admit);
  }
  static void Admit(void* task) {
    auto* self = reinterpret_cast<WriteTask*>(task);
    self->owner->executor()(
        [state = IntrusivePtr<WriteTask>(self, internal::adopt_object_ref)] {
          state->Retry();
        });
  }

  /// Writes an object to S3.
  void Retry() {
    if (!promise.result_needed()) {
      return;
    }
    upload_url_ =
        tensorstore::StrCat(owner->endpoint_, "/", encoded_object_name);
    auto maybe_credentials = owner->GetCredentials();
    if (!maybe_credentials.ok()) {
      promise.SetResult(maybe_credentials.status());
      return;
    }

    if (maybe_credentials.value().has_value()) {
      credentials_ = std::move(*maybe_credentials.value());
    }

    if (StorageGeneration::IsUnknown(options.if_equal)) {
      DoPut();
      return;
    }

    // S3 doesn't support conditional PUT, so we use a HEAD call
    // to test the if-match condition
    auto now = absl::Now();
    auto builder = S3RequestBuilder("HEAD", upload_url_);
    AddGenerationHeader(&builder, "if-match", options.if_equal);

    auto request = builder.MaybeAddRequesterPayer(owner->spec_.requester_pays)
                       .BuildRequest(owner->host_, credentials_,
                                     owner->aws_region_, kEmptySha256, now);

    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_REQUESTS)
        << "WriteTask (Peek): " << request;

    auto future = owner->transport_->IssueRequest(request, {});
    future.ExecuteWhenReady([self = IntrusivePtr<WriteTask>(this)](
                                ReadyFuture<HttpResponse> response) {
      self->OnPeekResponse(response.result());
    });
  }

  void OnPeekResponse(const Result<HttpResponse>& response) {
    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_RESPONSES && response.ok())
        << "WriteTask (Peek) " << *response;

    if (!response.ok()) {
      promise.SetResult(response.status());
      return;
    }

    TimestampedStorageGeneration r;
    r.time = absl::Now();
    switch (response.value().status_code) {
      case 304:
        // Not modified implies that the generation did not match.
        [[fallthrough]];
      case 412:
        // Failed precondition implies the generation did not match.
        r.generation = StorageGeneration::Unknown();
        promise.SetResult(r);
        return;
      case 404:
        if (!StorageGeneration::IsUnknown(options.if_equal) &&
            !StorageGeneration::IsNoValue(options.if_equal)) {
          r.generation = StorageGeneration::Unknown();
          promise.SetResult(r);
          return;
        }
        break;
      default:
        break;
    }

    DoPut();
  }

  void DoPut() {
    // NOTE: This was changed from POST to PUT as a basic POST does not work
    // Some more headers need to be added to allow POST to work:
    // https://docs.aws.amazon.com/AmazonS3/latest/API/sigv4-authentication-HTTPPOST.html
    upload_url_ =
        tensorstore::StrCat(owner->endpoint_, "/", encoded_object_name);

    start_time_ = absl::Now();
    auto content_sha256 = payload_sha256(value);

    auto request =
        S3RequestBuilder("PUT", upload_url_)
            .AddHeader("Content-Type: application/octet-stream")
            .AddHeader(absl::StrCat("Content-Length: ", value.size()))
            .MaybeAddRequesterPayer(owner->spec_.requester_pays)
            .BuildRequest(owner->host_, credentials_, owner->aws_region_,
                          content_sha256, start_time_);

    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_REQUESTS)
        << "WriteTask: " << request << " size=" << value.size();

    auto future = owner->transport_->IssueRequest(request, value);
    future.ExecuteWhenReady([self = IntrusivePtr<WriteTask>(this)](
                                ReadyFuture<HttpResponse> response) {
      self->OnResponse(response.result());
    });
  }

  void OnResponse(const Result<HttpResponse>& response) {
    if (!promise.result_needed()) {
      return;
    }
    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_RESPONSES && response.ok())
        << "WriteTask " << *response;

    absl::Status status = !response.ok()
                              ? response.status()
                              : HttpResponseCodeToStatus(response.value());

    if (!status.ok() && IsRetriable(status)) {
      status = owner->BackoffForAttemptAsync(status, attempt_++, this);
      if (status.ok()) {
        return;
      }
    }
    if (!status.ok()) {
      promise.SetResult(status);
      return;
    }

    promise.SetResult(FinishResponse(response.value()));
  }

  Result<TimestampedStorageGeneration> FinishResponse(
      const HttpResponse& response) {
    TimestampedStorageGeneration r;
    r.time = start_time_;
    switch (response.status_code) {
      case 404:
        if (!StorageGeneration::IsUnknown(options.if_equal)) {
          r.generation = StorageGeneration::Unknown();
          return r;
        }
    }

    auto latency = absl::Now() - start_time_;
    s3_write_latency_ms.Observe(absl::ToInt64Milliseconds(latency));
    s3_bytes_written.IncrementBy(value.size());
    TENSORSTORE_ASSIGN_OR_RETURN(
        r.generation, StorageGenerationFromHeaders(response.headers));
    return r;
  }
};

/// A DeleteTask is a function object used to satisfy a
/// S3KeyValueStore::Delete request.
struct DeleteTask : public RateLimiterNode,
                    public internal::AtomicReferenceCount<DeleteTask> {
  IntrusivePtr<S3KeyValueStore> owner;
  std::string resource;
  kvstore::WriteOptions options;
  Promise<TimestampedStorageGeneration> promise;

  int attempt_ = 0;
  absl::Time start_time_;
  S3Credentials credentials_;

  DeleteTask(IntrusivePtr<S3KeyValueStore> owner, std::string resource,
             kvstore::WriteOptions options,
             Promise<TimestampedStorageGeneration> promise)
      : owner(std::move(owner)),
        resource(std::move(resource)),
        options(std::move(options)),
        promise(std::move(promise)) {}

  ~DeleteTask() { owner->admission_queue().Finish(this); }

  static void Start(void* task) {
    auto* self = reinterpret_cast<DeleteTask*>(task);
    self->owner->write_rate_limiter().Finish(self);
    self->owner->admission_queue().Admit(self, &DeleteTask::Admit);
  }

  static void Admit(void* task) {
    auto* self = reinterpret_cast<DeleteTask*>(task);
    self->owner->executor()(
        [state = IntrusivePtr<DeleteTask>(self, internal::adopt_object_ref)] {
          state->Retry();
        });
  }

  /// Removes an object from S3.
  void Retry() {
    if (!promise.result_needed()) {
      return;
    }
    std::string delete_url = resource;

    if (!IsValidStorageGeneration(options.if_equal)) {
      promise.SetResult(
          absl::InvalidArgumentError("Malformed StorageGeneration"));
      return;
    }

    auto maybe_credentials = owner->GetCredentials();
    if (!maybe_credentials.ok()) {
      promise.SetResult(maybe_credentials.status());
      return;
    }

    if (maybe_credentials.value().has_value()) {
      credentials_ = std::move(*maybe_credentials.value());
    }

    if (StorageGeneration::IsUnknown(options.if_equal)) {
      DoDelete();
      return;
    }

    // S3 doesn't support conditional DELETE,
    // use a HEAD call to test the if-match condition
    auto now = absl::Now();
    auto builder = S3RequestBuilder("HEAD", delete_url);
    AddGenerationHeader(&builder, "if-match", options.if_equal);

    auto request = builder.MaybeAddRequesterPayer(owner->spec_.requester_pays)
                       .BuildRequest(owner->host_, credentials_,
                                     owner->aws_region_, kEmptySha256, now);

    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_REQUESTS)
        << "DeleteTask (Peek): " << request;

    auto future = owner->transport_->IssueRequest(request, {});
    future.ExecuteWhenReady([self = IntrusivePtr<DeleteTask>(this)](
                                ReadyFuture<HttpResponse> response) {
      self->OnPeekResponse(response.result());
    });
  }

  void OnPeekResponse(const Result<HttpResponse>& response) {
    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_RESPONSES && response.ok())
        << "DeleteTask (Peek) " << *response;

    if (!response.ok()) {
      promise.SetResult(response.status());
      return;
    }

    TimestampedStorageGeneration r;
    r.time = absl::Now();
    switch (response.value().status_code) {
      case 412:
        // Failed precondition implies the generation did not match.
        r.generation = StorageGeneration::Unknown();
        promise.SetResult(std::move(r));
        return;
      case 404:
        if (!StorageGeneration::IsUnknown(options.if_equal) &&
            !StorageGeneration::IsNoValue(options.if_equal)) {
          r.generation = StorageGeneration::Unknown();
          promise.SetResult(std::move(r));
          return;
        }
        break;
      default:
        break;
    }

    DoDelete();
  }

  void DoDelete() {
    start_time_ = absl::Now();

    auto request =
        S3RequestBuilder("DELETE", resource)
            .MaybeAddRequesterPayer(owner->spec_.requester_pays)
            .BuildRequest(owner->host_, credentials_, owner->aws_region_,
                          kEmptySha256, start_time_);

    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_REQUESTS)
        << "DeleteTask: " << request;

    auto future = owner->transport_->IssueRequest(request, {});
    future.ExecuteWhenReady([self = IntrusivePtr<DeleteTask>(this)](
                                ReadyFuture<HttpResponse> response) {
      self->OnResponse(response.result());
    });
  }

  void OnResponse(const Result<HttpResponse>& response) {
    if (!promise.result_needed()) {
      return;
    }
    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_RESPONSES && response.ok())
        << "DeleteTask " << *response;

    absl::Status status = [&]() -> absl::Status {
      if (!response.ok()) return response.status();
      switch (response.value().status_code) {
        case 404:
          return absl::OkStatus();
        default:
          break;
      }
      return HttpResponseCodeToStatus(response.value());
    }();

    if (!status.ok() && IsRetriable(status)) {
      status = owner->BackoffForAttemptAsync(status, attempt_++, this);
      if (status.ok()) {
        return;
      }
    }
    if (!status.ok()) {
      promise.SetResult(status);
      return;
    }

    TimestampedStorageGeneration r;
    r.time = start_time_;
    switch (response.value().status_code) {
      case 404:
        // 404 Not Found means aborted when a StorageGeneration was specified.
        if (!StorageGeneration::IsNoValue(options.if_equal) &&
            !StorageGeneration::IsUnknown(options.if_equal)) {
          r.generation = StorageGeneration::Unknown();
          break;
        }
        [[fallthrough]];
      default:
        r.generation = StorageGeneration::NoValue();
        break;
    }
    promise.SetResult(std::move(r));
  }
};

Future<TimestampedStorageGeneration> S3KeyValueStore::Write(
    Key key, std::optional<Value> value, WriteOptions options) {
  s3_write.Increment();
  if (!IsValidObjectName(key)) {
    return absl::InvalidArgumentError("Invalid S3 object name");
  }
  if (!IsValidStorageGeneration(options.if_equal)) {
    return absl::InvalidArgumentError("Malformed StorageGeneration");
  }

  std::string encoded_object_name = S3UriObjectKeyEncode(key);
  auto op = PromiseFuturePair<TimestampedStorageGeneration>::Make();

  if (value) {
    auto state = internal::MakeIntrusivePtr<WriteTask>(
        IntrusivePtr<S3KeyValueStore>(this), std::move(encoded_object_name),
        std::move(*value), std::move(options), std::move(op.promise));

    intrusive_ptr_increment(state.get());  // adopted by WriteTask::Start.
    write_rate_limiter().Admit(state.get(), &WriteTask::Start);
  } else {
    std::string resource =
        tensorstore::StrCat(endpoint_, "/", encoded_object_name);

    auto state = internal::MakeIntrusivePtr<DeleteTask>(
        IntrusivePtr<S3KeyValueStore>(this), std::move(resource),
        std::move(options), std::move(op.promise));

    intrusive_ptr_increment(state.get());  // adopted by DeleteTask::Start.
    write_rate_limiter().Admit(state.get(), &DeleteTask::Start);
  }
  return std::move(op.future);
}

/// ListTask implements the ListImpl execution flow.
struct ListTask : public RateLimiterNode,
                  public internal::AtomicReferenceCount<ListTask> {
  internal::IntrusivePtr<S3KeyValueStore> owner_;
  ListOptions options_;
  AnyFlowReceiver<absl::Status, Key> receiver_;
  std::string resource_;

  std::string continuation_token_;
  absl::Time start_time_;
  int attempt_ = 0;
  bool has_query_parameters_;
  std::atomic<bool> cancelled_{false};

  ListTask(internal::IntrusivePtr<S3KeyValueStore> owner, ListOptions options,
           AnyFlowReceiver<absl::Status, Key> receiver, std::string resource)
      : owner_(std::move(owner)),
        options_(std::move(options)),
        receiver_(std::move(receiver)),
        resource_(std::move(resource)) {}

  ~ListTask() { owner_->admission_queue().Finish(this); }

  inline bool is_cancelled() {
    return cancelled_.load(std::memory_order_relaxed);
  }

  static void Start(void* task) {
    auto* self = reinterpret_cast<ListTask*>(task);
    self->owner_->read_rate_limiter().Finish(self);
    self->owner_->admission_queue().Admit(self, &ListTask::Admit);
  }
  static void Admit(void* task) {
    auto* self = reinterpret_cast<ListTask*>(task);
    execution::set_starting(self->receiver_, [self] {
      self->cancelled_.store(true, std::memory_order_relaxed);
    });
    self->owner_->executor()(
        [state = IntrusivePtr<ListTask>(self, internal::adopt_object_ref)] {
          state->IssueRequest();
        });
  }

  void Retry() { IssueRequest(); }

  void IssueRequest() {
    if (is_cancelled()) {
      execution::set_done(receiver_);
      execution::set_stopping(receiver_);
      return;
    }

    // https://docs.aws.amazon.com/AmazonS3/latest/API/API_ListObjectsV2.html
    auto request_builder =
        S3RequestBuilder("GET", resource_).AddQueryParameter("list-type", "2");

    if (auto& prefix = options_.range.inclusive_min; !prefix.empty()) {
      if (options_.strip_prefix_length) {
        prefix = prefix.substr(0, options_.strip_prefix_length);
      }
      request_builder.AddQueryParameter("prefix", prefix);
    }

    if (!continuation_token_.empty()) {
      request_builder.AddQueryParameter("continuation-token",
                                        continuation_token_);
    }

    auto maybe_credentials = owner_->GetCredentials();
    if (!maybe_credentials.ok()) {
      execution::set_error(receiver_, std::move(maybe_credentials).status());
      execution::set_stopping(receiver_);
      return;
    }

    S3Credentials credentials;

    if (maybe_credentials.value().has_value()) {
      credentials = std::move(*maybe_credentials.value());
    }

    start_time_ = absl::Now();

    auto request = request_builder.BuildRequest(owner_->host_, credentials,
                                                owner_->aws_region_,
                                                kEmptySha256, start_time_);

    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_REQUESTS)
        << "List: " << request;

    auto future = owner_->transport_->IssueRequest(request, {});
    future.ExecuteWhenReady(WithExecutor(
        owner_->executor(), [self = IntrusivePtr<ListTask>(this)](
                                ReadyFuture<HttpResponse> response) {
          self->OnResponse(response.result());
        }));
  }

  void OnResponse(const Result<HttpResponse>& response) {
    auto status = OnResponseImpl(response);
    // OkStatus are handled by OnResponseImpl
    if (absl::IsCancelled(status)) {
      execution::set_done(receiver_);
      execution::set_stopping(receiver_);
      return;
    }
    if (!status.ok()) {
      execution::set_error(receiver_, std::move(status));
      execution::set_stopping(receiver_);
      return;
    }
  }

  absl::Status OnResponseImpl(const Result<HttpResponse>& response) {
    if (is_cancelled()) {
      return absl::CancelledError();
    }
    ABSL_LOG_IF(INFO, TENSORSTORE_INTERNAL_S3_LOG_RESPONSES && response.ok())
        << "List " << *response;

    absl::Status status =
        response.ok() ? HttpResponseCodeToStatus(*response) : response.status();
    if (!status.ok() && IsRetriable(status)) {
      return owner_->BackoffForAttemptAsync(status, attempt_++, this);
    }

    TagAndPosition tag_and_pos;
    auto cord = response->payload;
    auto payload = cord.Flatten();
    auto kListBucketOpenTag =
        "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
    TENSORSTORE_ASSIGN_OR_RETURN(
        auto start_pos, FindTag(payload, kListBucketOpenTag, 0, false));
    TENSORSTORE_ASSIGN_OR_RETURN(
        tag_and_pos, GetTag(payload, "<KeyCount>", "</KeyCount>", start_pos));
    std::size_t keycount = 0;
    if (!absl::SimpleAtoi(tag_and_pos.tag, &keycount)) {
      return absl::InvalidArgumentError(
          absl::StrCat("Malformed KeyCount ", tag_and_pos.tag));
    }

    for (std::size_t k = 0; k < keycount; ++k) {
      if (is_cancelled()) {
        return absl::CancelledError();
      }
      TENSORSTORE_ASSIGN_OR_RETURN(
          auto contents_pos,
          FindTag(payload, "<Contents>", tag_and_pos.pos, false));
      TENSORSTORE_ASSIGN_OR_RETURN(
          tag_and_pos, GetTag(payload, "<Key>", "</Key>", contents_pos));

      if (!options_.range.empty() &&
          tensorstore::Contains(options_.range, tag_and_pos.tag)) {
        if (options_.strip_prefix_length &&
            tag_and_pos.tag.size() >= options_.strip_prefix_length) {
          tag_and_pos.tag =
              tag_and_pos.tag.substr(options_.strip_prefix_length);
        }

        execution::set_value(receiver_, tag_and_pos.tag);
      }
    }

    // Successful request, so clear the retry_attempt for the next request.
    attempt_ = 0;
    TENSORSTORE_ASSIGN_OR_RETURN(
        tag_and_pos,
        GetTag(payload, "<IsTruncated>", "</IsTruncated>", start_pos));

    if (tag_and_pos.tag == "true") {
      TENSORSTORE_ASSIGN_OR_RETURN(
          tag_and_pos, GetTag(payload, "<NextContinuationToken>",
                              "</NextContinuationToken>", start_pos));
      continuation_token_ = tag_and_pos.tag;
      IssueRequest();
    } else {
      continuation_token_.clear();
      execution::set_done(receiver_);
      execution::set_stopping(receiver_);
    }
    return absl::OkStatus();
  }
};

void S3KeyValueStore::ListImpl(ListOptions options,
                               AnyFlowReceiver<absl::Status, Key> receiver) {
  s3_list.Increment();
  if (options.range.empty()) {
    execution::set_starting(receiver, [] {});
    execution::set_done(receiver);
    execution::set_stopping(receiver);
    return;
  }

  auto state = internal::MakeIntrusivePtr<ListTask>(
      IntrusivePtr<S3KeyValueStore>(this), std::move(options),
      std::move(receiver),
      /*resource=*/tensorstore::StrCat(endpoint_, "/"));

  intrusive_ptr_increment(state.get());  // adopted by ListTask::Start.
  read_rate_limiter().Admit(state.get(), &ListTask::Start);
}

// Receiver used by `DeleteRange` for processing the results from `List`.
struct DeleteRangeListReceiver {
  IntrusivePtr<S3KeyValueStore> owner_;
  Promise<void> promise_;
  FutureCallbackRegistration cancel_registration_;

  void set_starting(AnyCancelReceiver cancel) {
    cancel_registration_ = promise_.ExecuteWhenNotNeeded(std::move(cancel));
  }

  void set_value(std::string key) {
    assert(!key.empty());
    if (!key.empty()) {
      LinkError(promise_, owner_->Delete(std::move(key)));
    }
  }

  void set_error(absl::Status error) {
    SetDeferredResult(promise_, std::move(error));
    promise_ = Promise<void>();
  }

  void set_done() { promise_ = Promise<void>(); }

  void set_stopping() { cancel_registration_.Unregister(); }
};

Future<const void> S3KeyValueStore::DeleteRange(KeyRange range) {
  s3_delete_range.Increment();
  if (range.empty()) return absl::OkStatus();

  // TODO(jbms): It could make sense to rate limit the list operation, so that
  // we don't get way ahead of the delete operations.  Currently our
  // sender/receiver abstraction does not support back pressure, though.
  auto op = PromiseFuturePair<void>::Make(tensorstore::MakeResult());
  ListOptions list_options;
  list_options.range = std::move(range);
  ListImpl(list_options, DeleteRangeListReceiver{
                             internal::IntrusivePtr<S3KeyValueStore>(this),
                             std::move(op.promise)});
  return std::move(op.future);
}

Result<kvstore::Spec> ParseS3Url(std::string_view url) {
  auto parsed = internal::ParseGenericUri(url);
  assert(parsed.scheme == kUriScheme);
  if (!parsed.query.empty()) {
    return absl::InvalidArgumentError("Query string not supported");
  }
  if (!parsed.fragment.empty()) {
    return absl::InvalidArgumentError("Fragment identifier not supported");
  }
  size_t end_of_bucket = parsed.authority_and_path.find('/');
  std::string_view bucket = parsed.authority_and_path.substr(0, end_of_bucket);
  if (!IsValidBucketName(bucket)) {
    return absl::InvalidArgumentError(
        tensorstore::StrCat("Invalid S3 bucket name: ", QuoteString(bucket)));
  }
  std::string path = internal::PercentDecode(
      (end_of_bucket == std::string_view::npos)
          ? std::string_view{}
          : parsed.authority_and_path.substr(end_of_bucket + 1));
  auto driver_spec = internal::MakeIntrusivePtr<S3KeyValueStoreSpec>();
  driver_spec->data_.bucket = bucket;
  driver_spec->data_.request_concurrency =
      Context::Resource<S3ConcurrencyResource>::DefaultSpec();
  driver_spec->data_.retries =
      Context::Resource<S3RequestRetries>::DefaultSpec();
  driver_spec->data_.data_copy_concurrency =
      Context::Resource<DataCopyConcurrencyResource>::DefaultSpec();

  driver_spec->data_.requester_pays = false;
  driver_spec->data_.profile = "default";

  return {std::in_place, std::move(driver_spec), std::move(path)};
}

const tensorstore::internal_kvstore::DriverRegistration<
    tensorstore::S3KeyValueStoreSpec>
    registration;
const tensorstore::internal_kvstore::UrlSchemeRegistration
    url_scheme_registration{kUriScheme, tensorstore::ParseS3Url};

}  // namespace
}  // namespace tensorstore

TENSORSTORE_DECLARE_GARBAGE_COLLECTION_NOT_REQUIRED(
    tensorstore::S3KeyValueStore)