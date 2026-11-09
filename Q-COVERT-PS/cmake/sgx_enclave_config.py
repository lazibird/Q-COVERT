#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import sys
from dataclasses import dataclass
from typing import Any, Mapping, Optional

try:
    import tomllib
except ModuleNotFoundError:
    try:
        import tomli as tomllib
    except ModuleNotFoundError:
        tomllib = None


class ConfigError(Exception):
    """Raised when the TOML configuration is invalid."""


def load_toml(path: pathlib.Path) -> Mapping[str, Any]:
    if tomllib is None:
        raise ConfigError(
            "TOML parser unavailable: use Python >= 3.11 or install tomli with "
            f"'{sys.executable} -m pip install tomli' and select that interpreter "
            "with -DPython3_EXECUTABLE=/path/to/python"
        )
    try:
        with path.open("rb") as fh:
            return tomllib.load(fh)
    except FileNotFoundError as exc:
        raise ConfigError(f"configuration TOML file not found: {path}") from exc
    except tomllib.TOMLDecodeError as exc:
        raise ConfigError(f"TOML parsing error: {exc}") from exc


def parse_int(value: Any, field: str) -> int:
    if isinstance(value, bool):
        raise ConfigError(f"expected integer for '{field}', found boolean")
    if isinstance(value, int):
        if value < 0:
            raise ConfigError(f"'{field}' must be non-negative")
        return value
    if isinstance(value, str):
        text = value.strip().lower().replace("_", "")
        base = 10
        if text.startswith("0x"):
            base = 16
            text = text[2:]
        elif text.startswith("-"):
            raise ConfigError(f"'{field}' must be non-negative")
        try:
            parsed = int(text, base)
        except ValueError as exc:
            raise ConfigError(
                f"invalid integer literal for '{field}': {value!r}"
            ) from exc
        if parsed < 0:
            raise ConfigError(f"'{field}' must be non-negative")
        return parsed
    raise ConfigError(f"invalid type for '{field}': {type(value).__name__}")


SIZE_SUFFIXES = {
    "": 1,
    "b": 1,
    "byte": 1,
    "bytes": 1,
    "k": 1024,
    "kb": 1024,
    "kib": 1024,
    "m": 1024**2,
    "mb": 1024**2,
    "mib": 1024**2,
    "g": 1024**3,
    "gb": 1024**3,
    "gib": 1024**3,
    "t": 1024**4,
    "tb": 1024**4,
    "tib": 1024**4,
}


def parse_size(value: Any, field: str) -> int:
    if isinstance(value, int):
        if value <= 0:
            raise ConfigError(f"'{field}' must be positive")
        return value
    if isinstance(value, str):
        text = value.strip().lower().replace("_", "")
        if text.startswith("0x"):
            try:
                parsed = int(text, 16)
            except ValueError as exc:
                raise ConfigError(
                    f"invalid hexadecimal literal for '{field}': {value!r}"
                ) from exc
            if parsed <= 0:
                raise ConfigError(f"'{field}' must be positive")
            return parsed
        for suffix in (
            "kib",
            "kb",
            "k",
            "mib",
            "mb",
            "m",
            "gib",
            "gb",
            "g",
            "tib",
            "tb",
            "t",
            "bytes",
            "byte",
            "b",
            "",
        ):
            if text.endswith(suffix):
                number_part = text[: len(text) - len(suffix)] or "0"
                try:
                    number = int(number_part, 10)
                except ValueError as exc:
                    raise ConfigError(
                        f"invalid size literal for '{field}': {value!r}"
                    ) from exc
                multiplier = SIZE_SUFFIXES[suffix]
                total = number * multiplier
                if total <= 0:
                    raise ConfigError(f"'{field}' must be positive")
                return total
        raise ConfigError(f"invalid size literal for '{field}': {value!r}")
    raise ConfigError(f"invalid type for size field '{field}': {type(value).__name__}")


def format_hex(value: int) -> str:
    return f"0x{value:X}"


@dataclass
class EnclaveConfig:
    prod_id: int = 0
    isv_svn: int = 0
    stack_min_size: int = 0
    stack_max_size: int = 0
    heap_min_size: int = 0
    heap_init_size: int = 0
    heap_max_size: int = 0
    tcs_policy: int = 1
    num_tcs: int = 0
    disable_debug: int = 0
    misc_select: int = 0
    misc_mask: int = 0xFFFFFFFF
    enable_kss: int = 0


def parse_boolean(value: Any, field: str) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        lowered = value.strip().lower()
        if lowered in {"true", "yes", "on", "1"}:
            return True
        if lowered in {"false", "no", "off", "0"}:
            return False
    raise ConfigError(f"invalid boolean for '{field}': {value!r}")


