#include "ConfigLoader.h"

ConfigLoader::~ConfigLoader()
{
    stop();
}

void ConfigLoader::stop()
{
    if (!host_.eventLoop) return;
    if (debounceActive_) {
        host_.eventLoop->removeTimer(debounceTimer_);
        debounceActive_ = false;
    }
}

void ConfigLoader::installFileWatch(const std::string& path)
{
    if (path.empty() || !host_.eventLoop) return;
    host_.eventLoop->addFileWatch(path, [this]() {
        if (debounceActive_) {
            host_.eventLoop->removeTimer(debounceTimer_);
            debounceActive_ = false;
        }
        debounceTimer_ = host_.eventLoop->addTimer(300, false, [this]() {
            debounceActive_ = false;
            reloadNow();
        });
        debounceActive_ = true;
    });
}

void ConfigLoader::reloadNow()
{
    if (!host_.applyConfig) return;
    Config config = loadConfig();
    host_.applyConfig(config);
}
