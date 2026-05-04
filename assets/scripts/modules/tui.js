// mb:tui — Solid-like TUI widget library for popup and overlay applets.
// Pure JS module; no C++ required.

// ============================================================================
// Reactivity
// ============================================================================

let _tracking = null; // current subscriber being evaluated

// A dependency node: tracks which subscribers read it.
class Dep {
    constructor() { this._subs = new Set(); }
    track() {
        if (_tracking) {
            this._subs.add(_tracking);
            _tracking._deps.add(this);
        }
    }
    notify() {
        for (const sub of [...this._subs]) sub._invalidate();
    }
}

function cleanupSub(sub) {
    for (const dep of sub._deps) dep._subs.delete(sub);
    sub._deps.clear();
}

export function signal(init) {
    const dep = new Dep();
    let _val = init;
    return {
        get value() { dep.track(); return _val; },
        set value(v) {
            if (_val === v) return;
            _val = v;
            dep.notify();
        },
        _dep: dep,
    };
}

// Run fn immediately and re-run whenever its signal dependencies change.
export function effect(fn) {
    let scheduled = false;
    const eff = {
        _deps: new Set(),
        _invalidate() {
            if (scheduled) return;
            scheduled = true;
            Promise.resolve().then(() => {
                scheduled = false;
                cleanupSub(eff);
                const prev = _tracking;
                _tracking = eff;
                fn();
                _tracking = prev;
            });
        },
    };
    cleanupSub(eff);
    const prev = _tracking;
    _tracking = eff;
    fn();
    _tracking = prev;
}

export function computed(fn) {
    const dep = new Dep();
    let _val;
    let _dirty = true;
    const self = {
        _deps: new Set(),
        _dep: dep,
        get value() {
            dep.track();
            if (_dirty) {
                cleanupSub(self);
                const prev = _tracking;
                _tracking = self;
                _val = fn();
                _dirty = false;
                _tracking = prev;
            }
            return _val;
        },
        _invalidate() {
            if (!_dirty) {
                _dirty = true;
                dep.notify();
            }
        },
    };
    return self;
}

// ============================================================================
// Color utilities
// ============================================================================

const _namedColors = {
    black: 30, red: 31, green: 32, yellow: 33,
    blue: 34, magenta: 35, cyan: 36, white: 37, gray: 90,
};
const _brightNames = {
    'bright-black': 90, 'bright-red': 91, 'bright-green': 92, 'bright-yellow': 93,
    'bright-blue': 94, 'bright-magenta': 95, 'bright-cyan': 96, 'bright-white': 97,
};

function _colorCode(name, isBg) {
    if (!name || name === 'default') return null;
    const off = isBg ? 10 : 0;
    if (name in _namedColors) return String(_namedColors[name] + off);
    if (name in _brightNames) return String(_brightNames[name] + off);
    const m256 = name.match(/^color\((\d+)\)$/);
    if (m256) return `${38 + off};5;${m256[1]}`;
    const mhex = name.match(/^#([0-9a-fA-F]{6})$/);
    if (mhex) {
        const r = parseInt(mhex[1].slice(0,2), 16);
        const g = parseInt(mhex[1].slice(2,4), 16);
        const b = parseInt(mhex[1].slice(4,6), 16);
        return `${38 + off};2;${r};${g};${b}`;
    }
    const mrgb = name.match(/^rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)$/);
    if (mrgb) return `${38 + off};2;${mrgb[1]};${mrgb[2]};${mrgb[3]}`;
    return null;
}

// Parse a color string/object into { fgCode, bgCode, bold, italic, underline }.
function parseColor(colorStr) {
    if (!colorStr) return {};
    if (typeof colorStr === 'object') {
        return {
            fgCode:    colorStr.fg        ? _colorCode(colorStr.fg, false)  : null,
            bgCode:    colorStr.bg        ? _colorCode(colorStr.bg, true)   : null,
            bold:      !!colorStr.bold,
            italic:    !!colorStr.italic,
            underline: !!colorStr.underline,
        };
    }
    const parts = String(colorStr).split('.');
    const mods  = parts.slice(1);
    return {
        fgCode:    _colorCode(parts[0], false),
        bold:      mods.includes('bold'),
        italic:    mods.includes('italic'),
        underline: mods.includes('underline'),
    };
}

