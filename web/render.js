// The cell-grid renderer. It reads the kernel's Cell array straight out of
// linear memory and blits monospace glyphs (Concept.md §2.3, §3.5). There is
// no escape-sequence parser because there are no escape sequences.
//
// It draws and nothing else: calling back into wasm from here would re-enter
// the scheduler in the middle of a tick().

// Offsets into the Screen descriptor, in u32s. Mirrors src/kernel/screen.h.
const S_MAGIC = 0;
const S_COLS = 1;
const S_ROWS = 2;
const S_CURSOR_X = 3;
const S_CURSOR_Y = 4;
const S_CURSOR_ON = 5;
const S_CELLS = 6;

const MAGIC = 0x42534352; // 'BSCR'

// One Cell is {u32 ch, u32 fg|bg<<8|attrs<<16}, eight bytes.
const CELL_U32 = 2;

const ATTR_BOLD = 1;
const ATTR_UNDERLINE = 2;
const ATTR_REVERSE = 4;

// The palette the kernel's fg/bg indices name.
const PALETTE = [
    "#12131a", "#e5687a", "#8fcf7e", "#e0c184",
    "#7aa6e5", "#c58fe0", "#6fc7cf", "#c8cbd6",
    "#4a4e5e", "#ff8b9a", "#aeeb9c", "#ffe0a3",
    "#9cc4ff", "#e0acff", "#8fe8f0", "#ffffff",
];

const FONT_STACK = "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";

export class Renderer {
    constructor(canvas, mem) {
        this.canvas = canvas;
        this.ctx = canvas.getContext("2d", { alpha: false });
        this.mem = mem;
        this.info = 0; // descriptor address; 0 until the first resize
        this.fontPx = 15;
        this.dpr = 1;
        this.cellW = 8;
        this.cellH = 16;
        this.baseline = 12;
        this.measure();
    }

    // memory.grow detaches the buffer (Concept.md §8.4), so views are derived
    // on every use rather than cached across calls.
    u32() {
        return this.mem.u32();
    }

    // The device-pixel box the page measured. Sizing the backing store here,
    // rather than on the main thread, keeps the font and the geometry together.
    fit(width, height, dpr) {
        this.dpr = dpr;
        this.canvas.width = Math.max(1, Math.floor(width));
        this.canvas.height = Math.max(1, Math.floor(height));
        this.fontPx = Math.max(8, Math.round(15 * dpr));
        this.measure();
        return {
            cols: Math.max(1, Math.floor(this.canvas.width / this.cellW)),
            rows: Math.max(1, Math.floor(this.canvas.height / this.cellH)),
        };
    }

    measure() {
        const ctx = this.ctx;
        ctx.font = `${this.fontPx}px ${FONT_STACK}`;
        ctx.textBaseline = "alphabetic";

        const m = ctx.measureText("M");
        // Fractional advances drift over a row, so round once and place every
        // glyph at col * cellW rather than letting the font advance.
        this.cellW = Math.max(1, Math.round(m.width));
        const ascent = m.fontBoundingBoxAscent || this.fontPx * 0.8;
        const descent = m.fontBoundingBoxDescent || this.fontPx * 0.25;
        this.cellH = Math.max(1, Math.round(ascent + descent));
        this.baseline = Math.round(ascent);

        // A proportional font would put every glyph in the wrong place, and the
        // symptom looks like a kernel bug rather than a font one.
        if (Math.abs(ctx.measureText("i").width - m.width) > 0.5)
            console.warn("braam: the terminal font is not monospaced");
    }

    // The descriptor address, learned from resize(). Until it is set there is
    // nothing to draw, which is also the state during boot.
    attach(info) {
        this.info = info;
        this.repaint();
    }

    // Called by the host_present import, once per tick that changed anything.
    present(x, y, w, h) {
        const info = this.info;
        if (!info)
            return;
        const u32 = this.u32();
        const base = info >>> 2;
        if (u32[base + S_MAGIC] !== MAGIC) {
            console.error("braam: screen descriptor magic mismatch");
            return;
        }

        const cols = u32[base + S_COLS];
        const rows = u32[base + S_ROWS];
        const cells = u32[base + S_CELLS] >>> 2;

        const x1 = Math.min(x + w, cols);
        const y1 = Math.min(y + h, rows);
        const cx = Math.min(u32[base + S_CURSOR_X], cols - 1);
        const cy = u32[base + S_CURSOR_Y];
        const cursor = u32[base + S_CURSOR_ON] !== 0;

        const ctx = this.ctx;
        ctx.font = `${this.fontPx}px ${FONT_STACK}`;
        ctx.textBaseline = "alphabetic";

        for (let row = y; row < y1; row++) {
            for (let col = x; col < x1; col++) {
                const c = cells + (row * cols + col) * CELL_U32;
                const under = cursor && col === cx && row === cy;
                this.cell(col, row, u32[c], u32[c + 1], under);
            }
        }
    }

    cell(col, row, ch, attr, under) {
        let fg = attr & 0xff;
        let bg = (attr >>> 8) & 0xff;
        const attrs = (attr >>> 16) & 0xff;

        // The cursor is drawn, never stored: it reverses the cell it sits on.
        const reverse = (attrs & ATTR_REVERSE) !== 0;
        if (reverse !== under)
            [fg, bg] = [bg, fg];

        const px = col * this.cellW;
        const py = row * this.cellH;

        const ctx = this.ctx;
        ctx.fillStyle = PALETTE[bg & 0x0f];
        ctx.fillRect(px, py, this.cellW, this.cellH);

        if (ch === 0 || ch === 32)
            return;

        ctx.fillStyle = PALETTE[(attrs & ATTR_BOLD ? fg | 8 : fg) & 0x0f];
        ctx.fillText(String.fromCodePoint(ch), px, py + this.baseline);

        if (attrs & ATTR_UNDERLINE)
            ctx.fillRect(px, py + this.cellH - 1, this.cellW, 1);
    }

    // After a resize or a font change there is no damage to go on, so the whole
    // descriptor is repainted.
    repaint() {
        if (!this.info)
            return;
        const u32 = this.u32();
        const base = this.info >>> 2;
        this.present(0, 0, u32[base + S_COLS], u32[base + S_ROWS]);
    }
}
