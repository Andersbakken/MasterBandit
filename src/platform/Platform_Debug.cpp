#include "PlatformDawn.h"
#include "Utf8.h"
#include "Utils.h"
#include "Observability.h"
#include <glaze/glaze.hpp>

static void appendUtf8(std::string& s, uint32_t cp) { utf8::append(s, cp); }
static std::string codepointToUtf8(uint32_t cp) { return utf8::encode(cp); }

static std::string sanitizeUtf8(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    const char* p = in.data();
    const char* end = p + in.size();
    while (p < end) {
        char32_t cp = utf8::decodeAdvance(p, end);
        char buf[4];
        int n = utf8::encode(cp, buf);
        out.append(buf, n);
    }
    return out;
}

std::string PlatformDawn::gridToJson(Uuid id)
{
    Terminal* pane = scriptEngine_.terminal(id);
    if (!pane) return {};
    TerminalEmulator* term = pane;

    const IGrid& g = term->grid();

    glz::generic::object_t resp;
    resp["type"] = "screenshot";
    resp["format"] = "grid";
    if (!id.isNil()) resp["id"] = id.toString();
    resp["cols"] = static_cast<double>(term->width());
    resp["rows"] = static_cast<double>(term->height());
    resp["cursor"] = glz::generic::object_t{
        {"x", static_cast<double>(term->cursorX())},
        {"y", static_cast<double>(term->cursorY())}
    };

    glz::generic::array_t lines;
    for (int row = 0; row < g.rows(); ++row) {
        std::string text;
        for (int col = 0; col < g.cols(); ++col) {
            const Cell& c = g.cell(col, row);
            if (c.wc == 0) {
                text += ' ';
            } else {
                appendUtf8(text, c.wc);
            }
        }
        // Trim trailing spaces
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (!text.empty()) {
            glz::generic::object_t line;
            line["y"] = static_cast<double>(row);
            line["text"] = sanitizeUtf8(text);
            lines.emplace_back(std::move(line));
        }
    }
    resp["lines"] = std::move(lines);

    std::string buf;
    (void)glz::write_json(resp, buf);
    return buf;
}


std::string PlatformDawn::statsJson(int id)
{
    auto texStats     = renderEngine_->texturePool().stats();
    auto computeStats = renderEngine_->renderer().computePool().stats();

    glz::generic::object_t resp;
    resp["type"] = "stats";
    if (id) resp["id"] = static_cast<double>(id);

    auto toKB = [](size_t b) { return static_cast<double>(b) / 1024.0; };

    resp["texture_pool"] = glz::generic::object_t{
        {"total",       static_cast<double>(texStats.total)},
        {"in_use",      static_cast<double>(texStats.inUse)},
        {"free",        static_cast<double>(texStats.free)},
        {"total_kb",    toKB(texStats.totalBytes)},
        {"free_kb",     toKB(texStats.freeBytes)},
        {"limit_kb",    toKB(texStats.limitBytes)},
    };
    resp["compute_pool"] = glz::generic::object_t{
        {"total",       static_cast<double>(computeStats.total)},
        {"in_use",      static_cast<double>(computeStats.inUse)},
        {"free",        static_cast<double>(computeStats.free)},
        {"total_kb",    toKB(computeStats.totalBytes)},
        {"free_kb",     toKB(computeStats.freeBytes)},
        {"limit_kb",    toKB(computeStats.limitBytes)},
    };

    resp["obs"] = glz::generic::object_t{
        {"bytes_parsed",         static_cast<double>(obs::bytes_parsed.load(std::memory_order_relaxed))},
        {"frames_presented",     static_cast<double>(obs::frames_presented.load(std::memory_order_relaxed))},
        {"last_parse_time_us",   static_cast<double>(obs::last_parse_time_us.load(std::memory_order_relaxed))},
        {"frames_at_last_parse", static_cast<double>(obs::frames_at_last_parse.load(std::memory_order_relaxed))},
        {"now_us",               static_cast<double>(obs::now_us())},
    };

    glz::generic::array_t tabsArr;
    auto allTabs = scriptEngine_.tabSubtreeRoots();
    int activeIdx = scriptEngine_.activeTabIndex();
    for (int ti = 0; ti < static_cast<int>(allTabs.size()); ++ti) {
        Uuid sub = allTabs[ti];
        glz::generic::array_t panesArr;
        for (Terminal* panePtr : scriptEngine_.panesInSubtree(sub)) {
            Uuid pid = panePtr->nodeId();
            const PaneRenderPrivate* rs = renderEngine_->paneRenderPrivate(pid);
            Terminal* term = panePtr;
            glz::generic::object_t paneObj;
            paneObj["id"]   = pid.toString();
            paneObj["cols"] = static_cast<double>(term ? term->width()  : 0);
            paneObj["rows"] = static_cast<double>(term ? term->height() : 0);
            paneObj["cwd"]  = panePtr->cwd();
            if (rs) {
                paneObj["held_texture"] = rs->heldTexture != nullptr;
                paneObj["texture_kb"]   = rs->heldTexture
                    ? toKB(rs->heldTexture->sizeBytes) : 0.0;
                paneObj["has_divider"]  = rs->dividerVB != nullptr;
            }
            panesArr.emplace_back(std::move(paneObj));
        }
        glz::generic::object_t tabObj;
        tabObj["index"]  = static_cast<double>(ti);
        tabObj["active"] = (ti == activeIdx);
        tabObj["panes"]  = std::move(panesArr);
        // Tree-node UUID for the tab's subtree root (always a Stack). Tests
        // use this to assert tree shape end-to-end via `mb --test`.
        if (!sub.isNil()) tabObj["nodeId"] = sub.toString();
        tabsArr.emplace_back(std::move(tabObj));
    }
    resp["tabs"] = std::move(tabsArr);

    std::string buf;
    (void)glz::write_json(resp, buf);
    return buf;
}

