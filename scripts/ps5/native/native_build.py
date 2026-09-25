#!/usr/bin/env python3
"""Compile, link, convert and package wiliwili as a PS5 native application.

The payload build's compile database is the source of truth for sources, include
paths and flags. Compilation is retargeted at the ps5-native-app runtime, then
LLVM lld links the boilerplate PIE layout, the repository tooling converts the
result into a PS5 module, wraps it in a development FSELF, and the title folder
is assembled for ShadowMountPlus.

Run through scripts/ps5/native/build-native.sh.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

# Flags that describe the payload runtime and must not reach the native app.
DROP_EXACT = {
    "-DUSE_GL2",
    "-DUSE_GLES2",
    "-DUSE_GLES3",
}
DROP_PREFIX = ("-DUSE_GL2", "--sysroot=", "-D_GLIBCXX")
NATIVE_DEFINES = ('-DUSE_GL3', '-DPS5_NATIVE_APP',
                  '-DBRLS_RESOURCES="/app0/assets/"',
                  # the payload toolchain wrapper enables these explicitly; the
                  # boilerplate wrapper targets the same ABI without them
                  '-fexceptions', '-frtti')

# WILIWILI_NATIVE_PROBE=1 builds the startup probe instead of the full
# application (see wiliwili/source/main.cpp): used to bisect native startup
# failures on the console.
if os.environ.get("WILIWILI_NATIVE_PROBE"):
    NATIVE_DEFINES += ('-DWILI_NATIVE_PROBE',)

# PS5_NATIVE_OSMESA_DIR selects software rendering: OSMesa is linked in and the
# startup probe reports its context, because the sandbox refuses to load code at
# runtime.
if os.environ.get("PS5_NATIVE_OSMESA_DIR"):
    NATIVE_DEFINES += ('-DWILIWILI_OSMESA_PROBE', '-DWILIWILI_SOFTWARE_RENDER')
if os.environ.get("WILIWILI_SKIP_HOME_REQUEST") == "1":
    NATIVE_DEFINES += ('-DWILIWILI_SKIP_HOME_REQUEST',)

# libromfs bundles resources into the payload; the native title reads /app0.
# libromfs stays enabled: a title sandbox denies directory iteration, so the
# embedded resource image is what the loaders can enumerate.
SKIP_SOURCES = ()


def run(argv: list[str], env: dict[str, str], cwd: Path | None = None,
        capture: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(argv, env=env, cwd=cwd, check=False,
                          text=True, capture_output=capture)


def compile_plan(cdb: Path, root: Path, sdk: Path, sdl2: Path, gl: Path) -> list[tuple[str, str, list[str]]]:
    entries = json.loads(cdb.read_text())
    plan = []
    for entry in entries:
        source = entry["file"]
        relative = os.path.relpath(source, root)
        if any(marker in relative for marker in SKIP_SOURCES):
            continue
        args = shlex.split(entry["command"])
        keep: list[str] = []
        skip_value = False
        for arg in args:
            if skip_value:
                skip_value = False
                continue
            if arg.endswith("prospero-clang++") or arg.endswith("prospero-clang"):
                continue
            if arg == source:
                # the source is appended again below, after the retargeted includes
                continue
            if arg == "-o":
                skip_value = True
                continue
            if arg == "-c":
                continue
            if arg in DROP_EXACT or arg.startswith(DROP_PREFIX):
                continue
            if arg.startswith("-I"):
                include = arg[2:]
                # The patched payload SDL2 is replaced by the ps5-opengl SDL2.
                if "ps5-sdl-prefix" in include:
                    continue
            keep.append(arg)
        keep += [definition for definition in NATIVE_DEFINES
                 if not (definition.startswith('-DBRLS_RESOURCES')
                         and any(a.startswith('-DBRLS_RESOURCES') for a in args))]
        keep += [f"-I{sdl2}/include", f"-I{sdl2}/include/SDL2", f"-I{gl}/include"]
        keep += ["-c", source]
        obj = str(Path(root) / "build-ps5" / "native" / "obj" / (relative.replace("/", "_") + ".o"))
        plan.append((source, obj, keep))
    return plan


def object_is_current(obj: Path) -> bool:
    """True when the object is newer than the source and every header it used.

    Compilation writes a make-style dependency file next to the object, so a
    header change (shared defines such as PS5_NATIVE_APP resource paths) also
    triggers a rebuild instead of leaving stale objects behind.
    """
    if not obj.exists():
        return False
    dependencies = Path(str(obj) + ".d")
    if not dependencies.exists():
        return False
    stamp = obj.stat().st_mtime
    for line in dependencies.read_text().splitlines():
        if line.endswith("\\"):
            line = line[:-1]
        for token in line.split():
            if token.endswith(":") or token == "\\":
                continue
            if not token.startswith("/"):
                continue
            try:
                if Path(token).stat().st_mtime > stamp:
                    return False
            except OSError:
                return False
    return True


def compile_sources(plan, env: dict[str, str], wrapper: Path, jobs: int, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    failures: list[tuple[str, str]] = []
    done = 0
    skipped = 0

    def build(item):
        source, obj, flags = item
        if object_is_current(Path(obj)):
            return source, obj, None, True
        argv = (["sh", str(wrapper)] + flags
                + ["-MMD", "-MF", obj + ".d", "-MT", obj, "-o", obj])
        result = run(argv, env)
        return source, obj, result, False

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for source, obj, result, cached in pool.map(build, plan):
            done += 1
            if cached:
                skipped += 1
                continue
            if result.returncode != 0:
                failures.append((source, result.stderr.strip()))
            elif done % 25 == 0:
                print(f"    compiled {done}/{len(plan)}", flush=True)

    print(f"==> compiled {len(plan) - len(failures) - skipped}/{len(plan)} "
          f"translation units ({skipped} unchanged)")
    for source, log in failures[:5]:
        print(f"--- {source}\n{log[:4000]}", file=sys.stderr)
    if failures:
        raise SystemExit(f"{len(failures)} translation units failed to compile")



def compile_runtime_objects(root: Path, toolchain: Path, sdk: Path, wrapper: Path,
                            env: dict[str, str], out: Path, gl: Path) -> list[str]:
    """Compile the CRT objects plus the ps5-opengl runtime shims the SDK ships.

    app_heap.c provides the --wrap allocation family and runtime_shims.c fills
    libc gaps the clean-room runtime does not export. The AGC entry points are
    linked as imports from the SDK stubs: defining them locally (as the template
    did) turns every GPU submission into a no-op, which renders the application
    invisible while every GL call still reports success.
    """
    sources = [(toolchain / "tooling" / "native" / name, "-std=c++20",
                ("-fno-exceptions", "-fno-rtti")) for name in
               ("app_crt.cpp", "app_cpp_runtime.cpp")]
    native_app = gl.parent / "native-app"
    if not (native_app / "runtime_shims.c").is_file():
        native_app = Path(os.environ.get("PS5_OPENGL_NATIVE_APP",
                                        str(toolchain.parent / "ps5-opengl" / "native-app")))
    # runtime_shims.c is deliberately not compiled: it exists because the SDK's
    # own builders rely on the clean-room runtime, while this build links the
    # payload libc, which already provides mkstemps/openlog/popen/pclose.
    for name in ("app_heap.c",):
        source = native_app / name
        if source.is_file():
            sources.append((source, "-std=gnu11", ()))
        else:
            print(f"note: {source} not found, skipping", file=sys.stderr)
    # The platform glue is compiled with the same feature defines as the
    # application: the software rendering variant gates its startup probe on one
    # of them.
    feature_defines = tuple(d for d in NATIVE_DEFINES if d.startswith("-DWILIWILI_"))
    extra_sources = ["native_shims.c", "native_libc_compat.c", "native_regex.c", "videodec2_probe.c", "audio_probe.c", "audio2_probe.c"]
    # native_libc_trace.c reports every string and memory call reached with a
    # NULL argument, which is how the software renderer's crash was identified.
    # It wraps hot libc entry points, so it is a diagnostic aid and stays out of
    # the deliverable image unless PS5_NATIVE_LIBC_TRACE=1 asks for it.
    if os.environ.get("PS5_NATIVE_LIBC_TRACE") == "1":
        extra_sources.append("native_libc_trace.c")
    for extra in extra_sources:
        sources.append((Path(__file__).with_name(extra), "-std=gnu11",
                        feature_defines))

    objects = []
    for source, standard, extra in sources:
        obj = str(out / (source.name + ".o"))
        argv = ["sh", str(wrapper), standard, "-O2", "-ffunction-sections",
                "-fdata-sections", *extra, "-c", str(source), "-o", obj]
        result = run(argv, env)
        if result.returncode != 0:
            print(result.stderr, file=sys.stderr)
            raise SystemExit(f"failed to compile {source.name}")
        objects.append(obj)
    return objects




def link(plan, runtime_objects, root: Path, toolchain: Path, sdk: Path, sdl2: Path,
         gl: Path, native_tool: Path, env: dict[str, str], out: Path) -> Path:
    objects = [obj for _, obj, _ in plan] + runtime_objects
    homebrew = sdk / "target" / "user" / "homebrew" / "lib"
    payload_libs = [
        "-lmpv", "-lavfilter", "-lswscale", "-lpostproc", "-lavformat", "-lavcodec",
        "-lSceVideodec2", "-lSceAudioOut", "-lSceAudioOut2", "-lSceUserService",
        "-lx264", "-lpthread", "-lswresample", "-lavutil", "-lssl", "-lcrypto",
        "-lass", "-liconv", "-lfontconfig", "-lexpat", "-lharfbuzz", "-lfribidi",
        "-lfreetype", "-lbz2", "-lpng16", "-lwebp", "-lsharpyuv", "-lz", "-lm",
        "-lsamplerate", "-lSceNet", "-lmbedcrypto", "-lmbedtls", "-lmbedx509", "-lpsl",
        "-lSceAgc", "-lSceAgcDriver", "-lSceSysmodule",
    ]
    romfs = Path(os.environ.get(
        "PS5_NATIVE_ROMFS_ARCHIVE",
        root / "build-ps5" / "library" / "borealis" / "library" / "lib" /
        "extern" / "libromfs" / "lib" / "libromfs-wiliwili.a"))
    if not romfs.is_file():
        raise SystemExit(
            f"embedded resource archive not found: {romfs}\n"
            "run scripts/ps5/build.sh first; it regenerates the archive from resources/")

    archives = [
        str(romfs),
        str(sdl2 / "lib" / "libSDL2.a"),
        str(sdk / "target" / "lib" / "libunwind.a"),
        str(sdk / "target" / "lib" / "libc++abi.a"),
        str(sdk / "target" / "lib" / "libc++.a"),
        str(homebrew / "libcurl.a"),
    ]
    # Software rendering variant: the sandbox refuses to load code at runtime, so
    # OSMesa is linked in instead of the GL/AGC stack. The static archives come
    # from a Mesa 22.1.7 cross build (see scripts/ps5/native/build-osmesa.sh).
    osmesa = os.environ.get("PS5_NATIVE_OSMESA_DIR")
    if osmesa:
        osmesa_path = Path(osmesa)
        archives.extend([
            str(osmesa_path / "libosmesa_st.a"),
            *([str(osmesa_path / "libllvmpipe.a")]
              if os.environ.get("PS5_NATIVE_OSMESA_LLVM", "1") == "1" else []),
            str(osmesa_path / "libws_null.a"),
            str(osmesa_path / "libmesa_sse41.a"),
            str(osmesa_path / "libgallium.a"),
            str(osmesa_path / "libmesa.a"),
            str(osmesa_path / "libglapi_static.a"),
            str(osmesa_path / "libglcpp.a"),
            str(osmesa_path / "osmesa_target.c.o"),
            str(osmesa_path / "libglsl.a"),
            str(osmesa_path / "libnir.a"),
            str(osmesa_path / "libcompiler.a"),
            str(osmesa_path / "libmesa_util.a"),
            str(osmesa_path / "libmesa_format.a"),
            *([str(p) for p in sorted((osmesa_path / "llvm").glob("libLLVM*.a"))]
              if os.environ.get("PS5_NATIVE_OSMESA_LLVM", "1") == "1" else []),
        ])
    else:
        archives.append("-lPS5OpenGLCore33")
    compiler_runtime = subprocess.run(
        ["clang-18", "--print-resource-dir"], check=True, text=True,
        capture_output=True).stdout.strip()
    archives.append(f"{compiler_runtime}/lib/linux/libclang_rt.builtins-x86_64.a")

    # The payload libc is deliberately not linked: its process bootstrap is
    # written for a payload environment and crashes a title before main. The
    # clean-room runtime provides the C API at load time, and
    # native_libc_compat.c/native_regex.c supply what the port libraries reach
    # for on top of it.
    group = [*payload_libs]
    if osmesa:
        # The OSMesa front end resolves its screen through the driver entry
        # points, and a plain archive only pulls objects that satisfy an
        # undefined symbol: force the whole software stack in.
        group = ["--whole-archive",
                 str(osmesa_path / "libsoftpipe.a"),
                 "--no-whole-archive", *group]
    if os.environ.get("PS5_NATIVE_OSMESA_DIR"):
        group.extend(["-lz", "-lzstd", "-lm"])

    # The boilerplate's PIE layout, plus the frame-bound symbols the ps5-opengl
    # GL library references.
    base = toolchain / "tooling" / "native" / "ps5-pie.ld"
    if not base.is_file():
        raise SystemExit(f"linker script not found: {base}")
    script = out / "toolchain" / "native" / "ps5-pie.ld"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text(
        base.read_text()
        + "\nPROVIDE(__eh_frame_start = ADDR(.eh_frame));\n"
        + "PROVIDE(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));\n"
        + "PROVIDE(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));\n"
        + "PROVIDE(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));\n")

    pie = out / "llvm-pie.elf"
    # The allocation family is wrapped for the heap helper (app_heap.c); the rest
    # wraps hot libc entry points and only exists for the NULL-argument trace.
    wraps = ["--wrap=malloc", "--wrap=calloc", "--wrap=realloc", "--wrap=free",
             "--wrap=posix_memalign", "--wrap=malloc_usable_size"]
    if os.environ.get("PS5_NATIVE_LIBC_TRACE") == "1":
        wraps += [
            "--wrap=memcpy", "--wrap=memmove", "--wrap=memcmp", "--wrap=memchr",
            "--wrap=strlen", "--wrap=strcmp", "--wrap=strncmp", "--wrap=strchr",
            "--wrap=vsnprintf", "--wrap=fwrite", "--wrap=fread", "--wrap=fputs",
            "--wrap=fprintf", "--wrap=vfprintf", "--wrap=fopen", "--wrap=fclose",
            "--wrap=fflush", "--wrap=memset", "--wrap=strstr", "--wrap=strnlen",
            "--wrap=strlcpy", "--wrap=strlcat", "--wrap=strcpy", "--wrap=strncpy",
            "--wrap=strcat", "--wrap=__srget", "--wrap=__swbuf",
            "--wrap=mprotect", "--wrap=munmap", "--wrap=mmap",
        ]
    argv = [
        str(sdk / "bin" / "prospero-lld"),
        "-T", str(script),
        "--eh-frame-hdr",
        *wraps,
        "--version-script", str(Path(__file__).with_name("app-symbols.map")),
        "-e", "_start", "-o", str(pie),
        *objects,
        *archives,
        f"-L{gl}/lib", f"-L{homebrew}", f"-L{sdk}/target/lib",
        "--start-group", *group, "--end-group",
        "--as-needed", *[str(p) for p in sorted(sdk.glob("target/lib/*.so"))],
    ]
    result = run(argv, env)
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        raise SystemExit("link failed")

    # Debug sections are useless on the console and the system volume that holds
    # the application image has very little room; keep a copy with symbols for
    # offline crash resolution and convert the stripped image.
    symbols = out / "llvm-pie-symbols.elf"
    # Keep this in step with the image: crash addresses are only resolvable
    # against the build that produced them.
    symbols.write_bytes(pie.read_bytes())
    strip_tool = sdk / "bin" / "prospero-strip"
    if strip_tool.is_file():
        run([str(strip_tool), "--strip-debug", str(pie)], env)

    module = out / "eboot.elf"
    # The converter resolves the imported NIDs against a stub directory that
    # must contain the module stubs of every imported library. The payload SDK
    # alone lacks the AGC stubs (libSceAgc.prx / libSceAgcDriver.prx), which the
    # GL implementation imports, so a stub directory that carries them wins.
    stub_dir = Path(os.environ.get("PS5_NATIVE_STUB_DIR",
                                   str(gl.parent.parent.parent / "sdl-folder" / ".deps" / "native" /
                                       "ps5-payload-sdk" / "target" / "lib")))
    if not (stub_dir / "libSceAgc.so").is_file():
        stub_dir = sdk / "target" / "lib"

    result = run([str(native_tool), "link", "--in", str(pie), "--out", str(module),
                  "--stub-dir", str(stub_dir),
                  "--module-sdk", "0x02000009", "--companion-sdk", "0x08050001",
                  # The converted module keeps every debug section otherwise: the
                  # image would not fit the console's free space.
                  "--strip-sections",
                  "--file-name", "eboot.elf"], env)
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        raise SystemExit("module conversion failed")
    return module


def package(module: Path, root: Path, toolchain: Path, sdk: Path, out: Path,
            title_id: str) -> Path:
    dist = out / "dist" / title_id
    if dist.exists():
        shutil.rmtree(dist)
    (dist / "sce_sys").mkdir(parents=True)

    result = run([str(out / "ps5-native-tool"), "self", "--sign", "--in", str(module),
                  "--out", str(dist / "eboot.bin"), "--magic", "0x1D3D154F"], os.environ.copy())
    if result.returncode != 0:
        print(result.stdout, file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        raise SystemExit("FSELF signing failed")

    (dist / "sce_module").mkdir(parents=True, exist_ok=True)
    shutil.copy2(toolchain / "runtime" / "libc.prx", dist / "sce_module" / "libc.prx")

    # Resources are embedded (libromfs); only the trust store stays on disk,
    # because libcurl needs a real file for CURLOPT_CAINFO.

    # HTTPS requests point at /app0/assets/ca-bundle.crt in an installed title.
    (dist / "assets").mkdir(parents=True, exist_ok=True)
    ca_bundle = sdk / "target" / "user" / "homebrew" / "etc" / "ca-bundle.crt"
    if ca_bundle.is_file():
        shutil.copy2(ca_bundle, dist / "assets" / "ca-bundle.crt")
    else:
        print(f"warning: {ca_bundle} not found; HTTPS trust store missing",
              file=sys.stderr)

    # The shell expects a 512x512 launcher icon (PLATFORM_NOTES). wiliwili ships
    # smaller artwork, so scale it onto a square canvas when Pillow is present.
    icon = root / "resources" / "icon" / "icon.png"
    if not icon.is_file():
        icon = root / "resources" / "icon" / "icon.jpg"
    if icon.is_file():
        target = dist / "sce_sys" / "icon0.png"
        try:
            from PIL import Image  # type: ignore

            with Image.open(icon) as image:
                image = image.convert("RGBA")
                scale = min(512 / image.width, 512 / image.height)
                resized = image.resize(
                    (max(1, round(image.width * scale)), max(1, round(image.height * scale))),
                    Image.LANCZOS)
                canvas = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
                canvas.paste(resized, ((512 - resized.width) // 2,
                                       (512 - resized.height) // 2))
                canvas.save(target)
        except ImportError:
            shutil.copy2(icon, target)
            print("note: Pillow unavailable, icon0.png keeps its source size",
                  file=sys.stderr)
    else:
        print(f"warning: no launcher icon found under {icon.parent}", file=sys.stderr)

    template = json.loads((toolchain / "sce_sys" / "param.json").read_text())
    concept = title_id[-5:]
    template["titleId"] = title_id
    template["conceptId"] = concept
    template["contentId"] = f"UP9000-{title_id}_00-WILIWILI00000000"
    localized = template.setdefault("localizedParameters", {})
    for language in ("en-US", "zh-Hans"):
        localized.setdefault(language, {})["titleName"] = "wiliwili"
    localized["defaultLanguage"] = "zh-Hans"
    (dist / "sce_sys" / "param.json").write_text(json.dumps(template, indent=2) + "\n")
    return dist


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--toolchain", required=True)
    parser.add_argument("--sdk", required=True)
    parser.add_argument("--sdl2", required=True)
    parser.add_argument("--gl", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--cdb", required=True)
    parser.add_argument("--title-id", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    args = parser.parse_args()

    root, toolchain = Path(args.root), Path(args.toolchain)
    sdk, sdl2, gl, out = Path(args.sdk), Path(args.sdl2), Path(args.gl), Path(args.out)
    env = os.environ.copy()
    env["PS5_PAYLOAD_SDK"] = str(sdk)
    wrapper = toolchain / "tooling" / "prospero-clang18"

    # Compile flags are part of the build identity: a define change (for
    # example WILIWILI_NATIVE_PROBE) must invalidate cached objects even though
    # no source file changed.
    stamp = out / "compile-flags.txt"
    identity = "\n".join(NATIVE_DEFINES)
    if stamp.exists() and stamp.read_text() != identity:
        print("==> compile flags changed, rebuilding all translation units")
        for cached in (out / "obj").glob("*.o*"):
            cached.unlink()
    out.mkdir(parents=True, exist_ok=True)
    stamp.write_text(identity)

    plan = compile_plan(Path(args.cdb), root, sdk, sdl2, gl)
    print(f"==> compiling {len(plan)} translation units for the native runtime")
    compile_sources(plan, env, wrapper, args.jobs, out / "obj")
    runtime_objects = compile_runtime_objects(root, toolchain, sdk, wrapper, env, out, gl)
    module = link(plan, runtime_objects, root, toolchain, sdk, sdl2, gl,
                  out / "ps5-native-tool", env, out)
    dist = package(module, root, toolchain, sdk, out, args.title_id)
    print(f"==> native title assembled in {dist}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
