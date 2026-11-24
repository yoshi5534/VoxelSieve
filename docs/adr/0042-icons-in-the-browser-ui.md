# 0042: Icons in the browser UI

Status: accepted (2026-11-24)

## Context

The workspace (ADR 0040) and the regions (ADR 0041) gave the browser UI a clear structure, but
it still read as text: every button, layer, analysis group and card was a label, the cards
repeated the first sentence of their description, and the result of an analysis was a table. A
user scanning for "where are the pores" or "the 3D view" had to read. Icons that mean the same
thing everywhere, and large numbers for the key figures, let the eye find things first.

The UI is plain JS compiled into `vs-gui` (no bundler, no package manager at run time), so an
icon font or a component library would be a new build dependency.

## Decision

**The icons come from Lucide** (lucide-static 0.460.0, ISC licence): one consistent line style
in a 24-pixel grid, drawn with the text colour. Only the icons the UI uses are copied, as the
inner SVG markup, into `resources/ui/icons.js` with the licence text at its top; `icon(name)`
returns an inline SVG. There is no font, no network access and no vcpkg dependency; adding an
icon means copying its shapes from the same Lucide version. A node test checks that the shapes
are plain SVG shapes and that every icon the UI asks for by name exists.

**One meaning, one icon**: a kind of layer, an analysis and its group, the stage and the view
mode each have their icon wherever they appear (layer tree, cards, suggestions, file browser).
`el(tag, { icon })` puts an icon before the text; a button with only an icon keeps its `title`
as its accessible name, so screen readers and the browser walk (ADR 0038) still find it.

**Less text where it repeats**: the first sentence of an analysis and the hint of a group move
into tooltips (the full description stays under "What it does"); suggestions in the analyses
column show their reason as a tooltip; the key figures of a result are tiles with large
numbers instead of a table.

## Consequences

- Undo and redo are icon-only buttons; the other header buttons keep a short label next to the
  icon, since the icons for "new", "open" and "save as" are not self-explanatory on their own.
- New operations get an entry in `OPERATION_ICONS` (app.js); without one they show the plugin
  icon.
- Updating Lucide is a copy of the shapes again; nothing pins it but the comment in icons.js.
