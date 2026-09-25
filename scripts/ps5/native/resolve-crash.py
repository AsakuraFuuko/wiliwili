#!/usr/bin/env python3
"""Resolve a PS5 title crash address to a function in the linked image.

The kernel reports the faulting `rip` in absolute terms; the image is a PIE, so
the addresses only mean something once the load base is taken off. The base is
recovered from `wiliwili_boot_log`, whose symbol address the crash line prints as
`base=` (see native_shims.c). Arguments may be raw addresses or `rip:...` copied
straight out of the kernel log.

usage: resolve-crash.py 0x9662e2 [more addresses...]
       resolve-crash.py --base 0x9662e2        # print the load base only
"""

import bisect
import subprocess
import sys
from pathlib import Path

repo = Path(__file__).resolve().parents[3]
image = repo / "build-ps5" / "native" / "llvm-pie-symbols.elf"
fallback_image = repo / "build-ps5" / "native" / "llvm-pie.elf"

if not image.is_file():
    image = fallback_image
if not image.is_file():
    sys.exit(f"no linked image to symbolize against (looked for {image})")

symbols = []
for line in subprocess.run(
    ["readelf", "-sW", str(image)], capture_output=True, text=True
).stdout.splitlines():
    fields = line.split()
    if len(fields) >= 8 and fields[3] == "FUNC":
        try:
            symbols.append((int(fields[1], 16), fields[7]))
        except ValueError:
            pass
symbols.sort()

# wiliwili_boot_log is the symbol the crash line reports as `base=`.
boot_log = [address for address, name in symbols if name == "wiliwili_boot_log"]
if not boot_log:
    sys.exit("wiliwili_boot_log missing from the image; cannot recover the base")
boot_log_link = boot_log[0]

arguments = sys.argv[1:]
base_argument = None
if arguments and arguments[0] in ("--base", "-b"):
    base_argument = int(arguments[1], 16)
    arguments = arguments[2:]

if base_argument is None and not arguments:
    sys.exit(__doc__)

if base_argument is not None:
    print(f"load base = 0x{base_argument:x}")
    sys.exit(0)

if not arguments:
    sys.exit(__doc__)

def symbolize(address, base):
    index = bisect.bisect_right(symbols, (address, "\uffff")) - 1
    if index < 0:
        return "(below the first symbol)"
    start, name = symbols[index]
    return f"{name}+0x{address - start:x}"

# Every crash line carries `base=<runtime address of wiliwili_boot_log>`, so the
# caller can derive the base for each run; pass it as RIP=base pairs instead when
# several runs are involved. Here a single base is derived per group.
print(f"image: {image}")
print(f"wiliwili_boot_log at link address 0x{boot_log_link:x}")
for argument in arguments:
    raw = argument.split(":", 1)[-1]
    try:
        runtime = int(raw, 16)
    except ValueError:
        print(f"{argument}: not a hexadecimal address")
        continue
    print(f"{argument}: runtime 0x{runtime:x} (subtract the run's load base to symbolize)")