// ============================================================================
// Cell buffer
// ============================================================================

function makeCell() {
    return { ch: ' ', fg: null, bg: null, bold: false, italic: false, underline: false };
}

class CellBuffer {
    constructor(w, h) {
        this.w = w;
        this.h = h;
        this.cells = Array.from({ length: w * h }, makeCell);
    }

    set(x, y, ch, fg, bg, bold, italic, underline) {
        if (x < 0 || x >= this.w || y < 0 || y >= this.h) return;
        const c     = this.cells[y * this.w + x];
        c.ch        = ch        ?? ' ';
        c.fg        = fg        ?? null;
        c.bg        = bg        ?? null;
        c.bold      = bold      ?? false;
        c.italic    = italic    ?? false;
        c.underline = underline ?? false;
    }

    fill(x, y, w, h, ch, fg, bg, bold, italic, underline) {
        for (let row = y; row < y + h; row++)
            for (let col = x; col < x + w; col++)
                this.set(col, row, ch, fg, bg, bold, italic, underline);
    }
}

// ============================================================================
// Border styles
// ============================================================================

const _borders = {
    round: { tl:'╭', tr:'╮', bl:'╰', br:'╯', h:'─', v:'│' },
    line:  { tl:'┌', tr:'┐', bl:'└', br:'┘', h:'─', v:'│' },
    heavy: { tl:'┏', tr:'┓', bl:'┗', br:'┛', h:'━', v:'┃' },
};

// ============================================================================
// Theme
// ============================================================================

// Default theme — all colors are optional; null means use terminal default.
// `text.hoverFg` / `text.hoverBg` apply to text/button widgets that have an
// `onClick`. `list.hoverStyle` applies to a hovered list row that is not the
// currently selected row. `prefixFg` / `prefixBg` on selectedStyle/hoverStyle
// override the row colors for the prefix glyph only — useful for an accent
// color distinct from the row text.
export const defaultTheme = {
    bg:     null,                  // popup background; null = terminal default
    border: { color: 'gray' },
    text:   { color: null, bg: null, hoverFg: null, hoverBg: null },
    input:  { color: 'white.bold', bg: null },
    list: {
        selectedStyle: { bg: 'cyan', fg: 'black', prefix: '▌', prefixFg: null, prefixBg: null },
        hoverStyle:    { bg: null,   fg: null,    prefix: null, prefixFg: null, prefixBg: null },
        itemColor: null,
        bg: null,   // non-selected row background; null = inherit popup bg
    },
};

// createTheme(overrides) — deep-merge overrides onto defaultTheme.
export function createTheme(overrides) {
    const o = overrides || {};
    const ol = o.list || {};
    return {
        bg:     o.bg !== undefined ? o.bg : defaultTheme.bg,
        border: { ...defaultTheme.border, ...(o.border || {}) },
        text:   { ...defaultTheme.text,   ...(o.text   || {}) },
        input:  { ...defaultTheme.input,  ...(o.input  || {}) },
        list: {
            ...defaultTheme.list,
            ...ol,
            selectedStyle: { ...defaultTheme.list.selectedStyle, ...(ol.selectedStyle || {}) },
            hoverStyle:    { ...defaultTheme.list.hoverStyle,    ...(ol.hoverStyle    || {}) },
        },
    };
}

