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

Missing-docstring checks, D100 through D107, are temporarily disabled until
missing documentation is added in the next Python style PR. That PR removes
the D1 exemption. Existing docstring layout checks already run in CI. A whole
file or test directory must not be exempted simply to avoid writing useful
explanations.

## Generated and embedded code

Keep firmware payloads, protocol values and exact upstream search strings
unchanged during formatting. Python strings containing JavaScript, HTML, YAML,
C++ or assembly are data to the formatter; changing their contents needs a
separate review and the relevant generator or component tests.

Edit generators or source assets instead of generated headers. Preserve the
exact include wrappers under `lib/desk_protocol/include/desk_protocol`.
Formatter checks exclude ignored dependencies, caches and build outputs.
`.editorconfig` provides editor defaults without normalizing assembly, binary
fixtures or generated and hash-bound files.

Keep unrelated formatting and behavior changes out of the same PR. When a
style rollout includes lint corrections, review its pure formatting diff and
its other changes separately. Other languages retain their existing style.
