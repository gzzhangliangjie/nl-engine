#!/usr/bin/env python3
"""check-scene-shutdown.py -- Scene GPU resource shutdown coverage gate (2026-11-14).

Lesson this gate enforces (S15 + M14C1 recurrence, user-approved Option A):

  Every nvrhi::*Handle member declared in renderer/src/scene.hpp inside a class
  that owns `void shutdown();` MUST be released inside that class's ::shutdown()
  body (scene.cpp) -- because NVRHI handles released after vkDestroyDevice /
  D3D12 device teardown are use-after-free (rc=139 exit crash). The manual
  "shutdown checklist" discipline failed twice (S15 ~Scene UAF; M14C1 adaptive
  resources missing entirely), so the checklist is now machine-checked.

Rules (pure static text scan, stdlib only):
  1. Direct handle members: identifier must occur as a word in the class's
     shutdown body (`x_ = nullptr`, `for (auto& r : x_)`, `x_.clear()` ...).
     Exempt per-member with `// guard-exempt` on the declaration line
     (justification required in review).
  2. Nested-struct handles (released via their container): each outer-class
     member whose type is that nested struct (or std::vector of it) must occur
     in the shutdown body (`clear()` / loop). This is the realistic M14C1
     failure mode: a new container member added but never cleared.

Output anchors (repo HUD convention, ASCII only):
  [nl:scene-shutdown] PASS: <classes> classes, <handles> handles, <containers> containers audited
  [nl:scene-shutdown] FAIL: <Class>.<member> ... (with fix hint)

Exit codes: 0 pass, 1 fail (any missing release), 2 usage/parse error.

Usage:
    python tools/check-scene-shutdown.py             # gate mode (repo files)
    python tools/check-scene-shutdown.py --selftest  # synthetic samples, all green rc=0
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_HPP = os.path.join(HERE, "..", "renderer", "src", "scene.hpp")
DEFAULT_CPP = os.path.join(HERE, "..", "renderer", "src", "scene.cpp")

RE_HANDLE_DECL = re.compile(r"\bnvrhi::\w+Handle\b")
RE_CLASS_BLOCK = re.compile(r"\b(class|struct)\s+(\w+)\s*(?::[^{]*)?\{")
RE_METHOD_DECL = re.compile(r"\bvoid\s+shutdown\s*\(\s*\)\s*;")
RE_SHUTDOWN_DEF = re.compile(r"\bvoid\s+(\w+)::shutdown\s*\(\s*\)\s*\{")


# ---------------------------------------------------------------------------
# brace-aware block extraction
# ---------------------------------------------------------------------------

def find_block_end(text, open_idx):
    """Given index of '{', return index of matching '}'."""
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        # skip string/char literals and comments so braces inside don't confuse
        elif c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
        elif c == "'":
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\":
                    i += 1
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = (j - 1) if j != -1 else n
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i)
            i = (j + 1) if j != -1 else n
        i += 1
    return -1


def strip_comments(text):
    """Remove // and /* */ comments, preserving length (for offset mapping)."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j == -1 else j
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def line_of(text, idx):
    return text.count("\n", 0, max(0, idx)) + 1


# ---------------------------------------------------------------------------
# scene.hpp parsing
# ---------------------------------------------------------------------------

