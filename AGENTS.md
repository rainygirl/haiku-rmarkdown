# R Markdown — development notes

## Styling as one run array, not one call per span

`BTextView` relays out the whole document on every styling call, so applying
a style per span costs a full relayout per span and is unusable on slow
hardware. The parser fills one style byte per character over a flat array,
that array is compressed into a `text_run_array`, and `SetRunArray()` is
called once for the whole document.

Working on a flat array also keeps the rules independent: a block rule can
paint a whole line and an inline rule can overwrite part of it without either
having to know about run boundaries.

The pass itself is deferred with a one-shot `BMessageRunner` (180 ms), so a
burst of typing collapses into a single pass once it stops.

## Input methods and the restyle pass

Restyling replaces the run array for the whole document, and `BTextView`
keeps the half-finished syllable of an input method as inline text with a
style of its own. Rewriting every style underneath it destroys that state:
with a Korean keyboard the composing character was dropped or frozen on
whichever keystroke happened to fall after the restyle timer — which reads as
"Korean input does not work" rather than as a styling bug.

`MarkdownView::MessageReceived()` watches `B_INPUT_METHOD_EVENT` and holds
the pass back between `B_INPUT_METHOD_STARTED` and `B_INPUT_METHOD_STOPPED`,
then runs it once.

## Loading a file is not an edit

`LoadText()` sets the text with the restyle suppressed and then restyles
directly. Going through the deferred path would mark the document modified,
so opening a file flagged it dirty before the user had touched anything.

## The application thread may not touch the window's views

Files arrive as command-line arguments and as `B_REFS_RECEIVED`, both on the
application thread. Calling into the window's `BTextView` from there trips
`Debugger call: "Looper must be locked."` — every such path goes through
`_OpenLocked()`, which takes the window lock first.

## Two palettes for the themes, not one inverted

The dim colour used for syntax characters has to stay legible against its own
background. Inverting the light theme makes those characters vanish into a
dark page, so dark mode has its own palette.

The status strip is a `BView` of its own rather than a bare `BStringView`: a
`BStringView` only paints behind its own text, so in dark mode the panel
colour showed through everywhere around it.
