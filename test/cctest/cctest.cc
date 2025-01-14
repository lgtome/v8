// Copyright 2008 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "test/cctest/cctest.h"

#include "include/cppgc/platform.h"
#include "include/libplatform/libplatform.h"
#include "include/v8-array-buffer.h"
#include "include/v8-context.h"
#include "include/v8-function.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-locker.h"
#include "src/base/platform/condition-variable.h"
#include "src/base/platform/mutex.h"
#include "src/base/platform/semaphore.h"
#include "src/base/strings.h"
#include "src/codegen/compiler.h"
#include "src/codegen/optimized-compilation-info.h"
#include "src/common/globals.h"
#include "src/compiler/pipeline.h"
#include "src/debug/debug.h"
#include "src/flags/flags.h"
#include "src/objects/objects-inl.h"
#include "src/trap-handler/trap-handler.h"
#include "test/cctest/print-extension.h"
#include "test/cctest/profiler-extension.h"
#include "test/cctest/trace-extension.h"

#ifdef V8_USE_PERFETTO
#include "src/tracing/trace-event.h"
#endif  // V8_USE_PERFETTO

#if V8_OS_WIN
#include <windows.h>
#if V8_CC_MSVC
#include <crtdbg.h>
#endif
#endif

enum InitializationState { kUnset, kUninitialized, kInitialized };
static InitializationState initialization_state_ = kUnset;

CcTest* CcTest::last_ = nullptr;
bool CcTest::initialize_called_ = false;
v8::base::Atomic32 CcTest::isolate_used_ = 0;
v8::ArrayBuffer::Allocator* CcTest::allocator_ = nullptr;
v8::Isolate* CcTest::isolate_ = nullptr;

CcTest::CcTest(TestFunction* callback, const char* file, const char* name,
               bool enabled, bool initialize)
    : callback_(callback),
      name_(name),
      enabled_(enabled),
      initialize_(initialize),
      prev_(last_) {
  // Find the base name of this test (const_cast required on Windows).
  char *basename = strrchr(const_cast<char *>(file), '/');
  if (!basename) {
    basename = strrchr(const_cast<char *>(file), '\\');
  }
  if (!basename) {
    basename = v8::internal::StrDup(file);
  } else {
    basename = v8::internal::StrDup(basename + 1);
  }
  // Drop the extension, if there is one.
  char *extension = strrchr(basename, '.');
  if (extension) *extension = 0;
  // Install this test in the list of tests
  file_ = basename;
  prev_ = last_;
  last_ = this;
}


void CcTest::Run() {
  if (!initialize_) {
    CHECK_NE(initialization_state_, kInitialized);
    initialization_state_ = kUninitialized;
    CHECK_NULL(CcTest::isolate_);
  } else {
    CHECK_NE(initialization_state_, kUninitialized);
    initialization_state_ = kInitialized;
    if (isolate_ == nullptr) {
      v8::Isolate::CreateParams create_params;
      create_params.array_buffer_allocator = allocator_;
      isolate_ = v8::Isolate::New(create_params);
    }
    isolate_->Enter();
  }
#ifdef DEBUG
  const size_t active_isolates = i::Isolate::non_disposed_isolates();
#endif  // DEBUG
  callback_();
#ifdef DEBUG
  // This DCHECK ensures that all Isolates are properly disposed after finishing
  // the test. Stray Isolates lead to stray tasks in the platform which can
  // interact weirdly when swapping in new platforms (for testing) or during
  // shutdown.
  DCHECK_EQ(active_isolates, i::Isolate::non_disposed_isolates());
#endif  // DEBUG
  if (initialize_) {
    if (i_isolate()->was_locker_ever_used()) {
      v8::Locker locker(isolate_);
      EmptyMessageQueues(isolate_);
    } else {
      EmptyMessageQueues(isolate_);
    }
    isolate_->Exit();
  }
}

i::Heap* CcTest::heap() { return i_isolate()->heap(); }
i::ReadOnlyHeap* CcTest::read_only_heap() {
  return i_isolate()->read_only_heap();
}

void CcTest::AddGlobalFunction(v8::Local<v8::Context> env, const char* name,
                               v8::FunctionCallback callback) {
  v8::Local<v8::FunctionTemplate> func_template =
      v8::FunctionTemplate::New(isolate_, callback);
  v8::Local<v8::Function> func =
      func_template->GetFunction(env).ToLocalChecked();
  func->SetName(v8_str(name));
  env->Global()->Set(env, v8_str(name), func).FromJust();
}