// Pre-parse a theme into resolved SGR codes — called once per render() invocation.
function _parseTheme(raw) {
    const t  = createTheme(raw);
    const ss = t.list.selectedStyle;
    const hs = t.list.hoverStyle;
    return {
        bg:     t.bg ? _colorCode(t.bg, true) : null,
        border: { fg: _colorCode(t.border.color, false) },
        text:   {
            ...parseColor(t.text.color),
            bgCode:      t.text.bg      ? _colorCode(t.text.bg,      true)  : null,
            hoverFgCode: t.text.hoverFg ? _colorCode(t.text.hoverFg, false) : null,
            hoverBgCode: t.text.hoverBg ? _colorCode(t.text.hoverBg, true)  : null,
        },
        input:  { ...parseColor(t.input.color), bgCode: t.input.bg ? _colorCode(t.input.bg, true) : null },
        list: {
            selFg:        ss.fg            ? _colorCode(ss.fg,            false) : null,
            selBg:        ss.bg            ? _colorCode(ss.bg,            true)  : null,
            prefix:       ss.prefix        || '▌',
            selPrefixFg:  ss.prefixFg      ? _colorCode(ss.prefixFg,      false) : null,
            selPrefixBg:  ss.prefixBg      ? _colorCode(ss.prefixBg,      true)  : null,
            hovFg:        hs.fg            ? _colorCode(hs.fg,            false) : null,
            hovBg:        hs.bg            ? _colorCode(hs.bg,            true)  : null,
            hovPrefix:    hs.prefix        ?? null,
            hovPrefixFg:  hs.prefixFg      ? _colorCode(hs.prefixFg,      false) : null,
            hovPrefixBg:  hs.prefixBg      ? _colorCode(hs.prefixBg,      true)  : null,
            itemFg:       t.list.itemColor ? _colorCode(t.list.itemColor, false) : null,
            itemBg:       t.list.bg        ? _colorCode(t.list.bg,        true)  : null,
        },
    };
}

// ============================================================================
// Helpers
// ============================================================================

function getValue(v) {
    if (v !== null && typeof v === 'object' && 'value' in v) return v.value;
    return v;
}

// ============================================================================
// Node constructors
// ============================================================================

export function box(props, children) {
    if (Array.isArray(props)) { children = props; props = {}; }
    const p = props || {};
    return {
        type: 'box', props: p, children: children || [],
        _parsed: p.borderColor ? { fg: _colorCode(p.borderColor, false) } : null,
    };
}

export function col(props, children) {
    if (Array.isArray(props)) { children = props; props = {}; }
    return { type: 'col', props: props || {}, children: children || [] };
}

export function row(props, children) {
    if (Array.isArray(props)) { children = props; props = {}; }
    return { type: 'row', props: props || {}, children: children || [] };
}

export function text(props) {
    const p = props || {};
    return { type: 'text', props: p, children: [], _parsed: p.color ? parseColor(p.color) : null };
}

export function input(props) {
    const p = props || {};
    return { type: 'input', props: p, children: [], _parsed: p.color ? parseColor(p.color) : null };
}

export function list(props) {
    const p  = props || {};
    const ss = p.selectedStyle || {};
    const hs = p.hoverStyle    || {};
    return {
        type: 'list', props: p, children: [], _scroll: 0, _hoverIdx: -1,
        _parsed: {
            selFg:        ss.fg       ? _colorCode(ss.fg,       false) : null,
            selBg:        ss.bg       ? _colorCode(ss.bg,       true)  : null,
            prefix:       ss.prefix   || null,
            selPrefixFg:  ss.prefixFg ? _colorCode(ss.prefixFg, false) : null,
            selPrefixBg:  ss.prefixBg ? _colorCode(ss.prefixBg, true)  : null,
            hovFg:        hs.fg       ? _colorCode(hs.fg,       false) : null,
            hovBg:        hs.bg       ? _colorCode(hs.bg,       true)  : null,
            hovPrefix:    hs.prefix   ?? null,
            hovPrefixFg:  hs.prefixFg ? _colorCode(hs.prefixFg, false) : null,
            hovPrefixBg:  hs.prefixBg ? _colorCode(hs.prefixBg, true)  : null,
            itemFg:       p.itemColor ? _colorCode(p.itemColor, false) : null,
            itemBg:       p.itemBg    ? _colorCode(p.itemBg,    true)  : null,
        },
    };
}

// button({label, onClick, onMouseEnter, onMouseLeave, color, primary}) —
// convenience constructor for a clickable single-line button. Activates on
// mouse release when the press landed on the same node. Keyboard activation
// (Enter/Space) is not yet distinguished from text input — see the deferred
// press/release split.
export function button(props) {
    const p = props || {};
    const colorStr = p.color || (p.primary ? 'white.bold' : null);
    return text({
        value:        p.label || '',
        align:        'center',
        color:        colorStr,
        onClick:      p.onClick,
        onMouseEnter: p.onMouseEnter,
        onMouseLeave: p.onMouseLeave,
    });
}

