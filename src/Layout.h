#pragma once

// The Layout class was dissolved in the tree-cutover refactor. Its state
// migrated to Script::Engine (focus, zoom, paneId index, global layout
// params — divider / tab bar / framebuffer dims) and its methods moved onto
// the Tab handle (Tab.h / Tab.cpp). The subtree-root + tree shape is the
// single source of structural truth.
//
// This header survives only to alias `Layout` to `Tab` so the many legacy
// `Layout*` / `tab->layout()` call sites keep compiling. `Tab::layout()`
// returns `Tab*` (self), which means `tab.layout()->method()` is just a
// no-op hop that resolves to `tab.method()`. A cleanup pass can drop
// these indirections later. `LayoutNode::Dir` lives in Tab.h now.

#include "Tab.h"

using Layout = Tab;
