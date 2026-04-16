#include "ActionRouter.h"

void ActionRouter::dispatch(const Action::Any& action)
{
    if (host_.executeAction) host_.executeAction(action);
    listeners_.notify(action.index(), action);
    if (host_.flushScriptJobs) host_.flushScriptJobs();
}
