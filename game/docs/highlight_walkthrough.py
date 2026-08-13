#!/usr/bin/env python3
"""Generate and validate static C++/GLSL highlighting in walkthrough.html."""

from __future__ import annotations

import argparse
import html
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass
from html.parser import HTMLParser
from pathlib import Path


GAME_DIR = Path(__file__).resolve().parents[1]
WALKTHROUGH = Path(__file__).with_name("walkthrough.html")
TOKEN_CLASSES = frozenset({"pp", "cm", "st", "kw", "ty", "nu", "fnc"})
SOURCE_SUFFIXES = frozenset({".cpp", ".h", ".vert", ".frag", ".comp"})
# The Bruneton port includes two large verbatim upstream GLSL sources plus a
# tightly coupled group of generated precompute stages. The walkthrough's
# overview documents that subsystem as a unit instead of duplicating thousands
# of BSD-licensed source lines into this already self-contained HTML artifact.
WALKTHROUGH_SOURCE_EXCLUSIONS = frozenset({
    "render/bruneton_atmosphere_pass.h",
    "render/sky_atmosphere_dirty.h",
    "data/shaders/bruneton_atmosphere.h",
    "data/shaders/bruneton_definitions.h",
    "data/shaders/bruneton_direct_irradiance.frag",
    "data/shaders/bruneton_functions.h",
    "data/shaders/bruneton_indirect_irradiance.frag",
    "data/shaders/bruneton_multiple_scattering.frag",
    "data/shaders/bruneton_parameters.h",
    "data/shaders/bruneton_scattering_density.frag",
    "data/shaders/bruneton_single_scattering.frag",
    "data/shaders/bruneton_transmittance.frag",
    "data/shaders/bruneton_precompute.vert",
})

ROW_RE = re.compile(
    r'(?P<prefix><tr id="[^"]+" data-path="(?P<path>[^"]+)" '
    r'data-line="(?P<line>\d+)"><td class="ln"><a href="[^"]+">\d+</a>'
    r'</td><td class="src">)(?P<body>.*?)(?P<suffix></td></tr>)'
)
OPEN_SPAN_RE = re.compile(r'<span class="(?P<class>pp|cm|st|kw|ty|nu|fnc)">')
IDENT_RE = re.compile(r'[A-Za-z_$][A-Za-z0-9_$]*')
NUMBER_RE = re.compile(
    r'(?:'
    r'0[xX][0-9A-Fa-f](?:\'?[0-9A-Fa-f])*'
    r'|0[bB][01](?:\'?[01])*'
    r'|(?:\d(?:\'?\d)*)?\.\d(?:\'?\d)*(?:[eE][+-]?\d(?:\'?\d)*)?'
    r'|\d(?:\'?\d)*(?:[eE][+-]?\d(?:\'?\d)*)'
    r'|\d(?:\'?\d)*'
    r')'
    r'(?:[uU](?:ll|LL|l|L)?|(?:ll|LL|l|L)[uU]?|[fFhH])?'
)

