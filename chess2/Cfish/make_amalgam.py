import urllib.request
import zipfile
import io
import os
import re

# 1. Download the latest source code from syzygy1/Cfish
print("Downloading Cfish source from GitHub...")
url = "https://github.com/syzygy1/Cfish/archive/refs/heads/master.zip"
req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})

with urllib.request.urlopen(req) as response:
    zip_data = response.read()

# 2. Extract files into memory
print("Extracting files...")
zip_file = zipfile.ZipFile(io.BytesIO(zip_data))
file_map = {}
for name in zip_file.namelist():
    if "/src/" in name and (name.endswith(".c") or name.endswith(".h")):
        short_name = name.split("/src/")[-1]
        file_map[short_name] = zip_file.read(name).decode('utf-8', errors='ignore')

# 3. Define the exact compilation order to resolve dependencies
headers = [
    "types.h", "misc.h", "bitboard.h", "position.h", "movegen.h",
    "movepick.h", "pawns.h", "material.h", "endgame.h", "evaluate.h",
    "thread.h", "timeman.h", "tt.h", "uci.h", "tbprobe.h"
]

sources = [
    "misc.c", "bitboard.c", "position.c", "movegen.c", "movepick.c",
    "pawns.c", "material.c", "endgame.c", "evaluate.c", "thread.c",
    "timeman.c", "tt.c", "uci.c", "tbprobe.c", "main.c"
]

# 4. Process and combine
print("Amalgamating into a single file...")
system_includes = set()
output_body = []

# Regex to detect includes
sys_inc_re = re.compile(r'^\s*#\s*include\s*<([^>]+)>')
loc_inc_re = re.compile(r'^\s*#\s*include\s*"([^"]+)"')

def process_file(filename, content):
    output_body.append(f"\n/* ==========================================\n")
    output_body.append(f"   FILE: {filename}\n")
    output_body.append(f"   ========================================== */\n\n")
    
    for line in content.splitlines():
        # Capture system headers (e.g. <stdio.h>) to place them at the very top
        sys_match = sys_inc_re.match(line)
        if sys_match:
            system_includes.add(sys_match.group(1))
            continue
            
        # Strip local headers (e.g. "types.h") as they are now inlined
        if loc_inc_re.match(line):
            continue
            
        output_body.append(line + "\n")

# Process all headers first, then all source files
for h in headers:
    if h in file_map:
        process_file(h, file_map[h])
for s in sources:
    if s in file_map:
        process_file(s, file_map[s])

# 5. Write the final unified .c file
output_filename = "cfish_amalgamated.c"
with open(output_filename, "w", encoding="utf-8") as f:
    f.write("/* Cfish: Single-File Amalgamated Version */\n")
    f.write("/* Original code by Syzygy1 (GPv3 License) */\n\n")
    
    # Write system headers at the very top to prevent duplicate imports/ordering conflicts
    f.write("/* --- System Headers --- */\n")
    for header in sorted(system_includes):
        f.write(f"#include <{header}>\n")
    f.write("\n")
    
    # Write the combined code
    f.write("".join(output_body))

print(f"Success! Created '{output_filename}' (~12,500 lines of code).")