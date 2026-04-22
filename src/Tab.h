#pragma once

#include "Uuid.h"

#include <memory>
#include <string>
#include <vector>

class Layout;
class Terminal;
namespace Script { class Engine; }

// Non-owning handle for a tab. A tab identity is its subtreeRoot Uuid in
// the shared LayoutTree (a direct child of Engine::layoutRootStack_). All
// per-tab state — Layout, title, icon, overlays — lives on Script::Engine
// (or on the tree node's `label`). Tab is a lightweight value type:
// default-constructible (invalid), copyable, movable. The underlying state
// is stable for as long as the Engine's tab maps hold an entry for
// subtreeRoot; once closeTab removes it, any existing Tab handles for that
// subtreeRoot start returning null/empty.
class Tab {
public:
    Tab() = default;
    Tab(Script::Engine* eng, Uuid subtreeRoot) : eng_(eng), subtreeRoot_(subtreeRoot) {}

    bool valid() const { return eng_ && !subtreeRoot_.isNil(); }
    explicit operator bool() const { return valid(); }

    Uuid subtreeRoot() const { return subtreeRoot_; }
    Script::Engine* engine() const { return eng_; }

    Layout* layout() const;

    // Title lives on the tree node's label. An invalid handle or a missing
    // node returns an empty string reference.
    const std::string& title() const;
    void setTitle(const std::string& s);

    // Icon lives in Engine::tabIcons_ keyed by subtreeRoot.
    const std::string& icon() const;
    void setIcon(const std::string& s);

    // Overlay stack lives in Engine::tabOverlays_ keyed by subtreeRoot.
    bool hasOverlay() const;
    Terminal* topOverlay() const;
    Terminal* activeOverlay() const { return topOverlay(); }
    void pushOverlay(std::unique_ptr<Terminal> t);
    std::unique_ptr<Terminal> popOverlay();

private:
    Script::Engine* eng_ = nullptr;
    Uuid subtreeRoot_;
};