def parse_header(hpp_text):
    """Return dict: class_name -> {
        'shutdown_decl': bool,
        'direct_handles': [(name, line, exempt)],
        'nested': {nested_name: [(handle_name, line)]},
        'containers': [(member_name, nested_name, line)],
    }."""
    src = strip_comments(hpp_text)
    result = {}
    hpp_lines = hpp_text.split("\n")

    # top-level class/struct blocks
    blocks = []
    i, n = 0, len(src)
    while i < n:
        m = RE_CLASS_BLOCK.search(src, i)
        if not m:
            break
        end = find_block_end(src, m.end() - 1)
        if end == -1:
            break
        blocks.append((m.group(2), m.end(), end, m.start()))
        i = end + 1

    for cls_name, body_start, body_end, decl_start in blocks:
        body = src[body_start:body_end]
        base_line = line_of(src, body_start)

        # skip plain POD structs (no shutdown decl)
        has_shutdown = bool(RE_METHOD_DECL.search(body))

        # nested struct/class blocks inside this class
        nested_spans = []  # (name, start, end)
        j = 0
        while j < len(body):
            m2 = re.search(r"\b(?:class|struct)\s+(\w+)[^{};]*\{", body[j:])
            if not m2:
                break
            nstart = j + m2.start()
            nend_rel = find_block_end(body, j + m2.end() - 1)
            if nend_rel == -1:
                break
            nested_spans.append((m2.group(1), nstart, nend_rel))
            j = nend_rel + 1

        def in_nested(pos):
            for _, s, e in nested_spans:
                if s <= pos <= e:
                    return True
            return False

        direct, nested = [], {}
        for m3 in RE_HANDLE_DECL.finditer(body):
            # declaration: from match to ';'
            semi = body.find(";", m3.start())
            if semi == -1:
                continue
            decl = body[m3.start():semi]
            line = base_line + line_of(body, m3.start()) - 1
            raw_decl_line = hpp_lines[line - 1] if 0 <= line - 1 < len(hpp_lines) else ""
            exempt = "guard-exempt" in raw_decl_line
            # drop the "nvrhi::XxxHandle" prefix, split remaining by ','
            rest = body[m3.end():semi]
            names = []
            for part in rest.split(","):
                ident = part.strip()
                ident = re.sub(r"\[.*$", "", ident).strip()  # strip array suffix
                ident = re.sub(r"//.*$", "", ident).strip()
                if re.fullmatch(r"\w+", ident):
                    names.append(ident)
            if in_nested(m3.start()):
                # attribute to the innermost nested struct
                owner = None
                for nm, s, e in nested_spans:
                    if s <= m3.start() <= e:
                        owner = nm
                nested.setdefault(owner, []).extend(
                    (nm2, line) for nm2 in names)
            else:
                direct.extend((nm2, line, exempt) for nm2 in names)

        # container members of nested-struct types (outer class members)
        containers = []
        for nm, s, e in nested_spans:
            inner = body[s:e]
            if not RE_HANDLE_DECL.search(inner):
                continue  # nested struct holds no GPU handles -> not audited
            type_re = re.compile(
                r"(?:std::\s*vector\s*<\s*)?\b%s\b\s*(?:>\s*)?(\w+)\s*(?:\[[^;]*\])?\s*;"
                % re.escape(nm))
            for m4 in type_re.finditer(body):
                pos = m4.start()
                if in_nested(pos):
                    continue  # a nested member, not an outer container
                containers.append((m4.group(1), nm,
                                   base_line + line_of(body, pos) - 1))

        result[cls_name] = {
            "shutdown_decl": has_shutdown,
            "direct_handles": direct,
            "nested": nested,
            "containers": containers,
        }
    return result


# ---------------------------------------------------------------------------
# scene.cpp parsing
# ---------------------------------------------------------------------------

def parse_shutdown_bodies(cpp_text):
    """Return dict: class_name -> shutdown function body text."""
    src = strip_comments(cpp_text)
    bodies = {}
    for m in RE_SHUTDOWN_DEF.finditer(src):
        open_idx = src.find("{", m.end() - 1)
        end = find_block_end(src, open_idx)
        if end == -1:
            continue
        bodies[m.group(1)] = src[open_idx:end]
    return bodies


RE_METHOD_DEF = re.compile(r"\bvoid\s+(\w+)::(\w+)\s*\([^)]*\)\s*\{")


def parse_method_bodies(cpp_text):
    """Return dict: class_name -> {method_name: body} for void methods.
    Used to follow same-class helper release (e.g. shutdown() -> invalidate())."""
    src = strip_comments(cpp_text)
    methods = {}
    for m in RE_METHOD_DEF.finditer(src):
        open_idx = src.find("{", m.end() - 1)
        end = find_block_end(src, open_idx)
        if end == -1:
            continue
        methods.setdefault(m.group(1), {})[m.group(2)] = src[open_idx:end]
    return methods


