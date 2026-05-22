#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from lua_sync_common import ComponentSource, FileSyncPlan, LuaSyncConsole, LuaSyncError
from lua_sync_common import collect_build_component_sources, write_depfile, write_stamp


console = LuaSyncConsole()

SYNC_SUBDIRS = ('lib', 'test')


def add_lua_script(plan: FileSyncPlan, subdir: str, category_dir: Path, script_path: Path, owner: str) -> None:
    relative_path = script_path.relative_to(category_dir)
    output_name = str(Path(subdir) / relative_path)
    plan.add(output_name, script_path, owner)

    if subdir == 'lib':
        doc_path = script_path.with_suffix('.md')
        if not doc_path.is_file():
            raise LuaSyncError(f"Lua library '{script_path}' must have same-name markdown doc '{doc_path.name}'")
        doc_output_name = str(Path(subdir) / relative_path.with_suffix('.md'))
        plan.add(doc_output_name, doc_path, owner)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description='Sync builtin Lua module libraries and tests into the FATFS image.')
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--builtin-output-dir', required=True)
    parser.add_argument('--manifest-path', required=True)
    parser.add_argument('--stamp-path', required=True)
    parser.add_argument('--depfile', required=True)
    return parser.parse_args()


def collect_builtin_lua_module_resources(sources: list[ComponentSource], output_dir: Path, manifest_path: Path) -> FileSyncPlan:
    plan = FileSyncPlan(output_dir, manifest_path)

    for source in sources:
        # Only sync script categories defined by lua-spec.md; root-level scripts are not managed outputs.
        for subdir in SYNC_SUBDIRS:
            category_dir = source.root / subdir
            if not category_dir.is_dir():
                continue

            for script_path in sorted(category_dir.rglob('*.lua')):
                if not script_path.is_file():
                    continue
                add_lua_script(plan, subdir, category_dir, script_path, source.name)

    return plan


def main() -> int:
    args = parse_args()
    build_dir = Path(args.build_dir).resolve()
    output_dir = Path(args.builtin_output_dir).resolve()
    manifest_path = Path(args.manifest_path).resolve()
    stamp_path = Path(args.stamp_path).resolve()
    depfile_path = Path(args.depfile).resolve()

    sources = collect_build_component_sources(build_dir)
    plan = collect_builtin_lua_module_resources(sources, output_dir, manifest_path)
    plan.apply()
    write_depfile(depfile_path, stamp_path, list(plan.input_paths))
    write_stamp(stamp_path)
    console.success(f'CLAW lua module resource sync updated {plan.count} files into {output_dir}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except LuaSyncError as exc:
        console.error(f'sync_lua_module_resources.py: error: {exc}')
        sys.exit(1)