void CcTest::CollectGarbage(i::AllocationSpace space, i::Isolate* isolate) {
  i::Isolate* iso = isolate ? isolate : i_isolate();
  iso->heap()->CollectGarbage(space, i::GarbageCollectionReason::kTesting);
}

void CcTest::CollectAllGarbage(i::Isolate* isolate) {
  i::Isolate* iso = isolate ? isolate : i_isolate();
  iso->heap()->CollectAllGarbage(i::Heap::kNoGCFlags,
                                 i::GarbageCollectionReason::kTesting);
}

void CcTest::CollectAllAvailableGarbage(i::Isolate* isolate) {
  i::Isolate* iso = isolate ? isolate : i_isolate();
  iso->heap()->CollectAllAvailableGarbage(i::GarbageCollectionReason::kTesting);
}

void CcTest::PreciseCollectAllGarbage(i::Isolate* isolate) {
  i::Isolate* iso = isolate ? isolate : i_isolate();
  iso->heap()->PreciseCollectAllGarbage(i::Heap::kNoGCFlags,
                                        i::GarbageCollectionReason::kTesting);
}

i::Handle<i::String> CcTest::MakeString(const char* str) {
  i::Isolate* isolate = CcTest::i_isolate();
  i::Factory* factory = isolate->factory();
  return factory->InternalizeUtf8String(str);
}

i::Handle<i::String> CcTest::MakeName(const char* str, int suffix) {
  v8::base::EmbeddedVector<char, 128> buffer;
  v8::base::SNPrintF(buffer, "%s%d", str, suffix);
  return CcTest::MakeString(buffer.begin());
}

v8::base::RandomNumberGenerator* CcTest::random_number_generator() {
  return InitIsolateOnce()->random_number_generator();
}

v8::Local<v8::Object> CcTest::global() {
  return isolate()->GetCurrentContext()->Global();
}

void CcTest::InitializeVM() {
  CHECK(!v8::base::Relaxed_Load(&isolate_used_));
  CHECK(!initialize_called_);
  initialize_called_ = true;
  v8::HandleScope handle_scope(CcTest::isolate());
  v8::Context::New(CcTest::isolate())->Enter();
}

void CcTest::TearDown() {
  if (isolate_ != nullptr) isolate_->Dispose();
}

v8::Local<v8::Context> CcTest::NewContext(CcTestExtensionFlags extension_flags,
                                          v8::Isolate* isolate) {
  const char* extension_names[kMaxExtensions];
  int extension_count = 0;
  for (int i = 0; i < kMaxExtensions; ++i) {
    if (!extension_flags.contains(static_cast<CcTestExtensionId>(i))) continue;
    extension_names[extension_count] = kExtensionName[i];
    ++extension_count;
  }
  v8::ExtensionConfiguration config(extension_count, extension_names);
  v8::Local<v8::Context> context = v8::Context::New(isolate, &config);
  CHECK(!context.IsEmpty());
  return context;
}

LocalContext::~LocalContext() {
  v8::HandleScope scope(isolate_);
  v8::Local<v8::Context>::New(isolate_, context_)->Exit();
  context_.Reset();
}

void LocalContext::Initialize(v8::Isolate* isolate,
                              v8::ExtensionConfiguration* extensions,
                              v8::Local<v8::ObjectTemplate> global_template,
                              v8::Local<v8::Value> global_object) {
  v8::HandleScope scope(isolate);
  v8::Local<v8::Context> context =
      v8::Context::New(isolate, extensions, global_template, global_object);
  context_.Reset(isolate, context);
  context->Enter();
  // We can't do this later perhaps because of a fatal error.
  isolate_ = isolate;
}

// This indirection is needed because HandleScopes cannot be heap-allocated, and
// we don't want any unnecessary #includes in cctest.h.
class V8_NODISCARD InitializedHandleScopeImpl {
 public:
  explicit InitializedHandleScopeImpl(i::Isolate* isolate)
      : handle_scope_(isolate) {}

 private:
  i::HandleScope handle_scope_;
};

InitializedHandleScope::InitializedHandleScope(i::Isolate* isolate)
    : main_isolate_(isolate ? isolate : CcTest::InitIsolateOnce()),
      initialized_handle_scope_impl_(
          new InitializedHandleScopeImpl(main_isolate_)) {}

InitializedHandleScope::~InitializedHandleScope() = default;

