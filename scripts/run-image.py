#!/usr/bin/env python3
"""Compatibility entry point for native Rust image launch."""
import os
import sys

if len(sys.argv) < 2:
    sys.exit("usage: run-image.py SVMM [run options]")
binary = sys.argv[1]
os.execv(binary, [binary, "run", *sys.argv[2:]])
