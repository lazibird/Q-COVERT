const fs = require("fs/promises");
const path = require("path");
const { execFile } = require("child_process");
const { promisify } = require("util");

const { paperSourceRoot } = require("./paperCorpusPath");
const execFileAsync = promisify(execFile);

async function readPaperPdf(filePath) {
  if (typeof filePath !== "string" || !path.isAbsolute(filePath))
    throw new Error("Use an absolute PDF path from a paper search result.");
  const root = await fs.realpath(paperSourceRoot);
  const resolved = await fs.realpath(filePath);
  if (!resolved.startsWith(root + path.sep) || !resolved.toLowerCase().endsWith(".pdf"))
    throw new Error("PDF path is outside the configured paper corpus.");
  const { stdout } = await execFileAsync("pdftotext", ["-layout", resolved, "-"], {
    timeout: 60000,
    maxBuffer: 16 * 1024 * 1024,
  });
  if (!stdout.trim()) throw new Error("The PDF has no extractable text.");
  return { path: resolved, title: path.basename(resolved), text: stdout };
}

module.exports = { readPaperPdf };