HandleAndZoneScope::HandleAndZoneScope(bool support_zone_compression)
    : main_zone_(
          new i::Zone(&allocator_, ZONE_NAME, support_zone_compression)) {}

HandleAndZoneScope::~HandleAndZoneScope() = default;

i::Handle<i::JSFunction> Optimize(
    i::Handle<i::JSFunction> function, i::Zone* zone, i::Isolate* isolate,
    uint32_t flags, std::unique_ptr<i::compiler::JSHeapBroker>* out_broker) {
  i::Handle<i::SharedFunctionInfo> shared(function->shared(), isolate);
  i::IsCompiledScope is_compiled_scope(shared->is_compiled_scope(isolate));
  CHECK(is_compiled_scope.is_compiled() ||
        i::Compiler::Compile(isolate, function, i::Compiler::CLEAR_EXCEPTION,
                             &is_compiled_scope));

  CHECK_NOT_NULL(zone);

  i::OptimizedCompilationInfo info(zone, isolate, shared, function,
                                   i::CodeKind::TURBOFAN);

  if (flags & ~i::OptimizedCompilationInfo::kInlining) UNIMPLEMENTED();
  if (flags & i::OptimizedCompilationInfo::kInlining) {
    info.set_inlining();
  }

  CHECK(info.shared_info()->HasBytecodeArray());
  i::JSFunction::EnsureFeedbackVector(isolate, function, &is_compiled_scope);

  i::Handle<i::CodeT> code = i::ToCodeT(
      i::compiler::Pipeline::GenerateCodeForTesting(&info, isolate, out_broker)
          .ToHandleChecked(),
      isolate);
  info.native_context().AddOptimizedCode(*code);
  function->set_code(*code, v8::kReleaseStore);
  return function;
}

static void PrintTestList(CcTest* current) {
  if (current == nullptr) return;
  PrintTestList(current->prev());
  printf("%s/%s\n", current->file(), current->name());
}


static void SuggestTestHarness(int tests) {
  if (tests == 0) return;
  printf("Running multiple tests in sequence is deprecated and may cause "
         "bogus failure.  Consider using tools/run-tests.py instead.\n");
}

int main(int argc, char* argv[]) {
#if V8_OS_WIN
  UINT new_flags =
      SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;
  UINT existing_flags = SetErrorMode(new_flags);
  SetErrorMode(existing_flags | new_flags);
#if V8_CC_MSVC
  _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _set_error_mode(_OUT_TO_STDERR);
#endif  // V8_CC_MSVC
#endif  // V8_OS_WIN

  std::string usage = "Usage: " + std::string(argv[0]) + " [--list]" +
                      " [[V8_FLAGS] CCTEST]\n\n" + "Options:\n" +
                      "  --list:   list all cctests\n" +
                      "  CCTEST:   cctest identfier returned by --list\n" +
                      "  V8_FLAGS: see V8 options below\n\n\n";

#ifdef V8_USE_PERFETTO
  // Set up the in-process backend that the tracing controller will connect to.
  perfetto::TracingInitArgs init_args;
  init_args.backends = perfetto::BackendType::kInProcessBackend;
  perfetto::Tracing::Initialize(init_args);
#endif  // V8_USE_PERFETTO

  v8::V8::InitializeICUDefaultLocation(argv[0]);
  std::unique_ptr<v8::Platform> platform(v8::platform::NewDefaultPlatform());
  v8::V8::InitializePlatform(platform.get());
#ifdef V8_SANDBOX
  CHECK(v8::V8::InitializeSandbox());
#endif
  cppgc::InitializeProcess(platform->GetPageAllocator());
  using HelpOptions = v8::internal::FlagList::HelpOptions;
  v8::internal::FlagList::SetFlagsFromCommandLine(
      &argc, argv, true, HelpOptions(HelpOptions::kExit, usage.c_str()));
  v8::V8::Initialize();
  v8::V8::InitializeExternalStartupData(argv[0]);

#if V8_ENABLE_WEBASSEMBLY && V8_TRAP_HANDLER_SUPPORTED
  constexpr bool kUseDefaultTrapHandler = true;
  CHECK(v8::V8::EnableWebAssemblyTrapHandler(kUseDefaultTrapHandler));
#endif  // V8_ENABLE_WEBASSEMBLY && V8_TRAP_HANDLER_SUPPORTED

  CcTest::set_array_buffer_allocator(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());

  v8::RegisterExtension(std::make_unique<i::PrintExtension>());
  v8::RegisterExtension(std::make_unique<i::ProfilerExtension>());
  v8::RegisterExtension(std::make_unique<i::TraceExtension>());

  int tests_run = 0;
  bool print_run_count = true;
  for (int i = 1; i < argc; i++) {
    char* arg = argv[i];
    if (strcmp(arg, "--list") == 0) {
      PrintTestList(CcTest::last());
      print_run_count = false;

    } else {
      char* arg_copy = v8::internal::StrDup(arg);
      char* testname = strchr(arg_copy, '/');
      if (testname) {
        // Split the string in two by nulling the slash and then run
        // exact matches.
        *testname = 0;
        char* file = arg_copy;
        char* name = testname + 1;
        CcTest* test = CcTest::last();
        while (test != nullptr) {
          if (test->enabled()
              && strcmp(test->file(), file) == 0
              && strcmp(test->name(), name) == 0) {
            SuggestTestHarness(tests_run++);
            test->Run();
          }
          test = test->prev();
        }

      } else {
        // Run all tests with the specified file or test name.
        char* file_or_name = arg_copy;
        CcTest* test = CcTest::last();
        while (test != nullptr) {
          if (test->enabled()
              && (strcmp(test->file(), file_or_name) == 0
                  || strcmp(test->name(), file_or_name) == 0)) {
            SuggestTestHarness(tests_run++);
            test->Run();
          }
          test = test->prev();
        }
      }
      v8::internal::DeleteArray<char>(arg_copy);
    }
  }
  if (print_run_count && tests_run != 1) {
    printf("Ran %i tests.\n", tests_run);
  }
  CcTest::TearDown();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return 0;
}

