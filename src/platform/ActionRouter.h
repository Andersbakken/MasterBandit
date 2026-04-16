#pragma once

#include "Action.h"

#include <functional>

// Typed entry point for dispatching Actions. Owns the Action::Dispatcher
// observer registry (the JS-facing addListener / removeListener API) and the
// sequencing around a dispatch: execute the action, notify listeners, flush
// script-engine microtasks.
//
// The actual action-handling std::visit lives inside PlatformDawn::
// executeAction — every branch reaches into tab/pane/layout/renderState_/
// pending_/inputController_ which are all PlatformDawn-owned, so moving the
// visitor body out would produce a Host with ~25 callbacks wrapping the
// same coupling. ActionRouter therefore delegates via Host::executeAction.
// The extraction's value is consolidating the observer + microtask flush
// around each dispatch and removing the Action::Dispatcher member from
// PlatformDawn's header.
class ActionRouter {
public:
    struct Host {
        // Runs the std::visit over Action::Any, mutating PlatformDawn state.
        std::function<void(const Action::Any&)> executeAction;
        // Called after executeAction + notify — flushes JS microtasks so
        // action listeners update state before the next render.
        std::function<void()> flushScriptJobs;
    };

    ActionRouter() = default;
    ~ActionRouter() = default;

    ActionRouter(const ActionRouter&) = delete;
    ActionRouter& operator=(const ActionRouter&) = delete;

    void setHost(Host host) { host_ = std::move(host); }

    void dispatch(const Action::Any& action);

    Action::Dispatcher& listeners() { return listeners_; }
    const Action::Dispatcher& listeners() const { return listeners_; }

private:
    Host host_;
    Action::Dispatcher listeners_;
};
