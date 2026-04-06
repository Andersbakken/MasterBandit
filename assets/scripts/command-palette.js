// command-palette.js — fuzzy command palette, built with mb:tui
import { signal, computed, effect, render, createTheme, box, text, input, list } from "mb:tui";

const theme = createTheme({
    bg:     '#0d1b2a',
    border: { color: '#364d6a' },
    input:  { color: 'bright-white.bold', bg: '#162d4a' },
    list: {
        selectedStyle: { bg: '#263d5a', fg: 'white', prefix: '▌' },
        itemColor: '#8aabcf',
    },
});

mb.setNamespace("palette");
mb.registerAction("open");

function fuzzyScore(str, pattern) {
    if (!pattern) return 0;
    const lower = str.toLowerCase();
    const pat = pattern.toLowerCase();
    let score = 0, pi = 0, consecutive = 0;
    for (let i = 0; i < lower.length && pi < pat.length; i++) {
        if (lower[i] === pat[pi]) {
            pi++;
            consecutive++;
            score += consecutive * 2;
            if (i === 0 || str[i - 1] === ' ') score += 5;
        } else {
            consecutive = 0;
        }
    }
    return pi === pat.length ? score : -1;
}

let ui = null;

mb.addEventListener("action", "palette.open", () => {
    const pane = mb.activePane;
    if (!pane) return;

    if (ui) {
        ui.destroy(); // toggles closed; onDestroy sets ui = null
        return;
    }

    const allActions = mb.actions;
    const query    = signal("");
    const selected = signal(0);

    const filtered = computed(() => {
        const q = query.value;
        if (!q) return allActions.slice();
        return allActions
            .map(a => ({ action: a, score: fuzzyScore(a.label, q) }))
            .filter(x => x.score >= 0)
            .sort((a, b) => b.score - a.score)
            .map(x => x.action);
    });

    // Reset selection to top whenever the filtered list changes
    effect(() => {
        filtered.value,
        selected.value = 0;
    });

    const LIST_H = 12;
    // Total height: round border (2) + input (1) + divider (1) + list + status (1)
    const H = 2 + 1 + 1 + LIST_H + 1;
    const W = 48;
    const popup = pane.createPopup({
        id: "palette",
        x: Math.max(0, Math.floor((pane.cols - W) / 2)),
        y: Math.max(0, Math.floor((pane.rows - H) / 2)),
        w: W, h: H,
    });
    if (!popup) return;

    ui = render(popup,
        box({ border: "round" }, [
            input({ value: query, prompt: " > " }),
            box({ borderTop: "line" }),
            list({
                items: computed(() => filtered.value.map(a => a.label)),
                selected,
                height: LIST_H,
                onSelect: (idx) => {
                    const action = filtered.value[idx];
                    ui.destroy();
                    if (action) {
                        if (action.args && action.args.length > 0)
                            mb.invokeAction(action.name, ...action.args);
                        else
                            mb.invokeAction(action.name);
                    }
                },
            }),
            text({
                value: computed(() => ` ${filtered.value.length}/${allActions.length}`),
                align: "right",
                color: "#5a7b9f",
            }),
        ]),
        { theme, onDestroy: () => { ui = null; } }
    );

    mb.invokeAction("focus_popup");
});

console.log("command-palette: initialized");
