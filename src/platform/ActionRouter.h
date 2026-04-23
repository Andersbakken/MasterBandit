#pragma once

#include "Action.h"

class PlatformDawn;

// Typed entry point for dispatching Actions. Owns the Action::Dispatcher
// observer registry (the JS-facing addListener / removeListener API) and the
// sequencing around a dispatch: execute the action, notify listeners, flush
// script-engine microtasks.
class ActionRouter {
public:
    ActionRouter() = default;
    ~ActionRouter() = default;

    ActionRouter(const ActionRouter&) = delete;
    ActionRouter& operator=(const ActionRouter&) = delete;

    void setPlatform(PlatformDawn* p) { platform_ = p; }

    void dispatch(const Action::Any& action);

    Action::Dispatcher& listeners() { return listeners_; }
    const Action::Dispatcher& listeners() const { return listeners_; }

private:
    PlatformDawn* platform_ = nullptr;
    Action::Dispatcher listeners_;
};
