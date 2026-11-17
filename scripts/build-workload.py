#!/usr/bin/env python3
"""Export an OCI image into a writable SVMM root disk without running its code."""
import argparse
import json
import math
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

CONFIG_MAGIC = b"SVMMWL1\0"
CONFIG_MAX = 1024 * 1024
ITEMS_MAX = 4096


def run(*argv, **kwargs):
    return subprocess.run(argv, check=True, **kwargs)


def parts(name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError("unsafe archive path: " + repr(name))
    return tuple(p for p in path.parts if p != ".")


def directory(root_fd, components):
    """Walk ONLY real directories, never archive-created symlinks."""
    fd = os.dup(root_fd)
    try:
        for component in components:
            try:
                os.mkdir(component, 0o700, dir_fd=fd)
            except FileExistsError:
                pass
            child = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                            dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise


def attributes(fd, member):
    os.fchown(fd, member.uid, member.gid)
    os.fchmod(fd, member.mode & 0o7777)
    os.utime(fd, (member.mtime, member.mtime))


def extract(archive_path, root):
    """Preserve numeric owners and links, with fd-relative no-follow extraction."""
    root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    directories, links = [], []
    used = 0
    try:
        with tarfile.open(archive_path, "r:") as archive:
            for member in archive:
                path = parts(member.name)
                if not path:
                    continue
                parent = directory(root_fd, path[:-1])
                try:
                    name = path[-1]
                    if member.isdir():
                        fd = directory(root_fd, path)
                        os.close(fd)
                        directories.append((path, member))
                    elif member.isfile():
                        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                                     os.O_NOFOLLOW, 0o600, dir_fd=parent)
                        with os.fdopen(fd, "wb") as target, archive.extractfile(member) as source:
                            shutil.copyfileobj(source, target)
                            target.flush()
                            attributes(target.fileno(), member)
                        used += member.size
                    elif member.issym():
                        os.symlink(member.linkname, name, dir_fd=parent)
                        os.chown(name, member.uid, member.gid, dir_fd=parent,
                                 follow_symlinks=False)
                    elif member.islnk():
                        links.append((path, parts(member.linkname)))
                    elif member.isfifo():
                        os.mkfifo(name, member.mode & 0o7777, dir_fd=parent)
                        os.chown(name, member.uid, member.gid, dir_fd=parent,
                                 follow_symlinks=False)
                    else:
                        raise ValueError("unsupported special file in image: " + repr(member.name))
                finally:
                    os.close(parent)
        # Resolve forward hardlinks without ever following a symlink ancestor.
        while links:
            pending = []
            for path, source in links:
                if not source:
                    raise ValueError("empty hardlink target")
                src_fd = directory(root_fd, source[:-1])
                dst_fd = directory(root_fd, path[:-1])
                try:
                    try:
                        os.link(source[-1], path[-1], src_dir_fd=src_fd,
                                dst_dir_fd=dst_fd, follow_symlinks=False)
                    except FileNotFoundError:
                        pending.append((path, source))
                finally:
                    os.close(src_fd)
                    os.close(dst_fd)
            if len(pending) == len(links):
                raise ValueError("unresolved archive hardlinks")
            links = pending
        # Apply directory modes last, so read-only directories can be populated.
        for path, member in reversed(directories):
            fd = directory(root_fd, path)
            try:
                attributes(fd, member)
            finally:
                os.close(fd)
    finally:
        os.close(root_fd)
    return used


