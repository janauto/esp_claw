#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
app = component.parents[1]
cjson = app / "managed_components/espressif__cjson/cJSON"
with tempfile.TemporaryDirectory(prefix="ble-live-test-") as temp:
    binary = Path(temp) / "cache_test"
    data = Path(temp) / "data"
    data.mkdir()
    subprocess.run([
        "clang", "-std=c11", "-D_DARWIN_C_SOURCE", "-g", "-O1",
        "-fsanitize=address,undefined", "-Wno-deprecated-declarations",
        "-I" + str(component / "tests/include"),
        "-I" + str(component / "include"), "-I" + str(cjson),
        str(component / "tests/cache_regression.c"), str(component / "src/clip_feed.c"),
        str(component / "src/clip_live_format.c"), str(cjson / "cJSON.c"),
        "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(data)], check=True)
