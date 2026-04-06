// command-palette.js — fuzzy command palette for MasterBandit
//
// Bind in config.toml:
//   [[keybinding]]
//   keys = ["meta+shift+p"]   # or ctrl+shift+p on Linux
//   action = "palette.open"

mb.setNamespace("palette");
mb.registerAction("open");

function buildActionList() {
    return mb.actions;
}

let popup = null;
let query = "";
let selected = 0;
let allActions = [];
let filtered = [];

const W = 45;
const MAX_VISIBLE = 12;
const H = MAX_VISIBLE + 4; // border + input + border + items + border

function fuzzyMatch(str, pattern) {
    if (!pattern) return true;
    const lower = str.toLowerCase();
    const pat = pattern.toLowerCase();
    let pi = 0;
    for (let i = 0; i < lower.length && pi < pat.length; i++) {
        if (lower[i] === pat[pi]) pi++;
    }
    return pi === pat.length;
}

function fuzzyScore(str, pattern) {
    if (!pattern) return 0;
    const lower = str.toLowerCase();
    const pat = pattern.toLowerCase();
    let score = 0;
    let pi = 0;
    let consecutive = 0;
    for (let i = 0; i < lower.length && pi < pat.length; i++) {
        if (lower[i] === pat[pi]) {
            pi++;
            consecutive++;
            score += consecutive * 2;
            if (i === 0 || str[i - 1] === " ") score += 5; // word boundary bonus
        } else {
            consecutive = 0;
        }
    }
    return pi === pat.length ? score : -1;
}

function updateFilter() {
    if (!query) {
        filtered = allActions.slice();
    } else {
        filtered = allActions
            .map(a => ({ action: a, score: fuzzyScore(a.label, query) }))
            .filter(x => x.score >= 0)
            .sort((a, b) => b.score - a.score)
            .map(x => x.action);
    }
    if (selected >= filtered.length) selected = Math.max(0, filtered.length - 1);
}

function render() {
    if (!popup) return;
    const inner = W - 2;
    const reset = "\x1b[0m";
    const border = "\x1b[90m";
    const inputColor = "\x1b[1;37m";
    const selBg = "\x1b[46;30m"; // cyan bg, black fg
    const itemColor = "\x1b[37m";
    const dimColor = "\x1b[90m";

    const pad = (s, w) => {
        if (s.length >= w) return s.substring(0, w);
        return s + " ".repeat(w - s.length);
    };

    const hline = border + "+" + "-".repeat(inner) + "+" + reset;
    const line = (color, text) =>
        border + "|" + reset + color + pad(text, inner) + reset + border + "|" + reset;

    let row = 1;
    const at = (r) => "\x1b[" + r + ";1H";
    let out = at(row++);
    out += hline;

    // Input line with cursor
    const prompt = " > " + query;
    out += at(row++);
    out += line(inputColor, prompt);
    out += at(row++);
    out += hline;

    // Visible items
    const scrollOffset = Math.max(0, selected - MAX_VISIBLE + 1);
    for (let i = 0; i < MAX_VISIBLE; i++) {
        const idx = scrollOffset + i;
        out += at(row++);
        if (idx < filtered.length) {
            const a = filtered[idx];
            const prefix = idx === selected ? " > " : "   ";
            const color = idx === selected ? selBg : itemColor;
            out += line(color, prefix + a.label);
        } else {
            out += line("", "");
        }
    }

    // Bottom border with count
    const countStr = filtered.length + "/" + allActions.length;
    const bottomInner = "-".repeat(inner - countStr.length - 1) + " " + countStr;
    out += at(row++);
    out += border + "+" + bottomInner + "+" + reset;

    // Position cursor in input field (row 2, after " > ")
    out += "\x1b[2;" + (5 + query.length) + "H";

    popup.inject(out);
}

function open() {
    if (popup) return; // already open

    const pane = mb.activePane;
    if (!pane) return;

    const cols = pane.cols;
    const rows = pane.rows;
    const x = Math.max(0, Math.floor((cols - W) / 2));
    const y = Math.max(0, Math.floor((rows - H) / 2));

    popup = pane.createPopup({ id: "palette", x: x, y: y, w: W, h: H });
    if (!popup) return;

    query = "";
    selected = 0;
    allActions = buildActionList(); // refresh on each open to pick up new script actions
    filtered = allActions.slice();
    render();

    popup.addEventListener("input", (data) => {
        if (data === "\x1b" || data === "\x03") {
            // Escape or Ctrl+C — close
            close();
            return;
        }

        if (data === "\r" || data === "\n") {
            // Enter — execute selected action
            if (filtered.length > 0) {
                const action = filtered[selected];
                close();
                if (action.args && action.args.length > 0) {
                    mb.invokeAction(action.name, ...action.args);
                } else {
                    mb.invokeAction(action.name);
                }
            }
            return;
        }

        if (data === "\x1b[A") {
            // Up arrow
            if (selected > 0) selected--;
            render();
            return;
        }

        if (data === "\x1b[B") {
            // Down arrow
            if (selected < filtered.length - 1) selected++;
            render();
            return;
        }

        if (data === "\x7f" || data === "\b") {
            // Backspace
            if (query.length > 0) {
                query = query.substring(0, query.length - 1);
                updateFilter();
                selected = 0;
                render();
            }
            return;
        }

        // Printable characters
        if (data.length === 1 && data >= " " && data <= "~") {
            query += data;
            updateFilter();
            selected = 0;
            render();
        }
    });
}

function close() {
    if (popup) {
        popup.close();
        popup = null;
    }
}

mb.addEventListener("action", "palette.open", () => {
    const pane = mb.activePane;
    if (!pane) return;

    if (popup) {
        close();
    } else {
        open();
        // Auto-focus the popup so it receives input immediately
        mb.invokeAction("focus_popup");
    }
});

console.log("command-palette: initialized (bind palette.open to a key)");
