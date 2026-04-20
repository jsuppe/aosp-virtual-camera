---
description: Query Android.bp modules — find, list deps, reverse deps, defaults chain
allowed-tools: Bash, mcp__zoekt-aosp__search_code, mcp__zoekt-aosp__read_file, mcp__zoekt-aosp__grep_code
---

Query AOSP Soong (Android.bp) build modules using zoekt for file discovery and reading.

## Commands by argument

### Find a module: `/soong libcamera_metadata`

1. Search zoekt: `search_code('name: "libcamera_metadata" f:Android.bp', tree)`
2. Read the Android.bp file containing the match via `read_file`
3. Extract the full module block (from `cc_library {` to matching `}`)
4. Display: module type, file path, full block

### List modules in dir: `/soong list hardware/interfaces/camera`

1. Search zoekt: `search_code('f:hardware/interfaces/camera/Android.bp name:', tree)`
2. List all `name: "..."` entries with their module type

### Show deps: `/soong deps libvcam`

1. Find module (as above)
2. Extract these fields from the block:
   - `defaults:` — inherited defaults
   - `srcs:` — source files
   - `static_libs:` — static dependencies
   - `shared_libs:` — shared dependencies
   - `header_libs:` — header dependencies
   - `include_dirs:` / `local_include_dirs:`
   - `cflags:`
3. Display structured output

### Reverse deps: `/soong rdeps libcamera_metadata`

1. Search zoekt: `search_code('"libcamera_metadata" f:Android.bp', tree)`
2. For each hit, identify the module that references it and which field (static_libs, shared_libs, etc.)
3. Display: `<consumer_module> via <field> at <file>`

### Resolve defaults chain: `/soong defaults libvcam`

1. Find module, extract `defaults: [...]`
2. For each default, find its `cc_defaults` block
3. Follow chain until no more `defaults:` references
4. Display chain with inherited fields at each level

## Blueprint parsing tips

- Module blocks: `<type> { ... }` at column 0, braces balance
- String lists: `["a", "b"]`, may span multiple lines
- `select()` expressions: skip, note as "conditional"
- Comments: `//` line, `/* */` block — ignore when parsing
- `+` operator concatenates lists — note but don't evaluate

## Default tree

Use `a15` unless user specifies otherwise.
