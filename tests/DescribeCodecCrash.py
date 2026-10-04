#!/usr/bin/env python3
"""Describe an existing Windows test crash; never changes the test exit status.

Reads only this test's linker map and CTest log. No debugger, dumps, credentials,
or process-memory access. A non-main-module exception is left unidentified.
"""
import pathlib
import re
import sys


def describe(map_text, log_text):
    faults = re.findall(r"Windows exception (0x[0-9a-fA-F]+).*?image\+0x([0-9a-fA-F]+)", log_text)
    base = re.search(r"Preferred load address is\s+([0-9a-fA-F]+)", map_text)
    if not faults or not base:
        return ["No image-relative exception/map pair; use the last codec stage above."]
    preferred = int(base.group(1), 16)
    symbols = []
    for line in map_text.splitlines():
        match = re.match(r"\s+[0-9a-fA-F]{4}:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{8,16})\s+(.*)", line)
        if match:
            rva = int(match.group(2), 16) - preferred
            if 0 <= rva < 0x10000000:
                symbols.append((rva, match.group(1), match.group(3)))
    symbols.sort()
    result = []
    for code, offset in faults:
        rva = int(offset, 16)
        result.append(f"Codec exception {code}, executable RVA 0x{rva:x}")
        if rva >= 0x10000000:
            result.append("Fault is outside the executable image; no map-symbol inference.")
            continue
        preceding = [symbol for symbol in symbols if symbol[0] <= rva]
        if not preceding:
            result.append("No preceding public symbol in the linker map.")
        else:
            for address, name, source in preceding[-3:]:
                result.append(f"  preceding symbol +0x{rva-address:x}: {name} ({source})")
    return result


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: DescribeCodecCrash.py TEST.map LastTest.log")
    else:
        try:
            for item in describe(pathlib.Path(sys.argv[1]).read_text(errors="replace"),
                                 pathlib.Path(sys.argv[2]).read_text(errors="replace")):
                print(item)
        except OSError as error:
            print(f"Codec crash map/log unavailable: {error}")
