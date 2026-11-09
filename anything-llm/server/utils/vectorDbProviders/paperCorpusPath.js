const path = require("path");

// Keep the optional PDF agent's source directory inside this application by
// default. Existing corpora can be selected explicitly with the environment.
const paperSourceRoot = path.resolve(
  process.env.QCOVERT_PDF_SOURCE_DIR ||
    path.resolve(__dirname, "../../../Q-COVERT-VectorDB/pdf-corpus-v1/source")
);

module.exports = { paperSourceRoot };
