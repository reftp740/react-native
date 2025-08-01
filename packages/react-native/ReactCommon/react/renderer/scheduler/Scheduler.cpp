/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Scheduler.h"

#include <glog/logging.h>
#include <jsi/jsi.h>

#include <cxxreact/TraceSection.h>
#include <react/debug/react_native_assert.h>
#include <react/featureflags/ReactNativeFeatureFlags.h>
#include <react/renderer/componentregistry/ComponentDescriptorRegistry.h>
#include <react/renderer/core/EventQueueProcessor.h>
#include <react/renderer/core/LayoutContext.h>
#include <react/renderer/mounting/MountingOverrideDelegate.h>
#include <react/renderer/mounting/ShadowViewMutation.h>
#include <react/renderer/runtimescheduler/RuntimeScheduler.h>
#include <react/renderer/uimanager/UIManager.h>
#include <react/renderer/uimanager/UIManagerBinding.h>

namespace facebook::react {

Scheduler::Scheduler(
    const SchedulerToolbox& schedulerToolbox,
    UIManagerAnimationDelegate* animationDelegate,
    SchedulerDelegate* delegate)
    : runtimeExecutor_(schedulerToolbox.runtimeExecutor),
      contextContainer_(schedulerToolbox.contextContainer) {
  // Creating a container for future `EventDispatcher` instance.
  eventDispatcher_ = std::make_shared<std::optional<const EventDispatcher>>();

  // TODO(T182293888): remove singleton from PerformanceEntryReporter and move
  // creation here.
  auto performanceEntryReporter = PerformanceEntryReporter::getInstance();
  performanceEntryReporter_ = performanceEntryReporter;

  if (ReactNativeFeatureFlags::enableBridgelessArchitecture() &&
      ReactNativeFeatureFlags::cdpInteractionMetricsEnabled()) {
    cdpMetricsReporter_.emplace(CdpMetricsReporter{runtimeExecutor_});
    performanceEntryReporter_->addEventTimingListener(&*cdpMetricsReporter_);
  }

  eventPerformanceLogger_ =
      std::make_shared<EventPerformanceLogger>(performanceEntryReporter_);

  auto uiManager =
      std::make_shared<UIManager>(runtimeExecutor_, contextContainer_);
  auto eventOwnerBox = std::make_shared<EventBeat::OwnerBox>();
  eventOwnerBox->owner = eventDispatcher_;

  auto weakRuntimeScheduler =
      contextContainer_->find<std::weak_ptr<RuntimeScheduler>>(
          RuntimeSchedulerKey);
  react_native_assert(
      weakRuntimeScheduler.has_value() &&
      "Unexpected state: RuntimeScheduler was not provided.");

  runtimeScheduler_ = weakRuntimeScheduler.value().lock().get();

  runtimeScheduler_->setShadowTreeRevisionConsistencyManager(
      uiManager->getShadowTreeRevisionConsistencyManager());

  runtimeScheduler_->setEventTimingDelegate(eventPerformanceLogger_.get());

  auto eventPipe = [uiManager](
                       jsi::Runtime& runtime,
                       const EventTarget* eventTarget,
                       const std::string& type,
                       ReactEventPriority priority,
                       const EventPayload& payload) {
    uiManager->visitBinding(
        [&](const UIManagerBinding& uiManagerBinding) {
          uiManagerBinding.dispatchEvent(
              runtime, eventTarget, type, priority, payload);
        },
        runtime);
  };

  auto eventPipeConclusion = [runtimeScheduler =
                                  runtimeScheduler_](jsi::Runtime& runtime) {
    runtimeScheduler->callExpiredTasks(runtime);
  };

  auto statePipe = [uiManager](const StateUpdate& stateUpdate) {
    uiManager->updateState(stateUpdate);
  };

  auto eventBeat = schedulerToolbox.eventBeatFactory(std::move(eventOwnerBox));

  // Creating an `EventDispatcher` instance inside the already allocated
  // container (inside the optional).
  eventDispatcher_->emplace(
      EventQueueProcessor(
          eventPipe, eventPipeConclusion, statePipe, eventPerformanceLogger_),
      std::move(eventBeat),
      statePipe,
      eventPerformanceLogger_);

  // Casting to `std::shared_ptr<EventDispatcher const>`.
  auto eventDispatcher =
      EventDispatcher::Shared{eventDispatcher_, &eventDispatcher_->value()};

  componentDescriptorRegistry_ = schedulerToolbox.componentRegistryFactory(
      eventDispatcher, contextContainer_);

  uiManager->setDelegate(this);
  uiManager->setComponentDescriptorRegistry(componentDescriptorRegistry_);

  auto bindingsExecutor =
      schedulerToolbox.bridgelessBindingsExecutor.has_value()
      ? schedulerToolbox.bridgelessBindingsExecutor.value()
      : runtimeExecutor_;
  bindingsExecutor([uiManager](jsi::Runtime& runtime) {
    UIManagerBinding::createAndInstallIfNeeded(runtime, uiManager);
  });

  auto componentDescriptorRegistryKey =
      "ComponentDescriptorRegistry_DO_NOT_USE_PRETTY_PLEASE";
  contextContainer_->erase(componentDescriptorRegistryKey);
  contextContainer_->insert(
      componentDescriptorRegistryKey,
      std::weak_ptr<const ComponentDescriptorRegistry>(
          componentDescriptorRegistry_));

  delegate_ = delegate;
  commitHooks_ = schedulerToolbox.commitHooks;
  uiManager_ = uiManager;

  for (auto& commitHook : commitHooks_) {
    uiManager->registerCommitHook(*commitHook);
  }

  if (animationDelegate != nullptr) {
    animationDelegate->setComponentDescriptorRegistry(
        componentDescriptorRegistry_);
  }
  uiManager_->setAnimationDelegate(animationDelegate);

  uiManager->registerMountHook(*eventPerformanceLogger_);
}

Scheduler::~Scheduler() {
  LOG(WARNING) << "Scheduler::~Scheduler() was called (address: " << this
               << ").";

  auto weakRuntimeScheduler =
      contextContainer_->find<std::weak_ptr<RuntimeScheduler>>(
          RuntimeSchedulerKey);
  auto runtimeScheduler = weakRuntimeScheduler.has_value()
      ? weakRuntimeScheduler.value().lock()
      : nullptr;

  if (runtimeScheduler) {
    runtimeScheduler->setEventTimingDelegate(nullptr);
  }

  for (auto& commitHook : commitHooks_) {
    uiManager_->unregisterCommitHook(*commitHook);
  }

  // All Surfaces must be explicitly stopped before destroying `Scheduler`.
  // The idea is that `UIManager` is allowed to call `Scheduler` only if the
  // corresponding `ShadowTree` instance exists.

  // The thread-safety of this operation is guaranteed by this requirement.
  uiManager_->setDelegate(nullptr);
  uiManager_->setAnimationDelegate(nullptr);

  if (cdpMetricsReporter_) {
    performanceEntryReporter_->removeEventTimingListener(&*cdpMetricsReporter_);
  }

  // Then, let's verify that the requirement was satisfied.
  auto surfaceIds = std::vector<SurfaceId>{};
  uiManager_->getShadowTreeRegistry().enumerate(
      [&surfaceIds](const ShadowTree& shadowTree, bool&) {
        surfaceIds.push_back(shadowTree.getSurfaceId());
      });

  react_native_assert(
      surfaceIds.empty() &&
      "Scheduler was destroyed with outstanding Surfaces.");

  if (surfaceIds.empty()) {
    return;
  }

  LOG(ERROR) << "Scheduler was destroyed with outstanding Surfaces.";

  // If we are here, that means assert didn't fire which indicates that we in
  // production.

  // Now we have still-running surfaces, which is no good, no good.
  // That's indeed a sign of a severe issue on the application layer.
  // At this point, we don't have much to lose, so we are trying to unmount all
  // outstanding `ShadowTree`s to prevent all stored JSI entities from
  // overliving the `Scheduler`. (Unmounting `ShadowNode`s disables
  // `EventEmitter`s which destroys JSI objects.)
  for (auto surfaceId : surfaceIds) {
    uiManager_->getShadowTreeRegistry().visit(
        surfaceId,
        [](const ShadowTree& shadowTree) { shadowTree.commitEmptyTree(); });

    // Removing surfaces acquires mutex waiting for commits in flight; in
    // theory, it can deadlock.
    uiManager_->getShadowTreeRegistry().remove(surfaceId);
  }
}

void Scheduler::registerSurface(
    const SurfaceHandler& surfaceHandler) const noexcept {
  surfaceHandler.setContextContainer(getContextContainer());
  surfaceHandler.setUIManager(uiManager_.get());
}

void Scheduler::unregisterSurface(
    const SurfaceHandler& surfaceHandler) const noexcept {
  surfaceHandler.setUIManager(nullptr);
}

const ComponentDescriptor*
Scheduler::findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN(
    ComponentHandle handle) const {
  return componentDescriptorRegistry_
      ->findComponentDescriptorByHandle_DO_NOT_USE_THIS_IS_BROKEN(handle);
}

#pragma mark - Delegate

void Scheduler::setDelegate(SchedulerDelegate* delegate) {
  delegate_ = delegate;
}

SchedulerDelegate* Scheduler::getDelegate() const {
  return delegate_;
}

#pragma mark - UIManagerAnimationDelegate

void Scheduler::animationTick() const {
  uiManager_->animationTick();
}

#pragma mark - UIManagerDelegate

void Scheduler::uiManagerDidFinishTransaction(
    std::shared_ptr<const MountingCoordinator> mountingCoordinator,
    bool mountSynchronously) {
  TraceSection s("Scheduler::uiManagerDidFinishTransaction");

  if (delegate_ != nullptr) {
    // This is no-op on all platforms except for Android where we need to
    // observe each transaction to be able to mount correctly.
    delegate_->schedulerDidFinishTransaction(mountingCoordinator);

    if (!mountSynchronously) {
      auto surfaceId = mountingCoordinator->getSurfaceId();

      runtimeScheduler_->scheduleRenderingUpdate(
          surfaceId,
          [delegate = delegate_,
           mountingCoordinator = std::move(mountingCoordinator)]() {
            delegate->schedulerShouldRenderTransactions(mountingCoordinator);
          });
    } else {
      delegate_->schedulerShouldRenderTransactions(mountingCoordinator);
    }
  }
}

void Scheduler::uiManagerDidCreateShadowNode(const ShadowNode& shadowNode) {
  if (delegate_ != nullptr) {
    delegate_->schedulerDidRequestPreliminaryViewAllocation(shadowNode);
  }
}

void Scheduler::uiManagerDidDispatchCommand(
    const std::shared_ptr<const ShadowNode>& shadowNode,
    const std::string& commandName,
    const folly::dynamic& args) {
  TraceSection s("Scheduler::uiManagerDispatchCommand");

  if (delegate_ != nullptr) {
    auto shadowView = ShadowView(*shadowNode);
    runtimeScheduler_->scheduleRenderingUpdate(
        shadowNode->getSurfaceId(),
        [delegate = delegate_,
         shadowView = std::move(shadowView),
         commandName,
         args]() {
          delegate->schedulerDidDispatchCommand(shadowView, commandName, args);
        });
  }
}

void Scheduler::uiManagerDidSendAccessibilityEvent(
    const std::shared_ptr<const ShadowNode>& shadowNode,
    const std::string& eventType) {
  TraceSection s("Scheduler::uiManagerDidSendAccessibilityEvent");

  if (delegate_ != nullptr) {
    auto shadowView = ShadowView(*shadowNode);
    delegate_->schedulerDidSendAccessibilityEvent(shadowView, eventType);
  }
}

/*
 * Set JS responder for a view.
 */
void Scheduler::uiManagerDidSetIsJSResponder(
    const std::shared_ptr<const ShadowNode>& shadowNode,
    bool isJSResponder,
    bool blockNativeResponder) {
  if (delegate_ != nullptr) {
    delegate_->schedulerDidSetIsJSResponder(
        ShadowView(*shadowNode), isJSResponder, blockNativeResponder);
  }
}

void Scheduler::uiManagerShouldSynchronouslyUpdateViewOnUIThread(
    Tag tag,
    const folly::dynamic& props) {
  if (delegate_ != nullptr) {
    delegate_->schedulerShouldSynchronouslyUpdateViewOnUIThread(tag, props);
  }
}

void Scheduler::uiManagerDidUpdateShadowTree(
    const std::unordered_map<Tag, folly::dynamic>& tagToProps) {
  if (delegate_ != nullptr) {
    delegate_->schedulerDidUpdateShadowTree(tagToProps);
  }
}

void Scheduler::uiManagerShouldAddEventListener(
    std::shared_ptr<const EventListener> listener) {
  addEventListener(listener);
}

void Scheduler::uiManagerShouldRemoveEventListener(
    const std::shared_ptr<const EventListener>& listener) {
  removeEventListener(listener);
}

void Scheduler::uiManagerDidStartSurface(const ShadowTree& shadowTree) {
  std::shared_lock lock(onSurfaceStartCallbackMutex_);
  if (onSurfaceStartCallback_) {
    onSurfaceStartCallback_(shadowTree);
  }
}

void Scheduler::reportMount(SurfaceId surfaceId) const {
  uiManager_->reportMount(surfaceId);
}

std::shared_ptr<const ContextContainer> Scheduler::getContextContainer() const {
  return contextContainer_;
}

std::shared_ptr<UIManager> Scheduler::getUIManager() const {
  return uiManager_;
}

void Scheduler::addEventListener(
    std::shared_ptr<const EventListener> listener) {
  if (eventDispatcher_->has_value()) {
    eventDispatcher_->value().addListener(std::move(listener));
  }
}

void Scheduler::removeEventListener(
    const std::shared_ptr<const EventListener>& listener) {
  if (eventDispatcher_->has_value()) {
    eventDispatcher_->value().removeListener(listener);
  }
}

void Scheduler::uiManagerShouldSetOnSurfaceStartCallback(
    OnSurfaceStartCallback&& callback) {
  std::shared_lock lock(onSurfaceStartCallbackMutex_);
  onSurfaceStartCallback_ = std::move(callback);
}

} // namespace facebook::react