// ============================================================================
// Layout — assigns _x, _y, _w, _h to every node; returns height consumed
// ============================================================================

function layout(node, x, y, availW, availH) {
    node._x = x; node._y = y; node._w = availW;

    switch (node.type) {
    case 'box': {
        const p    = node.props;
        const bw   = (p.border && p.border !== 'none') ? 1 : 0;
        const bTop = bw || (p.borderTop    ? 1 : 0);
        const bBot = bw || (p.borderBottom ? 1 : 0);
        const bLft = bw || (p.borderLeft   ? 1 : 0);
        const bRgt = bw || (p.borderRight  ? 1 : 0);
        const pad  = p.padding || 0;
        const iX   = x + bLft + pad;
        const iY   = y + bTop + pad;
        const iW   = Math.max(0, availW - bLft - bRgt - pad * 2);
        let   iH   = 0;
        for (const child of node.children)
            iH += layout(child, iX, iY + iH, iW, Math.max(0, availH - bTop - bBot - pad * 2 - iH));
        node._h    = iH + bTop + bBot + pad * 2;
        node._bTop = bTop; node._bBot = bBot; node._bLft = bLft; node._bRgt = bRgt;
        return node._h;
    }
    case 'col': {
        let h = 0;
        for (const child of node.children)
            h += layout(child, x, y + h, availW, Math.max(0, availH - h));
        node._h = h;
        return h;
    }
    case 'row': {
        const gap      = node.props.gap || 0;
        const kids     = node.children;
        let fixedW     = 0, totalFlex = 0;
        for (const c of kids) {
            if (c.props && c.props.width) fixedW += c.props.width;
            else totalFlex += (c.props && c.props.flex) || 1;
        }
        const gaps  = gap * Math.max(0, kids.length - 1);
        const flexW = Math.max(0, availW - fixedW - gaps);
        let cx = x, maxH = 0;
        for (const c of kids) {
            const cw = (c.props && c.props.width)
                ? c.props.width
                : (totalFlex > 0 ? Math.floor(((c.props && c.props.flex) || 1) / totalFlex * flexW) : 0);
            const ch = layout(c, cx, y, cw, availH);
            cx += cw + gap;
            if (ch > maxH) maxH = ch;
        }
        node._h = maxH;
        return maxH;
    }
    case 'text':
    case 'input':
        node._h = 1;
        return 1;
    case 'list':
        node._h = node.props.height || 5;
        return node._h;
    default:
        node._h = 0;
        return 0;
    }
}

// ============================================================================
// Rendering — write node tree into a CellBuffer
// ============================================================================

function renderNode(node, buf, focused, hovered, theme) {
    switch (node.type) {
    case 'box':   _renderBox(node, buf, focused, hovered, theme);          break;
    case 'col':
    case 'row':
        for (const c of node.children) renderNode(c, buf, focused, hovered, theme);
        break;
    case 'text':  _renderText(node, buf, hovered, theme);                  break;
    case 'input': _renderInput(node, buf, node === focused, theme);         break;
    case 'list':  _renderList(node, buf, node === focused, theme);          break;
    }
}

