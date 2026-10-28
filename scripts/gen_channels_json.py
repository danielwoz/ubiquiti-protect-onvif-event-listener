#!/usr/bin/env python3
"""Writes channels.json, the release-channel manifest served with the apt repo.

The channel ids, aliases, labels, descriptions, order and default come from
the metadata file (gh-pages/channels.meta.json on main).  For every channel
the generator adds the package version currently in its apt suite, read from
the first matching stanza of dists/<id>/main/binary-<arch>/Packages (or
Packages.gz), and the suite's Release Date as updated_at.  A suite without a
Packages file or without the package gets "version": null.

The metadata is validated strictly: channel ids end up in a root shell
script on the router, so an id that is not lowercase letters, digits and
'-' (2 to 32 characters, starting with a letter) is an error, as are
duplicate ids or aliases, over-long or multi-line labels and descriptions,
and a default that is not one of the ids.  Any error leaves the output
untouched and exits 1.

The output is written atomically.  When it would differ from the existing
file only in generated_at, the existing file is kept, so a run with no apt
or metadata change leaves no diff to commit.

Python standard library only.

Usage:
  gen_channels_json.py --apt-root DIR --meta FILE --out FILE
"""

import argparse
import datetime
import email.utils
import gzip
import json
import os
import re
import sys
import tempfile

SCHEMA = 1
MAX_BYTES = 64 * 1024
MAX_LABEL = 40
MAX_DESCRIPTION = 200
ID_RE = re.compile(r"[a-z][a-z0-9-]{0,30}[a-z0-9]")
VERSION_RE = re.compile(r"[0-9][A-Za-z0-9.+~:-]{0,63}")
TIMESTAMP_RE = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z")


class GenError(Exception):
    """A metadata or apt-repo problem that must stop the generator."""


def _check_id(value, where):
    if not isinstance(value, str) or not ID_RE.fullmatch(value):
        raise GenError(f"{where}: invalid channel id {value!r} "
                       "(expected ^[a-z][a-z0-9-]{0,30}[a-z0-9]$)")
    return value


def _check_text(value, where, limit, required):
    if value is None and not required:
        return ""
    if not isinstance(value, str):
        raise GenError(f"{where}: expected a string, got {value!r}")
    if any(ord(c) < 0x20 or ord(c) == 0x7F for c in value):
        raise GenError(f"{where}: control characters are not allowed")
    value = value.strip()
    if required and not value:
        raise GenError(f"{where}: must not be empty")
    if len(value) > limit:
        raise GenError(f"{where}: longer than {limit} characters")
    return value


def load_meta(path):
    """Returns the validated metadata as a dict with default, channels and installer."""
    try:
        with open(path, encoding="utf-8") as f:
            meta = json.load(f)
    except (OSError, ValueError) as e:
        raise GenError(f"{path}: {e}") from e
    if not isinstance(meta, dict):
        raise GenError(f"{path}: top level must be an object")
    schema = meta.get("schema")
    if type(schema) is not int or schema != SCHEMA:
        raise GenError(f"{path}: schema must be {SCHEMA}, got {schema!r}")

    raw_channels = meta.get("channels")
    if not isinstance(raw_channels, list) or not raw_channels:
        raise GenError(f"{path}: channels must be a non-empty list")
    names = set()
    channels = []
    for i, ch in enumerate(raw_channels):
        where = f"channels[{i}]"
        if not isinstance(ch, dict):
            raise GenError(f"{where}: must be an object")
        cid = _check_id(ch.get("id"), f"{where}.id")
        if cid in names:
            raise GenError(f"{where}.id: {cid!r} is already used")
        names.add(cid)
        aliases = ch.get("aliases", [])
        if not isinstance(aliases, list):
            raise GenError(f"{where}.aliases: must be a list")
        for j, alias in enumerate(aliases):
            _check_id(alias, f"{where}.aliases[{j}]")
            if alias in names:
                raise GenError(f"{where}.aliases[{j}]: {alias!r} is already used")
            names.add(alias)
        channels.append({
            "id": cid,
            "aliases": list(aliases),
            "label": _check_text(ch.get("label"), f"{where}.label", MAX_LABEL, True),
            "description": _check_text(ch.get("description"), f"{where}.description",
                                       MAX_DESCRIPTION, False),
        })

    default = meta.get("default")
    if default not in [c["id"] for c in channels]:
        raise GenError(f"{path}: default {default!r} is not one of the channel ids")

    installer = meta.get("installer", {})
    if not isinstance(installer, dict):
        raise GenError(f"{path}: installer must be an object")
    min_version = installer.get("min_version")
    if min_version is not None and (not isinstance(min_version, str)
                                    or not VERSION_RE.fullmatch(min_version)):
        raise GenError(f"{path}: installer.min_version {min_version!r} is not a version")
    download_url = installer.get("download_url")
    if download_url is not None and (not isinstance(download_url, str)
                                     or not download_url.startswith("https://")
                                     or any(c.isspace() for c in download_url)):
        raise GenError(f"{path}: installer.download_url must be an https:// URL")

    return {
        "default": default,
        "channels": channels,
        "installer": {"min_version": min_version, "download_url": download_url},
    }


