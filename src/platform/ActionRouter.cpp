#include "ActionRouter.h"

#include "PlatformDawn.h"
#include "ScriptEngine.h"

void ActionRouter::dispatch(const Action::Any& action)
{
    platform_->executeAction(action);
    listeners_.notify(action.index(), action);
    platform_->scriptEngine_.executePendingJobs();
}
