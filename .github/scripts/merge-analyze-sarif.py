#!/usr/bin/env python3
"""Merge the per-translation-unit SARIF logs written by MSVC /analyze into the
single file the code-scanning API accepts.

/analyze:autolog writes one log beside every .obj, so a build of the emule
target leaves a few hundred of them scattered through the CMake build tree.
The code-scanning upload takes one file and a bounded number of runs, so they
have to be combined here rather than handed over as a directory.

Three things happen on the way through, none of them cosmetic:

  * Paths are resolved and rewritten. MSVC does not put a uri on a result at
    all: every artifactLocation carries an "index" into the run's own
    artifacts table, and only that table holds the path - as an absolute
    location in the runner's checkout (file:///D:/a/eMule-build/...), with the
    drive and directories cased inconsistently between entries. Code scanning
    matches alerts to blobs by repository-relative path and silently drops what
    it cannot resolve, so a merge that does not follow the indirection uploads
    a green run with no alerts attached to any file.

  * Path casing is restored from the filesystem. MSVC lowercases the
    translation unit it is analysing but keeps the original casing for every
    header it reached, so the same finding in a shared header arrives as both
    updownclient.h and UpDownClient.h. Code scanning matches blobs by exact
    path, so the lowercased half would attach to nothing - and the two spellings
    defeat the duplicate collapsing below, reporting one defect twice.

  * Results outside emule/srchybrid are discarded. /analyze:external- already
    keeps the analysis off MFC, the CRT and Crypto++, but a finding can still
    surface in a non-external header, and only the patch series' own perimeter
    is actionable here.

  * Duplicates are collapsed. Every translation unit includes Stdafx.h, so a
    finding in a shared header is reported once per TU that pulled it in - the
    same defect, up to 244 times.
"""

import argparse
import json
import os
import pathlib
import sys
import urllib.parse

# github/codeql-action/upload-sarif rejects a file carrying more than this many
# results. Hitting it means the report needs narrowing, not truncating, so the
# script says so loudly instead of quietly cutting the tail off.
MAX_RESULTS = 5000

PERIMETER = "emule/srchybrid/"

# Excluded for the same reason as in .github/codeql/codeql-config.yml: these two
# are bison/flex output, committed as generated. A finding in them cannot be
# fixed at the source, only in the .y/.l grammars or not at all. Scanner.cpp
# additionally carries #line directives naming the directory it was generated in
# two decades ago (d:/Data/Src/eMule_CVS/...), so its findings do not resolve to
# the checkout in the first place - excluded here explicitly rather than left to
# drop out by accident.
EXCLUDED = (
    "emule/srchybrid/parser.cpp",
    "emule/srchybrid/scanner.cpp",
)


def build_case_index(source_tree):
    """Map lowercased repository-relative path -> the path as it is on disk.

    MSVC's casing cannot be trusted (see the module docstring) and the code
    scanning API is case-sensitive, so the only authority is the checkout.
    """
    index = {}
    base = os.path.join(source_tree, *PERIMETER.strip("/").split("/"))
    for dirpath, _dirnames, filenames in os.walk(base):
        for filename in filenames:
            rel = os.path.relpath(os.path.join(dirpath, filename), source_tree)
            rel = rel.replace(os.sep, "/")
            index[rel.lower()] = rel
    return index


def normalize(path):
    """Collapse a Windows or POSIX path to one forward-slashed form.

    Deliberately string-only: os.path and pathlib interpret separators and
    drive letters according to the platform running the script, and this has
    to give the same answer for a D:\\a\\... path whether it is merging on the
    runner or being tested on Linux.
    """
    path = path.replace("\\", "/")
    while "//" in path:
        path = path.replace("//", "/")
    return path.rstrip("/")