function _renderBox(node, buf, focused, hovered, theme) {
    const p  = node.props;
    const x  = node._x, y = node._y, w = node._w, h = node._h;
    // Pre-parsed border color from props; fall back to theme
    const fg = (node._parsed ? node._parsed.fg : null) ?? theme.border.fg;

    const full = p.border && p.border !== 'none' ? (_borders[p.border] || _borders.line) : null;
    const tS   = full || (p.borderTop    ? (_borders[p.borderTop]    || _borders.line) : null);
    const bS   = full || (p.borderBottom ? (_borders[p.borderBottom] || _borders.line) : null);
    const lS   = full || (p.borderLeft   ? (_borders[p.borderLeft]   || _borders.line) : null);
    const rS   = full || (p.borderRight  ? (_borders[p.borderRight]  || _borders.line) : null);

    const bbg = theme.bg;
    if (tS) {
        buf.set(x,         y, lS ? tS.tl : tS.h, fg, bbg, false, false, false);
        for (let c = x + 1; c < x + w - 1; c++) buf.set(c, y, tS.h, fg, bbg, false, false, false);
        buf.set(x + w - 1, y, rS ? tS.tr : tS.h, fg, bbg, false, false, false);
    }
    if (bS) {
        buf.set(x,         y+h-1, lS ? bS.bl : bS.h, fg, bbg, false, false, false);
        for (let c = x + 1; c < x + w - 1; c++) buf.set(c, y+h-1, bS.h, fg, bbg, false, false, false);
        buf.set(x + w - 1, y+h-1, rS ? bS.br : bS.h, fg, bbg, false, false, false);
    }
    if (lS) for (let r = y + (tS?1:0); r < y + h - (bS?1:0); r++) buf.set(x,         r, lS.v, fg, bbg, false, false, false);
    if (rS) for (let r = y + (tS?1:0); r < y + h - (bS?1:0); r++) buf.set(x + w - 1, r, rS.v, fg, bbg, false, false, false);

    for (const child of node.children) renderNode(child, buf, focused, hovered, theme);
}

function _renderText(node, buf, hovered, theme) {
    const p       = node.props;
    const x       = node._x, y = node._y, w = node._w;
    const raw     = String(getValue(p.value) ?? '');
    // Merge: theme provides defaults, node props override per-field
    const col     = node._parsed ? { ...theme.text,  ...node._parsed } : theme.text;
    const isHover = hovered === node && p.onClick;
    const fg      = isHover ? (theme.text.hoverFgCode ?? col.fgCode ?? null) : (col.fgCode ?? null);
    const bg      = isHover ? (theme.text.hoverBgCode ?? col.bgCode ?? theme.bg) : (col.bgCode ?? theme.bg);
    const align   = p.align || 'left';

    buf.fill(x, y, w, 1, ' ', null, bg, false, false, false);

    const str    = raw.length > w ? raw.slice(0, w) : raw;
    let   startX = x;
    if (align === 'right')  startX = x + w - str.length;
    if (align === 'center') startX = x + Math.floor((w - str.length) / 2);
    for (let i = 0; i < str.length; i++)
        buf.set(startX + i, y, str[i], fg, bg, col.bold, col.italic, col.underline);
}

function _renderInput(node, buf, focused, theme) {
    const p      = node.props;
    const x      = node._x, y = node._y, w = node._w;
    const prompt = p.prompt || '';
    const val    = String(getValue(p.value) ?? '');
    // Merge: theme provides defaults, node props override per-field
    const col    = node._parsed ? { ...theme.input, ...node._parsed } : theme.input;
    const fg     = col.fgCode ?? null;
    const bg     = col.bgCode ?? theme.bg;

    buf.fill(x, y, w, 1, ' ', null, bg, false, false, false);

    let cx = x;
    for (let i = 0; i < prompt.length && cx < x + w; i++, cx++)
        buf.set(cx, y, prompt[i], fg, bg, col.bold, false, false);
    for (let i = 0; i < val.length && cx < x + w; i++, cx++)
        buf.set(cx, y, val[i], fg, bg, col.bold, col.italic, col.underline);

    if (focused && cx < x + w)
        buf.set(cx, y, ' ', bg || '39', fg || '49', false, false, false);
}

