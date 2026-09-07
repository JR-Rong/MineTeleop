#!/usr/bin/env python3
"""Shared fallback YAML editor for the administrative identity scripts.

The shell entrypoints deliberately retain their own argument, credential, and
publish transactions.  This program owns only the common YAML queries and the
small text-preserving edits used when mikefarah/yq v4 is unavailable.  PyYAML
validates and queries the document; mutations keep surrounding comments, key
order, indentation, and trailing-newline behavior intact.
"""

import re
import sys


if len(sys.argv) < 3:
    sys.stderr.write("yaml edit failed: expected MODE FILE [ARG ...]\n")
    raise SystemExit(2)

mode, path = sys.argv[1], sys.argv[2]
arguments = sys.argv[3:]


def die(message):
    sys.stderr.write("yaml edit failed: %s\n" % message)
    raise SystemExit(2)


def load_text():
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    trailing_newline = text.endswith("\n")
    lines = text.split("\n")
    if trailing_newline:
        lines.pop()
    return lines, trailing_newline


def store_text(lines, trailing_newline):
    text = "\n".join(lines)
    if trailing_newline:
        text += "\n"
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def is_filler(line):
    stripped = line.strip()
    return stripped == "" or stripped.startswith("#")


def block_end(lines, key_index, limit=None):
    """Return the index just past the content owned by lines[key_index]."""
    base = indent_of(lines[key_index])
    limit = len(lines) if limit is None else limit
    end = key_index + 1
    index = key_index + 1
    while index < limit:
        line = lines[index]
        if is_filler(line):
            index += 1
            continue
        current = indent_of(line)
        if current > base or (current == base and line.lstrip().startswith("- ")):
            end = index + 1
            index += 1
            continue
        break
    return end


def find_key(lines, start, limit, key, indent=None):
    pattern = re.compile(r"^( *)" + re.escape(key) + r":( *)(.*)$")
    for index in range(start, limit):
        match = pattern.match(lines[index])
        if match is None:
            continue
        if indent is not None and len(match.group(1)) != indent:
            continue
        return index, match.group(3).strip()
    return -1, ""


def child_indent(lines, key_index, limit, default):
    for index in range(key_index + 1, limit):
        if is_filler(lines[index]):
            continue
        return indent_of(lines[index])
    return default


def sequence_indent(lines, start, limit, default):
    for index in range(start, limit):
        line = lines[index]
        if is_filler(line):
            continue
        if line.lstrip().startswith("- "):
            return indent_of(line)
    return default


def auth_section(lines, section):
    auth_index, auth_inline = find_key(lines, 0, len(lines), "auth", indent=0)
    if auth_index < 0:
        die("top level `auth` mapping not found")
    if auth_inline:
        die("`auth` must be a block mapping, found an inline value")
    auth_limit = block_end(lines, auth_index)
    auth_child = child_indent(lines, auth_index, auth_limit, 2)
    section_index, section_inline = find_key(
        lines, auth_index + 1, auth_limit, section, indent=auth_child)
    return auth_index, auth_limit, auth_child, section_index, section_inline


def quote(value):
    if re.match(r"^[A-Za-z0-9._/-]+$", value):
        return value
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"')


def parsed_document():
    try:
        import yaml
    except ImportError:
        die("PyYAML is required for the python fallback backend")
    with open(path, "r", encoding="utf-8") as handle:
        document = yaml.safe_load(handle)
    if document is None:
        document = {}
    if not isinstance(document, dict):
        die("config root must be a mapping")
    return document


def entries(section):
    document = parsed_document()
    auth = document.get("auth") or {}
    if not isinstance(auth, dict):
        die("`auth` must be a mapping")
    items = auth.get(section) or []
    if not isinstance(items, list):
        die("`auth.%s` must be a sequence" % section)
    return items


def driver_span(lines, driver_id, limit_to_auth):
    """Return (start, stop, item_indent) for a driver entry, if present."""
    _, auth_limit, auth_child, drivers_index, drivers_inline = auth_section(lines, "drivers")
    if drivers_index < 0:
        die("`auth.drivers` not found")
    if drivers_inline:
        die("`auth.drivers` uses an inline sequence; install mikefarah/yq v4 to edit it")
    drivers_limit = block_end(lines, drivers_index, auth_limit if limit_to_auth else None)
    item = sequence_indent(lines, drivers_index + 1, drivers_limit, auth_child + 2)
    starts = [index for index in range(drivers_index + 1, drivers_limit)
              if indent_of(lines[index]) == item and lines[index].lstrip().startswith("- ")]
    for position, start in enumerate(starts):
        stop = starts[position + 1] if position + 1 < len(starts) else drivers_limit
        head = re.match(r"^ *- +id: *(.*)$", lines[start])
        matched = head is not None and head.group(1).strip().strip("\"'") == driver_id
        if not matched:
            body, _ = find_key(lines, start + 1, stop, "id", indent=item + 2)
            if body >= 0:
                matched = lines[body].split(":", 1)[1].strip().strip("\"'") == driver_id
        if matched:
            return start, stop, item
    return -1, -1, item