def encode(config, options):
    entrypoint = config.get("Entrypoint") or []
    if options.entrypoint is not None:
        entrypoint = [options.entrypoint] if options.entrypoint else []
    command = options.command
    if command and command[0] == "--":
        command = command[1:]
    argv = entrypoint + (command if command else (config.get("Cmd") or []))
    if not argv or not argv[0]:
        raise ValueError("image has no executable; supply --entrypoint or a command")
    env = {}
    for item in [*(config.get("Env") or []), *options.env]:
        if "=" not in item or not item.split("=", 1)[0]:
            raise ValueError("environment must be KEY=VALUE")
        key, value = item.split("=", 1)
        env[key] = value
    env_list = [key + "=" + value for key, value in env.items()]
    cwd = options.workdir if options.workdir is not None else config.get("WorkingDir") or "/"
    user = options.user if options.user is not None else config.get("User") or "0:0"
    if not cwd.startswith("/"):
        raise ValueError("working directory must be absolute")
    strings = [cwd, user, *argv, *env_list]
    if len(argv) > ITEMS_MAX or len(env_list) > ITEMS_MAX:
        raise ValueError("too many command arguments or environment variables")
    if any(not isinstance(s, str) or "\0" in s for s in strings):
        raise ValueError("configuration strings must not contain NUL")
    blob = CONFIG_MAGIC + struct.pack("<II", len(argv), len(env_list))
    blob += b"".join(s.encode("utf-8") + b"\0" for s in strings)
    if len(blob) > CONFIG_MAX:
        raise ValueError("workload configuration exceeds 1 MiB")
    return blob


def build(options):
    output = Path(options.output).absolute()
    if os.path.lexists(output):
        raise ValueError("output already exists: " + str(output))
    if os.geteuid() != 0:
        raise ValueError("run the builder as root to preserve image UID/GID and permissions")
    for tool in ("docker", "mkfs.ext4"):
        if not shutil.which(tool):
            raise ValueError("missing required tool: " + tool)
    if options.size_mb is not None and options.size_mb < 16:
        raise ValueError("--size-mb must be at least 16")
    try:
        inspected = run("docker", "image", "inspect", options.image, capture_output=True)
    except subprocess.CalledProcessError:
        run("docker", "pull", "--platform", "linux/amd64", options.image)
        inspected = run("docker", "image", "inspect", options.image, capture_output=True)
    image = json.loads(inspected.stdout)[0]
    if image.get("Os") != "linux" or image.get("Architecture") != "amd64":
        raise ValueError("only linux/amd64 images are supported")
    blob = encode(image["Config"], options)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="svmm-workload-build-") as work:
        work = Path(work)
        root = work / "root"
        root.mkdir()
        container = None
        try:
            created = run("docker", "create", "--label", "svmm.workload-builder=true",
                          "--entrypoint", "/__svmm_export_only__", image["Id"],
                          capture_output=True)
            container = created.stdout.decode().strip()
            archive = work / "rootfs.tar"
            run("docker", "export", "--output", str(archive), container)
            used = extract(archive, root)
        finally:
            if container:
                run("docker", "rm", "-v", container, stdout=subprocess.DEVNULL)
        # Reserved path must not redirect our metadata writes through image links.
        metadata = root / ".svmm"
        if os.path.lexists(metadata):
            raise ValueError("image contains reserved /.svmm path")
        metadata.mkdir(mode=0o700)
        (metadata / "workload.bin").write_bytes(blob)
        (metadata / "image.json").write_text(json.dumps(image, indent=2) + "\n")
        for item in metadata.iterdir():
            item.chmod(0o600)
        size_mb = options.size_mb or max(64, math.ceil(used * 1.3 / (1024 * 1024)) + 64)
        fd, temporary = tempfile.mkstemp(prefix="." + output.name + "-", dir=output.parent)
        try:
            with os.fdopen(fd, "wb") as disk:
                disk.truncate(size_mb * 1024 * 1024)
            run("mkfs.ext4", "-q", "-F", "-d", str(root), temporary)
            with open(temporary, "rb") as disk:
                os.fsync(disk.fileno())
            # Atomic NOREPLACE publication, including a concurrent creator.
            os.link(temporary, output)
            parent_fd = os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(parent_fd)
            finally:
                os.close(parent_fd)
        finally:
            os.unlink(temporary)
    print("workload: wrote %s (%d MiB, %s)" % (output, size_mb, image["Id"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--entrypoint")
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--workdir")
    parser.add_argument("--user")
    parser.add_argument("--size-mb", type=int)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    options = parser.parse_args()
    try:
        build(options)
    except (OSError, ValueError, tarfile.TarError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            sys.stderr.write(error.stderr.decode(errors="replace"))
        print("workload: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