def expand_body(body, methods, max_steps=4):
    """Follow same-class helper methods called from `body` (e.g.
    `invalidate()`), concatenating their bodies. Bounded iteration with a
    visited set; releases credited via helpers are real releases."""
    expanded = body
    visited = {"shutdown"}
    for _ in range(max_steps):
        added = ""
        for mname, mbody in methods.items():
            if mname in visited:
                continue
            if re.search(r"\b%s\s*\(" % re.escape(mname), expanded):
                visited.add(mname)
                added += mbody
        if not added:
            break
        expanded += added
    return expanded


# ---------------------------------------------------------------------------
# gate
# ---------------------------------------------------------------------------

def word_present(body, name):
    return re.search(r"\b%s\b" % re.escape(name), body) is not None


def audit(header_map, cpp_bodies, class_filter=None, cpp_methods=None):
    """Return list of failure strings (empty = pass) + stats dict."""
    fails, stats = [], {"classes": 0, "handles": 0, "containers": 0}
    cpp_methods = cpp_methods or {}
    for cls, info in sorted(header_map.items()):
        if class_filter and cls != class_filter:
            continue
        if not info["shutdown_decl"]:
            continue
        if cls not in cpp_bodies:
            fails.append("%s: declares void shutdown() but no %s::shutdown() "
                         "definition found in the cpp" % (cls, cls))
            continue
        body = expand_body(cpp_bodies[cls], cpp_methods.get(cls, {}))
        stats["classes"] += 1

        for name, line, exempt in info["direct_handles"]:
            stats["handles"] += 1
            if exempt:
                continue
            if not word_present(body, name):
                fails.append(
                    "%s.%s (decl line %d): GPU handle member is NOT released "
                    "in %s::shutdown() -- device-dead UAF risk (rc=139). Fix: "
                    "`%s = nullptr;` in shutdown, or add `// guard-exempt` "
                    "with justification if released elsewhere while the "
                    "device is alive."
                    % (cls, name, line, cls, name))

        for member, nested_name, line in info["containers"]:
            stats["containers"] += 1
            if not word_present(body, member):
                fails.append(
                    "%s.%s (decl line %d, container of %s holding GPU "
                    "handles): container is NOT cleared/iterated in "
                    "%s::shutdown(). Fix: `%s.clear();` (or a nullptr loop "
                    "over it) in shutdown."
                    % (cls, member, line, nested_name, cls, member))
    return fails, stats


def run_gate(hpp_path, cpp_path, out):
    try:
        with open(hpp_path, encoding="utf-8", errors="replace") as f:
            hpp_text = f.read()
        with open(cpp_path, encoding="utf-8", errors="replace") as f:
            cpp_text = f.read()
    except OSError as exc:
        out.write("[nl:scene-shutdown] ERROR: cannot read sources: %s\n" % exc)
        return 2
    header_map = parse_header(hpp_text)
    cpp_bodies = parse_shutdown_bodies(cpp_text)
    cpp_methods = parse_method_bodies(cpp_text)
    fails, stats = audit(header_map, cpp_bodies,
                         cpp_methods=cpp_methods)
    if fails:
        for f_ in fails:
            out.write("[nl:scene-shutdown] FAIL: %s\n" % f_)
        out.write("[nl:scene-shutdown] FAIL: %d problem(s) in %d class(es)\n"
                  % (len(fails), stats["classes"]))
        return 1
    out.write("[nl:scene-shutdown] PASS: %d classes, %d handle members, "
              "%d containers audited (scene.hpp x scene.cpp)\n"
              % (stats["classes"], stats["handles"], stats["containers"]))
    return 0


# ---------------------------------------------------------------------------
# selftest: synthetic in-memory samples, no repo files touched
# ---------------------------------------------------------------------------