if mode == "tag":
    node = parsed_document()
    for key in arguments[0].split("."):
        if not isinstance(node, dict):
            node = None
            break
        node = node.get(key)
    kinds = ((bool, "!!bool"), (dict, "!!map"), (list, "!!seq"),
             (str, "!!str"), (int, "!!int"), (float, "!!float"))
    printed = "!!null"
    if node is not None:
        printed = "!!unknown"
        for kind, name in kinds:
            if isinstance(node, kind):
                printed = name
                break
    print(printed)
    raise SystemExit(0)

if mode == "length":
    print(len(entries(arguments[0])))
    raise SystemExit(0)

if mode == "ids":
    for entry in entries(arguments[0]):
        if isinstance(entry, dict) and entry.get("id") is not None:
            print(entry["id"])
    raise SystemExit(0)

if mode == "driver-vehicles":
    driver_id = arguments[0]
    for entry in entries("drivers"):
        if not isinstance(entry, dict) or entry.get("id") != driver_id:
            continue
        for vehicle in entry.get("vehicles") or []:
            print(vehicle)
    raise SystemExit(0)

if mode == "show-driver":
    lines, _ = load_text()
    start, stop, _ = driver_span(lines, arguments[0], True)
    if start < 0:
        die("driver `%s` not found under auth.drivers" % arguments[0])
    while stop > start and is_filler(lines[stop - 1]):
        stop -= 1
    for line in lines[start:stop]:
        print(line)
    raise SystemExit(0)

if mode == "add-driver":
    driver_id, password_file = arguments[0], arguments[1]
    vehicles = [item for item in arguments[2].split(",") if item]
    if not vehicles:
        die("the new driver needs at least one vehicle")
    lines, trailing_newline = load_text()
    auth_index, auth_limit, auth_child, index, inline = auth_section(lines, "drivers")

    payload = []
    if index < 0:
        item = auth_child + 2
        insert_at = block_end(lines, auth_index)
        payload.append(" " * auth_child + "drivers:")
    else:
        if inline and inline != "[]":
            die("`auth.drivers` uses an inline sequence; install mikefarah/yq v4 to edit it")
        if inline == "[]":
            lines[index] = " " * auth_child + "drivers:"
            item = auth_child + 2
            insert_at = index + 1
        else:
            limit = block_end(lines, index, auth_limit)
            item = sequence_indent(lines, index + 1, limit, auth_child + 2)
            insert_at = limit

    payload.extend([
        " " * item + "- id: " + quote(driver_id),
        " " * (item + 2) + "password_file: " + quote(password_file),
        " " * (item + 2) + "vehicles:",
    ])
    payload.extend(" " * (item + 4) + "- " + quote(vehicle) for vehicle in vehicles)

    lines[insert_at:insert_at] = payload
    store_text(lines, trailing_newline)
    raise SystemExit(0)

if mode == "add-vehicle":
    vehicle_id, token_path = arguments[0], arguments[1]
    lines, trailing_newline = load_text()
    auth_index, auth_limit, auth_child, index, inline = auth_section(lines, "vehicles")

    if index < 0:
        item = auth_child + 2
        insert_at = block_end(lines, auth_index)
        payload = [
            " " * auth_child + "vehicles:",
            " " * item + "- id: " + quote(vehicle_id),
            " " * (item + 2) + "device_token_file: " + quote(token_path),
        ]
    else:
        if inline and inline != "[]":
            die("`auth.vehicles` uses an inline sequence; install mikefarah/yq v4 to edit it")
        if inline == "[]":
            lines[index] = " " * auth_child + "vehicles:"
            item = auth_child + 2
            insert_at = index + 1
        else:
            limit = block_end(lines, index, auth_limit)
            item = sequence_indent(lines, index + 1, limit, auth_child + 2)
            insert_at = limit
        payload = [
            " " * item + "- id: " + quote(vehicle_id),
            " " * (item + 2) + "device_token_file: " + quote(token_path),
        ]

    lines[insert_at:insert_at] = payload
    store_text(lines, trailing_newline)
    raise SystemExit(0)

if mode == "assign":
    driver_id, vehicle_id = arguments[0], arguments[1]
    lines, trailing_newline = load_text()
    start, stop, item = driver_span(lines, driver_id, False)
    if start < 0:
        die("driver `%s` not found under auth.drivers" % driver_id)

    key_index, inline = find_key(lines, start, stop, "vehicles", indent=item + 2)
    if key_index < 0:
        insert_at = block_end(lines, start, stop)
        payload = [
            " " * (item + 2) + "vehicles:",
            " " * (item + 4) + "- " + quote(vehicle_id),
        ]
    elif inline:
        if not (inline.startswith("[") and inline.endswith("]")):
            die("driver `%s` has an unsupported `vehicles` value" % driver_id)
        existing = inline[1:-1].strip()
        merged = "[%s]" % (quote(vehicle_id) if not existing
                           else "%s, %s" % (existing, quote(vehicle_id)))
        lines[key_index] = " " * (item + 2) + "vehicles: " + merged
        store_text(lines, trailing_newline)
        raise SystemExit(0)
    else:
        limit = block_end(lines, key_index, stop)
        entry_indent = sequence_indent(lines, key_index + 1, limit, item + 4)
        insert_at = limit
        payload = [" " * entry_indent + "- " + quote(vehicle_id)]

    lines[insert_at:insert_at] = payload
    store_text(lines, trailing_newline)
    raise SystemExit(0)

die("unknown mode: %s" % mode)
