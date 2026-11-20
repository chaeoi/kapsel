import re
import subprocess
from datetime import datetime, timezone

DEVELOPMENT_VERSION = "0.0.0-dev"


def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def parse_version(value):
    if not re.fullmatch(r"[2-9][0-9]{3}\.([1-9]|1[0-2])\.[1-9][0-9]*", value):
        return None
    return tuple(map(int, value.split(".")))


def select_version(head):
    date = datetime.fromisoformat(git("show", "-s", "--format=%cI", head))
    utc = date.astimezone(timezone.utc)
    month = (utc.year, utc.month)
    tags = {}
    tags_with_targets = git(
        "for-each-ref", "--format=%(refname:strip=2) %(objectname) %(*objectname)", "refs/tags"
    )
    for ref in tags_with_targets.splitlines():
        name, target, *peeled = ref.split()
        number = parse_version(name)
        if number:
            tags[number] = peeled[0] if peeled else target

    assigned = [number for number, target in tags.items() if target == head]
    if assigned:
        return ".".join(map(str, max(assigned)))

    for target in set(tags.values()):
        ancestor = subprocess.run(["git", "merge-base", "--is-ancestor", head, target])
        if ancestor.returncode == 0:
            return None
        if ancestor.returncode != 1:
            ancestor.check_returncode()
    sequence = max((number[2] for number in tags if number[:2] == month), default=0) + 1
    return f"{utc.year}.{utc.month}.{sequence}"


if __name__ == "__main__":
    print(select_version(git("rev-parse", "HEAD")) or "")
