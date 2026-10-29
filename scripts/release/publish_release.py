"""CI-only publication of both verified installers; never overwrite an existing release.

Once a version is published, only the newest RETAINED_RELEASES stay on GitHub: every older
release is deleted, its tag kept, so the releases page holds the versions worth downloading.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from release import ROOT, digest, validate_tag
from installer.install import payload_inventory
from installer.releases import runtime_name
from installer.versions import is_prerelease, version_order

RETAINED_RELEASES = 10


def require_new_version(tag, existing_tags):
    """Published releases and drafts both reserve their version; never reuse or downgrade."""
    candidate = version_order(tag)
    if candidate is None:
        raise ValueError("A release requires a semantic version")
    for existing in existing_tags:
        if existing == tag:
            raise ValueError(f"Release {tag} already exists; increment VERSION instead")
        previous = version_order(existing)
        if previous is not None and candidate <= previous:
            raise ValueError(f"Release {tag} must be newer than existing release {existing}")


def changelog(tag):
    """Every new release must carry reviewed notes for exactly its version."""
    if not tag.startswith("v") or version_order(tag) is None:
        raise ValueError("A changelog requires a semantic version tag")
    path = ROOT / "docs" / "changelog" / f"{tag[1:]}.md"
    try:
        notes = path.read_text(encoding="utf-8").strip()
    except FileNotFoundError as error:
        raise ValueError(f"Missing release changelog: {path}") from error
    heading, _, body = notes.partition("\n")
    if heading != f"# {tag[1:]}" or not body.strip():
        raise ValueError(f"Changelog must have a matching version heading and release notes: {path}")
    sections = []
    has_notes = False
    for line in body.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("#"):
            sections.append(line)
        elif not sections:
            raise ValueError(f"Changelog notes must be under Added or Fixed: {path}")
        else:
            has_notes = True
    if sections != ["## Added", "## Fixed"] or not has_notes:
        raise ValueError(f"Changelog must contain only Added and Fixed sections: {path}")
    return notes


def require_green_ci(sha, wait_minutes=90, poll_seconds=60, sleep=time.sleep):
    """The CI workflow must have passed on the exact commit being released. A run still
    going is waited for; none at all, or anything but success, refuses the release."""
    deadline = time.monotonic() + wait_minutes * 60
    while True:
        rows = subprocess.check_output(
            ["gh", "api", f"repos/{{owner}}/{{repo}}/actions/workflows/ci.yml/runs?head_sha={sha}&per_page=20",
             "--jq", ".workflow_runs[] | [.status, (.conclusion // \"\")] | @tsv"],
            cwd=ROOT, text=True).splitlines()
        if not rows:
            raise ValueError(f"No CI run exists for {sha}; push the commit to main and let CI pass first")
        status, conclusion = rows[0].split("\t")
        if status == "completed":
            if conclusion == "success":
                return
            raise ValueError(f"CI concluded '{conclusion}' for {sha}; a release needs a green run")
        if time.monotonic() >= deadline:
            raise ValueError(f"CI is still '{status}' for {sha} after {wait_minutes} minutes")
        sleep(poll_seconds)


def check_publication():
    """Fail closed before building and again before publishing, without mutating GitHub."""
    if os.environ.get("GITHUB_EVENT_NAME") != "push":
        raise ValueError("Only a pushed version tag may publish; manual runs are build-only")
    tag = validate_tag(os.environ["RELEASE_TAG"])
    if os.environ.get("GITHUB_REF") != f"refs/tags/{tag}":
        raise ValueError("Publication requires the matching version tag, not a branch")
    changelog(tag)
    publication_is_prerelease(tag)
    # --paginate includes old versions and drafts. Authentication/network failures abort,
    # rather than being mistaken for proof that the version has not been published.
    existing = subprocess.check_output(
        ["gh", "api", "--paginate", "repos/{owner}/{repo}/releases", "--jq", ".[].tag_name"],
        cwd=ROOT, text=True).splitlines()
    require_new_version(tag, existing)
    require_green_ci(os.environ["GITHUB_SHA"])
    return tag


def publication_is_prerelease(tag):
    """An optional version-bound policy separates GitHub status from the build version."""
    path = ROOT / "docs" / "changelog" / f"{tag[1:]}.release.json"
    if not path.exists():
        return is_prerelease(tag)
    policy = json.loads(path.read_text(encoding="utf-8"))
    if (not isinstance(policy, dict) or set(policy) != {"version", "prerelease"}
            or policy["version"] != tag[1:] or type(policy["prerelease"]) is not bool):
        raise ValueError(f"Invalid version-bound release policy: {path}")
    return policy["prerelease"]


def retired_releases(rows, keep=RETAINED_RELEASES):
    """The tags of the published releases past the `keep` newest versions, oldest last.
    `rows` pair each release's tag with whether it is a draft; drafts are never counted
    nor deleted, and a tag that is not a version is left alone."""
    published = sorted((tag for tag, draft in rows if not draft and version_order(tag) is not None),
                       key=version_order, reverse=True)
    return published[keep:]


def prune_releases(keep=RETAINED_RELEASES):
    """Deletes every published release past the `keep` newest, keeping its tag."""
    listed = subprocess.check_output(
        ["gh", "api", "--paginate", "repos/{owner}/{repo}/releases", "--jq",
         ".[] | [.tag_name, (.draft | tostring)] | @tsv"],
        cwd=ROOT, text=True).splitlines()
    rows = []
    for line in listed:
        tag, _, draft = line.partition("\t")
        rows.append((tag, draft == "true"))
    retired = retired_releases(rows, keep)
    for tag in retired:
        subprocess.run(["gh", "release", "delete", tag, "--yes"], cwd=ROOT, check=True)
    return retired


def validated_assets(folder, tag):
    """Require both platform packages and their exact checksum inventories before publishing."""
    assets = []
    for system, suffix in (("windows-x64", ".exe"), ("linux-x64", ".tar.gz")):
        stem = f"GauntletDarkLegacy-{tag[1:]}-{system}-setup"
        checksum = folder / f"{stem}.sha256"
        expected = {f"{stem}{suffix}", f"{stem}-licenses.zip", runtime_name(tag[1:], system)}
        seen = set()
        for line in checksum.read_text(encoding="ascii").splitlines():
            sha, name = line.split("  ", 1)
            if name not in expected or name in seen or digest(folder / name) != sha:
                raise ValueError(f"Invalid release checksum: {name}")
            seen.add(name)
            assets.append(folder / name)
        if seen != expected:
            raise ValueError(f"Incomplete {system} release")
        metadata, _ = payload_inventory(folder / runtime_name(tag[1:], system))
        executable = "gauntlet.exe" if system == "windows-x64" else "gauntlet"
        if (metadata["version"] != tag[1:] or metadata["platform"] != system or
                metadata["executable"] != executable):
            raise ValueError(f"Runtime inventory disagrees with the {system} release")
        assets.append(checksum)
    return assets


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="check eligibility without publishing")
    args = parser.parse_args(argv)
    tag = check_publication()
    if args.check:
        print(f"Eligible new release: {tag}")
        return
    assets = validated_assets(ROOT / "release-assets", tag)
    prerelease = publication_is_prerelease(tag)
    notes = changelog(tag) + "\n"
    with tempfile.TemporaryDirectory(prefix="gdl-release-notes-") as temporary:
        path = Path(temporary) / "notes.txt"
        path.write_text(notes, encoding="utf-8")
        subprocess.run(["gh", "release", "create", tag, "--verify-tag", *(["--prerelease"] if prerelease else ["--latest"]),
                        "--title", f"Gauntlet Dark Legacy {tag}", "--notes-file", str(path),
                        *map(str, assets)], cwd=ROOT, check=True)
    for retired in prune_releases():
        print(f"Retired release {retired}; its tag remains")


if __name__ == "__main__":
    main()
