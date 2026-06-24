# BoltOS Browser — Upgrade Audit & Plan

Goal: move the browser from an HTML *flattener* to a standards-ish engine that
renders real sites (Google, GitHub, Wikipedia, Reddit) usefully. This file is
the living plan; each iteration below ships **compilable** kernel code.

---

## 0. Hard platform limits (set the realistic ceiling)

These cannot be "fixed", they bound what "perfect" can mean:

1. **No hardware float.** Kernel builds `-mno-80387` (and JS is int64). Layout,
   color math, transforms must be fixed-point / integer. No sub-pixel AA.
2. **8×8 / 8×16 bitmap font, ASCII only.** No glyph shaping, no CJK/emoji, no
   web-font (`@font-face` woff2) rasterization. Text metrics are a fixed grid.
3. **No JS JIT, no modern bundle support.** BoltJS is a tree-walker; it cannot
   run React/Polymer/closure-compiled Google app code. Sites that render
   *entirely* client-side will never run; we scrape/server-fallback instead
   (already done for YouTube + Google SERP → DuckDuckGo html endpoint).
4. **RAM/time budgets.** Pages capped at 256 KB, images 7 MB resident, 14 s
   image budget. Big SPA bundles (2–8 MB JS) won't fully load.

**Therefore "render Google.com exactly like Chrome" is not literally reachable.**
What *is* reachable and is the real target: correct **box-model block/inline
layout**, a real **DOM tree** with query selectors + mutations + events, a real
**CSS cascade** (specificity/inheritance), `rgb()/hsl()` colors, flexbox
approximated, cookies, and the static/SSR parts of modern pages laid out
faithfully. We push as far up that curve as the hardware allows.

---

## 1. Limitation inventory (every gap, by subsystem)

### HTML
- **No DOM tree** — `html_parse` emits a flat run list. Blocks: querySelector,
  mutations, event bubbling, real layout tree, JS DOM API. *(ROOT cause of most
  breakage.)*
- No `querySelector/All`, no `getElementsByClassName`, only id→first-run.
- Forms: GET only, no POST, no method/enctype, no checkbox/radio/select state
  collected on submit (only text/textarea names are serialized).
- Tables flattened to 2-space-separated inline text — no columns/rows.
- `<svg>`, `<canvas>`, `<video>`, `<iframe>` ignored.

### CSS
- Selector reduced to **rightmost simple selector only** → `nav ul li a` matches
  any `a`. No descendant/child/sibling combinators, no `:hover/:nth-child`, no
  attribute selectors, no specificity weighting beyond tag<class<id pass order.
- **`rgb()/rgba()/hsl()/hsla()` not parsed** → returns HCOL_NONE → colors fall
  back to defaults. Google's CSS is ~all `rgb()`. *(High-impact, cheap.)* ✅ iter1
- Only ~6 properties: color, background, text-align, font-weight, font-style,
  display/visibility. **No margin, padding, border, width/height, position,
  float, overflow, flex, grid, transform, font-size(real), line-height.**
- No `@media` (skipped), no `@font-face`, no `@keyframes`/animation.
- No external stylesheet loading (`<link rel=stylesheet>` ignored).
- No inheritance model beyond the format stack; no `inherit/initial/var()`.

### Layout
- Line-based word wrap only. **No block formatting context**, no box widths, no
  margins collapsing, no positioning, no stacking, no overflow/scroll regions,
  no flex/grid, no float, no z-order. Everything is one top-to-bottom column.
- Text measurement = fixed glyph grid (ok for the bitmap font).

### JavaScript
- No DOM tree binding (only id→text get/set, document.write append).
- **No timers** (setTimeout/setInterval), no `requestAnimationFrame`.
- **No events** (addEventListener, click/input/submit dispatch).
- **No fetch / XMLHttpRequest**, no Promise/async, no JSON over network.
- No localStorage/sessionStorage, no `document.cookie`.
- Numbers int64 → no `0.5`, breaks math-heavy scripts.

### Networking
- **No cookie jar** → logins, sessions, consent walls, anti-CSRF all fail; some
  sites redirect-loop. ✅ iter1
