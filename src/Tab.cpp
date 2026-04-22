#include "Tab.h"

#include "Layout.h"
#include "LayoutTree.h"
#include "Terminal.h"
#include "script/ScriptEngine.h"

namespace {
const std::string& kEmpty() {
    static const std::string s;
    return s;
}
} // namespace

Layout* Tab::layout() const
{
    if (!valid()) return nullptr;
    return eng_->tabLayout(subtreeRoot_);
}

const std::string& Tab::title() const
{
    if (!valid()) return kEmpty();
    const Node* n = eng_->layoutTree().node(subtreeRoot_);
    return n ? n->label : kEmpty();
}

void Tab::setTitle(const std::string& s)
{
    if (!valid()) return;
    eng_->layoutTree().setLabel(subtreeRoot_, s);
}

const std::string& Tab::icon() const
{
    if (!valid()) return kEmpty();
    return eng_->tabIcon(subtreeRoot_);
}

void Tab::setIcon(const std::string& s)
{
    if (!valid()) return;
    eng_->setTabIcon(subtreeRoot_, s);
}

bool Tab::hasOverlay() const
{
    if (!valid()) return false;
    return eng_->hasTabOverlay(subtreeRoot_);
}

Terminal* Tab::topOverlay() const
{
    if (!valid()) return nullptr;
    return eng_->topTabOverlay(subtreeRoot_);
}

void Tab::pushOverlay(std::unique_ptr<Terminal> t)
{
    if (!valid()) return;
    eng_->pushTabOverlay(subtreeRoot_, std::move(t));
}

std::unique_ptr<Terminal> Tab::popOverlay()
{
    if (!valid()) return nullptr;
    return eng_->popTabOverlay(subtreeRoot_);
}
