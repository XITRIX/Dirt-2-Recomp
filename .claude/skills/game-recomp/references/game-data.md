# Patching game data and menus (without touching the game folder)

## Rules

- **Serve patched copies through VFS overrides.** Generate them at start-up into `<data>/overlay` from the user's original files and register them with `vfs::set_override(relative_path, host_file)`. Make it **all or nothing**: half a patch can show wrong options.
- **Generate at run time rather than ship patched files.** Users have different game versions, the original stays the input, and the patch can depend on the device (for example, hide a row when MetalFX isn't supported).
- **Prefer data over code.** Look for the data that drives a feature before reverse-engineering its code. In DiRT 2, menus, combo options, presets and the hardware config were all data.

## Compiled UI XML: the DiRT 2 `states.bin` example

Codemasters' compiled frontend XML. The same approach works for any binary XML: decode it, prove the round trip is byte-identical, then edit.

- **Chunks:** a u32 tag `0x7252221x` plus a u32 size.
  - `0x1A` is the file. It contains `0x17` = {`0x1D` strings, NUL-terminated and padded to 4; `0x1E` u32 offsets}, then `0x1B` nodes and `0x1C` attributes.
- **Node record:** 6×u32 (name, text, attr_count, first_attr, child_count, first_child).
  - `text` is the string index of the node's character data (0 for none).
  - `first_attr` is 0 when there are no attributes.
- **Layout:** nodes are visited depth first. A visited node's children take the next contiguous block of slots, and a leaf's `first_child` equals the slot cursor at its visit. Attributes are in visiting order. String order is the original order, with new strings appended.
- **Tool:** `tools/scripts/states_bin.py dump <file> [name|id=VALUE]` prints the XML; `roundtrip` checks that encode(decode(x)) == x. It took three attempts to get the slot layout right (breadth first was wrong). Prove the round trip before trusting edits.
- **Menu semantics:**
  - a row's position is its slot number, written three times: `glyph="N.root.library"`, `IBScreenGenericData index="N"` and `IBGlyphLink linked_glyph="N.root.library.pill"`;
  - pad order is the `itemflow` node's text, **one id per line**, which the game splits on line breaks;
  - visibility widgets such as `IBDX11OnlyVisibility dx_11_only="true"` hide rows on a code path;
  - `IBHWConfigBasedCombo data="pc_gfx_*"` binds a row to a hardware-settings category by name: the widget registers under its `data` name, and the code side collects it.

## Adding a settings row the cheap way

The approach that worked:

1. **Repurpose a row the port can't use.** Here: DX11-only ambient occlusion, hidden on the DX9 path. It comes with layout, glyphs, combo machinery and persistence for free. Adding a 28th row would have needed a layout glyph that doesn't exist.
2. **Unhide it, retitle it and move it** by renumbering slots: the moved row takes slot k, rows k..old−1 shift down one, and its id moves in `itemflow`. Keep the item ids.
3. **Redefine its options** in the options XML. Name them `off` and `on` to get the game's own Off/On labels, as other rows do, and remove the row's category from the detail presets so a preset doesn't flip it.
4. **Make the options match the saved element exactly.** In practice the combo showed the wrong value until every attribute matched: the saved `<dynamic_ambient_occ enabled="true" quality="1">` matched neither `on` nor `off` until the options carried `quality="0"` too. Normalise the saved file and the first-run defaults to match.
5. **Read the value on the host** from the saved settings file, since the renderer never reads that element. Watch the file (an mtime check once a second) to pick up changes.
6. **Title text:** the language file was Huffman-coded, so instead of re-encoding it, hook the localisation lookup (`thiscall(table, const char* id) → char*`) and answer the new id. Match the menu's style (capitals).

## Hardware and settings files

- Find the categories, option fragments and presets in `hardware_settings_options.xml`; the defaults in `system/hardware_settings_config.xml` are copied to the user config at first run. A restrictions file can disable options on some hardware.
- `normalise_resolution`-style fix-ups run before the game reads the user config: rewrite values the port doesn't offer.

## Language files (Codemasters `.lng`)

- These are chunked: `LNGT` header, `HSHS`/`HSHT` hash tables, `SIDA`/`SIDB` string ids, `LNGB` with Huffman `TREE` blocks. Not plain text.
- A missing id is shown as `<id>`, from the format `"<%s>"`. Several functions share that format, including an XML tag writer, so hook the lookup instead of the format.
