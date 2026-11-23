#!/usr/bin/env python3
"""Build and check the documentation reference layer.

`docs/reference/` holds the canonical facts; `docs/reference/addresses.tsv` is
the address registry. This tool renders the registry into the pages that carry
an `<!-- BEGIN GENERATED: addresses:<section> -->` block, renders the two
documentation indexes, and checks the result:

* every relative markdown link in `docs/**/*.md` and the root documents resolves
* every registry row names an existing page and a generated section
* no address is registered twice with a different name for the same kind
* the generated blocks are current (`build` rewrites them, `check` fails)

Commands:
  build          rewrite the generated blocks and indexes
  check          verify links, registry rows and generated blocks (exit 1 on a problem)
  unregistered   list guest addresses in a handoff that the registry does not carry
  find           show one address or name, and the docs that mention it
  stats          address coverage and the most-mentioned gaps

No dependency outside the standard library.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
REF = DOCS / "reference"
REGISTRY = REF / "addresses.tsv"

FIELDS = [
    "address",
    "name",
    "kind",
    "page",
    "section",
    "note",
    "evidence",
    "session",
    "confidence",
]

KINDS = {
    "asset",
    "buffer",
    "code",
    "constant",
    "descriptor",
    "field",
    "framebuffer",
    "function",
    "global",
    "rom",
    "struct",
    "table",
    "ucode",
    "window",
}

CONFIDENCE = {"high", "medium", "low"}

BEGIN = "<!-- BEGIN GENERATED: {key} -->"
END = "<!-- END GENERATED: {key} -->"

ADDRESS_RE = re.compile(r"^0x[0-9A-Fa-f]{6,8}$")
SESSION_RE = re.compile(r"^(?:\d+(?:-\d+)?|-)$")
HANDOFF_RE = re.compile(r"HANDOFF-\d{4}-\d{2}-\d{2}(?:-session\d+)?\.md")
HANDOFF_FILE_RE = re.compile(
    r"HANDOFF-(\d{4})-(\d{2})-(\d{2})(?:-session(\d+))?\.md$"
)
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")

# Pages outside docs/reference/ that are reference material rather than a log.
# (link path, title, status, session, summary)
EXTRA_PAGES = [
    (
        "scenes.md",
        "What each screen must show",
        "living",
        "-",
        "Per-screen intent from the developer, the 25 scene types, the "
        "descriptor table and the New Game opening.",
    ),
    (
        "symbols.md",
        "Proposed symbol names",
        "living",
        "-",
        "Address to proposed name with evidence and confidence, the naming "
        "convention, and what a rename costs.",
    ),
]


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def parse_front_matter(text: str) -> tuple[dict[str, str], str]:
    """Return (metadata, body) for a page that starts with a `---` block."""
    if not text.startswith("---\n"):
        return {}, text
    end = text.find("\n---\n", 4)
    if end < 0:
        raise ValueError("unterminated front matter")
    meta: dict[str, str] = {}
    for line in text[4:end].splitlines():
        if not line.strip():
            continue
        key, _, value = line.partition(":")
        meta[key.strip()] = value.strip()
    return meta, text[end + 5 :]


def page_path(page: str) -> Path:
    """Registry `page` value -> file. `symbols` -> docs/symbols.md."""
    return DOCS / f"{page}.md"


def load_registry() -> tuple[list[dict[str, str]], list[str]]:
    errors: list[str] = []
    rows: list[dict[str, str]] = []
    if not REGISTRY.exists():
        return rows, [f"{REGISTRY.relative_to(ROOT)}: missing"]
    lines = read_text(REGISTRY).splitlines()
    if not lines:
        return rows, [f"{REGISTRY.relative_to(ROOT)}: empty"]
    header = lines[0].split("\t")
    if header != FIELDS:
        return rows, [
            f"{REGISTRY.relative_to(ROOT)}: header is {header!r}, expected {FIELDS!r}"
        ]
    for number, line in enumerate(lines[1:], start=2):
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) != len(FIELDS):
            errors.append(
                f"{REGISTRY.relative_to(ROOT)}:{number}: {len(parts)} columns, "
                f"expected {len(FIELDS)}"
            )
            continue
        rows.append(dict(zip(FIELDS, parts)))
    return rows, errors


def md_cell(value: str) -> str:
    return value.replace("|", "\\|")


def render_table(rows: list[dict[str, str]]) -> str:
    out = [
        "| address | name | note | kind | evidence | session | conf |",
        "|---|---|---|---|---|---|---|",
    ]
    for row in rows:
        out.append(
            "| `{address}` | `{name}` | {note} | {kind} | {evidence} | {session} | {conf} |".format(
                address=row["address"],
                name=row["name"],
                note=md_cell(row["note"]),
                kind=row["kind"],
                evidence=md_cell(row["evidence"]),
                session=row["session"],
                conf=row["confidence"],
            )
        )
    return "\n".join(out)


def reference_pages() -> list[tuple[Path, dict[str, str]]]:
    pages: list[tuple[Path, dict[str, str]]] = []
    for path in sorted(REF.glob("*.md")):
        if path.name == "README.md":
            continue
        meta, _ = parse_front_matter(read_text(path))
        pages.append((path, meta))
    pages.sort(key=lambda item: (int(item[1].get("order", "999")), item[0].name))
    return pages


def first_heading(text: str) -> str:
    for line in text.splitlines():
        if line.startswith("# "):
            return line[2:].strip()
    return ""


def first_paragraph(text: str) -> str:
    """First prose sentence: join wrapped lines, stop at the first sentence."""
    lines = text.splitlines()
    index = 0
    while index < len(lines) and (
        not lines[index].strip()
        or lines[index].startswith("#")
        or lines[index].startswith("---")
    ):
        index += 1
    paragraph: list[str] = []
    while index < len(lines) and lines[index].strip() and not lines[index].startswith("#"):
        paragraph.append(lines[index].strip())
        index += 1
    text = " ".join(paragraph)
    parts = re.split(r"(?<=[.!?])(?:[*_]*)\s", text, maxsplit=2)
    sentence = parts[0].strip()
    if re.fullmatch(r"\*\*[^*]+[.!?]", sentence) and len(parts) > 1:
        sentence = parts[1].strip()
    return shorten(sentence)


def shorten(sentence: str, limit: int = 180) -> str:
    """Cut at a clause boundary so an index cell ends on a complete phrase."""
    sentence = sentence.strip()
    if len(sentence) <= limit:
        return sentence
    end = limit - 3
    cut = max(sentence.rfind(",", 0, end), sentence.rfind(";", 0, end), sentence.rfind(":", 0, end))
    if cut < limit // 2:
        cut = sentence.rfind(" ", 0, end)
    out = sentence[:cut].rstrip(" ,;:") + "."
    if out.count("`") % 2:
        out = out[:-1] + "`."
    if out.count("**") % 2:
        out = out[:-1] + "**."
    return out


def paragraphs(text: str) -> list[str]:
    """Prose blocks, with fenced code dropped and blockquote markers stripped."""
    blocks: list[str] = []
    current: list[str] = []
    in_fence = False
    for line in text.splitlines():
        if line.startswith("```"):
            in_fence = not in_fence
            if current:
                blocks.append(" ".join(current))
                current = []
            continue
        if in_fence:
            continue
        if not line.strip() or line.startswith("#"):
            if current:
                blocks.append(" ".join(current))
                current = []
            continue
        current.append(line.strip().lstrip("> ").strip())
    if current:
        blocks.append(" ".join(current))
    return blocks


def best_summary(text: str) -> str:
    """First paragraph with enough text to describe the document."""
    blocks = paragraphs(text)
    for block in blocks:
        if len(block) >= 60:
            return block
    return blocks[0] if blocks else ""


def guide_rows() -> list[tuple[str, str, str]]:
    rows = []
    for path in sorted((DOCS / "guides").glob("*.md")):
        text = read_text(path)
        summary = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", best_summary(text))
        summary = summary.replace("*", "")
        rows.append((path.name, first_heading(text) or path.stem, summary))
    return rows


def render_reference_index(prefix: str, ref_prefix: str = "reference/") -> str:
    """`prefix` is the path from the hosting page to docs/."""
    lines = [
        "| Page | Status | Last session | What it covers |",
        "|---|---|---|---|",
        f"| [Working rules for agents]({prefix}../AGENTS.md) | binding | - | "
        "How a session works: ask the developer for intent, verify a handoff at "
        "instruction level, isolate one variable. |",
        f"| [Plan and open work]({prefix}../PLAN.md) | living | - | "
        "Port scope, current status, the open-work list and the browser section. |",
    ]
    for path, meta in reference_pages():
        link = f"{ref_prefix}{path.name}"
        lines.append(
            "| [{title}]({link}) | {status} | {session} | {summary} |".format(
                title=meta.get("title", path.stem),
                link=link,
                status=meta.get("status", "unknown"),
                session=meta.get("session", "-"),
                summary=md_cell(meta.get("summary", "")),
            )
        )
    for name, title, status, session, summary in EXTRA_PAGES:
        lines.append(
            f"| [{title}]({prefix}{name}) | {status} | {session} | {md_cell(summary)} |"
        )
    lines.append(
        f"| [Decision log]({prefix}DECISIONS.md) | binding | - | "
        "The durable-decisions table and the session reasoning below it. |"
    )
    lines.append(
        f"| [Session status log]({prefix}STATUS-LOG.md) | historical | - | "
        "The per-session status that `PLAN.md` carried from session 3 to 97. |"
    )
    return "\n".join(lines)


def render_guide_index(prefix: str) -> str:
    lines = [
        "| Guide | What it covers |",
        "|---|---|",
    ]
    for name, title, summary in guide_rows():
        lines.append(f"| [{title}]({prefix}guides/{name}) | {md_cell(summary)} |")
    return "\n".join(lines)


def handoff_rows() -> list[tuple[int, str, str, str]]:
    """(session, date, filename, short summary), newest first."""
    rows: list[tuple[int, str, str, str]] = []
    for path in sorted(DOCS.glob("HANDOFF-*.md")):
        match = HANDOFF_FILE_RE.match(path.name)
        if not match:
            continue
        year, month, day, session = match.groups()
        date = f"{year}-{month}-{day}"
        text = read_text(path)
        summary = ""
        for line in text.splitlines():
            if line.startswith("> **Summary.** "):
                summary = line[len("> **Summary.** ") :].strip()
                break
        if not summary:
            summary = best_summary(text)
        summary = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", summary).replace("*", "")
        rows.append((int(session) if session else 0, date, path.name, shorten(summary)))
    rows.sort(key=lambda row: (row[1], row[0]), reverse=True)
    return rows


def other_docs() -> list[tuple[str, str, str]]:
    """Top-level docs that are neither a reference page, a guide nor a log."""
    skip = {"README.md", "DECISIONS.md", "STATUS-LOG.md", "scenes.md", "symbols.md"}
    rows = []
    for path in sorted(DOCS.glob("*.md")):
        if path.name in skip or path.name.startswith("HANDOFF-"):
            continue
        text = read_text(path)
        summary = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", best_summary(text))
        summary = summary.replace("*", "")
        rows.append((path.name, first_heading(text) or path.stem, summary))
    return rows


def render_other_index() -> str:
    lines = [
        "| Document | What it covers |",
        "|---|---|",
    ]
    for name, title, summary in other_docs():
        lines.append(f"| [{title}]({name}) | {md_cell(summary)} |")
    return "\n".join(lines)


def render_handoff_index() -> str:
    lines = [
        "| Session | Date | Handoff | Summary |",
        "|---|---|---|---|",
    ]
    for session, date, name, summary in handoff_rows():
        label = str(session) if session else date
        shown = str(session) if session else "-"
        lines.append(f"| {shown} | {date} | [{label}]({name}) | {md_cell(summary)} |")
    return "\n".join(lines)


def replace_block(text: str, key: str, body: str) -> str:
    begin = BEGIN.format(key=key)
    end = END.format(key=key)
    if text.count(begin) != 1 or text.count(end) != 1:
        raise ValueError(f"expected exactly one {begin!r} / {end!r}")
    pre, rest = text.split(begin, 1)
    _, post = rest.split(end, 1)
    return f"{pre}{begin}\n{body}\n{end}{post}"


def expected_text(path: Path, rows: list[dict[str, str]]) -> str:
    text = read_text(path)
    page_key = path.relative_to(DOCS).with_suffix("").as_posix()
    registry = [row for row in rows if row["page"] == page_key]
    for section in sorted({row["section"] for row in registry}):
        body = render_table([row for row in registry if row["section"] == section])
        text = replace_block(text, f"addresses:{section}", body)
    if path == REF / "README.md":
        text = replace_block(text, "reference-index", render_reference_index("../", ""))
    if path == DOCS / "README.md":
        text = replace_block(text, "docs-index", render_reference_index(""))
        text = replace_block(text, "docs-guides", render_guide_index(""))
        text = replace_block(text, "docs-other", render_other_index())
        text = replace_block(text, "handoff-index", render_handoff_index())
    return text


def target_pages() -> list[Path]:
    paths = [path for path, _ in reference_pages()] + [
        REF / "README.md",
        DOCS / "README.md",
        DOCS / "symbols.md",
        DOCS / "scenes.md",
    ]
    return [path for path in paths if path.exists()]


def build(write: bool) -> tuple[list[Path], list[str]]:
    rows, errors = load_registry()
    changed: list[Path] = []
    for path in target_pages():
        try:
            expected = expected_text(path, rows)
        except ValueError as exc:
            errors.append(f"{path.relative_to(ROOT)}: {exc}")
            continue
        current = read_text(path)
        if current != expected:
            changed.append(path)
            if write:
                path.write_text(expected, encoding="utf-8")
    return changed, errors


def check_links() -> list[str]:
    errors: list[str] = []
    paths = list(DOCS.glob("**/*.md")) + [ROOT / "AGENTS.md", ROOT / "PLAN.md", ROOT / "README.md"]
    for path in paths:
        if not path.exists():
            continue
        text = re.sub(r"```.*?```", "", read_text(path), flags=re.DOTALL)
        for match in LINK_RE.finditer(text):
            target = match.group(1).strip()
            if target.startswith("<") and target.endswith(">"):
                target = target[1:-1]
            elif any(ch.isspace() for ch in target):
                # Not a link: a C++ lambda such as [&](size_t x) survives the regex.
                continue
            if not target or target.startswith(("http://", "https://", "#", "mailto:")):
                continue
            target = target.split("#", 1)[0].replace("%20", " ")
            if not target or "{" in target or "}" in target:
                continue
            resolved = (path.parent / target).resolve()
            if not resolved.exists():
                errors.append(
                    f"{path.relative_to(ROOT)}: link target does not exist: {target}"
                )
    return errors


def check_registry(rows: list[dict[str, str]]) -> list[str]:
    errors: list[str] = []
    seen: dict[str, tuple[str, str]] = {}
    for row in rows:
        address = row["address"]
        label = f"{REGISTRY.relative_to(ROOT)}: {address}"
        if not ADDRESS_RE.match(address):
            errors.append(f"{label}: not a 0x address")
        if not row["name"].strip() or "\t" in row["name"]:
            errors.append(f"{label}: missing or malformed name")
        if row["kind"] not in KINDS:
            errors.append(f"{label}: unknown kind {row['kind']!r}")
        if row["confidence"] not in CONFIDENCE:
            errors.append(f"{label}: confidence {row['confidence']!r} not in {sorted(CONFIDENCE)}")
        if not SESSION_RE.match(row["session"]):
            errors.append(f"{label}: session {row['session']!r} is not N, N-M or -")
        page = page_path(row["page"])
        if not page.exists():
            errors.append(f"{label}: page docs/{row['page']}.md does not exist")
        elif (
            BEGIN.format(key=f"addresses:{row['section']}")
            not in read_text(page)
        ):
            errors.append(
                f"{label}: docs/{row['page']}.md has no "
                f"`addresses:{row['section']}` block"
            )
        if address in seen:
            old_name, old_kind = seen[address]
            if old_kind == row["kind"] and old_name != row["name"]:
                errors.append(
                    f"{label}: registered as {old_name!r} and {row['name']!r} "
                    f"for kind {row['kind']}"
                )
        else:
            seen[address] = (row["name"], row["kind"])
    return errors


def check_handoff_refs() -> list[str]:
    errors: list[str] = []
    for path in sorted(REF.glob("*.md")):
        text = read_text(path)
        for match in HANDOFF_RE.finditer(text):
            if not (DOCS / match.group(0)).exists():
                errors.append(
                    f"{path.relative_to(ROOT)}: cites missing {match.group(0)}"
                )
    return errors


def check_front_matter() -> list[str]:
    errors: list[str] = []
    required = ["title", "summary", "status", "session", "order"]
    for path, meta in reference_pages():
        missing = [key for key in required if not meta.get(key)]
        if missing:
            errors.append(
                f"{path.relative_to(ROOT)}: front matter missing {', '.join(missing)}"
            )
        if "order" in meta and not meta["order"].isdigit():
            errors.append(f"{path.relative_to(ROOT)}: order must be an integer")
    return errors


def check_handoffs() -> list[str]:
    errors: list[str] = []
    files = sorted(DOCS.glob("HANDOFF-*.md"))
    for path in files:
        if not HANDOFF_FILE_RE.match(path.name):
            errors.append(
                f"docs/{path.name}: filename is not HANDOFF-YYYY-MM-DD[-sessionN].md"
            )
    rows = handoff_rows()
    if len(rows) != len(files):
        errors.append(
            f"{len(files)} handoff file(s) but {len(rows)} index row(s)"
        )
    for session, date, name, summary in rows:
        if not summary:
            errors.append(f"docs/{name}: no summary and no first paragraph")
    return errors


def markdown_files() -> list[Path]:
    return list(DOCS.glob("**/*.md")) + [
        ROOT / "AGENTS.md",
        ROOT / "PLAN.md",
        ROOT / "README.md",
    ]


def cmd_find(args: argparse.Namespace) -> int:
    rows, _ = load_registry()
    query = args.query.strip()
    if not query:
        print("docindex: empty query", file=sys.stderr)
        return 1
    if query.lower().startswith("0x"):
        addresses = [query]
        names = []
    else:
        names = [row for row in rows if query.lower() in row["name"].lower()]
        addresses = [row["address"] for row in names]
        if not addresses:
            print(f"docindex: no registry name contains {query!r}", file=sys.stderr)
            return 1
    wanted = {address.lower() for address in addresses}
    found = [row for row in rows if row["address"].lower() in wanted]
    if found:
        print("registry:")
    else:
        print("registry: no row for this address")
    for row in found:
        if True:
            note = f" {row['note']}" if row["note"] else ""
            print(
                f"  {row['address']}  {row['name']}{note}  "
                f"[{row['kind']}, {row['confidence']}]  {row['page']}#{row['section']}"
            )
            print(f"    {row['evidence']}")
    pattern = re.compile("|".join(re.escape(a) for a in addresses), re.IGNORECASE)
    hits = []
    for path in markdown_files():
        if not path.exists():
            continue
        for number, line in enumerate(read_text(path).splitlines(), start=1):
            if pattern.search(line):
                hits.append((path, number, line.strip()))
    print(f"mentions: {len(hits)} line(s) in {len({h[0] for h in hits})} file(s)")
    shown = hits if args.all else hits[: args.limit]
    for path, number, line in shown:
        if len(line) > 200:
            line = line[:197] + "..."
        print(f"  {path.relative_to(ROOT)}:{number}: {line}")
    if not args.all and len(hits) > len(shown):
        print(f"  ... {len(hits) - len(shown)} more (use --all)")
    return 0


def cmd_stats(args: argparse.Namespace) -> int:
    rows, _ = load_registry()
    counts: dict[str, int] = {}
    for path in markdown_files():
        if not path.exists():
            continue
        for match in re.finditer(r"0x8[0-9A-Fa-f]{7}", read_text(path)):
            address = "0x" + match.group(0)[2:].upper()
            counts[address] = counts.get(address, 0) + 1
    registered = {"0x" + row["address"][2:].upper() for row in rows if row["address"].lower().startswith("0x8")}
    covered = sum(1 for address in counts if address in registered)
    print(f"guest addresses mentioned (0x8xxxxxxx): {len(counts)}")
    print(f"registered rows: {len(rows)}")
    print(f"mentioned and registered: {covered}")
    print("by page:")
    for page in sorted({row["page"] for row in rows}):
        print(f"  {page}: {sum(1 for row in rows if row['page'] == page)}")
    missing = sorted(
        ((count, address) for address, count in counts.items() if address not in registered),
        reverse=True,
    )
    print(f"mentioned, not registered: {len(missing)}")
    print("most mentioned, not registered:")
    for count, address in missing[: args.limit]:
        print(f"  {address}  x{count}")
    return 0


def cmd_build(args: argparse.Namespace) -> int:
    changed, errors = build(write=True)
    for path in changed:
        print(f"docindex: wrote {path.relative_to(ROOT)}")
    for error in errors:
        print(f"docindex: {error}", file=sys.stderr)
    if not changed and not errors:
        print("docindex: generated blocks are current")
    return 1 if errors else 0


def cmd_check(args: argparse.Namespace) -> int:
    rows, errors = load_registry()
    errors += check_registry(rows)
    errors += check_front_matter()
    errors += check_handoff_refs()
    errors += check_links()
    errors += check_handoffs()
    stale, build_errors = build(write=False)
    errors += build_errors
    for path in stale:
        errors.append(
            f"{path.relative_to(ROOT)}: generated block is stale "
            "(run `make docs-index`)"
        )
    if args.verbose:
        print(
            f"docindex: {len(reference_pages())} reference page(s), "
            f"{len(rows)} registry row(s)"
        )
    for error in errors:
        print(f"docindex: {error}", file=sys.stderr)
    if errors:
        print(f"docindex: {len(errors)} problem(s)", file=sys.stderr)
        return 1
    print(
        f"docindex: ok - {len(reference_pages())} reference page(s), "
        f"{len(rows)} address row(s), links resolve"
    )
    return 0


def cmd_unregistered(args: argparse.Namespace) -> int:
    rows, _ = load_registry()
    registered = {row["address"].lower() for row in rows}
    if args.files:
        paths = [Path(name) for name in args.files]
    else:
        handoffs = sorted(DOCS.glob("HANDOFF-*.md"), reverse=True)
        if not handoffs:
            print("docindex: no handoff found", file=sys.stderr)
            return 1
        paths = [handoffs[0]]
    for path in paths:
        if not path.exists():
            print(f"docindex: {path}: missing", file=sys.stderr)
            return 1
        counts: dict[str, int] = {}
        for match in re.finditer(r"0x8[0-9A-Fa-f]{7}", read_text(path)):
            address = match.group(0).lower()
            if address not in registered:
                counts[address] = counts.get(address, 0) + 1
        print(f"{path}: {len(counts)} unregistered guest address(es)")
        for address, count in sorted(counts.items()):
            print(f"  {address}  x{count}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    build_parser = sub.add_parser("build", help="rewrite the generated blocks")
    build_parser.set_defaults(func=cmd_build)

    check_parser = sub.add_parser("check", help="verify links, registry and blocks")
    check_parser.add_argument("-v", "--verbose", action="store_true")
    check_parser.set_defaults(func=cmd_check)

    unregistered_parser = sub.add_parser(
        "unregistered", help="addresses in a handoff that the registry lacks"
    )
    unregistered_parser.add_argument("files", nargs="*")
    unregistered_parser.set_defaults(func=cmd_unregistered)

    find_parser = sub.add_parser(
        "find", help="show a registry address or name and the docs that mention it"
    )
    find_parser.add_argument("query")
    find_parser.add_argument("--all", action="store_true", help="show every mention")
    find_parser.add_argument("--limit", type=int, default=20)
    find_parser.set_defaults(func=cmd_find)

    stats_parser = sub.add_parser(
        "stats", help="address coverage: mentioned, registered, most-mentioned gaps"
    )
    stats_parser.add_argument("--limit", type=int, default=15)
    stats_parser.set_defaults(func=cmd_stats)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
