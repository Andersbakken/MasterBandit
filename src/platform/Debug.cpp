#include "PlatformDawn.h"
#include "Log.h"
#include "Utils.h"
#include <glaze/glaze.hpp>

static void appendUtf8(std::string& s, uint32_t cp)
{
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

static std::string codepointToUtf8(uint32_t cp)
{
    std::string s;
    appendUtf8(s, cp);
    return s;
}

static std::string sanitizeUtf8(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const auto* end = p + in.size();
    while (p < end) {
        if (*p < 0x80) {
            out += static_cast<char>(*p++);
        } else if ((*p & 0xE0) == 0xC0 && p + 1 < end && (p[1] & 0xC0) == 0x80) {
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
        } else if ((*p & 0xF0) == 0xE0 && p + 2 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
        } else if ((*p & 0xF8) == 0xF0 && p + 3 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
            out += static_cast<char>(*p++);
        } else {
            out += "\xEF\xBF\xBD"; // U+FFFD
            ++p;
        }
    }
    return out;
}

std::string PlatformDawn::gridToJson(int id)
{
    // Search for the pane across all tabs
    Pane* pane = nullptr;
    for (auto& tabPtr : tabs_) {
        pane = tabPtr->layout()->pane(id);
        if (pane) break;
    }
    if (!pane) return {};
    TerminalEmulator* term = pane->activeTerm();
    if (!term) return {};

    const IGrid& g = term->grid();

    glz::generic::object_t resp;
    resp["type"] = "screenshot";
    resp["format"] = "grid";
    if (id) resp["id"] = static_cast<double>(id);
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
    auto texStats     = texturePool_.stats();
    auto computeStats = renderer_.computePool().stats();

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

    glz::generic::array_t tabsArr;
    for (int ti = 0; ti < static_cast<int>(tabs_.size()); ++ti) {
        Tab* tab = tabs_[ti].get();
        glz::generic::array_t panesArr;
        for (auto& panePtr : tab->layout()->panes()) {
            int pid = panePtr->id();
            auto it = paneRenderStates_.find(pid);
            TerminalEmulator* term = panePtr->activeTerm();
            glz::generic::object_t paneObj;
            paneObj["id"]   = static_cast<double>(pid);
            paneObj["cols"] = static_cast<double>(term ? term->width()  : 0);
            paneObj["rows"] = static_cast<double>(term ? term->height() : 0);
            if (it != paneRenderStates_.end()) {
                const auto& rs = it->second;
                paneObj["held_texture"] = rs.heldTexture != nullptr;
                paneObj["texture_kb"]   = rs.heldTexture
                    ? toKB(rs.heldTexture->sizeBytes) : 0.0;
                paneObj["has_divider"]  = rs.dividerVB != nullptr;
            }
            panesArr.emplace_back(std::move(paneObj));
        }
        glz::generic::object_t tabObj;
        tabObj["index"]  = static_cast<double>(ti);
        tabObj["active"] = (ti == activeTabIdx_);
        tabObj["panes"]  = std::move(panesArr);
        tabsArr.emplace_back(std::move(tabObj));
    }
    resp["tabs"] = std::move(tabsArr);

    std::string buf;
    (void)glz::write_json(resp, buf);
    return buf;
}