function _renderList(node, buf, focused, theme) {
    const p             = node.props;
    const x             = node._x, y = node._y, w = node._w, h = node._h;
    const items         = getValue(p.items) ?? [];
    const sel           = getValue(p.selected) ?? 0;
    const hovIdx        = node._hoverIdx ?? -1;
    // Pre-parsed from props; fall back to theme
    const np            = node._parsed;
    const selFg         = (np && np.selFg        !== null ? np.selFg        : null) ?? theme.list.selFg;
    const selBg         = (np && np.selBg        !== null ? np.selBg        : null) ?? theme.list.selBg;
    const prefix        = (np && np.prefix       !== null ? np.prefix       : null) ?? theme.list.prefix;
    const selPrefixFg   = (np && np.selPrefixFg  !== null ? np.selPrefixFg  : null) ?? theme.list.selPrefixFg;
    const selPrefixBg   = (np && np.selPrefixBg  !== null ? np.selPrefixBg  : null) ?? theme.list.selPrefixBg;
    const hovFg         = (np && np.hovFg        !== null ? np.hovFg        : null) ?? theme.list.hovFg;
    const hovBg         = (np && np.hovBg        !== null ? np.hovBg        : null) ?? theme.list.hovBg;
    const hovPrefix     = (np && np.hovPrefix    !== null ? np.hovPrefix    : null) ?? theme.list.hovPrefix;
    const hovPrefixFg   = (np && np.hovPrefixFg  !== null ? np.hovPrefixFg  : null) ?? theme.list.hovPrefixFg;
    const hovPrefixBg   = (np && np.hovPrefixBg  !== null ? np.hovPrefixBg  : null) ?? theme.list.hovPrefixBg;
    const itemFg        = (np && np.itemFg       !== null ? np.itemFg       : null) ?? theme.list.itemFg;
    const itemBg        = (np && np.itemBg       !== null ? np.itemBg       : null) ?? theme.list.itemBg ?? theme.bg;

    if (sel < node._scroll) node._scroll = sel;
    if (sel >= node._scroll + h) node._scroll = sel - h + 1;

    for (let row = 0; row < h; row++) {
        const idx   = node._scroll + row;
        const item  = idx < items.length ? String(items[idx]) : '';
        const isSel = idx === sel;
        const isHov = !isSel && idx === hovIdx;
        const fg    = isSel ? selFg : isHov ? (hovFg ?? itemFg) : itemFg;
        const bg    = isSel ? selBg : isHov ? (hovBg ?? itemBg) : itemBg;
        const px    = isSel ? prefix : isHov ? (hovPrefix ?? ' ') : ' ';
        // Prefix-specific colors override row colors when set
        const pxFg  = isSel ? (selPrefixFg ?? selFg) : isHov ? (hovPrefixFg ?? hovFg ?? itemFg) : itemFg;
        const pxBg  = isSel ? (selPrefixBg ?? selBg) : isHov ? (hovPrefixBg ?? hovBg ?? itemBg) : itemBg;

        buf.fill(x, y + row, w, 1, ' ', null, bg, false, false, false);
        buf.set(x, y + row, px, pxFg, pxBg, false, false, false);
        const str = item.length > w - 1 ? item.slice(0, w - 1) : item;
        for (let ci = 0; ci < str.length; ci++)
            buf.set(x + 1 + ci, y + row, str[ci], fg, bg, false, false, false);
    }
}

// ============================================================================
// Diff — old vs new cell buffer → minimal escape sequence string
// ============================================================================

function _sgrTransition(cell, cur, out) {
    const needReset = (cur.bold && !cell.bold) ||
                      (cur.italic && !cell.italic) ||
                      (cur.underline && !cell.underline);
    if (needReset) {
        out.push('\x1b[0m');
        cur.fg = null; cur.bg = null;
        cur.bold = false; cur.italic = false; cur.underline = false;
    }
    if (cell.fg !== cur.fg) {
        out.push(cell.fg ? `\x1b[${cell.fg}m` : '\x1b[39m');
        cur.fg = cell.fg;
    }
    if (cell.bg !== cur.bg) {
        out.push(cell.bg ? `\x1b[${cell.bg}m` : '\x1b[49m');
        cur.bg = cell.bg;
    }
    if (cell.bold      && !cur.bold)      { out.push('\x1b[1m'); cur.bold      = true; }
    if (cell.italic    && !cur.italic)    { out.push('\x1b[3m'); cur.italic    = true; }
    if (cell.underline && !cur.underline) { out.push('\x1b[4m'); cur.underline = true; }
}

function _cellsEqual(a, b) {
    return a.ch === b.ch && a.fg === b.fg && a.bg === b.bg &&
           a.bold === b.bold && a.italic === b.italic && a.underline === b.underline;
}