SELFTEST_HPP = """
class Direct {
public:
    void shutdown();
private:
    nvrhi::BufferHandle    a_;
    nvrhi::TextureHandle   b_;      // guard-exempt: released per-frame
    nvrhi::BufferHandle    c1_, c2_;
    nvrhi::BufferHandle    arr_[2];
};
class Container {
public:
    void shutdown();
private:
    struct Inner {
        nvrhi::BufferHandle set;
        nvrhi::BufferHandle extra;
    };
    nvrhi::BufferHandle      own_;
    std::vector<Inner>       pool_;
    Inner                    single_;
};
class ViaHelper {
public:
    void shutdown();
private:
    void invalidate();
    nvrhi::BufferHandle h1_;
    nvrhi::BufferHandle h2_;
};
class NoShutdown {
public:
private:
    nvrhi::BufferHandle loose_;    // no shutdown decl -> not audited
};
"""

SELFTEST_CPP = """
void Direct::shutdown() {
    a_ = nullptr;
    for (auto& r : arr_) r = nullptr;
    c1_ = nullptr;
    c2_ = nullptr;
}
void Container::shutdown() {
    own_ = nullptr;
    pool_.clear();
    single_.set = nullptr;
}
void ViaHelper::shutdown() {
    invalidate();
}
void ViaHelper::invalidate() {
    h1_ = nullptr;
    h2_ = nullptr;
}
"""


def run_selftest(out):
    ok = True

    def expect(label, hpp, cpp, want_fail_count):
        nonlocal ok
        fails, _ = audit(parse_header(hpp), parse_shutdown_bodies(cpp),
                         cpp_methods=parse_method_bodies(cpp))
        if want_fail_count is None:
            if fails:
                ok = False
                out.write("  selftest %s: unexpected FAIL -> %s\n"
                          % (label, fails))
            else:
                out.write("  selftest %s: pass as expected\n" % label)
        else:
            if len(fails) == want_fail_count:
                out.write("  selftest %s: caught %d problem(s) as expected\n"
                          % (label, len(fails)))
            else:
                ok = False
                out.write("  selftest %s: expected %d fail(s), got %d -> %s\n"
                          % (label, want_fail_count, len(fails), fails))

    expect("direct-all-released", SELFTEST_HPP, SELFTEST_CPP, None)
    expect("direct-missing", SELFTEST_HPP,
           SELFTEST_CPP.replace("c2_ = nullptr;", "// oops"), 1)
    expect("container-cleared", SELFTEST_HPP, SELFTEST_CPP, None)
    expect("container-missing", SELFTEST_HPP,
           SELFTEST_CPP.replace("pool_.clear();", "// oops"), 1)
    expect("container-missing-single", SELFTEST_HPP,
           SELFTEST_CPP.replace("single_.set = nullptr;", "// oops"), 1)
    expect("helper-following-released", SELFTEST_HPP, SELFTEST_CPP, None)
    expect("helper-partial", SELFTEST_HPP,
           SELFTEST_CPP.replace("h2_ = nullptr;", "// oops"), 1)
    # exempt member b_ must never be flagged
    fails, _ = audit(parse_header(SELFTEST_HPP), parse_shutdown_bodies(SELFTEST_CPP),
                     cpp_methods=parse_method_bodies(SELFTEST_CPP))
    if any(".b_ " in f or ".b_(" in f or "b_ (decl" in f for f in fails):
        ok = False
        out.write("  selftest guard-exempt: exempt member b_ was flagged!\n")
    else:
        out.write("  selftest guard-exempt: honored\n")

    if ok:
        out.write("check-scene-shutdown selftest: PASS\n")
        return 0
    out.write("check-scene-shutdown selftest: FAIL\n")
    return 1


def main(argv):
    out = sys.stdout
    if "--selftest" in argv:
        return run_selftest(out)
    if "--help" in argv or "-h" in argv:
        out.write(__doc__)
        return 0
    hpp = DEFAULT_HPP
    cpp = DEFAULT_CPP
    args = argv[1:]
    if len(args) == 2:
        hpp, cpp = args
    elif args:
        out.write("usage: check-scene-shutdown.py [--selftest | <scene.hpp> "
                  "<scene.cpp>]\n")
        return 2
    return run_gate(hpp, cpp, out)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