CPP_KEYWORDS = frozenset(
    "alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept "
    "auto bitand bitor break case catch class "
    "compl concept const consteval constexpr constinit const_cast continue co_await "
    "co_return co_yield decltype default delete do dynamic_cast else enum "
    "explicit export extern false for friend goto if inline mutable "
    "namespace new noexcept not not_eq nullptr operator or or_eq private protected "
    "public reflexpr register reinterpret_cast requires return sizeof "
    "static static_assert static_cast struct switch synchronized template this "
    "thread_local throw true try typedef typeid typename union using virtual "
    "volatile while xor xor_eq override final".split()
)
CPP_TYPES = frozenset(
    "bool char char8_t char16_t char32_t double float int long short signed unsigned "
    "void wchar_t size_t ptrdiff_t intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t "
    "uint16_t uint32_t uint64_t i8 i16 i32 i64 u8 u16 u32 u64 f32 f64 b8 "
    "string string_view optional vector array span map unordered_map pair tuple "
    "unique_ptr shared_ptr weak_ptr function filesystem path FILE VkBool32 VkFlags "
    "VkDeviceSize VmaAllocation".split()
)
GLSL_KEYWORDS = frozenset(
    "attribute break buffer case centroid coherent const continue default discard do "
    "else flat for highp if in invariant inout layout lowp mediump noperspective out "
    "patch precise readonly restrict return sample shared smooth struct subroutine "
    "switch uniform varying volatile while writeonly true false".split()
)
GLSL_TYPES = frozenset(
    "void bool int uint float double atomic_uint "
    "bvec2 bvec3 bvec4 ivec2 ivec3 ivec4 uvec2 uvec3 uvec4 vec2 vec3 vec4 "
    "dvec2 dvec3 dvec4 mat2 mat3 mat4 mat2x2 mat2x3 mat2x4 mat3x2 mat3x3 mat3x4 "
    "mat4x2 mat4x3 mat4x4 dmat2 dmat3 dmat4 dmat2x2 dmat2x3 dmat2x4 dmat3x2 "
    "dmat3x3 dmat3x4 dmat4x2 dmat4x3 dmat4x4 "
    "sampler1D sampler2D sampler3D samplerCube sampler2DRect sampler1DArray "
    "sampler2DArray samplerCubeArray samplerBuffer sampler2DMS sampler2DMSArray "
    "isampler1D isampler2D isampler3D isamplerCube isampler2DRect isampler1DArray "
    "isampler2DArray isamplerCubeArray isamplerBuffer isampler2DMS isampler2DMSArray "
    "usampler1D usampler2D usampler3D usamplerCube usampler2DRect usampler1DArray "
    "usampler2DArray usamplerCubeArray usamplerBuffer usampler2DMS usampler2DMSArray "
    "sampler1DShadow sampler2DShadow samplerCubeShadow sampler2DRectShadow "
    "sampler1DArrayShadow sampler2DArrayShadow samplerCubeArrayShadow "
    "image1D image2D image3D imageCube image2DRect image1DArray image2DArray "
    "imageCubeArray imageBuffer image2DMS image2DMSArray iimage1D iimage2D iimage3D "
    "iimageCube iimage2DRect iimage1DArray iimage2DArray iimageCubeArray iimageBuffer "
    "iimage2DMS iimage2DMSArray uimage1D uimage2D uimage3D uimageCube uimage2DRect "
    "uimage1DArray uimage2DArray uimageCubeArray uimageBuffer uimage2DMS "
    "uimage2DMSArray".split()
)


class WalkthroughError(RuntimeError):
    pass


@dataclass
class LexerState:
    block_comment: bool = False
    quote: str | None = None
    raw_terminator: str | None = None
    preprocessor_continued: bool = False


@dataclass(frozen=True)
class Token:
    text: str
    kind: str | None = None


def source_path(data_path: str) -> Path:
    relative = Path(data_path)
    if relative.is_absolute() or ".." in relative.parts:
        raise WalkthroughError(f"unsafe data-path in walkthrough: {data_path!r}")
    if data_path.startswith("data/shaders/"):
        return GAME_DIR / relative
    return GAME_DIR / "src" / relative


def language_for(data_path: str) -> str:
    return "glsl" if data_path.startswith("data/shaders/") else "cpp"


def decode_source_cell(body: str, data_path: str, line_number: int) -> str:
    without_open = OPEN_SPAN_RE.sub("", body)
    without_spans = without_open.replace("</span>", "")
    if "<" in without_spans or ">" in without_spans:
        raise WalkthroughError(
            f"unsupported HTML in {data_path}:{line_number}; only syntax spans are allowed"
        )
    if without_open.count("</span>") != len(OPEN_SPAN_RE.findall(body)):
        raise WalkthroughError(f"unbalanced syntax spans in {data_path}:{line_number}")
    return html.unescape(without_spans)


def is_project_type(identifier: str, language: str) -> bool:
    if language == "glsl":
        return identifier in GLSL_TYPES
    if identifier in CPP_TYPES:
        return True
    if identifier.startswith(("Vk", "Vma", "HMM_", "JPH_", "ImGui")):
        return True
    return len(identifier) > 1 and identifier[0].isupper() and not identifier.isupper()


def next_nonspace(line: str, offset: int) -> str:
    while offset < len(line) and line[offset].isspace():
        offset += 1
    return line[offset : offset + 1]


