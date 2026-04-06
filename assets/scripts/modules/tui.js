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
    return { type: 'box', props: props || {}, children: children || [] };
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
    return { type: 'text', props: props || {}, children: [] };
}

export function input(props) {
    return { type: 'input', props: props || {}, children: [] };
}

export function list(props) {
    return { type: 'list', props: props || {}, children: [], _scroll: 0 };
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

function renderNode(node, buf, focused) {
    switch (node.type) {
    case 'box':   _renderBox(node, buf, focused);          break;
    case 'col':
    case 'row':
        for (const c of node.children) renderNode(c, buf, focused);
        break;
    case 'text':  _renderText(node, buf);                  break;
    case 'input': _renderInput(node, buf, node === focused); break;
    case 'list':  _renderList(node, buf, node === focused);  break;
    }
}

function _renderBox(node, buf, focused) {
    const p  = node.props;
    const x  = node._x, y = node._y, w = node._w, h = node._h;
    const bc = parseColor(p.borderColor);
    const fg = bc.fgCode ?? null;

    const full = p.border && p.border !== 'none' ? (_borders[p.border] || _borders.line) : null;
    const tS   = full || (p.borderTop    ? (_borders[p.borderTop]    || _borders.line) : null);
    const bS   = full || (p.borderBottom ? (_borders[p.borderBottom] || _borders.line) : null);
    const lS   = full || (p.borderLeft   ? (_borders[p.borderLeft]   || _borders.line) : null);
    const rS   = full || (p.borderRight  ? (_borders[p.borderRight]  || _borders.line) : null);

    if (tS) {
        buf.set(x,         y, lS ? tS.tl : tS.h, fg, null, false, false, false);
        for (let c = x + 1; c < x + w - 1; c++) buf.set(c, y, tS.h, fg, null, false, false, false);
        buf.set(x + w - 1, y, rS ? tS.tr : tS.h, fg, null, false, false, false);
    }
    if (bS) {
        buf.set(x,         y+h-1, lS ? bS.bl : bS.h, fg, null, false, false, false);
        for (let c = x + 1; c < x + w - 1; c++) buf.set(c, y+h-1, bS.h, fg, null, false, false, false);
        buf.set(x + w - 1, y+h-1, rS ? bS.br : bS.h, fg, null, false, false, false);
    }
    if (lS) for (let r = y + (tS?1:0); r < y + h - (bS?1:0); r++) buf.set(x,         r, lS.v, fg, null, false, false, false);
    if (rS) for (let r = y + (tS?1:0); r < y + h - (bS?1:0); r++) buf.set(x + w - 1, r, rS.v, fg, null, false, false, false);

    for (const child of node.children) renderNode(child, buf, focused);
}

function _renderText(node, buf) {
    const p    = node.props;
    const x    = node._x, y = node._y, w = node._w;
    const raw  = String(getValue(p.value) ?? '');
    const col  = parseColor(p.color);
    const fg   = col.fgCode ?? null;
    const bg   = col.bgCode ?? null;
    const align = p.align || 'left';

    buf.fill(x, y, w, 1, ' ', null, bg, false, false, false);

    const str    = raw.length > w ? raw.slice(0, w) : raw;
    let   startX = x;
    if (align === 'right')  startX = x + w - str.length;
    if (align === 'center') startX = x + Math.floor((w - str.length) / 2);
    for (let i = 0; i < str.length; i++)
        buf.set(startX + i, y, str[i], fg, bg, col.bold, col.italic, col.underline);
}

function _renderInput(node, buf, focused) {
    const p      = node.props;
    const x      = node._x, y = node._y, w = node._w;
    const prompt = p.prompt || '';
    const val    = String(getValue(p.value) ?? '');
    const col    = parseColor(p.color);
    const fg     = col.fgCode ?? null;
    const bg     = col.bgCode ?? null;

    buf.fill(x, y, w, 1, ' ', null, bg, false, false, false);

    let cx = x;
    for (let i = 0; i < prompt.length && cx < x + w; i++, cx++)
        buf.set(cx, y, prompt[i], fg, bg, col.bold, false, false);
    for (let i = 0; i < val.length && cx < x + w; i++, cx++)
        buf.set(cx, y, val[i], fg, bg, col.bold, col.italic, col.underline);

    // Block cursor at end of value when focused
    if (focused && cx < x + w)
        buf.set(cx, y, ' ', bg || '39', fg || '49', false, false, false);
}

function _renderList(node, buf, focused) {
    const p      = node.props;
    const x      = node._x, y = node._y, w = node._w, h = node._h;
    const items  = getValue(p.items) ?? [];
    const sel    = getValue(p.selected) ?? 0;
    const ss     = p.selectedStyle || {};
    const prefix = ss.prefix || ' ';
    const selFg  = ss.fg ? _colorCode(ss.fg, false) : null;
    const selBg  = ss.bg ? _colorCode(ss.bg, true)  : null;

    // Keep selected item visible
    if (sel < node._scroll) node._scroll = sel;
    if (sel >= node._scroll + h) node._scroll = sel - h + 1;

    for (let row = 0; row < h; row++) {
        const idx   = node._scroll + row;
        const item  = idx < items.length ? String(items[idx]) : '';
        const isSel = focused && idx === sel;
        const fg    = isSel ? selFg : null;
        const bg    = isSel ? selBg : null;

        buf.fill(x, y + row, w, 1, ' ', null, bg, false, false, false);
        buf.set(x, y + row, isSel ? prefix : ' ', fg, bg, false, false, false);
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
// RenderInstance
// ============================================================================

class RenderInstance {
    constructor(target, root) {
        this._target    = target;
        this._root      = root;
        this._w         = target.cols;
        this._h         = target.rows;
        this._oldBuf    = null;
        this._pending   = false;
        this._destroyed = false;
        this._focusables = [];
        this._focusIdx   = 0;

        const self = this;
        this._effect = {
            _deps: new Set(),
            _invalidate() { self._schedule(); },
        };

        this._layout();
        this._runEffect();

        this._inputCb = (data) => this._handleInput(data);
        target.addEventListener('input', this._inputCb);
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
        buf.fill(0, 0, this._w, this._h, ' ', null, null, false, false, false);
        renderNode(this._root, buf, this._focusables[this._focusIdx] ?? null);
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
            const sig = focused.props.value;
            if (!sig || typeof sig.value === 'undefined') return;
            if (data === '\x7f' || data === '\x08') {
                if (sig.value.length > 0) sig.value = sig.value.slice(0, -1);
            } else if (data.length === 1 && data.charCodeAt(0) >= 32) {
                sig.value = sig.value + data;
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

    destroy() {
        if (this._destroyed) return;
        this._destroyed = true;
        cleanupSub(this._effect);
        this._target.close();
    }
}

// ============================================================================
// Public API
// ============================================================================

export function render(target, root) {
    return new RenderInstance(target, root);
}