RegisterThreadedTest* RegisterThreadedTest::first_ = nullptr;
int RegisterThreadedTest::count_ = 0;

bool IsValidUnwrapObject(v8::Object* object) {
  i::Address addr = *reinterpret_cast<i::Address*>(object);
  auto instance_type = i::Internals::GetInstanceType(addr);
  return (v8::base::IsInRange(instance_type,
                              i::Internals::kFirstJSApiObjectType,
                              i::Internals::kLastJSApiObjectType) ||
          instance_type == i::Internals::kJSObjectType ||
          instance_type == i::Internals::kJSSpecialApiObjectType);
}

ManualGCScope::ManualGCScope(i::Isolate* isolate)
    : flag_concurrent_marking_(i::FLAG_concurrent_marking),
      flag_concurrent_sweeping_(i::FLAG_concurrent_sweeping),
      flag_stress_concurrent_allocation_(i::FLAG_stress_concurrent_allocation),
      flag_stress_incremental_marking_(i::FLAG_stress_incremental_marking),
      flag_parallel_marking_(i::FLAG_parallel_marking),
      flag_detect_ineffective_gcs_near_heap_limit_(
          i::FLAG_detect_ineffective_gcs_near_heap_limit) {
  // Some tests run threaded (back-to-back) and thus the GC may already be
  // running by the time a ManualGCScope is created. Finalizing existing marking
  // prevents any undefined/unexpected behavior.
  if (isolate && isolate->heap()->incremental_marking()->IsMarking()) {
    CcTest::CollectGarbage(i::OLD_SPACE, isolate);
  }

  i::FLAG_concurrent_marking = false;
  i::FLAG_concurrent_sweeping = false;
  i::FLAG_stress_incremental_marking = false;
  i::FLAG_stress_concurrent_allocation = false;
  // Parallel marking has a dependency on concurrent marking.
  i::FLAG_parallel_marking = false;
  i::FLAG_detect_ineffective_gcs_near_heap_limit = false;
}

ManualGCScope::~ManualGCScope() {
  i::FLAG_concurrent_marking = flag_concurrent_marking_;
  i::FLAG_concurrent_sweeping = flag_concurrent_sweeping_;
  i::FLAG_stress_concurrent_allocation = flag_stress_concurrent_allocation_;
  i::FLAG_stress_incremental_marking = flag_stress_incremental_marking_;
  i::FLAG_parallel_marking = flag_parallel_marking_;
  i::FLAG_detect_ineffective_gcs_near_heap_limit =
      flag_detect_ineffective_gcs_near_heap_limit_;
}

TestPlatform::TestPlatform() : old_platform_(i::V8::GetCurrentPlatform()) {}

void TestPlatform::NotifyPlatformReady() {
  i::V8::SetPlatformForTesting(this);
  CHECK(!active_);
  active_ = true;
}