def _read_text(path):
    """Returns the file's text, trying path then path.gz; None when neither exists."""
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        pass
    try:
        with gzip.open(path + ".gz", "rt", encoding="utf-8") as f:
            return f.read()
    except FileNotFoundError:
        return None


def _stanzas(text):
    """Yields each deb822 stanza as a dict of its single-line field values."""
    stanza = {}
    for line in text.splitlines():
        if not line.strip():
            if stanza:
                yield stanza
            stanza = {}
            continue
        if line[0] in " \t":
            continue
        key, sep, value = line.partition(":")
        if sep:
            stanza.setdefault(key.strip(), value.strip())
    if stanza:
        yield stanza


def suite_version(apt_root, suite, package, arch):
    """Returns the package version in the suite, or None when it has none."""
    path = os.path.join(apt_root, "dists", suite, "main", f"binary-{arch}", "Packages")
    text = _read_text(path)
    if text is None:
        return None
    for stanza in _stanzas(text):
        if stanza.get("Package") != package:
            continue
        version = stanza.get("Version", "")
        if not VERSION_RE.fullmatch(version):
            raise GenError(f"{path}: invalid version {version!r} for {package}")
        return version
    return None


def suite_updated_at(apt_root, suite):
    """Returns the suite's Release Date as an ISO 8601 UTC string, or None."""
    text = _read_text(os.path.join(apt_root, "dists", suite, "Release"))
    if text is None:
        return None
    date = next(_stanzas(text), {}).get("Date")
    if not date:
        return None
    try:
        when = email.utils.parsedate_to_datetime(date)
    except (TypeError, ValueError):
        return None
    if when.tzinfo is None:
        when = when.replace(tzinfo=datetime.timezone.utc)
    return when.astimezone(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def build_manifest(meta, apt_root, package, arch, now):
    channels = []
    for ch in meta["channels"]:
        entry = dict(ch)
        entry["version"] = suite_version(apt_root, ch["id"], package, arch)
        entry["updated_at"] = suite_updated_at(apt_root, ch["id"])
        channels.append(entry)
    return {
        "schema": SCHEMA,
        "generated_at": now,
        "default": meta["default"],
        "channels": channels,
        "installer": meta["installer"],
    }


def _same_apart_from_generated_at(path, manifest):
    try:
        with open(path, encoding="utf-8") as f:
            old = json.load(f)
    except (OSError, ValueError):
        return False
    if not isinstance(old, dict):
        return False
    old = {k: v for k, v in old.items() if k != "generated_at"}
    new = {k: v for k, v in manifest.items() if k != "generated_at"}
    return old == new


def write_atomic(path, data):
    directory = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(prefix=".channels-", suffix=".tmp", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--apt-root", required=True,
                        help="apt repository root (the directory holding dists/)")
    parser.add_argument("--meta", required=True, help="channels.meta.json")
    parser.add_argument("--out", required=True, help="channels.json to write")
    parser.add_argument("--package", default="onvif-recorder")
    parser.add_argument("--arch", default="arm64")
    parser.add_argument("--now", help="generated_at value (YYYY-MM-DDTHH:MM:SSZ); "
                                      "default: the current UTC time")
    args = parser.parse_args(argv)

    now = args.now or datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")
    if not TIMESTAMP_RE.fullmatch(now):
        parser.error(f"--now must look like 2026-01-31T12:00:00Z, got {now!r}")

    try:
        meta = load_meta(args.meta)
        manifest = build_manifest(meta, args.apt_root, args.package, args.arch, now)
    except GenError as e:
        print(f"gen_channels_json: error: {e}", file=sys.stderr)
        return 1

    summary = " ".join(f"{c['id']}={c['version'] or '-'}" for c in manifest["channels"])
    if _same_apart_from_generated_at(args.out, manifest):
        print(f"{args.out}: unchanged ({summary})")
        return 0

    data = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    if len(data.encode("utf-8")) > MAX_BYTES:
        print(f"gen_channels_json: error: output is larger than {MAX_BYTES} bytes",
              file=sys.stderr)
        return 1
    write_atomic(args.out, data)
    print(f"{args.out}: written ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
