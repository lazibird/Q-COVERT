const path = require("path");

const { paperSourceRoot } = require("./paperCorpusPath");

function paperPdfPath(sourcePdf) {
  if (typeof sourcePdf !== "string" || !sourcePdf.startsWith("source/"))
    return null;
  const absolute = path.resolve(path.dirname(paperSourceRoot), sourcePdf);
  if (!absolute.startsWith(paperSourceRoot + path.sep) || !absolute.toLowerCase().endsWith(".pdf"))
    return null;
  return absolute;
}

function paperContext(metadata = {}) {
  const text = metadata.text || "";
  if (metadata.docSource !== "SP2026 paper PDF") return text;
  const pdfPath = metadata.source_pdf_path || paperPdfPath(metadata.source_pdf);
  const pages = metadata.page_start === metadata.page_end
    ? String(metadata.page_start)
    : `${metadata.page_start}-${metadata.page_end}`;
  return [
    `Paper title: ${metadata.title || "Unknown"}`,
    ...(metadata.is_summary ? ["Content type: machine-generated summary; verify claims against the PDF."] : []),
    `PDF page: ${pages}`,
    ...(pdfPath ? [`Full PDF path: ${pdfPath}`] : []),
    "",
    text,
  ].join("\n");
}

module.exports = { paperContext, paperPdfPath };