def parse_config(data: Mapping[str, Any]) -> EnclaveConfig:
    config = EnclaveConfig()

    enclave = data.get("enclave", {})
    if not isinstance(enclave, Mapping):
        raise ConfigError("table [enclave] must be a TOML table if specified")
    if "prod_id" in enclave:
        config.prod_id = parse_int(enclave["prod_id"], "enclave.prod_id")
    if "isv_svn" in enclave:
        config.isv_svn = parse_int(enclave["isv_svn"], "enclave.isv_svn")
    if "misc_select" in enclave:
        config.misc_select = parse_int(enclave["misc_select"], "enclave.misc_select")
    if "misc_mask" in enclave:
        config.misc_mask = parse_int(enclave["misc_mask"], "enclave.misc_mask")
    if "enable_kss" in enclave:
        config.enable_kss = (
            1 if parse_boolean(enclave["enable_kss"], "enclave.enable_kss") else 0
        )
    debug_value = enclave.get("debug", True)
    config.disable_debug = 0 if parse_boolean(debug_value, "enclave.debug") else 1

    mem = data.get("mem")
    if not isinstance(mem, Mapping):
        raise ConfigError("missing required table [mem]")

    config.stack_min_size, config.stack_max_size = parse_size_pair(
        mem, base_key="stack", default_key="stack_size"
    )
    config.heap_min_size, config.heap_init_size, config.heap_max_size = (
        parse_heap_sizes(mem)
    )

    threads = data.get("threads")
    if not isinstance(threads, Mapping):
        raise ConfigError("missing required table [threads]")
    if "num_tcs" not in threads:
        raise ConfigError("missing required key 'threads.num_tcs'")
    config.num_tcs = parse_int(threads["num_tcs"], "threads.num_tcs")
    if config.num_tcs <= 0:
        raise ConfigError("'threads.num_tcs' must be positive")

    if "tcs_policy" in threads:
        config.tcs_policy = parse_int(threads["tcs_policy"], "threads.tcs_policy")
    if config.tcs_policy not in (0, 1):
        raise ConfigError("'threads.tcs_policy' must be 0 or 1")

    return config


def parse_size_pair(
    table: Mapping[str, Any], *, base_key: str, default_key: str
) -> tuple[int, int]:
    min_key = f"{base_key}_min_size"
    max_key = f"{base_key}_max_size"

    min_value: Optional[int] = None
    max_value: Optional[int] = None

    if min_key in table:
        min_value = parse_size(table[min_key], f"mem.{min_key}")
    if max_key in table:
        max_value = parse_size(table[max_key], f"mem.{max_key}")

    if default_key in table:
        default_value = parse_size(table[default_key], f"mem.{default_key}")
        if min_value is not None and min_value != default_value:
            raise ConfigError(
                f"mem.{default_key} must match mem.{min_key} when both are specified"
            )
        if max_value is not None and max_value != default_value:
            raise ConfigError(
                f"mem.{default_key} must match mem.{max_key} when both are specified"
            )
        min_value = max_value = default_value

    if min_value is None or max_value is None:
        raise ConfigError(
            f"either provide mem.{default_key} or both mem.{min_key} and mem.{max_key}"
        )

    if min_value > max_value:
        raise ConfigError(f"mem.{min_key} must be <= mem.{max_key}")

    return min_value, max_value


def parse_heap_sizes(table: Mapping[str, Any]) -> tuple[int, int, int]:
    min_size, max_size = parse_size_pair(table, base_key="heap", default_key="heap_size")
    init_size = min_size
    if "heap_init_size" in table:
        init_size = parse_size(table["heap_init_size"], "mem.heap_init_size")
    if init_size < min_size:
        raise ConfigError("mem.heap_init_size must be >= mem.heap_min_size")
    if init_size > max_size:
        raise ConfigError("mem.heap_init_size must be <= mem.heap_max_size")
    return min_size, init_size, max_size


def to_element_tree(config: EnclaveConfig):
    import xml.etree.ElementTree as ET

    root = ET.Element("EnclaveConfiguration")

    ET.SubElement(root, "ProdID").text = str(config.prod_id)
    ET.SubElement(root, "ISVSVN").text = str(config.isv_svn)
    ET.SubElement(root, "StackMaxSize").text = format_hex(config.stack_max_size)
    ET.SubElement(root, "HeapMaxSize").text = format_hex(config.heap_max_size)
    # Min/init fields opt into SGX2 EDMM metadata.  Omit them for fixed-size
    # enclaves so the same signed image works with the SGX1 simulation runtime.
    if config.stack_min_size != config.stack_max_size:
        ET.SubElement(root, "StackMinSize").text = format_hex(config.stack_min_size)
    if config.heap_min_size != config.heap_max_size or config.heap_init_size != config.heap_max_size:
        ET.SubElement(root, "HeapMinSize").text = format_hex(config.heap_min_size)
        ET.SubElement(root, "HeapInitSize").text = format_hex(config.heap_init_size)
    ET.SubElement(root, "TCSPolicy").text = str(config.tcs_policy)
    ET.SubElement(root, "TCSNum").text = str(config.num_tcs)
    ET.SubElement(root, "DisableDebug").text = str(config.disable_debug)
    ET.SubElement(root, "MiscSelect").text = format_hex(config.misc_select)
    ET.SubElement(root, "MiscMask").text = format_hex(config.misc_mask)
    if config.enable_kss:
        ET.SubElement(root, "EnableKSS").text = str(config.enable_kss)

    return root


def serialize_xml(root) -> bytes:
    import xml.etree.ElementTree as ET

    indent_element(root)
    tree = ET.ElementTree(root)
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def indent_element(elem, level: int = 0) -> None:
    indent = "    "
    children = list(elem)
    if not children:
        return
    elem.text = "\n" + indent * (level + 1)
    for child in children:
        indent_element(child, level + 1)
    children[-1].tail = "\n" + indent * level
    for child in children[:-1]:
        child.tail = "\n" + indent * (level + 1)
    if level == 0:
        elem.tail = "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Generate SGX enclave config XML from TOML"
    )
    parser.add_argument(
        "--input",
        required=True,
        type=pathlib.Path,
        help="Input TOML configuration path",
    )
    parser.add_argument(
        "--output", required=True, type=pathlib.Path, help="Output XML path"
    )
    args = parser.parse_args(argv)

    try:
        data = load_toml(args.input)
        config = parse_config(data)
        root = to_element_tree(config)
        xml_bytes = serialize_xml(root)
    except ConfigError as exc:
        print(f"sgx enclave config error: {exc}", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(xml_bytes)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
