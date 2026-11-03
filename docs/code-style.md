# Code style

Python follows [PEP 8](https://peps.python.org/pep-0008/) and
[PEP 257](https://peps.python.org/pep-0257/), with the repository settings below.
Ruff controls formatting and lint checks. Review still covers clear names,
useful comments and understandable control flow.

## Python

Use Python 3.12 or later. Install the pinned checker in your development virtual
environment:

```sh
python -m pip install -r requirements-style.txt
```

Before completing Python changes, run the same checks as CI:

```sh
ruff format --check .
ruff check .
```

Use `ruff format path/to/changed_file.py` to apply formatting. Review lint fixes
individually and run the tests relevant to the changed code. Do not use unsafe
fixes or add suppressions just to make checks pass. The `style` CI job is also
required by release validation.

`ruff.toml` sets four-space indentation, double quotes, LF line endings and an
88-character formatting target. Long URLs and exact source strings may exceed
that target; E501 is disabled. Ruff formats Python files and checks imports,
undefined or unused names, naming conventions and existing docstring layout.
The stable formatter handles layout rules that Ruff's stable linter does not
cover. Preview rules are not enabled.

Use `snake_case` for functions and variables, `PascalCase` for classes and
`UPPER_CASE` for constants. Keep names required by ESPHome, callbacks and other
public interfaces. Do not rename an API as part of formatting.

Group standard-library, third-party and project imports. Within each group,
plain imports and `from` imports share an alphabetical order. Aliased names
from the same module are grouped in one import. The repository's
`tools`, `tests/release` and `tests/esphome` directories are local module roots.
Tests that add `tools` to `sys.path` keep their dependent imports after that
bootstrap, with a line-specific E402 explanation. Check import side effects
before moving or removing imports.

Use complete sentences in comments. Explain a constraint or non-obvious choice
rather than restating the code. Prefer explicit control flow to nested ternary
expressions. Split functions when they have separate responsibilities; do not
change behavior while applying mechanical formatting.

## Docstrings

Write a short summary of what the function or class does. Use
[Google-style sections](https://google.github.io/styleguide/pyguide.html#38-comments-and-docstrings)
when parameters, return values or exceptions need further explanation. Avoid
sections that merely repeat names or types. D417 is disabled, so Args sections
need not list every parameter.

Test docstrings describe the property or failure case being checked. CLI
module docstrings may appear in `--help`; review that output when changing them.
Preserve accurate claims about simulation, firmware bytes and hardware behavior.

CI requires docstrings for public modules, classes, functions, methods and
constructors, and checks the layout of every docstring. Describe constructors
by the state or resources they initialize. Do not exempt a whole file or test
directory just to avoid writing useful explanations.

## C and C++

Use clang-format 23.1.0. The shared helper selects tracked component and test
sources, `tools/test_reset_speed_header.cpp`, `tools/test_tb01_core.c`, and the
hand-maintained C sources and headers for RS01, the UART parser, TB01 and the
shared firmware platform under `tools/patches`. It checks the formatter version before running:

```sh
python tools/format_cpp.py --check
python tools/format_cpp.py --fix
```

Use `--clang-format /path/to/clang-format` if that version is not on `PATH`.
CI runs the same check in `native (Debug)` using the pinned LLVM toolchain.

`.clang-format` uses Google-derived layout with two-space indentation and a
100-character target. It uses an LLVM base with explicit Google settings to
keep raw-string formatting disabled. Include order, using-declaration order,
qualifiers and string contents stay unchanged. Short single-statement inline
methods may stay on one line; longer bodies, conditionals and loops are expanded.
The formatter parses C++17 syntax used by ESPHome components. Native tests
enforce C++11 compatibility for the portable protocol library.

Use descriptive `snake_case` names for functions and variables and `PascalCase`
for types. Follow the surrounding convention for constants and ESPHome members.
Preserve public names and interfaces during formatting. Keep changes to naming,
control flow and ownership separate from layout changes. Explain constraints in
comments instead of restating statements.

The helper excludes generated firmware headers, the generated platform layout,
the exact protocol include wrappers, and assembly sources under `tools/patches`.
Do not format these files by hand or edit generated output. For C++ changes, run the native
Debug and Release tests with sanitizers and the component tests affected by the
change.

Formatting must preserve generated firmware bytes and accepted image hashes.
Follow the [patch reproduction checks](validation.md#patch-reproduction) when
changing patch sources.

## Web

Use Node 26.13.0 and the Prettier version locked in
`tools/style/package-lock.json`. The helper installs dependencies and its cache
under `TMPDIR`, then formats the tracked maintenance-page source and its
JavaScript test:

```sh
python tools/format_assets.py --check --group web
python tools/format_assets.py --write --group web
```

Use `--node /path/to/node` to select the pinned runtime. CI installs its verified
Node archive for formatting; runtime tests keep their existing Node environment.
The shared Prettier settings use two spaces, a 100-character target, semicolons
and double quotes. HTML whitespace is treated as significant.

After changing the page, regenerate `ui.h` with `tools/embed_firmware_ui.py` and
run the page state and firmware-maintenance tests. Check embedded-page size and
firmware build size. Keep Python-embedded payloads and search strings unchanged;
extracting those sources requires a separate change.

## YAML and JSON

Use the same pinned Node and Prettier setup for tracked YAML and hand-maintained
JSON, including workflows, examples, schemas and manifests:

```sh
python tools/format_assets.py --check --group data
python tools/format_assets.py --write --group data
```

The npm lockfile and generated build output are excluded. Preserve YAML tags,
substitutions and lambda text. Validate ESPHome configurations and compile the
affected examples after formatting. Check release packaging when examples or
manifests change; formatting a catalog must not change its firmware hashes.

## Markdown

Use the same helper for tracked documentation:

```sh
python tools/format_assets.py --check --group markdown
python tools/format_assets.py --write --group markdown
```

The Markdown settings preserve prose wrapping where possible and leave code
blocks unchanged. Review tables, lists, links and rendered meaning. Keep the
existing concise writing style; the formatter does not check claims or wording.
Documentation-only PRs check Markdown in the lightweight CI path. Full runs check
all asset groups in the style job.

## Generated and embedded code

Keep firmware payloads, protocol values and exact upstream search strings
unchanged during formatting. Python strings containing JavaScript, HTML, YAML,
C++ or assembly are data to the formatter; changing their contents needs a
separate review and the relevant generator or component tests.

Edit generators or source assets instead of generated headers. Preserve the
exact include wrappers under `lib/desk_protocol/include/desk_protocol`.
Formatter checks exclude ignored dependencies, caches and build outputs.
`.editorconfig` provides editor defaults. Assembly, binary fixtures and the
protected firmware headers keep their existing whitespace.

Keep unrelated formatting and behavior changes out of the same PR. When a
style rollout includes lint corrections, review its pure formatting diff and
its other changes separately. Languages without a formatter configuration
retain their existing style.