def to_repo_relative(uri, root, root_len):
    """Return a repository-relative, forward-slashed path, or None if the file
    lies outside the checkout. `root` is already normalized and lowercased."""
    if not uri:
        return None
    if uri.startswith("file:"):
        uri = urllib.parse.unquote(urllib.parse.urlparse(uri).path)
        # file:///D:/a/... parses to /D:/a/...
        if len(uri) > 2 and uri[0] == "/" and uri[2] == ":":
            uri = uri[1:]
    path = normalize(uri)
    # Windows paths are case-insensitive and the compiler does not always
    # spell the drive or the directories the way the checkout did.
    if path.lower().startswith(root + "/"):
        return path[root_len + 1:]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--repo-root", required=True,
                    help="absolute path the compiler recorded, used only to "
                         "strip the prefix off the SARIF locations")
    ap.add_argument("--source-tree", default=".",
                    help="the checkout on disk, walked to recover path casing; "
                         "the same directory as --repo-root when merging on "
                         "the machine that built")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    repo_root = normalize(args.repo_root).lower()
    repo_root_len = len(repo_root)
    case_index = build_case_index(args.source_tree)
    print("files indexed for casing: %d" % len(case_index))

    logs = sorted(pathlib.Path(args.build_dir).rglob("*.sarif"))
    print("per-TU SARIF logs found: %d" % len(logs))
    if not logs:
        # /analyze:autolog names its files <source>.nativecodeanalysis.<ext>,
        # and the extension is what /analyze:autolog:ext sets. If that flag was
        # not honoured the logs are still there under the default extension, so
        # say what the build tree actually contains rather than reporting an
        # empty analysis as if it were a clean one.
        strays = sorted(pathlib.Path(args.build_dir).rglob("*nativecodeanalysis*"))
        print("no *.sarif logs found under %s" % args.build_dir, file=sys.stderr)
        print("files matching *nativecodeanalysis*: %d" % len(strays), file=sys.stderr)
        for stray in strays[:10]:
            print("  %s" % stray, file=sys.stderr)

    rule_ids = set()
    results = []
    seen = set()        # (ruleId, path, line, column, message) - see docstring
    unreadable = 0
    outside = 0
    unresolved = 0
    no_location = 0
    duplicates = 0
    excluded = 0
    # A dropped result is indistinguishable from a clean one in the totals, so
    # keep a few verbatim samples of each reason. The URI shape MSVC actually
    # writes is the thing most likely to be wrong here, and it cannot be read
    # off the counts.
    samples = {"unresolved": [], "outside": [], "no_location": []}

    def sample(kind, value):
        if len(samples[kind]) < 5 and value not in samples[kind]:
            samples[kind].append(value)

    for log in logs:
        try:
            with log.open(encoding="utf-8-sig") as fh:
                doc = json.load(fh)
        except (OSError, ValueError):
            unreadable += 1
            continue

        for run in doc.get("runs", []):
            # The artifacts table is what every location in this run points
            # into. It is per-run, so it must be read before the results and
            # cannot be shared between logs.
            artifact_uris = [
                (artifact.get("location") or {}).get("uri")
                for artifact in run.get("artifacts", []) or []
            ]

            def rewrite(artifact_location):
                """Resolve one artifactLocation to a repo-relative path and
                rewrite it in place. Returns the path, or None."""
                uri = artifact_location.get("uri")
                if uri is None:
                    index = artifact_location.get("index")
                    if isinstance(index, int) and 0 <= index < len(artifact_uris):
                        uri = artifact_uris[index]
                rel = to_repo_relative(uri, repo_root, repo_root_len)
                if rel is None:
                    return None
                rel = case_index.get(rel.lower(), rel)
                artifact_location["uri"] = rel
                # The merged run has no artifacts table, so any surviving index
                # would dangle.
                artifact_location.pop("index", None)
                artifact_location.pop("uriBaseId", None)
                return rel

            for result in run.get("results", []):
                rule_id = result.get("ruleId")
                result.pop("ruleIndex", None)
                # Also an index into the artifacts table being dropped.
                result.pop("analysisTarget", None)

                locations = result.get("locations") or []
                if not locations:
                    no_location += 1
                    sample("no_location", json.dumps(result)[:300])
                    continue

                primary = None
                keep = False
                resolved_any = False
                is_generated = False
                for loc in locations:
                    art = loc.get("physicalLocation", {}).get("artifactLocation", {})
                    rel = rewrite(art)
                    if rel is None:
                        sample("unresolved", json.dumps(loc)[:300])
                        continue
                    resolved_any = True
                    if rel.lower() in EXCLUDED:
                        is_generated = True
                        break
                    if rel.startswith(PERIMETER):
                        keep = True
                        if primary is None:
                            region = loc.get("physicalLocation", {}).get("region", {})
                            primary = (rel, region.get("startLine"), region.get("startColumn"))

                if is_generated:
                    excluded += 1
                    continue

                # relatedLocations are shown in the alert's detail view and
                # point into the same table, so they need resolving too.
                for loc in result.get("relatedLocations") or []:
                    rewrite(loc.get("physicalLocation", {}).get("artifactLocation", {}))

                if not keep:
                    if resolved_any:
                        outside += 1
                        sample("outside", (locations[0].get("physicalLocation", {})
                                           .get("artifactLocation", {}).get("uri", "")))
                    else:
                        unresolved += 1
                    continue

                key = (rule_id, primary, result.get("message", {}).get("text"))
                if key in seen:
                    duplicates += 1
                    continue
                seen.add(key)
                rule_ids.add(rule_id)
                results.append(result)

    # MSVC emits no rule metadata at all - no tool.driver.rules, just a ruleId
    # per result - so the table is synthesized here. Only the documentation
    # link is added: inventing a description from the first message text would
    # put one result's wording on every other result sharing the rule.
    merged_rules = [{
        "id": rule_id,
        "helpUri": "https://learn.microsoft.com/cpp/code-quality/%s" % rule_id.lower(),
    } for rule_id in sorted(rule_ids) if rule_id]
    index_of = {rule["id"]: i for i, rule in enumerate(merged_rules)}
    for result in results:
        idx = index_of.get(result.get("ruleId"))
        if idx is not None:
            result["ruleIndex"] = idx

    results.sort(key=lambda r: (
        r.get("ruleId") or "",
        (r.get("locations") or [{}])[0]
            .get("physicalLocation", {}).get("artifactLocation", {}).get("uri", ""),
        (r.get("locations") or [{}])[0]
            .get("physicalLocation", {}).get("region", {}).get("startLine", 0),
    ))

    sarif = {
        "version": "2.1.0",
        "$schema": "https://json.schemastore.org/sarif-2.1.0.json",
        "runs": [{
            "tool": {"driver": {
                "name": "MSVC Code Analysis",
                "informationUri": "https://learn.microsoft.com/cpp/code-quality/",
                "rules": merged_rules,
            }},
            "results": results,
        }],
    }

    with open(args.output, "w", encoding="utf-8") as fh:
        json.dump(sarif, fh, indent=1)

    print("unreadable logs:        %d" % unreadable)
    print("dropped, generated source: %d" % excluded)
    print("dropped, no location:   %d" % no_location)
    print("dropped, path not under the checkout: %d" % unresolved)
    print("dropped, outside %s: %d" % (PERIMETER, outside))
    print("dropped as duplicate:   %d" % duplicates)
    print("repo root used for matching: %s" % repo_root)
    for kind in ("unresolved", "outside", "no_location"):
        if samples[kind]:
            print("--- sample %s ---" % kind)
            for value in samples[kind]:
                print("  %s" % value)
    print("distinct rules:         %d" % len(merged_rules))
    print("results written:        %d" % len(results))
    if merged_rules:
        counts = {}
        for result in results:
            counts[result.get("ruleId")] = counts.get(result.get("ruleId"), 0) + 1
        print("--- by rule ---")
        for rule_id, n in sorted(counts.items(), key=lambda kv: -kv[1]):
            print("%6d  %s" % (n, rule_id))

    if len(results) > MAX_RESULTS:
        print("ERROR: %d results exceeds the %d the upload accepts; narrow the "
              "analysis rather than truncating." % (len(results), MAX_RESULTS),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