def string_start(line: str, offset: int, language: str) -> tuple[int, str, str | None] | None:
    if language == "cpp":
        raw_match = re.match(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(', line[offset:])
        if raw_match:
            return raw_match.end(), '"', ")" + raw_match.group(1) + '"'
        regular_match = re.match(r'(?:u8|u|U|L)?(["\'])', line[offset:])
        if regular_match:
            return regular_match.end(), regular_match.group(1), None
    elif line[offset : offset + 1] == '"':
        return 1, '"', None
    return None


def consume_quoted(line: str, offset: int, quote: str) -> tuple[int, bool]:
    escaped = False
    cursor = offset
    while cursor < len(line):
        char = line[cursor]
        cursor += 1
        if escaped:
            escaped = False
        elif char == "\\":
            escaped = True
        elif char == quote:
            return cursor, True
    return cursor, False


def append_token(tokens: list[Token], text: str, kind: str | None = None) -> None:
    if not text:
        return
    if tokens and tokens[-1].kind == kind:
        previous = tokens[-1]
        tokens[-1] = Token(previous.text + text, kind)
    else:
        tokens.append(Token(text, kind))


def highlight_line(line: str, language: str, state: LexerState) -> list[Token]:
    tokens: list[Token] = []
    cursor = 0
    first_code = len(line) - len(line.lstrip())
    preprocessor = state.preprocessor_continued or (
        not state.block_comment
        and state.quote is None
        and state.raw_terminator is None
        and line[first_code : first_code + 1] == "#"
    )

    while cursor < len(line):
        if state.block_comment:
            end = line.find("*/", cursor)
            if end < 0:
                append_token(tokens, line[cursor:], "cm")
                cursor = len(line)
                continue
            append_token(tokens, line[cursor : end + 2], "cm")
            cursor = end + 2
            state.block_comment = False
            continue

        if state.raw_terminator is not None:
            end = line.find(state.raw_terminator, cursor)
            if end < 0:
                append_token(tokens, line[cursor:], "st")
                cursor = len(line)
                continue
            end += len(state.raw_terminator)
            append_token(tokens, line[cursor:end], "st")
            cursor = end
            state.raw_terminator = None
            continue

        if state.quote is not None:
            end, closed = consume_quoted(line, cursor, state.quote)
            append_token(tokens, line[cursor:end], "st")
            cursor = end
            if closed:
                state.quote = None
            continue

        if line.startswith("//", cursor):
            append_token(tokens, line[cursor:], "cm")
            cursor = len(line)
            continue
        if line.startswith("/*", cursor):
            end = line.find("*/", cursor + 2)
            if end < 0:
                append_token(tokens, line[cursor:], "cm")
                state.block_comment = True
                cursor = len(line)
            else:
                append_token(tokens, line[cursor : end + 2], "cm")
                cursor = end + 2
            continue

        start = string_start(line, cursor, language)
        if start is not None:
            prefix_length, quote, raw_terminator = start
            if raw_terminator is not None:
                end = line.find(raw_terminator, cursor + prefix_length)
                if end < 0:
                    append_token(tokens, line[cursor:], "st")
                    state.raw_terminator = raw_terminator
                    cursor = len(line)
                else:
                    end += len(raw_terminator)
                    append_token(tokens, line[cursor:end], "st")
                    cursor = end
                continue
            end, closed = consume_quoted(line, cursor + prefix_length, quote)
            append_token(tokens, line[cursor:end], "st")
            cursor = end
            if not closed and line.endswith("\\"):
                state.quote = quote
            continue

        if preprocessor:
            next_special = len(line)
            for marker in ('"', "'", "//", "/*"):
                found = line.find(marker, cursor + 1)
                if found >= 0:
                    next_special = min(next_special, found)
            append_token(tokens, line[cursor:next_special], "pp")
            cursor = next_special
            continue

        identifier_match = IDENT_RE.match(line, cursor)
        if identifier_match:
            identifier = identifier_match.group(0)
            end = identifier_match.end()
            keywords = GLSL_KEYWORDS if language == "glsl" else CPP_KEYWORDS
            if identifier in keywords:
                kind = "kw"
            elif is_project_type(identifier, language):
                kind = "ty"
            elif next_nonspace(line, end) == "(":
                kind = "fnc"
            else:
                kind = None
            append_token(tokens, identifier, kind)
            cursor = end
            continue

        number_match = NUMBER_RE.match(line, cursor)
        if number_match:
            append_token(tokens, number_match.group(0), "nu")
            cursor = number_match.end()
            continue

        append_token(tokens, line[cursor])
        cursor += 1

    state.preprocessor_continued = preprocessor and line.rstrip().endswith("\\")
    return tokens


def render_tokens(tokens: list[Token]) -> str:
    pieces: list[str] = []
    for token in tokens:
        escaped = html.escape(token.text, quote=False)
        if token.kind:
            pieces.append(f'<span class="{token.kind}">{escaped}</span>')
        else:
            pieces.append(escaped)
    return "".join(pieces)


def source_files() -> set[str]:
    files: set[str] = set()
    for root in (GAME_DIR / "src", GAME_DIR / "data" / "shaders"):
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                if root.name == "src":
                    files.add(path.relative_to(root).as_posix())
                else:
                    files.add(path.relative_to(GAME_DIR).as_posix())
    return files - WALKTHROUGH_SOURCE_EXCLUSIONS


class WalkthroughStructureParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.section_depth = 0
        self.div_depth = 0
        self.panel_depth: int | None = None
        self.code_depth: int | None = None
        self.current_panel: str | None = None
        self.panels: dict[str, dict[str, int]] = {}
        self.line_ids: set[str] = set()
        self.duplicate_line_ids: set[str] = set()
        self.note_anchors: list[str] = []
        self.errors: list[str] = []

    def handle_starttag(self, tag: str, attrs_list: list[tuple[str, str | None]]) -> None:
        attrs = dict(attrs_list)
        classes = set((attrs.get("class") or "").split())
        if tag == "section":
            self.section_depth += 1
            data_path = attrs.get("data-file")
            if "panel" in classes and data_path and data_path != "overview":
                if self.current_panel is not None:
                    self.errors.append(f"nested source panel: {data_path}")
                if data_path in self.panels:
                    self.errors.append(f"duplicate source panel: {data_path}")
                self.current_panel = data_path
                self.panel_depth = self.section_depth
                self.panels[data_path] = {"code_blocks": 0, "tables": 0, "rows": 0}
        elif tag == "div":
            self.div_depth += 1
            if self.current_panel and "source-code" in classes:
                self.panels[self.current_panel]["code_blocks"] += 1
                self.code_depth = self.div_depth
        elif tag == "table" and self.current_panel and self.code_depth is not None:
            self.panels[self.current_panel]["tables"] += 1
        elif tag == "tr" and attrs.get("data-path") and attrs.get("data-line"):
            data_path = attrs["data-path"]
            row_id = attrs.get("id")
            if self.current_panel != data_path:
                self.errors.append(
                    f"row {data_path}:{attrs['data-line']} is outside its source panel"
                )
            if self.current_panel:
                self.panels[self.current_panel]["rows"] += 1
            if not row_id:
                self.errors.append(f"source row lacks an id: {data_path}:{attrs['data-line']}")
            elif row_id in self.line_ids:
                self.duplicate_line_ids.add(row_id)
            else:
                self.line_ids.add(row_id)
        elif tag == "article" and self.current_panel and "note" in classes:
            anchor = attrs.get("data-anchor")
            if anchor:
                self.note_anchors.append(anchor)
            else:
                self.errors.append(f"note in {self.current_panel} lacks data-anchor")

    def handle_endtag(self, tag: str) -> None:
        if tag == "div":
            if self.code_depth == self.div_depth:
                self.code_depth = None
            self.div_depth -= 1
        elif tag == "section":
            if self.panel_depth == self.section_depth:
                self.current_panel = None
                self.panel_depth = None
                self.code_depth = None
            self.section_depth -= 1


def validate_structure(document: str) -> None:
    parser = WalkthroughStructureParser()
    parser.feed(document)
    errors = list(parser.errors)
    expected_files = source_files()
    seen_files = set(parser.panels)
    if seen_files != expected_files:
        missing = sorted(expected_files - seen_files)
        extra = sorted(seen_files - expected_files)
        if missing:
            errors.append("missing source panels: " + ", ".join(missing))
        if extra:
            errors.append("unexpected source panels: " + ", ".join(extra))
    for data_path, counts in parser.panels.items():
        if counts["code_blocks"] != 1 or counts["tables"] != 1:
            errors.append(
                f"{data_path} must contain exactly one source code table; "
                f"found {counts['code_blocks']} blocks and {counts['tables']} tables"
            )
        if counts["rows"] == 0:
            errors.append(f"{data_path} contains no source rows")
    if parser.duplicate_line_ids:
        errors.append("duplicate line ids: " + ", ".join(sorted(parser.duplicate_line_ids)))
    missing_note_anchors = sorted(set(parser.note_anchors) - parser.line_ids)
    if missing_note_anchors:
        errors.append("invalid note anchors: " + ", ".join(missing_note_anchors))
    if 'class="block"' in document or "Current implementation · part" in document:
        errors.append("legacy walkthrough blocks or generated part headings remain")

    outline_match = re.search(
        r'<script type="application/json" id="outline-data">(.*?)</script>',
        document,
        re.DOTALL,
    )
    if not outline_match:
        errors.append("outline data is missing")
    else:
        try:
            outlines = json.loads(outline_match.group(1))
        except json.JSONDecodeError as error:
            errors.append(f"invalid outline data: {error}")
        else:
            missing_outline_files = sorted(expected_files - set(outlines))
            if missing_outline_files:
                errors.append("missing file outlines: " + ", ".join(missing_outline_files))
            invalid_outline_anchors = sorted(
                {
                    item.get("id", "")
                    for data_path, items in outlines.items()
                    if data_path != "overview"
                    for item in items
                    if item.get("id") not in parser.line_ids
                }
            )
            if invalid_outline_anchors:
                errors.append("invalid outline anchors: " + ", ".join(invalid_outline_anchors))
    if errors:
        raise WalkthroughError("structural validation failed; " + "; ".join(errors))


def build_highlighted_html(document: str) -> tuple[str, int, int]:
    validate_structure(document)
    states: dict[str, LexerState] = {}
    source_lines: dict[str, list[str]] = {}
    next_lines: dict[str, int] = {}
    seen_files: set[str] = set()
    row_count = 0
    token_count = 0

    def replace_row(match: re.Match[str]) -> str:
        nonlocal row_count, token_count
        data_path = match.group("path")
        line_number = int(match.group("line"))
        if data_path not in source_lines:
            path = source_path(data_path)
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                raise WalkthroughError(f"missing or unsupported source file: {data_path}")
            source_lines[data_path] = path.read_text(encoding="utf-8").splitlines()
            states[data_path] = LexerState()
            next_lines[data_path] = 1
            seen_files.add(data_path)

        expected_line = next_lines[data_path]
        if line_number != expected_line:
            raise WalkthroughError(
                f"non-sequential row for {data_path}: expected {expected_line}, got {line_number}"
            )
        lines = source_lines[data_path]
        if line_number > len(lines):
            raise WalkthroughError(f"extra walkthrough row {data_path}:{line_number}")
        embedded = decode_source_cell(match.group("body"), data_path, line_number)
        source = lines[line_number - 1]
        if embedded != source:
            raise WalkthroughError(
                f"stale walkthrough source at {data_path}:{line_number}: "
                f"embedded text does not match the workspace"
            )

        tokens = highlight_line(source, language_for(data_path), states[data_path])
        rendered = render_tokens(tokens)
        if decode_source_cell(rendered, data_path, line_number) != source:
            raise WalkthroughError(f"highlighting changed source text at {data_path}:{line_number}")
        token_count += sum(token.kind is not None for token in tokens)
        row_count += 1
        next_lines[data_path] += 1
        return match.group("prefix") + rendered + match.group("suffix")

    highlighted = ROW_RE.sub(replace_row, document)
    if row_count == 0:
        raise WalkthroughError("no source rows found in walkthrough.html")

    expected_files = source_files()
    if seen_files != expected_files:
        missing = sorted(expected_files - seen_files)
        extra = sorted(seen_files - expected_files)
        details = []
        if missing:
            details.append("missing panels: " + ", ".join(missing))
        if extra:
            details.append("unexpected panels: " + ", ".join(extra))
        raise WalkthroughError("source-file coverage mismatch; " + "; ".join(details))
    for data_path, lines in source_lines.items():
        embedded_count = next_lines[data_path] - 1
        if embedded_count != len(lines):
            raise WalkthroughError(
                f"incomplete panel for {data_path}: {embedded_count} of {len(lines)} lines"
            )
    if token_count == 0:
        raise WalkthroughError("no syntax tokens were generated")
    return highlighted, row_count, token_count


def write_atomic(path: Path, content: str) -> None:
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", newline="", dir=path.parent, delete=False
    ) as handle:
        temporary = Path(handle.name)
        handle.write(content)
    try:
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate source parity and generated highlighting without writing",
    )
    arguments = parser.parse_args()

    try:
        current = WALKTHROUGH.read_text(encoding="utf-8")
        highlighted, row_count, token_count = build_highlighted_html(current)
        second_pass, second_rows, second_tokens = build_highlighted_html(highlighted)
        if second_pass != highlighted or second_rows != row_count or second_tokens != token_count:
            raise WalkthroughError("highlighting is not idempotent")
        if arguments.check:
            if current != highlighted:
                raise WalkthroughError(
                    "walkthrough highlighting is out of date; run "
                    "python3 game/docs/highlight_walkthrough.py"
                )
            print(f"walkthrough highlighting OK: {row_count} rows, {token_count} tokens")
            return 0
        if current == highlighted:
            print(f"walkthrough highlighting already current: {row_count} rows, {token_count} tokens")
            return 0
        write_atomic(WALKTHROUGH, highlighted)
        print(f"highlighted walkthrough: {row_count} rows, {token_count} tokens")
        return 0
    except (OSError, UnicodeError, WalkthroughError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
