#!/usr/bin/env python3
"""Prepare page-preserving, resumable PDF text/chunks for Q-COVERT."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import unicodedata
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

VERSION = "sp2026-poppler-raw-v1"
DOI_RE = re.compile(r"10\.\d{4,9}/[^\s;,]+", re.I)


def now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def atomic_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def atomic_json(path: Path, value: object) -> None:
    atomic_bytes(path, (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))


def jsonl_bytes(rows: list[dict]) -> bytes:
    return b"".join((json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8") for row in rows)


def run(command: list[str], timeout: int) -> str:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout, check=False)
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()[-500:]
        raise RuntimeError(f"{' '.join(command[:2])} failed ({result.returncode}): {detail}")
    return result.stdout.decode("utf-8", "replace")


def info_fields(pdf: Path) -> dict[str, str]:
    fields = {}
    for line in run(["pdfinfo", str(pdf)], 90).splitlines():
        key, marker, value = line.partition(":")
        if marker:
            fields[key.strip()] = value.strip()
    return fields


def clean_page(raw: str) -> str:
    text = unicodedata.normalize("NFKC", raw).replace("\u00ad", "")
    text = re.sub(r"[\x00-\x08\x0b-\x1f\x7f]", "", text)
    lines = [re.sub(r"[ \t]+", " ", line).strip() for line in text.splitlines()]
    paragraphs: list[str] = []
    current = ""
    for line in lines + [""]:
        if not line:
            if current:
                paragraphs.append(current.strip())
                current = ""
            continue
        if not current:
            current = line
        elif current.endswith("-") and line[:1].islower():
            current = current[:-1] + line
        else:
            current += " " + line
    return "\n\n".join(paragraphs)


def segment_page(text: str, target: int, overlap: int):
    """Split within a page; never invent a page span or drop text."""
    position = 0
    while position < len(text):
        remaining = len(text) - position
        if remaining <= target:
            yield text[position:].strip()
            break
        candidate = position + target
        search_start = position + max(target // 2, 1)
        endings = [text.rfind(token, search_start, candidate) for token in ("\n\n", ". ", "? ", "! ", "; ", " ")]
        ending = max(endings)
        if ending > search_start:
            candidate = ending + (2 if text[ending : ending + 2] in ("\n\n", ". ", "? ", "! ", "; ") else 1)
        part = text[position:candidate].strip()
        if part:
            yield part
        position = max(position + 1, candidate - overlap)
        while position < len(text) and text[position].isspace():
            position += 1


def title_from_filename(pdf: Path) -> str:
    return re.sub(r"^\d{1,4}\s*[-–]\s*", "", pdf.stem).strip()


def prepare_one(pdf: Path, source_dir: Path, output: Path, target: int, overlap: int, year: int) -> dict:
    source_pdf = pdf.relative_to(output).as_posix() if pdf.is_relative_to(output) else os.path.relpath(pdf, output).replace(os.sep, "/")
    source_hash = file_digest(pdf)
    paper_id = f"sha256:{source_hash}"
    info = info_fields(pdf)
    expected_pages = int(info.get("Pages", "0"))
    if expected_pages < 1:
        raise RuntimeError("pdfinfo returned no pages")
    raw = run(["pdftotext", "-raw", "-enc", "UTF-8", str(pdf), "-"], 180)
    raw_pages = raw.split("\f")
    if raw_pages and not raw_pages[-1].strip():
        raw_pages.pop()
    if len(raw_pages) < expected_pages:
        raw_pages.extend([""] * (expected_pages - len(raw_pages)))
    if len(raw_pages) != expected_pages:
        # Some PDFs contain literal form-feed glyphs inside a page's text.
        # Ask Poppler for each physical page so those glyphs cannot shift citations.
        raw_pages = [run(["pdftotext", "-f", str(page), "-l", str(page), "-raw", "-enc", "UTF-8", str(pdf), "-"], 90).replace("\f", "")
                     for page in range(1, expected_pages + 1)]
    subject = info.get("Subject", "")
    doi_match = DOI_RE.search(subject)
    doi = doi_match.group(0).rstrip(".)") if doi_match else None
    title = info.get("Title", "").strip()
    if not title or title.lower() in ("untitled", "unknown"):
        title = title_from_filename(pdf)
    author = info.get("Author", "").strip()
    authors = [piece.strip() for piece in re.split(r"\s*;\s*", author) if piece.strip()] if author else []
    pages: list[dict] = []
    chunks: list[dict] = []
    warnings: list[dict] = []
    for page_number, raw_page in enumerate(raw_pages, 1):
        cleaned = clean_page(raw_page)
        flags = []
        if len(cleaned) < 80:
            flags.append("low_text_ocr_review")
            warnings.append({"source_pdf": source_pdf, "page_number": page_number, "stage": "extraction", "reason": "less than 80 characters; OCR may be needed"})
        elif cleaned.count("\ufffd") > 3:
            flags.append("replacement_characters_review")
            warnings.append({"source_pdf": source_pdf, "page_number": page_number, "stage": "extraction", "reason": "replacement characters in extracted text"})
        pages.append({"paper_id": paper_id, "page_number": page_number, "text": cleaned, "extraction_method": "pdf_text", "text_sha256": digest(cleaned.encode()), "quality_flags": flags})
        if not cleaned:
            continue
        for piece in segment_page(cleaned, target, overlap):
            if len(piece) < 30:
                continue
            ordinal = len(chunks)
            chunk_id = "p-" + digest(f"{paper_id}\n{VERSION}\n{ordinal}".encode())[:32]
            embedding_text = f"{title}\n{piece}"
            if len(embedding_text) + len("passage: ") > 1000:
                raise RuntimeError(f"embedding input exceeds 1000 characters on page {page_number}")
            metadata = {"text": piece, "title": title, "url": f"https://doi.org/{doi}" if doi else None,
                        "docAuthor": "; ".join(authors) if authors else None, "published": str(year),
                        "docSource": "SP2026 paper PDF", "paper_id": paper_id,
                        "page_start": page_number, "page_end": page_number, "section": None,
                        "source_pdf": source_pdf}
            chunks.append({"id": chunk_id, "paper_id": paper_id, "chunk_ordinal": ordinal,
                           "page_start": page_number, "page_end": page_number, "section": None,
                           "text": piece, "embedding_text": embedding_text, "metadata": metadata})
    if not chunks:
        raise RuntimeError("no usable text chunks; OCR required or PDF is empty")
    document = {"paper_id": paper_id, "source_pdf": source_pdf, "source_sha256": source_hash,
                "title": title, "authors": authors, "doi": doi, "year": year,
                "page_count": expected_pages, "extraction_status": "review" if warnings else "complete"}
    return {"document": document, "pages": pages, "chunks": chunks, "warnings": warnings}


def progress_markdown(progress: dict) -> str:
    counts = Counter(item["status"] for item in progress["items"].values())
    lines = ["# SP2026 PDF 处理进度", "", f"更新时间：{progress['updated_at']}", "",
             f"- PDF 总数：{progress['total']}", f"- 提取完成：{counts['done']}",
             f"- 失败：{counts['failed']}", f"- 待处理：{progress['total'] - counts['done'] - counts['failed']}",
             f"- 已提取片段：{sum(x.get('chunks', 0) for x in progress['items'].values() if x['status'] == 'done')}", "",
             "## 阶段", "", "- PDF 逐篇提取：" + ("完成" if counts["done"] + counts["failed"] == progress["total"] else "运行中/可续跑"),
             "- 向量生成：见 `embedding-progress.json`（启动后更新）", "- 数据库导入：尚未开始", "",
             "## 失败文件", ""]
    failures = [(name, item.get("error", "")) for name, item in progress["items"].items() if item["status"] == "failed"]
    lines += [f"- `{name}`：{reason}" for name, reason in failures] or ["无"]
    lines += ["", "续跑命令：`python3 scripts/prepare_pdf_corpus.py`", ""]
    return "\n".join(lines)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=root / "pdf-corpus-v1/source")
    parser.add_argument("--output", type=Path, default=root / "pdf-corpus-v1")
    parser.add_argument("--target-chars", type=int, default=700)
    parser.add_argument("--overlap-chars", type=int, default=100)
    parser.add_argument("--year", type=int, default=2026)
    parser.add_argument("--limit", type=int, default=0, help="Process only first N PDFs, for smoke tests")
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if not source.is_dir():
        parser.error(f"source directory not found: {source}")
    if args.target_chars < 100 or args.target_chars > 800 or args.overlap_chars < 0 or args.overlap_chars >= args.target_chars // 2:
        parser.error("target must be 100..800 and overlap must be less than half the target")
    pdfs = sorted((path for path in source.rglob("*") if path.is_file() and path.suffix.lower() == ".pdf"), key=lambda p: p.relative_to(source).as_posix())
    if args.limit:
        pdfs = pdfs[:args.limit]
    config = {"processing_version": VERSION, "source": str(source), "target_chars": args.target_chars,
              "overlap_chars": args.overlap_chars, "year": args.year}
    progress_path = output / "progress.json"
    if progress_path.exists():
        progress = json.loads(progress_path.read_text(encoding="utf-8"))
        if progress.get("config") != config:
            parser.error("progress config differs; use another --output directory for changed settings")
    else:
        progress = {"config": config, "total": 0, "items": {}, "updated_at": now()}
    progress["total"] = len(pdfs)
    cache_dir = output / "work/extracted"
    cache_dir.mkdir(parents=True, exist_ok=True)
    for index, pdf in enumerate(pdfs, 1):
        relative = pdf.relative_to(source).as_posix()
        stat = pdf.stat()
        key = digest(relative.encode())
        cache_path = cache_dir / f"{key}.json"
        prior = progress["items"].get(relative, {})
        if prior.get("status") == "done" and prior.get("size") == stat.st_size and prior.get("mtime_ns") == stat.st_mtime_ns and cache_path.exists():
            continue
        try:
            result = prepare_one(pdf, source, output, args.target_chars, args.overlap_chars, args.year)
            atomic_json(cache_path, result)
            progress["items"][relative] = {"status": "done", "size": stat.st_size, "mtime_ns": stat.st_mtime_ns,
                                           "pages": len(result["pages"]), "chunks": len(result["chunks"]),
                                           "warnings": len(result["warnings"]), "completed_at": now()}
            print(f"[{index}/{len(pdfs)}] OK {relative}: {len(result['pages'])} pages, {len(result['chunks'])} chunks, {len(result['warnings'])} warnings", flush=True)
        except (OSError, RuntimeError, subprocess.TimeoutExpired, ValueError) as error:
            progress["items"][relative] = {"status": "failed", "size": stat.st_size, "mtime_ns": stat.st_mtime_ns,
                                           "error": str(error)[:600], "completed_at": now()}
            print(f"[{index}/{len(pdfs)}] FAILED {relative}: {error}", file=sys.stderr, flush=True)
        progress["updated_at"] = now()
        atomic_json(progress_path, progress)
        atomic_bytes(output / "处理进度.md", progress_markdown(progress).encode())
    documents, pages, chunks, rejected = [], [], [], []
    seen_hashes = set()
    for pdf in pdfs:
        relative = pdf.relative_to(source).as_posix()
        item = progress["items"].get(relative, {})
        if item.get("status") != "done":
            rejected.append({"source_pdf": str(pdf.relative_to(output)) if pdf.is_relative_to(output) else relative,
                             "stage": "extraction", "reason": item.get("error", "not processed")})
            continue
        result = json.loads((cache_dir / f"{digest(relative.encode())}.json").read_text(encoding="utf-8"))
        source_hash = result["document"]["source_sha256"]
        if source_hash in seen_hashes:
            rejected.append({"source_pdf": result["document"]["source_pdf"], "stage": "deduplication", "reason": "duplicate PDF SHA-256"})
            continue
        seen_hashes.add(source_hash)
        documents.append(result["document"])
        pages.extend(result["pages"])
        chunks.extend(result["chunks"])
        rejected.extend(result["warnings"])
    for filename, rows in (("documents.jsonl", documents), ("pages.jsonl", pages), ("chunks.jsonl", chunks)):
        atomic_bytes(output / filename, jsonl_bytes(rows))
    if rejected:
        atomic_bytes(output / "rejected.jsonl", jsonl_bytes(rejected))
    elif (output / "rejected.jsonl").exists():
        (output / "rejected.jsonl").unlink()
    summary = {"format_version": 1, "status": "text_complete" if len(documents) == len(pdfs) and not rejected else "needs_review",
               "dataset_name": "SP2026-paper-pdf-corpus", "processing_version": VERSION,
               "source_pdf_count": len(pdfs), "pdf_count": len(documents), "page_count": len(pages), "chunk_count": len(chunks),
               "rejected_count": len(rejected), "chunking": {"target_chars": args.target_chars, "overlap_chars": args.overlap_chars},
               "files": {name: file_digest(output / name) for name in ("documents.jsonl", "pages.jsonl", "chunks.jsonl")},
               "updated_at": now()}
    atomic_json(output / "text-manifest.json", summary)
    embedding_progress_path = output / "embedding-progress.json"
    if embedding_progress_path.exists():
        embedding_progress = json.loads(embedding_progress_path.read_text(encoding="utf-8"))
        if (embedding_progress.get("expected_rows") != len(chunks) or
                (embedding_progress.get("chunks_sha256") is not None and
                 embedding_progress["chunks_sha256"] != summary["files"]["chunks.jsonl"])):
            embedding_progress["status"] = "stale_input"
            embedding_progress["current_text_rows"] = len(chunks)
            embedding_progress["updated_at"] = now()
            atomic_json(embedding_progress_path, embedding_progress)
            atomic_bytes(output / "向量进度.md", (
                "# SP2026 PDF 向量生成进度\n\n"
                f"旧检查点：{embedding_progress.get('completed_rows', 0)}/{embedding_progress.get('expected_rows')} 片段。\n\n"
                f"当前文本：{len(chunks)} 片段。旧向量检查点与新文本不兼容，尚未恢复向量生成。\n\n"
                "需要续跑时执行 `node scripts/embed_pdf_corpus.mjs --restart-cache yes`；旧检查点将归档到 `work/embedding-backups/`。\n"
            ).encode())
    print(f"Text stage: {len(documents)}/{len(pdfs)} PDFs, {len(pages)} pages, {len(chunks)} chunks, {len(rejected)} review items", flush=True)
    return 0 if chunks else 1


if __name__ == "__main__":
    raise SystemExit(main())
