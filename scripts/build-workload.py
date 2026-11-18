#!/usr/bin/env python3
"""Compatibility entry point for the native Rust OCI image builder."""
import os
from pathlib import Path
import sys

binary = Path(__file__).resolve().parent.parent / "bin/svmm"
os.execv(binary, [str(binary), "image-build", *sys.argv[1:]])
