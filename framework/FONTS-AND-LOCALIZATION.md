# Shared text, localization and brand fonts

These services belong to Cell SDK and can be consumed by every plugin. They do not change the operating system or C numeric locale. Audio state, pitch/time values and file formats retain locale-independent numeric encodings.

`cell::Text` owns UTF-8 text. Iteration and substring indexes use Unicode codepoints, including safe progress over malformed input. Case comparison uses generated Unicode15.0.0 mappings independently of the host's locale, including expansions such as sharp S. Natural sorting compares numeric runs without narrowing long numbers. Original text is preserved; normalization is not applied to track names or paths.

`cell::TextList` is a dynamically sized, owning collection. It supports UTF-8 lines, quoted search tokens, stable natural order and case-aware deduplication. Native text/list code does not retain foreign framework objects.

`cell::i18n::Catalog::fromJson` loads a validated, immutable language resource. `Translator` selects an explicit language, its parent tags, then a fallback such as English. Keep a separate translator snapshot per editor/plugin. A change in one plugin cannot alter another plugin's locale. Load catalogs outside painting and audio processing. `view()` borrows text for rendering; its translator/catalog and caller-provided fallback must outlive the borrow. `text()` returns an owning value.

```json
{
  "language": "fr-CA",
  "direction": "ltr",
  "numbers": {"decimal": ",", "group": " ", "grouping": 3},
  "messages": {
    "transport.play": "Lecture",
    "track.greeting": "Bonjour, {name} !",
    "notes.count": {"one": "{count} note", "other": "{count} notes"}
  }
}
```

Named placeholders allow translators to change word order. Invalid placeholder syntax is rejected; a translation whose placeholders differ from its source fallback is not applied. Double braces produce literal braces. `prepare()` compiles a binding outside painting, so repeated formatting does not parse the template or perform a global lookup. Integer cardinal rules cover one/other, other-only, French, Slavic, Polish, Czech/Slovak, Arabic and Hebrew cases; this is not full fractional CLDR/ICU MessageFormat support. Extend and test the rule set when adding a language that needs another rule.

`NumberSymbols`, `formatInteger`, `formatDecimal` and `parseDecimal` provide explicit per-language digits/separators. Invalid or partially numeric input is rejected. Consumer controls must use the corresponding parser when displaying localized values. Machine state serializers must keep their locale-independent encodings.

The renderer and widget `Font` share `Typeface` roles. Interface text remains Inter, timeline/value text can use the existing monospace role, and logos can select `brandGrotesk` or `brandCondensed`. Measurement, clipping and drawing use the same selected face. The brand faces are bundled and share the existing prepared font cache across editor instances within a loaded module. `assets/BRAND-FONTS.json` records the sources and licenses. No font installation or network request is required at runtime.

`Canvas::missingGlyphs` reports unsupported characters before shipping a language pack. A zero missing-glyph count does not prove correct text shaping. Full bidi/layout, complex-script shaping, grapheme editing, platform IME input, language-font fallback and complete product string extraction still need integration and native qualification. The catalog's RTL direction is metadata for that integration. This checkpoint does not qualify a complete foreign-language release. English remains the current product UI until translated resources are integrated and tested.
