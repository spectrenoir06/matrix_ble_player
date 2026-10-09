# PlatformIO pre-script: warn when the linked spectre_protocol isn't the commit
# in spectre_protocol.ref (what CI builds with): update the file with
#   git -C ../../spectre_protocol rev-parse HEAD > spectre_protocol.ref
Import("env")  # noqa: F821  (PlatformIO)
import subprocess
from pathlib import Path

root = Path(env["PROJECT_DIR"])  # noqa: F821
proto = root / ".." / ".." / "spectre_protocol"
try:
    head = subprocess.run(["git", "-C", str(proto), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", str(proto), "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True).stdout.strip()
    pinned = (root / "spectre_protocol.ref").read_text().strip()
    if head and (head != pinned or dirty):
        print(f"\n*** spectre_protocol: building with {head[:7]}{' + uncommitted changes' if dirty else ''}, "
              f"CI uses {pinned[:7]} (spectre_protocol.ref) ***\n")
except OSError:
    pass
