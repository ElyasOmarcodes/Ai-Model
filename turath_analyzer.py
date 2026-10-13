#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Turath.io Islamic Library Database & Size Analyzer
Calculates total books, text version sizes, and PDF version sizes,
exports JSON/CSV/Markdown reports, and packages everything into a ZIP archive.
"""

import argparse
import asyncio
import csv
import json
import logging
import os
import re
import sqlite3
import sys
import time
import zipfile
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple
from urllib.parse import quote, unquote, urlparse

import aiohttp

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s | %(levelname)-7s | %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("TurathAnalyzer")

# Known Turath CDN & App endpoints
APP_URL = "https://app.turath.io"
CATALOG_CANDIDATES = [
    "https://files.turath.io/data-v3.json",
    "https://files.turath.io/data-v2.json",
    "https://files.turath.io/data.json",
]
BOOK_BASE_CANDIDATES = [
    "https://files.turath.io/books-v2",
    "https://files.turath.io/books-v3",
    "https://files.turath.io/books",
]

DEFAULT_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/128.0.0.0 Safari/537.36"
    ),
    "Accept": "application/json, text/plain, */*",
}


@dataclass
class BookRecord:
    book_id: int
    name: str
    author_id: Optional[int] = None
    cat_id: Optional[int] = None
    text_size_bytes: int = 0
    has_pdf: bool = False
    pdf_files_count: int = 0
    pdf_size_bytes: int = 0
    pdf_urls: List[str] = field(default_factory=list)


def format_bytes(size_bytes: int) -> str:
    """Convert raw bytes into human-readable string (KB, MB, GB, TB)."""
    if size_bytes <= 0:
        return "0 B"
    units = ["B", "KB", "MB", "GB", "TB"]
    size = float(size_bytes)
    unit_idx = 0
    while size >= 1024.0 and unit_idx < len(units) - 1:
        size /= 1024.0
        unit_idx += 1
    return f"{size:,.2f} {units[unit_idx]}"


def extract_meta_from_prefix(raw_text: str) -> Optional[Dict[str, Any]]:
    """
    Safely extract the 'meta' JSON object from the first few kilobytes
    of a Turath book JSON file without downloading the entire book.
    """
    match = re.search(r'"meta"\s*:\s*\{', raw_text)
    if not match:
        return None
    brace_start = raw_text.find("{", match.start())
    if brace_start == -1:
        return None
    try:
        decoder = json.JSONDecoder()
        meta_obj, _ = decoder.raw_decode(raw_text, brace_start)
        if isinstance(meta_obj, dict):
            return meta_obj
    except json.JSONDecodeError:
        pass
    return None


def parse_pdf_links_field(pdf_data: Any, info_text: str = "") -> List[str]:
    """
    Normalize all variations of Turath `pdf_links` metadata into full direct URLs.
    """
    urls: List[str] = []

    def normalize_single_url(root: str, file_part: str) -> Optional[str]:
        file_part = file_part.strip()
        if not file_part or file_part.startswith("#"):
            return None
        if file_part.startswith(("http://", "https://")):
            return file_part

        root = root.strip()
        if not root:
            # Usually an Archive.org relative identifier/file
            return f"https://archive.org/download/{file_part.lstrip('/')}"
        if not root.startswith(("http://", "https://")):
            root = f"https://archive.org/download/{root.strip('/')}/"
        elif not root.endswith("/"):
            root += "/"
        return root + file_part.lstrip("/")

    if isinstance(pdf_data, dict):
        root = str(pdf_data.get("root", "") or "")
        files = pdf_data.get("files") or pdf_data.get("links") or []
        if isinstance(files, list):
            for f in files:
                if isinstance(f, str):
                    u = normalize_single_url(root, f)
                    if u:
                        urls.append(u)
                elif isinstance(f, dict) and "url" in f:
                    urls.append(str(f["url"]))
        elif isinstance(files, str):
            u = normalize_single_url(root, files)
            if u:
                urls.append(u)
        elif root and not files:
            if root.lower().endswith(".pdf") or "archive.org" in root:
                u = normalize_single_url("", root)
                if u:
                    urls.append(u)

    elif isinstance(pdf_data, list):
        for item in pdf_data:
            if isinstance(item, str):
                u = normalize_single_url("", item)
                if u:
                    urls.append(u)
            elif isinstance(item, dict):
                urls.extend(parse_pdf_links_field(item))

    elif isinstance(pdf_data, str) and pdf_data.strip():
        u = normalize_single_url("", pdf_data.strip())
        if u:
            urls.append(u)

    # Also check info text for direct PDF links if none were found in pdf_links
    if not urls and info_text:
        found = re.findall(r'https?://[^\s"\'<>]+\.pdf', info_text, flags=re.IGNORECASE)
        urls.extend(found)

    # Deduplicate while preserving order
    seen: Set[str] = set()
    clean_urls: List[str] = []
    for u in urls:
        # Ensure proper URL encoding for Arabic/non-ASCII filenames
        parsed = urlparse(u)
        safe_path = quote(unquote(parsed.path), safe="/:@&+$,;=-_.~")
        reconstructed = f"{parsed.scheme}://{parsed.netloc}{safe_path}"
        if parsed.query:
            reconstructed += f"?{parsed.query}"
        if reconstructed not in seen:
            seen.add(reconstructed)
            clean_urls.append(reconstructed)

    return clean_urls


async def discover_endpoints(session: aiohttp.ClientSession) -> Tuple[List[str], List[str]]:
    """
    Inspect app.turath.io bundles to dynamically discover any updated CDN URLs,
    falling back to known candidates.
    """
    catalog_urls = list(CATALOG_CANDIDATES)
    book_bases = list(BOOK_BASE_CANDIDATES)

    try:
        async with session.get(APP_URL, timeout=aiohttp.ClientTimeout(total=15)) as resp:
            if resp.status == 200:
                html = await resp.text()
                js_paths = re.findall(r'src=["\'](/[^"\']+\.js)["\']', html)
                for js_path in js_paths[:5]:
                    js_url = f"{APP_URL.rstrip('/')}{js_path}"
                    try:
                        async with session.get(js_url, timeout=aiohttp.ClientTimeout(total=15)) as js_resp:
                            if js_resp.status == 200:
                                js_text = await js_resp.text()
                                found_data = re.findall(
                                    r'https://files\.turath\.io/data[^"\'\s`]*\.json', js_text
                                )
                                for d_url in reversed(found_data):
                                    if d_url not in catalog_urls:
                                        catalog_urls.insert(0, d_url)
                                found_books = re.findall(
                                    r'https://files\.turath\.io/books(?:-[a-zA-Z0-9]+)?', js_text
                                )
                                for b_url in reversed(found_books):
                                    if b_url not in book_bases:
                                        book_bases.insert(0, b_url)
                    except Exception:
                        continue
    except Exception as exc:
        logger.debug("Bundle discovery skipped (%s); using standard CDN endpoints.", exc)

    return catalog_urls, book_bases


def load_local_database(db_path: Path) -> List[BookRecord]:
    """Load books from a local SQLite (.db/.sqlite) or JSON file if provided."""
    logger.info("Loading local database from: %s", db_path)
    records: List[BookRecord] = []

    if db_path.suffix.lower() == ".json":
        raw_data = json.loads(db_path.read_text(encoding="utf-8"))
        return normalize_catalog_json(raw_data)

    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()

    tables = [
        r[0] for r in cur.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall()
    ]
    book_table = next((t for t in tables if t.lower() in ("books", "book", "main", "0bok")), None)
    if not book_table:
        raise ValueError(f"Could not find a books table in {db_path}. Found tables: {tables}")

    rows = cur.execute(f"SELECT * FROM [{book_table}]").fetchall()
    for idx, row in enumerate(rows, start=1):
        row_dict = dict(row)
        b_id = int(row_dict.get("id") or row_dict.get("bkid") or row_dict.get("book_id") or idx)
        name = str(row_dict.get("name") or row_dict.get("bk") or row_dict.get("title") or f"Book {b_id}")
        size_val = int(row_dict.get("size") or row_dict.get("text_size") or 0)
        pdf_raw = row_dict.get("pdf_links") or row_dict.get("pdf") or ""
        if isinstance(pdf_raw, str) and pdf_raw.strip().startswith(("{", "[")):
            try:
                pdf_raw = json.loads(pdf_raw)
            except Exception:
                pass
        pdf_urls = parse_pdf_links_field(pdf_raw, str(row_dict.get("info") or ""))
        records.append(
            BookRecord(
                book_id=b_id,
                name=name,
                author_id=row_dict.get("author_id") or row_dict.get("authno"),
                cat_id=row_dict.get("cat_id") or row_dict.get("cat"),
                text_size_bytes=size_val,
                has_pdf=bool(pdf_urls),
                pdf_files_count=len(pdf_urls),
                pdf_urls=pdf_urls,
            )
        )
    conn.close()
    return records


def normalize_catalog_json(data: Any) -> List[BookRecord]:
    """Normalize Turath master JSON data into a list of BookRecord objects."""
    books_raw = data.get("books", data) if isinstance(data, dict) else data
    records: List[BookRecord] = []

    if isinstance(books_raw, dict):
        items = []
        for k, v in books_raw.items():
            if isinstance(v, dict):
                v.setdefault("id", k)
                items.append(v)
        books_raw = items

    if not isinstance(books_raw, list):
        raise ValueError("Unexpected catalog JSON format for 'books'.")

    for idx, item in enumerate(books_raw, start=1):
        if not isinstance(item, dict):
            continue
        b_id = int(item.get("id") or item.get("book_id") or idx)
        name = str(item.get("name") or item.get("title") or f"Book {b_id}")
        author_id = item.get("author_id") or item.get("author")
        cat_id = item.get("cat_id") or item.get("cat")
        size_val = int(item.get("size") or item.get("file_size") or 0)

        pdf_field = item.get("pdf_links") or item.get("pdf") or item.get("meta", {}).get("pdf_links")
        info_str = str(item.get("info") or item.get("meta", {}).get("info") or "")
        pdf_urls = parse_pdf_links_field(pdf_field, info_str)

        records.append(
            BookRecord(
                book_id=b_id,
                name=name,
                author_id=int(author_id) if isinstance(author_id, (int, str)) and str(author_id).isdigit() else None,
                cat_id=int(cat_id) if isinstance(cat_id, (int, str)) and str(cat_id).isdigit() else None,
                text_size_bytes=size_val,
                has_pdf=bool(pdf_urls),
                pdf_files_count=len(pdf_urls),
                pdf_urls=pdf_urls,
            )
        )

    return records


async def fetch_online_catalog(session: aiohttp.ClientSession, catalog_urls: List[str]) -> List[BookRecord]:
    """Download and parse the master Turath catalog JSON."""
    for url in catalog_urls:
        try:
            logger.info("Fetching Turath master catalog from: %s", url)
            async with session.get(url, timeout=aiohttp.ClientTimeout(total=60)) as resp:
                if resp.status == 200:
                    data = await resp.json(content_type=None)
                    records = normalize_catalog_json(data)
                    if records:
                        logger.info("Successfully loaded %d books from catalog.", len(records))
                        return records
        except Exception as exc:
            logger.warning("Failed to load catalog from %s: %s", url, exc)

    raise RuntimeError("Could not fetch Turath catalog from any known endpoint.")


async def detect_working_book_base(
    session: aiohttp.ClientSession, book_bases: List[str], sample_book_id: int
) -> str:
    """Find which CDN path serves individual book JSON files."""
    for base in book_bases:
        test_url = f"{base.rstrip('/')}/{sample_book_id}.json"
        try:
            async with session.get(
                test_url,
                headers={"Range": "bytes=0-255", "Accept-Encoding": "identity"},
                timeout=aiohttp.ClientTimeout(total=10),
            ) as resp:
                if resp.status in (200, 206):
                    logger.info("Detected active book text endpoint: %s", base)
                    return base.rstrip("/")
        except Exception:
            continue
    return book_bases[0].rstrip("/")


async def fetch_single_book_text_info(
    session: aiohttp.ClientSession,
    semaphore: asyncio.Semaphore,
    book: BookRecord,
    book_base_url: str,
    need_meta_parse: bool,
) -> None:
    """
    Fetch the exact byte size of a book's text JSON file, and optionally parse
    its 'meta' header block if PDF links were not present in the master catalog.
    """
    url = f"{book_base_url}/{book.book_id}.json"
    range_end = 24575 if need_meta_parse else 0
    headers = {
        "Range": f"bytes=0-{range_end}",
        "Accept-Encoding": "identity",
    }

    async with semaphore:
        for attempt in range(3):
            try:
                async with session.get(
                    url, headers=headers, timeout=aiohttp.ClientTimeout(total=20)
                ) as resp:
                    if resp.status == 206:
                        content_range = resp.headers.get("Content-Range", "")
                        # Format: bytes 0-24575/1234567
                        if "/" in content_range:
                            total_str = content_range.split("/")[-1].strip()
                            if total_str.isdigit():
                                book.text_size_bytes = int(total_str)
                        if need_meta_parse:
                            raw_bytes = await resp.content.read(range_end + 1)
                            if book.text_size_bytes == 0:
                                book.text_size_bytes = len(raw_bytes)
                            meta = extract_meta_from_prefix(raw_bytes.decode("utf-8", errors="ignore"))
                            if meta:
                                pdf_urls = parse_pdf_links_field(
                                    meta.get("pdf_links") or meta.get("pdf"),
                                    str(meta.get("info") or ""),
                                )
                                if pdf_urls:
                                    book.pdf_urls = pdf_urls
                                    book.has_pdf = True
                                    book.pdf_files_count = len(pdf_urls)
                        return

                    elif resp.status == 200:
                        cl = resp.headers.get("Content-Length")
                        raw_bytes = await resp.content.read(range_end + 1)
                        if cl and cl.isdigit():
                            book.text_size_bytes = int(cl)
                        else:
                            # Read remaining stream length if Content-Length was omitted
                            rest = await resp.content.read()
                            book.text_size_bytes = len(raw_bytes) + len(rest)

                        if need_meta_parse:
                            meta = extract_meta_from_prefix(raw_bytes.decode("utf-8", errors="ignore"))
                            if meta:
                                pdf_urls = parse_pdf_links_field(
                                    meta.get("pdf_links") or meta.get("pdf"),
                                    str(meta.get("info") or ""),
                                )
                                if pdf_urls:
                                    book.pdf_urls = pdf_urls
                                    book.has_pdf = True
                                    book.pdf_files_count = len(pdf_urls)
                        return

                    elif resp.status == 404:
                        return
            except Exception:
                if attempt == 2:
                    return
                await asyncio.sleep(0.5 * (attempt + 1))


async def analyze_all_text_versions(
    session: aiohttp.ClientSession,
    books: List[BookRecord],
    book_base_url: str,
    concurrency: int,
) -> None:
    """Concurrently inspect text versions for all books."""
    catalog_has_pdfs = any(b.has_pdf for b in books)
    catalog_has_sizes = sum(1 for b in books if b.text_size_bytes > 0) > (len(books) * 0.5)

    need_meta_parse = not catalog_has_pdfs
    if catalog_has_sizes and catalog_has_pdfs:
        logger.info("Catalog already includes text sizes and PDF metadata.")
        return

    logger.info(
        "Scanning text versions & metadata for %d books (concurrency=%d)...",
        len(books),
        concurrency,
    )
    semaphore = asyncio.Semaphore(concurrency)
    total = len(books)
    completed = 0
    start_ts = time.time()

    async def worker(b: BookRecord) -> None:
        nonlocal completed
        await fetch_single_book_text_info(session, semaphore, b, book_base_url, need_meta_parse)
        completed += 1
        if completed % 500 == 0 or completed == total:
            elapsed = time.time() - start_ts
            rate = completed / elapsed if elapsed > 0 else 0
            logger.info("Text scan progress: %d / %d books (%.1f books/sec)", completed, total, rate)

    await asyncio.gather(*(worker(b) for b in books))


def extract_archive_org_item_and_file(url: str) -> Optional[Tuple[str, str]]:
    """
    If URL is an Archive.org download link, return (identifier, filename).
    Example: https://archive.org/download/waq12345/01.pdf -> ('waq12345', '01.pdf')
    """
    parsed = urlparse(url)
    if "archive.org" not in parsed.netloc.lower():
        return None
    parts = [p for p in unquote(parsed.path).split("/") if p]
    if len(parts) >= 3 and parts[0] in ("download", "details"):
        identifier = parts[1]
        subpath = "/".join(parts[2:])
        return identifier, subpath
    return None


async def fetch_archive_org_item_sizes(
    session: aiohttp.ClientSession,
    semaphore: asyncio.Semaphore,
    identifier: str,
) -> Dict[str, int]:
    """
    Query https://archive.org/metadata/{identifier} to get exact byte sizes
    of all files inside an Archive.org item in a single fast API call.
    """
    meta_url = f"https://archive.org/metadata/{quote(identifier)}"
    file_sizes: Dict[str, int] = {}

    async with semaphore:
        for attempt in range(3):
            try:
                async with session.get(
                    meta_url, timeout=aiohttp.ClientTimeout(total=20)
                ) as resp:
                    if resp.status == 200:
                        data = await resp.json(content_type=None)
                        files_list = data.get("files", []) if isinstance(data, dict) else []
                        for f in files_list:
                            if not isinstance(f, dict):
                                continue
                            name = str(f.get("name", "")).lstrip("/")
                            size_str = str(f.get("size", "0"))
                            if name and size_str.isdigit():
                                sz = int(size_str)
                                file_sizes[name] = sz
                                file_sizes[Path(name).name] = sz
                        return file_sizes
            except Exception:
                if attempt == 2:
                    break
                await asyncio.sleep(0.5 * (attempt + 1))

    return file_sizes


async def fetch_direct_url_size(
    session: aiohttp.ClientSession,
    semaphore: asyncio.Semaphore,
    url: str,
) -> int:
    """
    Get exact file size in bytes of a remote URL using HEAD and Range fallback.
    """
    async with semaphore:
        for attempt in range(2):
            try:
                # 1. Try HEAD request
                async with session.head(
                    url,
                    headers={"Accept-Encoding": "identity"},
                    allow_redirects=True,
                    timeout=aiohttp.ClientTimeout(total=20),
                ) as resp:
                    if resp.status == 200:
                        cl = resp.headers.get("Content-Length")
                        if cl and cl.isdigit() and int(cl) > 0:
                            return int(cl)

                # 2. Fallback to 1-byte Range GET request
                async with session.get(
                    url,
                    headers={"Range": "bytes=0-0", "Accept-Encoding": "identity"},
                    allow_redirects=True,
                    timeout=aiohttp.ClientTimeout(total=20),
                ) as resp:
                    if resp.status == 206:
                        cr = resp.headers.get("Content-Range", "")
                        if "/" in cr:
                            total_str = cr.split("/")[-1].strip()
                            if total_str.isdigit():
                                return int(total_str)
                    elif resp.status == 200:
                        cl = resp.headers.get("Content-Length")
                        if cl and cl.isdigit():
                            return int(cl)
            except Exception:
                if attempt == 1:
                    return 0
                await asyncio.sleep(0.5)
    return 0


async def analyze_all_pdf_versions(
    session: aiohttp.ClientSession,
    books: List[BookRecord],
    concurrency: int,
) -> None:
    """
    Calculate PDF sizes for all books that have PDF versions.
    Uses Archive.org Metadata batching + direct HEAD/Range fallback.
    """
    books_with_pdf = [b for b in books if b.has_pdf and b.pdf_urls]
    if not books_with_pdf:
        logger.info("No books with PDF links found.")
        return

    logger.info("Found %d books with PDF versions. Resolving PDF sizes...", len(books_with_pdf))

    # Group Archive.org items for ultra-fast metadata lookup
    archive_items: Set[str] = set()
    for b in books_with_pdf:
        for u in b.pdf_urls:
            parsed_ia = extract_archive_org_item_and_file(u)
            if parsed_ia:
                archive_items.add(parsed_ia[0])

    ia_cache: Dict[str, Dict[str, int]] = {}
    semaphore = asyncio.Semaphore(concurrency)

    if archive_items:
        logger.info("Batch-querying %d unique Archive.org items...", len(archive_items))
        ia_list = sorted(archive_items)
        completed_ia = 0

        async def ia_worker(ident: str) -> None:
            nonlocal completed_ia
            ia_cache[ident] = await fetch_archive_org_item_sizes(session, semaphore, ident)
            completed_ia += 1
            if completed_ia % 250 == 0 or completed_ia == len(ia_list):
                logger.info("Archive.org metadata progress: %d / %d", completed_ia, len(ia_list))

        await asyncio.gather(*(ia_worker(ident) for ident in ia_list))

    # Resolve sizes for each book's PDF URLs
    fallback_urls: Set[str] = set()
    for b in books_with_pdf:
        for u in b.pdf_urls:
            parsed_ia = extract_archive_org_item_and_file(u)
            if parsed_ia:
                ident, subpath = parsed_ia
                item_map = ia_cache.get(ident, {})
                sz = item_map.get(subpath) or item_map.get(Path(subpath).name) or 0
                if sz == 0 and item_map:
                    # If only 1 PDF exists in the item and subpath was generic
                    pdf_candidates = [v for k, v in item_map.items() if k.lower().endswith(".pdf")]
                    if len(pdf_candidates) == 1:
                        sz = pdf_candidates[0]
                if sz == 0:
                    fallback_urls.add(u)
            else:
                fallback_urls.add(u)

    direct_url_cache: Dict[str, int] = {}
    if fallback_urls:
        logger.info("Querying %d remaining PDF URLs via direct HEAD/Range...", len(fallback_urls))

        async def direct_worker(u: str) -> None:
            direct_url_cache[u] = await fetch_direct_url_size(session, semaphore, u)

        await asyncio.gather(*(direct_worker(u) for u in fallback_urls))

    # Sum up PDF sizes per book
    for b in books_with_pdf:
        total_pdf_bytes = 0
        for u in b.pdf_urls:
            parsed_ia = extract_archive_org_item_and_file(u)
            sz = 0
            if parsed_ia:
                ident, subpath = parsed_ia
                item_map = ia_cache.get(ident, {})
                sz = item_map.get(subpath) or item_map.get(Path(subpath).name) or 0
                if sz == 0 and item_map:
                    pdf_candidates = [v for k, v in item_map.items() if k.lower().endswith(".pdf")]
                    if len(pdf_candidates) == 1:
                        sz = pdf_candidates[0]
            if sz == 0:
                sz = direct_url_cache.get(u, 0)
            total_pdf_bytes += sz
        b.pdf_size_bytes = total_pdf_bytes


def generate_reports(books: List[BookRecord], output_dir: Path) -> Dict[str, Any]:
    """Generate JSON, CSV, Markdown reports, GitHub Step Summary, and ZIP bundle."""
    output_dir.mkdir(parents=True, exist_ok=True)

    total_books = len(books)
    books_with_text = sum(1 for b in books if b.text_size_bytes > 0)
    total_text_bytes = sum(b.text_size_bytes for b in books)

    books_with_pdf = sum(1 for b in books if b.has_pdf)
    total_pdf_files = sum(b.pdf_files_count for b in books if b.has_pdf)
    total_pdf_bytes = sum(b.pdf_size_bytes for b in books)

    combined_bytes = total_text_bytes + total_pdf_bytes
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    summary_data = {
        "generated_at": timestamp,
        "total_books_count": total_books,
        "text_versions": {
            "books_with_text_count": books_with_text,
            "total_size_bytes": total_text_bytes,
            "total_size_human": format_bytes(total_text_bytes),
            "average_book_text_size_bytes": int(total_text_bytes / total_books) if total_books else 0,
            "average_book_text_size_human": format_bytes(
                int(total_text_bytes / total_books) if total_books else 0
            ),
        },
        "pdf_versions": {
            "books_with_pdf_count": books_with_pdf,
            "books_without_pdf_count": total_books - books_with_pdf,
            "total_pdf_volumes_or_files": total_pdf_files,
            "total_size_bytes": total_pdf_bytes,
            "total_size_human": format_bytes(total_pdf_bytes),
            "average_pdf_book_size_bytes": int(total_pdf_bytes / books_with_pdf) if books_with_pdf else 0,
            "average_pdf_book_size_human": format_bytes(
                int(total_pdf_bytes / books_with_pdf) if books_with_pdf else 0
            ),
        },
        "combined_total": {
            "total_size_bytes": combined_bytes,
            "total_size_human": format_bytes(combined_bytes),
        },
    }

    # 1. Save summary.json
    summary_json_path = output_dir / "summary.json"
    summary_json_path.write_text(
        json.dumps(summary_data, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    # 2. Save books_details.json
    details_json_path = output_dir / "books_details.json"
    books_sorted = sorted(books, key=lambda x: x.book_id)
    details_payload = [
        {
            "book_id": b.book_id,
            "name": b.name,
            "author_id": b.author_id,
            "cat_id": b.cat_id,
            "text_size_bytes": b.text_size_bytes,
            "text_size_human": format_bytes(b.text_size_bytes),
            "has_pdf": b.has_pdf,
            "pdf_files_count": b.pdf_files_count,
            "pdf_size_bytes": b.pdf_size_bytes,
            "pdf_size_human": format_bytes(b.pdf_size_bytes),
            "pdf_urls": b.pdf_urls,
        }
        for b in books_sorted
    ]
    details_json_path.write_text(
        json.dumps(details_payload, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    # 3. Save books_details.csv
    csv_path = output_dir / "books_details.csv"
    with csv_path.open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "book_id",
            "name",
            "author_id",
            "cat_id",
            "text_size_bytes",
            "text_size_human",
            "has_pdf",
            "pdf_files_count",
            "pdf_size_bytes",
            "pdf_size_human",
            "pdf_urls",
        ])
        for b in books_sorted:
            writer.writerow([
                b.book_id,
                b.name,
                b.author_id or "",
                b.cat_id or "",
                b.text_size_bytes,
                format_bytes(b.text_size_bytes),
                b.has_pdf,
                b.pdf_files_count,
                b.pdf_size_bytes,
                format_bytes(b.pdf_size_bytes),
                " | ".join(b.pdf_urls),
            ])

    # 4. Generate Markdown Report (REPORT.md)
    top_text_books = sorted(books, key=lambda x: x.text_size_bytes, reverse=True)[:15]
    top_pdf_books = sorted(books, key=lambda x: x.pdf_size_bytes, reverse=True)[:15]

    md_lines = [
        "# 📚 د تراث اسلامي مکتبې (Turath.io) د ډیټابیس بشپړ راپور",
        f"**د راپور نېټه:** `{timestamp}`\n",
        "## 📊 عمومي احصائیه (Overall Summary)",
        "| ځانګړنه (Metric) | ارزښت (Value) | سایز په بایټ (Exact Bytes) |",
        "|---|---|---|",
        f"| **د ټولو کتابونو شمېر** | **{total_books:,} کتابونه** | — |",
        f"| **د متني نسخو (Text) مجموعي سایز** | **{format_bytes(total_text_bytes)}** | `{total_text_bytes:,}` |",
        f"| **د هر کتاب منځنی متني سایز** | **{summary_data['text_versions']['average_book_text_size_human']}** | `{summary_data['text_versions']['average_book_text_size_bytes']:,}` |",
        f"| **هغه کتابونه چې PDF نسخه لري** | **{books_with_pdf:,} کتابونه** | — |",
        f"| **د ټولو PDF فایلونو/جلدونو شمېر** | **{total_pdf_files:,} فایلونه** | — |",
        f"| **د PDF نسخو مجموعي سایز** | **{format_bytes(total_pdf_bytes)}** | `{total_pdf_bytes:,}` |",
        f"| **مجموعي سایز (Text + PDF)** | **{format_bytes(combined_bytes)}** | `{combined_bytes:,}` |",
        "",
        "## 📖 تر ټولو لوی ۱۵ کتابونه (د متني نسخې له مخې)",
        "| ID | د کتاب نوم | متني سایز | PDF لري؟ | د PDF سایز |",
        "|---|---|---|---|---|",
    ]
    for b in top_text_books:
        md_lines.append(
            f"| `{b.book_id}` | {b.name} | **{format_bytes(b.text_size_bytes)}** | "
            f"{'✅ هو' if b.has_pdf else '❌ نه'} | {format_bytes(b.pdf_size_bytes)} |"
        )

    md_lines.extend([
        "",
        "## 📕 تر ټولو لوی ۱۵ کتابونه (د PDF نسخې له مخې)",
        "| ID | د کتاب نوم | د PDF جلدونه | د PDF سایز | متني سایز |",
        "|---|---|---|---|---|",
    ])
    for b in top_pdf_books:
        md_lines.append(
            f"| `{b.book_id}` | {b.name} | `{b.pdf_files_count}` | "
            f"**{format_bytes(b.pdf_size_bytes)}** | {format_bytes(b.text_size_bytes)} |"
        )

    report_md_content = "\n".join(md_lines) + "\n"
    report_md_path = output_dir / "REPORT.md"
    report_md_path.write_text(report_md_content, encoding="utf-8")

    # 5. Package all outputs into a single ZIP file
    zip_path = output_dir / "turath_database_stats.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        zf.write(summary_json_path, arcname="summary.json")
        zf.write(details_json_path, arcname="books_details.json")
        zf.write(csv_path, arcname="books_details.csv")
        zf.write(report_md_path, arcname="REPORT.md")

    # 6. Print to Console and GitHub Actions Step Summary
    print("\n" + "=" * 72)
    print(report_md_content)
    print("=" * 72 + "\n")

    github_summary_file = os.environ.get("GITHUB_STEP_SUMMARY")
    if github_summary_file:
        with open(github_summary_file, "a", encoding="utf-8") as gh_f:
            gh_f.write(report_md_content)

    logger.info("All reports & ZIP archive saved to: %s", output_dir.resolve())
    return summary_data


async def async_main(args: argparse.Namespace) -> None:
    connector = aiohttp.TCPConnector(limit=args.concurrency * 2, ssl=False)
    async with aiohttp.ClientSession(connector=connector, headers=DEFAULT_HEADERS) as session:
        if args.local_db:
            books = load_local_database(Path(args.local_db))
            _, book_bases = await discover_endpoints(session)
        else:
            catalog_urls, book_bases = await discover_endpoints(session)
            books = await fetch_online_catalog(session, catalog_urls)

        if args.limit and args.limit > 0:
            logger.info("Applying test limit: analyzing first %d books.", args.limit)
            books = books[: args.limit]

        if not books:
            raise RuntimeError("No books found to analyze.")

        working_book_base = await detect_working_book_base(
            session, book_bases, books[0].book_id
        )

        # Analyze Text versions
        await analyze_all_text_versions(
            session, books, working_book_base, args.concurrency
        )

        # Analyze PDF versions
        if not args.skip_pdf_sizes:
            await analyze_all_pdf_versions(session, books, args.concurrency)

        # Save & Print Results
        generate_reports(books, Path(args.output_dir))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze Turath.io Islamic Library database, text sizes, and PDF sizes."
    )
    parser.add_argument(
        "--local-db",
        type=str,
        default="",
        help="Optional path to a local Turath SQLite (.db) or JSON file.",
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default="output",
        help="Directory where JSON, CSV, Markdown, and ZIP reports will be saved.",
    )
    parser.add_argument(
        "--concurrency",
        type=int,
        default=40,
        help="Number of concurrent async HTTP connections (default: 40).",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Limit number of books to scan (0 = scan ALL books).",
    )
    parser.add_argument(
        "--skip-pdf-sizes",
        action="store_true",
        help="Only count PDF links without querying remote PDF byte sizes.",
    )
    return parser.parse_args()


if __name__ == "__main__":
    cli_args = parse_args()
    try:
        asyncio.run(async_main(cli_args))
    except KeyboardInterrupt:
        logger.warning("Interrupted by user.")
        sys.exit(130)
