#!/usr/bin/env python3
"""Host-only checks using production PCM and queue code; no game data needed."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
here = Path(__file__).resolve().parent
repo = here.parents[2]
with tempfile.TemporaryDirectory(prefix="towns-audio-test-") as folder:
    tmp = Path(folder)
    shutil.copytree(here / "stubs", tmp, dirs_exist_ok=True)
    pcm = repo / "audio/softsynth/fmtowns_pc98/pcm_common.cpp"
    (tmp / "pcm_common.cpp").write_text(pcm.read_text().replace(
        '#include "audio/softsynth/fmtowns_pc98/pcm_common.h"', '#include "pcm_common.h"'))
    shutil.copy(pcm.with_suffix(".h"), tmp / "pcm_common.h")
    compiler = os.environ.get("CXX", "c++")
    flags = [compiler, "-std=c++11", "-O2", "-fsanitize=address,undefined"]
    subprocess.run(flags + ["-I" + str(tmp), str(tmp / "pcm_common.cpp"),
        str(here / "pcm-test.cpp"), "-o", str(tmp / "pcm-test")], check=True)
    env = os.environ.copy()
    env.pop("PCM_TEST_BANK", None)
    subprocess.run([str(tmp / "pcm-test")], env=env, check=True)
    env["PCM_TEST_BANK"] = "1"
    subprocess.run([str(tmp / "pcm-test")], env=env, check=True)
    source = (repo / "backends/mixer/sdl/sdl-mixer.cpp").read_text()
    start = source.index("void SdlMixerManager::pump()")
    end = source.index("\n}\n", start) + 3
    queue = (here / "queue-test.in.cpp").read_text().replace(
        "// PRODUCTION_PUMP", source[start:end])
    (tmp / "queue-test.cpp").write_text(queue)
    subprocess.run(flags + [str(tmp / "queue-test.cpp"), "-o", str(tmp / "queue-test")], check=True)
    subprocess.run([str(tmp / "queue-test")], check=True)
