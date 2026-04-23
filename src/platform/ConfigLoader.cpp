#include "ConfigLoader.h"

#include "PlatformDawn.h"

ConfigLoader::~ConfigLoader()
{
    stop();
}

void ConfigLoader::stop()
{
    EventLoop* el = platform_ ? platform_->eventLoop_.get() : nullptr;
    if (!el) return;
    if (debounceActive_) {
        el->removeTimer(debounceTimer_);
        debounceActive_ = false;
    }
}

void ConfigLoader::installFileWatch(const std::string& path)
{
    EventLoop* el = platform_ ? platform_->eventLoop_.get() : nullptr;
    if (path.empty() || !el) return;
    el->addFileWatch(path, [this, el]() {
        if (debounceActive_) {
            el->removeTimer(debounceTimer_);
            debounceActive_ = false;
        }
        debounceTimer_ = el->addTimer(300, false, [this]() {
            debounceActive_ = false;
            reloadNow();
        });
        debounceActive_ = true;
    });
}

void ConfigLoader::reloadNow()
{
    if (!platform_) return;
    Config config = loadConfig();
    platform_->applyConfig(config);
}
