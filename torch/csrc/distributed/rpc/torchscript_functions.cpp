#include <ATen/ThreadLocalState.h>
#include <fmt/format.h>
#include <torch/csrc/autograd/record_function_ops.h>
#include <torch/csrc/distributed/autograd/utils.h>
#include <torch/csrc/distributed/rpc/message.h>
#include <torch/csrc/distributed/rpc/profiler/remote_profiler_manager.h>
#include <torch/csrc/distributed/rpc/rpc_agent.h>
#include <torch/csrc/distributed/rpc/rref_proto.h>
#include <torch/csrc/distributed/rpc/script_call.h>
#include <torch/csrc/distributed/rpc/torchscript_functions.h>
#include <torch/csrc/distributed/rpc/utils.h>

namespace torch {
namespace distributed {
namespace rpc {

c10::intrusive_ptr<JitFuture> rpcTorchscript(
    const std::string& dstWorkerName,
    const c10::QualifiedName& qualifiedName,
    const c10::FunctionSchema& functionSchema,
    std::vector<c10::IValue>& stack,
    const float rpcTimeoutSeconds,
    const bool isAsyncExecution) {
  // This dummy tensor holds an at::RecordFunction when profiling is enabled.
  // This is because at::RecordFunction is not yet registered as a TorchScript
  // custom class (https://github.com/pytorch/pytorch/issues/35026)
  at::Tensor handle = at::zeros(1);
  auto shouldProfile = torch::autograd::profiler::profilerEnabled() &&
      !torch::distributed::rpc::RemoteProfilerManager::getInstance()
           .isCurrentKeySet();
  if (shouldProfile) {
    auto rpcAsyncJitKey = fmt::format(
        "rpc_async_jit#{}({} -> {})",
        qualifiedName
            .qualifiedName(), /* name of torchscript function being run */
        RpcAgent::getCurrentRpcAgent()->getWorkerInfo().name_,
        dstWorkerName);
    handle = torch::autograd::profiler::record_function_enter(rpcAsyncJitKey);
    auto& remoteProfilerManager =
        torch::distributed::rpc::RemoteProfilerManager::getInstance();
    remoteProfilerManager.setCurrentKey(rpcAsyncJitKey);
  }
  auto scriptCall = std::make_unique<ScriptCall>(
      qualifiedName, std::move(stack), isAsyncExecution);
  auto rpcAgentPtr = RpcAgent::getCurrentRpcAgent();
  auto jitFuture = autograd::sendMessageWithAutograd(
      *rpcAgentPtr,
      rpcAgentPtr->getWorkerInfo(dstWorkerName),
      std::move(*scriptCall).toMessage(),
      true /*forceGradRecording*/,
      rpcTimeoutSeconds);

  // Get function return type to construct JitFuture.
  auto returns = functionSchema.returns();
  // Script call only allows single IValue returned.
  TORCH_INTERNAL_ASSERT(
      returns.size() == 1,
      "Return value of an annotated torchScript function should be a single "
      "IValue.",
      returns.size());
  auto returnType = returns.at(0).type();

  // Create a JIT future and pass it to futMessage's callback to set state
  // of the JIT future.
  auto futPtr = jitFuture->createInstance(returnType);
  std::weak_ptr<JitFuture> wp = jitFuture;
  jitFuture->addCallback(at::wrapPropagateTLSState<void>([futPtr, wp]() {
    auto future = wp.lock();
    if (future->hasError()) {
      futPtr->setError(future->exception_ptr());
    } else {
      futPtr->markCompleted(
          deserializeRespToIValue(
              *future->constValue().toCustomClass<Message>()),
          future->dataPtrs());
    }
  }));
  if (shouldProfile) {
    auto profiledFutPtr =
        torch::autograd::profiler::_call_end_callbacks_on_fut(handle, futPtr);
    return profiledFutPtr;
  }
  return futPtr;
}

c10::intrusive_ptr<RRef> remoteTorchscript(
    const std::string& dstWorkerName,
    const c10::QualifiedName& qualifiedName,
    const c10::FunctionSchema& functionSchema,
    std::vector<c10::IValue>& stack,
    const float rpcTimeoutSeconds,
    const bool isAsyncExecution) {
  auto rpcAgentPtr = RpcAgent::getCurrentRpcAgent();
  auto dstWorkerInfo = rpcAgentPtr->getWorkerInfo(dstWorkerName);
  auto& ctx = RRefContext::getInstance();

  // Get function return type to construct UserRRef.
  auto returns = functionSchema.returns();
  // Script call only allows single IValue returned.
  TORCH_INTERNAL_ASSERT(
      returns.size() == 1,
      "Return value of an annotated torchScript function should be a single "
      "IValue.",
      returns.size());
  auto returnType = returns.at(0).type();

  if (ctx.getWorkerId() != dstWorkerInfo.id_) {
    auto userRRefPtr = ctx.createUserRRef(dstWorkerInfo.id_, returnType);

    auto scriptRemoteCall = std::make_unique<ScriptRemoteCall>(
        qualifiedName,
        std::move(stack),
        userRRefPtr->rrefId(),
        userRRefPtr->forkId(),
        isAsyncExecution);

    auto jitFuture = torch::distributed::autograd::sendMessageWithAutograd(
        *rpcAgentPtr,
        dstWorkerInfo,
        std::move(*scriptRemoteCall).toMessage(),
        true /*forceGradRecording*/,
        rpcTimeoutSeconds /* timeout */);

    userRRefPtr->registerOwnerCreationFuture(jitFuture);
    ctx.addPendingUser(userRRefPtr->forkId(), userRRefPtr);
    std::weak_ptr<JitFuture> wp = jitFuture;
    jitFuture->addCallback(
        at::wrapPropagateTLSState<void>([wp, forkId{userRRefPtr->forkId()}]() {
          callback::confirmPendingUser(*wp.lock(), forkId);
        }));

    return userRRefPtr;
  } else {
    auto ownerRRefPtr = ctx.createOwnerRRef(returnType);
    // prevent this owner RRef from being deleted due to other forks
    ctx.addSelfAsFork(ownerRRefPtr);

    auto scriptRemoteCall = std::make_unique<ScriptRemoteCall>(
        qualifiedName,
        std::move(stack),
        ownerRRefPtr->rrefId(),
        ownerRRefPtr->rrefId(),
        isAsyncExecution);

    auto jitFuture = torch::distributed::autograd::sendMessageWithAutograd(
        *rpcAgentPtr,
        dstWorkerInfo,
        std::move(*scriptRemoteCall).toMessage(),
        true /*forceGradRecording*/,
        rpcTimeoutSeconds /* timeout */);

    ownerRRefPtr->registerOwnerCreationFuture(jitFuture);
    std::weak_ptr<JitFuture> wp = jitFuture;
    jitFuture->addCallback(at::wrapPropagateTLSState<void>(
        [wp, ownerRRefId = ownerRRefPtr->rrefId()]() {
          callback::finishCreatingOwnerRRef(*wp.lock(), ownerRRefId);
        }));
    return ownerRRefPtr;
  }
}

} // namespace rpc
} // namespace distributed
} // namespace torch
