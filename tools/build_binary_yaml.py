#!/usr/bin/python3
# -*- coding: utf-8 -*-
"""Wrapper around build_binary_file.py to support YAML configuration files."""

import argparse
import os
import sys
from pathlib import Path

# Add vendored PyYAML implementation to import path before importing build_binary_file
_THIS_DIR = Path(__file__).resolve().parent
_THIRD_PARTY = _THIS_DIR / "thirdparty" / "pyyaml"
if _THIRD_PARTY.exists():
    sys.path.insert(0, str(_THIRD_PARTY))

try:
    import yaml  # type: ignore
except ModuleNotFoundError as exc:  # pragma: no cover - should not happen after vendoring
    raise SystemExit(f"Failed to import yaml module: {exc}") from exc

import build_binary_file as bbf  # noqa: E402

# Inject yaml module into build_binary_file namespace so extract_yaml_config can use it
bbf.yaml = yaml  # type: ignore[attr-defined]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Parse YAML config file, collect related files, and build image file.",
    )
    parser.add_argument('-c', dest='yaml_file', required=True, help='configuration yaml file')
    parser.add_argument('-o', dest='output_file', default='img.bin', help='output file')

    args = parser.parse_args(argv)

    yaml_file = os.path.abspath(args.yaml_file)
    output_file = os.path.abspath(args.output_file)

    log = bbf.common_decorator.Logger()
    image = bbf.ImageBinary(log)
    config_info_dict = image.extract_yaml_config(yaml_file)
    if config_info_dict and image.verify_config(config_info_dict):
        image.build_iamge(config_info_dict, output_file)


if __name__ == '__main__':
    main()