v8::PageAllocator* TestPlatform::GetPageAllocator() {
  return old_platform()->GetPageAllocator();
}

void TestPlatform::OnCriticalMemoryPressure() {
  old_platform()->OnCriticalMemoryPressure();
}

bool TestPlatform::OnCriticalMemoryPressure(size_t length) {
  return old_platform()->OnCriticalMemoryPressure(length);
}

int TestPlatform::NumberOfWorkerThreads() {
  return old_platform()->NumberOfWorkerThreads();
}

std::shared_ptr<v8::TaskRunner> TestPlatform::GetForegroundTaskRunner(
    v8::Isolate* isolate) {
  return old_platform()->GetForegroundTaskRunner(isolate);
}

void TestPlatform::CallOnWorkerThread(std::unique_ptr<v8::Task> task) {
  old_platform()->CallOnWorkerThread(std::move(task));
}

void TestPlatform::CallDelayedOnWorkerThread(std::unique_ptr<v8::Task> task,
                                             double delay_in_seconds) {
  old_platform()->CallDelayedOnWorkerThread(std::move(task), delay_in_seconds);
}

std::unique_ptr<v8::JobHandle> TestPlatform::PostJob(
    v8::TaskPriority priority, std::unique_ptr<v8::JobTask> job_task) {
  return old_platform()->PostJob(priority, std::move(job_task));
}

double TestPlatform::MonotonicallyIncreasingTime() {
  return old_platform()->MonotonicallyIncreasingTime();
}

double TestPlatform::CurrentClockTimeMillis() {
  return old_platform()->CurrentClockTimeMillis();
}

bool TestPlatform::IdleTasksEnabled(v8::Isolate* isolate) {
  return old_platform()->IdleTasksEnabled(isolate);
}

v8::TracingController* TestPlatform::GetTracingController() {
  return old_platform()->GetTracingController();
}

namespace {

class ShutdownTask final : public v8::Task {
 public:
  ShutdownTask(v8::base::Semaphore* destruction_barrier,
               v8::base::Mutex* destruction_mutex,
               v8::base::ConditionVariable* destruction_condition,
               bool* can_destruct)
      : destruction_barrier_(destruction_barrier),
        destruction_mutex_(destruction_mutex),
        destruction_condition_(destruction_condition),
        can_destruct_(can_destruct)

  {}

  void Run() final {
    destruction_barrier_->Signal();
    {
      v8::base::MutexGuard guard(destruction_mutex_);
      while (!*can_destruct_) {
        destruction_condition_->Wait(destruction_mutex_);
      }
    }
    destruction_barrier_->Signal();
  }

 private:
  v8::base::Semaphore* const destruction_barrier_;
  v8::base::Mutex* const destruction_mutex_;
  v8::base::ConditionVariable* const destruction_condition_;
  bool* const can_destruct_;
};

}  // namespace

void TestPlatform::RemovePlatform() {
  DCHECK_EQ(i::V8::GetCurrentPlatform(), this);
  // Destruction helpers.
  // Barrier to wait until all shutdown tasks actually run (and subsequently
  // block).
  v8::base::Semaphore destruction_barrier{0};
  // Primitives for blocking until `can_destruct` is true.
  v8::base::Mutex destruction_mutex;
  v8::base::ConditionVariable destruction_condition;
  bool can_destruct = false;

  for (int i = 0; i < NumberOfWorkerThreads(); i++) {
    old_platform()->CallOnWorkerThread(
        std::make_unique<ShutdownTask>(&destruction_barrier, &destruction_mutex,
                                       &destruction_condition, &can_destruct));
  }
  // Wait till all worker threads reach the barrier.
  for (int i = 0; i < NumberOfWorkerThreads(); i++) {
    destruction_barrier.Wait();
  }
  // At this point all worker threads are blocked, so the platform can be
  // swapped back.
  i::V8::SetPlatformForTesting(old_platform_);
  CHECK(active_);
  active_ = false;
  // Release all worker threads again.
  {
    v8::base::MutexGuard guard(&destruction_mutex);
    can_destruct = true;
    destruction_condition.NotifyAll();
  }
  // Wait till all worker threads resume. This is necessary as the threads would
  // otherwise try to unlock `destruction_mutex` which may already be gone.
  for (int i = 0; i < NumberOfWorkerThreads(); i++) {
    destruction_barrier.Wait();
  }
}

TestPlatform::~TestPlatform() { CHECK(!active_); }
