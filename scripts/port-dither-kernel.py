#!/usr/bin/env python3
"""Compatibility entry point for the shared Linux kernel translator."""
import runpy
from pathlib import Path
runpy.run_path(str(Path(__file__).with_name("port-kernel.py")), run_name="__main__")
