# Faceplates

A unit's plugin can have its own user interface: a **faceplate**, one HTML page with its CSS and
JavaScript inline. configui shows it in place of the generic faders on its Parameters panel, on a
phone, a tablet or a laptop, and metalhead.link's store runs it as a live demo. A faceplate is
responsive (the frame is as wide as the panel it sits in; height is up to the faceplate), and
it reaches the unit only through the `metalhead` object described here.

`metalhead-essentials` has six faceplates and the kit they share (`faceplates/kit.js`: knobs,
switches, selectors, meters, canvas displays), and `tools/build-store.py`, which checks and
packages them. Start from one of those.

## Where it runs

configui (`host/configui/unit-ui.js` in metalhead-firmware) loads `unit-frame.html` into an
`<iframe sandbox="allow-scripts">` and sends it the faceplate's HTML. That page puts the HTML into
itself, runs its scripts, and gives them `window.metalhead`. So a faceplate:

- has an **opaque origin**: no cookies, no `localStorage`/`IndexedDB` (they throw), no popups,
  forms, `alert()`, or navigating the page around it;
- has **no network** at all (served by metalhead-connect, the frame's Content-Security-Policy is
  `default-src 'none'` with inline scripts and styles, `data:`/`blob:` images, fonts and media):
  put images in as `data:` URIs or draw them;
- gets **only its own unit**: its parameters, their values and its levels. Not the device, the
  config, other units, the room or who is looking.

configui checks everything a faceplate sends: known messages only, its own parameters only,
values clamped to their range (and whole for stepped ones), at most 120 messages a second. A
faceplate that floods, doesn't call home within 8 seconds of loading, or stops answering (a
loop that never ends) is removed, and the generic controls come back. The person can always
switch to the generic controls themselves.

## The `metalhead` object

```js
const info = await metalhead.ready;   // once configui has said hello
info.unit       // { name: "eq", type: "EQ", vendor, version, description, inputs, outputs }
info.params     // [{ id, name, module, flags, min, max, default }], hidden ones left out
info.values     // { name: { value, text } } as last reported (text is the unit's paramValueToText)
info.enabled    // false while the unit isn't running (not written yet, device offline)
info.demo       // true in the store's demo: values are simulated
info.theme      // { mode, colors: {...}, fonts } — also applied as CSS variables --mh-<color>

metalhead.set('midgain', 3.5)          // a plain value in min..max; comes back as a 'param' event
metalhead.trigger('kick', 0.8)         // a trigger parameter, with a velocity (default: max)
metalhead.touch('midgain', true)       // a gesture starts; false when it ends (see below)
metalhead.meters(true)                 // ask for 'meters' events
metalhead.log('text')                  // to configui's console (200 characters, a few a second)
metalhead.value('midgain')             // { value, text } or null
metalhead.param('midgain')             // the description, or null
metalhead.flags                        // STEPPED, PERIODIC, HIDDEN, READONLY, BYPASS, AUTOMATABLE, ENUM, TRIGGER

metalhead.on('param', (name, { value, text, remote }) => {})   // remote: not this faceplate's change
metalhead.on('state', ({ enabled, reason }) => {})
metalhead.on('touch', (name, on) => {})                        // someone else's finger on it
metalhead.on('meters', ({ inputs, outputs }) => {})            // dBFS per channel, null if unknown
metalhead.on('theme', (theme) => {})
```

- **Values** are plain, in each parameter's own unit, as in `UnitParamInfo` (a frequency in Hz,
  a time in ms). The text of a value is the unit's own (`paramValueToText`): show it when you can.
- **Touch** brackets a gesture (a knob being turned, a dot dragged). While it lasts, values read
  back from the device don't move the control under the finger, and through metalhead.link the
  others in the room see who has it.
- **Set** as often as the finger moves; configui sends the device the latest value and drops the
  ones in between. The kit sends at most once per animation frame.
- **Read-only parameters** (`UNIT_PARAM_IS_READONLY`, like the Compressor's gain reduction) arrive
  as `param` events when configui reads them, a few times a second over USB.
- **Meters** come at about 15 a second while the live view runs (over USB, or metalhead.link),
  one value per input and output channel of the unit, in dBFS, `null` for silence or unknown.
- The frame's **height** follows the page: configui watches the document's size. Don't make the
  page's height depend on the frame's (`100vh`), or it grows until configui stops following.

## Responsive

Assume anything from 320 to 1200 pixels wide. Media queries inside the frame see the frame's
width, not the screen's, so they work like container queries. Use pointer events (they cover
mouse, touch and pen), `touch-action: none` on controls you drag so the page doesn't scroll
instead, and targets of 30 pixels or more. Double-tap is the touch equivalent of a double-click.

## Packaging

A faceplate is one file, `<Plugin>.html`, at most 256 KB with everything inlined. For the store,
`tools/build-store.py` in metalhead-essentials inlines `kit.css`/`kit.js`, checks that every
`param="..."` names a real parameter of the plugin (from `make describe`), and writes
`faceplates/<Plugin>.html` next to the unit files and `catalog.json`.

`make describe` (tools/describe.c) prints a unit file's plugins and parameters as JSON, with
`texts`: the unit's own text for values across each parameter's range. The store's demos use
them to show "1.25 kHz" without a device.