function buildDiff(oldBuf, newBuf) {
    const w   = newBuf.w, h = newBuf.h;
    const out = [];
    const cur = { fg: null, bg: null, bold: false, italic: false, underline: false };

    for (let row = 0; row < h; row++) {
        let inRun = false;
        for (let col = 0; col < w; col++) {
            const i  = row * w + col;
            const nc = newBuf.cells[i];
            const oc = oldBuf ? oldBuf.cells[i] : null;
            if (oc && _cellsEqual(oc, nc)) { inRun = false; continue; }
            if (!inRun) { out.push(`\x1b[${row+1};${col+1}H`); inRun = true; }
            _sgrTransition(nc, cur, out);
            out.push(nc.ch);
        }
    }
    if (out.length) out.push('\x1b[0m');
    return out.join('');
}

// ============================================================================
// Focus collection
// ============================================================================

function collectFocusables(node, out) {
    if (node.type === 'input' || node.type === 'list') out.push(node);
    for (const c of node.children) collectFocusables(c, out);
}

// ============================================================================
// Hit testing — cell coords → deepest containing node, or null if outside
// ============================================================================

function hitTest(node, x, y) {
    if (x < node._x || x >= node._x + node._w ||
        y < node._y || y >= node._y + node._h) return null;
    for (const c of node.children) {
        const hit = hitTest(c, x, y);
        if (hit) return hit;
    }
    return node;
}

// ============================================================================
// RenderInstance
// ============================================================================

class RenderInstance {
    constructor(target, root, opts) {
        this._target    = target;
        this._root      = root;
        this._onDestroy = opts && opts.onDestroy;
        this._theme     = _parseTheme(opts && opts.theme);
        this._w         = target.cols;
        this._h         = target.rows;
        this._oldBuf    = null;
        this._pending   = false;
        this._destroyed = false;
        this._focusables = [];
        this._focusIdx   = 0;
        this._pressedNode = null;
        this._hoverNode  = null;

        const self = this;
        this._effect = {
            _deps: new Set(),
            _invalidate() { self._schedule(); },
        };

        this._layout();
        this._runEffect();
        target.inject('\x1b[?25l'); // hide terminal cursor while tui is active

        this._inputCb = (data) => this._handleInput(data);
        target.addEventListener('input', this._inputCb);

        this._mouseCb = (ev) => this._handleMouse(ev);
        target.addEventListener('mouse', this._mouseCb);

        this._mouseMoveCb = (ev) => this._handleMouseMove(ev);
        target.addEventListener('mousemove', this._mouseMoveCb);
    }

    _layout() {
        layout(this._root, 0, 0, this._w, this._h);
        this._focusables = [];
        collectFocusables(this._root, this._focusables);
        this._focusIdx = Math.min(this._focusIdx, Math.max(0, this._focusables.length - 1));
    }

    _runEffect() {
        cleanupSub(this._effect);
        const prev = _tracking;
        _tracking = this._effect;
        this._doRender();
        _tracking = prev;
    }

    _doRender() {
        const buf = new CellBuffer(this._w, this._h);
        buf.fill(0, 0, this._w, this._h, ' ', null, this._theme.bg, false, false, false);
        renderNode(this._root, buf,
            this._focusables[this._focusIdx] ?? null,
            this._hoverNode,
            this._theme);
        const out = buildDiff(this._oldBuf, buf);
        if (out) this._target.inject(out);
        this._oldBuf = buf;
    }

    _schedule() {
        if (this._pending || this._destroyed) return;
        this._pending = true;
        Promise.resolve().then(() => {
            if (this._destroyed) return;
            this._pending = false;
            this._runEffect();
        });
    }

