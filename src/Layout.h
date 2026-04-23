#pragma once

// Legacy alias. All structural state lives in the shared LayoutTree;
// Script::Engine owns the Terminal map, focus, and global layout params.
// Tab is a thin 2-pointer handle. This alias keeps pre-cutover call sites
// (`Layout*` / `tab->layout()`) compiling while callers migrate.
// `LayoutNode::Dir` lives in Tab.h.

#include "Tab.h"

using Layout = Tab;