- No `deflate`/`br` content-encoding (only gzip). ✅ iter1 (deflate; br N/A)
- HTTP/1.x only, no HTTP/2/3 (fine functionally, slower).
- No response cache / conditional requests (ETag/If-None-Match).
- No CSP awareness (we don't enforce; mostly irrelevant to a reader).
- Redirect handling capped + a bit ad-hoc across http_get vs fetch.

### Rendering
- No compositing/layers, no GPU accel for page paint (GPU driver exists but
  unused by browser), no damage-rect repaint (full redraw each frame).
- Image decode: PNG/JPEG/GIF/BMP ✅ (good). No WEBP/AVIF/SVG.
- Font: bitmap blit, no AA, no fallback faces.

---

## 2. Upgrade plan (dependency order)

Each phase is independently buildable and improves real pages.

- **iter1 (this commit): cheap high-impact wins, zero new architecture**
  - `rgb()/rgba()/hsl()/hsla()` + `transparent` + 60 more named colors in
    `parse_color_token` (html.c). Fixes color of nearly every modern page.
  - HTTP **cookie jar** (net/http.c + net/cookies) — send/store per host.
  - HTTP **deflate** content-encoding (net/http.c, uses existing inflate_raw).
  - CSS **font-size px→scale** + recognise margin/padding (no crash, used later).

- **iter2: real CSS cascade** — selector specificity (a,b,c), descendant/child
  combinators, attribute & `:nth/:hover` (static), inheritance pass. Keep the
  flat run list as output for now.

- **iter3: DOM tree** — new `dom.c`: `dom_node{tag,attrs,children,...}`. Parse
  into a tree; derive the run list *from* the tree (compat shim). Unlocks
  querySelector, mutations, events.

- **iter4: box-model block/inline layout** — `layout.c`: layout tree with
  margin/padding/border/width, block + inline boxes, real reflow. Replace the
  ad-hoc line wrapper.

- **iter5: flexbox (single-line + wrap)**, then a grid approximation.

- **iter6: JS platform** — timers (PIT-driven), event dispatch, `fetch()` via
  http stack + microtask Promise, `localStorage` (ramfs-backed),
  `document.cookie`.

- **iter7: position/overflow/scroll containers, transforms (integer),
  animations (PIT tick driven).**

---

## 3. Status log
- **iter1: DONE + verified.** rgb()/rgba()/hsl()/hsla() + transparent + ~65 named
  colors (kernel/html.c parse_color_token, integer HSL→RGB); cookie jar
  (net/http.c, send Cookie: + store Set-Cookie per host); deflate Content-Encoding
  (net/http.c via inflate_raw); font-size px/em/%/keyword → text scale 1..3
  (css_decl_apply→css_rule→css_match→format stack→layout). HTTP requests upgraded
  to 1.1. Build clean. Verified in QEMU: welcome page renders (colors/italic/links
  ok), `browse https://example.com` → HTTP 200 559B parsed.
  Test harness: `shot.py` (GUI via QMP rel-mouse + screendump→PNG; double-click
  flaky), `testnet.py` (deterministic serial-shell network test).
- **iter2a: DONE + verified.** External stylesheet loading: parser collects
  `<link rel=stylesheet href>` (html.c, doc->csslinks); browser fetches them
  (keep-alive session, redirect hop, local file), concatenates, and re-parses via
  new `html_parse_ext(html, css)` which pre-seeds the rule set (app_browser.c
  load_external_css). Verified: bundled /web/styled.html renders blue centered
  32px H1, 1.6em paragraph, green hsl() line, #f1f3f4 card bg, rebeccapurple
  anchor — ALL from the separate site.css. Screenshot harness fixed: do NOT add
  usb-kbd (steals key routing from PS/2 which the GUI reads).
- iter2b: NEXT — CSS cascade depth: specificity weights (a,b,c), descendant/child
  combinators (needs ancestor stack — best done with the iter3 DOM tree),
  inheritance pass, @import, @media (width-based).
- **iter3: DOM tree DONE + verified.** New `kernel/dom.c` + `include/dom.h`: real
  node tree (parent/child/sibling links), tokenizer + tree builder with implicit
  -close rules (p/li/tr/td/option/dd/dt, block-closes-p), void + self-close +
  raw-text (script/style/textarea/title) handling, attribute lists, bump arena.
  Selector engine: type/.class/#id/universal, compound (`p.note`), descendant
  (` `) and child (`>`) combinators, selector lists (`,`) — `dom_query`,
  `dom_query_all` (querySelectorAll), `dom_get_by_id`, `dom_by_tag`,
  `dom_by_class`. Mutations: create/append/insert_before/remove/set_text/attr_set.
  Serialization: `dom_inner_text`/`dom_inner_html`. Shell cmd `domq URL|FILE SEL`
  for deterministic testing. Verified: `.card p`→1 (not all 4 `p`), `p.note`→1,
  `div>p`→1, tag/class/id all correct.
- **iter4: CSS cascade + box-model layout engine DONE + verified.** New
  `kernel/layout.c` + `include/layout.h`: full `css_color` (#hex/rgb/rgba/hsl/
  hsla/named/transparent); stylesheet parser → rules with specificity
  (dom_specificity); per-element computed style via cascade (selection-sorted by
  spec,order) + inheritance (color/font/align inherit; box props don't) +
  intrinsic tag defaults + inline `style=""` (highest); box tree build (skips
  display:none); block layout (vertical stacking, margins, width/padding/border
  → border-box resolution, nesting/containing-blocks) + inline layout (text word
  -wrap + inline elements flow, text measured on 8px grid). Exposed
  `dom_matches`/`dom_specificity`. Shell cmd `domlayout URL|FILE`. Verified
  /web/box.html: `.panel` x20 y20 w436(=400+16*2+2*2) bg#eef; nested `.inner` p
  x38 y50(=38+12mt) col=rgb(200,0,0) — every box edge + colour exact.
- **iter5: Flexbox DONE + verified.** layout.c `layout_flex`: row/column,
  flex-grow (distribute remaining main space, re-layout grown items), gap,
  justify-content (start/center/end/space-between/space-around), align-items
  (start/center/end), max-content flex-basis for auto-width items (`maxc_w`),
  subtree translate (`shift_box`). Verified /web/flex.html: `.a` w100, `.b`
  flex:1→w560 (=760-100-80-2*10gap), `.c` x680 w80, gap+space-between correct.
- **iter5c: CSS Grid DONE + verified.** layout.c `layout_grid` + `parse_tracks`:
  grid-template-columns with px + fr tracks + `repeat(n,..)`, gap, row-major
  placement, row wrapping, auto row heights. Verified /web/grid.html
  (`100px 1fr 1fr`, 6 items): cols 100/320/320, two rows at y0/y28, x0/x110/x440.
  Layout engine now covers block + inline + flex + grid.
- **iter5b: layout engine wired into the GUI renderer DONE + verified.**
  app_browser now parses each page into a DOM tree (`build_boxes`), collects
  inline `<style>` + external `<link>` CSS (`collect_css`), runs the cascade +
  box/flex/grid layout (`ensure_layout`, rebuilt on width change = responsive),
  and paints the box tree (`paint_box`/`paint_text_box`): backgrounds, borders,
  wrapped text (proportional metric shared with layout via
  `layout_set_text_measurer`), images (fetched+cached via `box_image`), inputs,
  links (hit-tested through `<a>` ancestry). Flat run-list path kept as fallback
  (YouTube scrape, plain-text files, build failure). Verified in GUI:
  styled.html renders box-model card background band + cascade colors + 32px H1;
  grid.html renders the 3-col × 2-row grid on screen.
- **iter6: JS↔DOM platform (sync parts) DONE + verified.** js_host extended
  (js.h) with DOM-tree ops + storage + cookies; js_builtins.h wires
  querySelector → host->query (full selector engine), createElement, node
  .setAttribute/.getAttribute/.appendChild, el.id/className/href/src/value get+set,
  document.cookie get/set, localStorage/sessionStorage getItem/setItem/removeItem.
  app_browser jd_* callbacks back these with the real DOM tree (st->bdom; mutations
  set laid_w=-1 → reflow), an in-RAM localStorage table, and the http cookie jar
  (http_cookie_get/set). Verified /web/js.html: querySelector+textContent mutated
  a node; createElement+setAttribute(class)+body.appendChild inserted a node that
  picked up its `.made` CSS rule; localStorage round-tripped (hits=7).
  NOT done (need a persistent VM + event loop, js.c is one-shot arena): events
  (addEventListener/dispatch), timers (setTimeout/setInterval), fetch + Promises.
- **iter6b: persistent JS VM + event loop DONE + verified.** `js_vm_*` (js.c)
  keeps the interpreter (arena + global env + timer/listener registries) alive
  across the page lifetime. app_browser creates the VM in `run_scripts`, evals
  each `<script>` into it, `js_vm_pump(pit_ticks())` on tick fires due timers,
  and a click hit-test calls `js_vm_dispatch(node,"click")`. Builtins:
  addEventListener (document/window/element), setTimeout/setInterval/raf +
  clearTimeout/clearInterval. **fetch() + Promises DONE + verified** (this pass):
  a microtask FIFO on the Interp drained after every eval/pump/dispatch;
  `fetch(url)` performs a synchronous GET via a new `js_host.fetch` callback
  (app_browser `jd_fetch` = http_open/http_fetch with one redirect+retry, or a
  local file; cmd_js `cli_fetch` = http_get) and settles a Promise with a
  Response `{status, ok, url, text(), json()}`. Promise: `.then/.catch/.finally`
  with value+promise chaining, `Promise.resolve/reject/all`, `new Promise(exec)`
  (N_NEW now also invokes K_NATIVE constructors; resolve/reject bound via the
  native `dom`→bound_this path). `JSON.parse` upgraded from a stub to a real
  recursive parser (objects/arrays/strings/int/bool/null, depth-guarded). Verified
  over the serial shell: `js /home/promise.js` → correct microtask ordering
  (sync before async; then-chaining `then1=10`→`then2=15`; `caught=boom`;
  `all=123`; `ctor=made`; `json=42,hi,2,true`); `js /home/fetch.js` → live GET of
  example.com prints `status=200 ok=true` then `bodylen=559`.
  Numbers stay int64 (no float); `Math.random` still 0.
- iter7: positioning (relative/absolute), overflow/scroll containers, transforms
  (integer), animations (PIT-tick), HTTP/2, response cache, CSP.
</content>
</invoke>