    _handleInput(data) {
        if (this._destroyed) return;

        if (data === '\x1b') { this.destroy(); return; }

        if (data === '\t') {
            if (this._focusables.length > 1) {
                this._focusIdx = (this._focusIdx + 1) % this._focusables.length;
                this._schedule();
            }
            return;
        }

        const focused = this._focusables[this._focusIdx];
        if (!focused) return;

        if (focused.type === 'input') {
            const isPrintable = data.length === 1 && data.charCodeAt(0) >= 32;
            const isBackspace = data === '\x7f' || data === '\x08';

            if (isPrintable || isBackspace) {
                const sig = focused.props.value;
                if (!sig || typeof sig.value === 'undefined') return;
                if (isBackspace) {
                    if (sig.value.length > 0) sig.value = sig.value.slice(0, -1);
                } else {
                    sig.value = sig.value + data;
                }
                return;
            }

            // Any other key (arrows, Enter, etc.) — forward to first list in the tree
            const listNode = this._focusables.find(f => f.type === 'list');
            if (listNode) {
                const sel   = listNode.props.selected;
                const items = getValue(listNode.props.items) ?? [];
                if (data === '\x1b[A') {
                    if (sel && sel.value > 0) sel.value = sel.value - 1;
                } else if (data === '\x1b[B') {
                    if (sel && sel.value < items.length - 1) sel.value = sel.value + 1;
                } else if (data === '\r' || data === '\n') {
                    listNode.props.onSelect?.(getValue(listNode.props.selected) ?? 0);
                }
            }
            return;
        }

        if (focused.type === 'list') {
            const sel   = focused.props.selected;
            const items = getValue(focused.props.items) ?? [];
            if (data === '\x1b[A') { // up
                if (sel && sel.value > 0) sel.value = sel.value - 1;
            } else if (data === '\x1b[B') { // down
                if (sel && sel.value < items.length - 1) sel.value = sel.value + 1;
            } else if (data === '\r' || data === '\n') {
                focused.props.onSelect?.(getValue(focused.props.selected) ?? 0);
            }
        }
    }

    _handleMouse(ev) {
        if (this._destroyed) return;
        const hit = hitTest(this._root, ev.cellX, ev.cellY);

        if (ev.type === 'press') {
            this._pressedNode = hit;
            if (hit) {
                const idx = this._focusables.indexOf(hit);
                if (idx >= 0 && idx !== this._focusIdx) {
                    this._focusIdx = idx;
                    this._schedule();
                }
            }
            return;
        }

        if (ev.type === 'release') {
            const pressed = this._pressedNode;
            this._pressedNode = null;
            if (!hit || hit !== pressed) return;

            if (hit.props && hit.props.onClick) hit.props.onClick(ev);

            if (hit.type === 'list') {
                const localY = ev.cellY - hit._y;
                const idx    = hit._scroll + localY;
                const items  = getValue(hit.props.items) ?? [];
                if (idx >= 0 && idx < items.length) {
                    const sel = hit.props.selected;
                    if (sel) sel.value = idx;
                    hit.props.onSelect?.(idx);
                }
            }
        }
    }

    _handleMouseMove(ev) {
        if (this._destroyed) return;
        const hit = hitTest(this._root, ev.cellX, ev.cellY);

        let changed = false;

        if (hit !== this._hoverNode) {
            const prev = this._hoverNode;
            this._hoverNode = hit;
            if (prev) {
                if (prev.type === 'list') prev._hoverIdx = -1;
                prev.props?.onMouseLeave?.(ev);
            }
            if (hit) hit.props?.onMouseEnter?.(ev);
            changed = true;
        }

        if (hit && hit.type === 'list') {
            const localY = ev.cellY - hit._y;
            const idx    = hit._scroll + localY;
            const items  = getValue(hit.props.items) ?? [];
            const newIdx = (idx >= 0 && idx < items.length) ? idx : -1;
            if (hit._hoverIdx !== newIdx) {
                hit._hoverIdx = newIdx;
                changed = true;
            }
        }

        if (changed) this._schedule();
    }

    // Call after resizing the target popup/overlay to update layout and re-render.
    // Pass a new root if widget structure needs to change (e.g. list height changed).
    resize(w, h, newRoot) {
        if (this._destroyed) return;
        this._w = w;
        this._h = h;
        this._oldBuf = null; // force full redraw
        if (newRoot) this._root = newRoot;
        this._layout();
        this._runEffect();
    }

    destroy() {
        if (this._destroyed) return;
        this._destroyed = true;
        cleanupSub(this._effect);
        this._target.removeEventListener('input',     this._inputCb);
        this._target.removeEventListener('mouse',     this._mouseCb);
        this._target.removeEventListener('mousemove', this._mouseMoveCb);
        this._target.inject('\x1b[?25h'); // restore cursor before closing
        this._target.close();
        if (this._onDestroy) this._onDestroy();
    }
}

// ============================================================================
// Public API
// ============================================================================

export function render(target, root, opts) {
    return new RenderInstance(target, root, opts);
}
