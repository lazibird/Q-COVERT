#!/usr/bin/env python3
"""Build one document-level embedding input per completed paper summary."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parent.parent / 'pdf-corpus-v1'
out = root / 'summary-v1'
rows = [json.loads(line) for line in (out / 'summaries.jsonl').read_text().splitlines()]
if len(rows) != 235 or len({x['paper_id'] for x in rows}) != 235:
    raise SystemExit('Expected 235 unique completed summaries')
chunks = []
for row in rows:
    s = row['summary']
    title = row['title']
    text = ' '.join([f"Research type: {s['work_type']}.", f"Contribution: {s['contribution']}",
                     f"Goal: {s['research_goal']}", f"Method: {s['method']}",
                     f"System: {s['studied_system']}", f"Findings: {s['key_findings']}"])
    text = text[:max(100, 970-len(title))].rstrip()
    page = s['evidence_pages'][0]
    relative = str(Path(row['source_pdf_path']).relative_to(root))
    metadata = {'text':text, 'title':title, 'url':f"https://doi.org/{row['doi']}" if row.get('doi') else None,
                'docSource':'SP2026 paper PDF', 'paper_id':row['paper_id'], 'page_start':page,
                'page_end':page, 'source_pdf':relative, 'is_summary':True}
    chunks.append({'id':'s-'+hashlib.sha256(row['paper_id'].encode()).hexdigest()[:32],
                   'paper_id':row['paper_id'], 'text':text, 'embedding_text':title+'\n'+text,
                   'metadata':metadata})
chunkfile = out / 'chunks.jsonl'
if chunkfile.exists():
    raise SystemExit('Summary index input already exists; refusing to overwrite')
chunkfile.write_text(''.join(json.dumps(x,ensure_ascii=False)+'\n' for x in chunks))
for name in ('documents.jsonl','pages.jsonl'):
    dest=out/name
    if not dest.exists(): dest.symlink_to(root/name)
digest=hashlib.sha256(chunkfile.read_bytes()).hexdigest()
manifest={'status':'text_complete','dataset_name':'SP2026-general-summaries',
          'processing_version':'general-summary-v1','pdf_count':235,'page_count':4536,
          'chunk_count':len(chunks),'rejected_count':0,'chunking':{'kind':'one summary per paper'},
          'files':{'chunks.jsonl':digest}}
(out/'text-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(f'Prepared {len(chunks)} summary records')
