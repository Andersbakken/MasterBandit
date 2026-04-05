#include "Action.h"

namespace Action {

ListenerId Dispatcher::addListener(TypeIndex type, Listener fn)
{
    ListenerId id = nextId_++;
    listeners_.push_back({id, type, false, std::move(fn)});
    return id;
}

ListenerId Dispatcher::addListener(Listener fn)
{
    ListenerId id = nextId_++;
    listeners_.push_back({id, {}, true, std::move(fn)});
    return id;
}

void Dispatcher::removeListener(ListenerId id)
{
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
        if (it->id == id) {
            listeners_.erase(it);
            return;
        }
    }
}

void Dispatcher::notify(TypeIndex type, const Any& action) const
{
    for (const auto& entry : listeners_) {
        if (entry.allTypes || entry.type == type) {
            entry.fn(type, action);
        }
    }
}

} // namespace Action
