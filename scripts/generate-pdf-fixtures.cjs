const fs = require('node:fs');
const path = require('node:path');

const corpusRoot = path.resolve(__dirname, '..', 'tests', 'corpus', 'generated');
fs.mkdirSync(corpusRoot, { recursive: true });

function stream(dictionary, content) {
  return `<< ${dictionary} /Length ${Buffer.byteLength(content, 'ascii')} >>\nstream\n${content}endstream`;
}

function buildPdf(label, objects) {
  let document = `%PDF-1.7\n% Context Reader generated fixture: ${label}\n`;
  const offsets = [0];
  for (let index = 0; index < objects.length; index += 1) {
    offsets.push(Buffer.byteLength(document, 'ascii'));
    document += `${index + 1} 0 obj\n${objects[index]}\nendobj\n`;
  }

  const xrefOffset = Buffer.byteLength(document, 'ascii');
  document += `xref\n0 ${objects.length + 1}\n`;
  document += '0000000000 65535 f \n';
  for (const offset of offsets.slice(1)) {
    document += `${String(offset).padStart(10, '0')} 00000 n \n`;
  }
  document += `trailer\n<< /Size ${objects.length + 1} /Root 1 0 R >>\n`;
  document += `startxref\n${xrefOffset}\n%%EOF\n`;
  return document;
}

const basicContent = 'BT /F1 18 Tf 72 680 Td (Context Reader P1) Tj ET\n';
const basicDocument = buildPdf('basic-rotated-cropbox', [
  '<< /Type /Catalog /Pages 2 0 R >>',
  '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
  '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /CropBox [36 72 576 720] /Rotate 90 /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
  '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
  stream('', basicContent),
]);
fs.writeFileSync(
  path.join(corpusRoot, 'basic-rotated-cropbox.pdf'),
  basicDocument,
  'ascii',
);

const cjkContent = 'BT /F1 36 Tf 50 150 Td <01020304> Tj ET\n';
const cjkCmap = `/CIDInit /ProcSet findresource begin
12 dict begin
begincmap
/CIDSystemInfo << /Registry (ContextReader) /Ordering (Generated) /Supplement 0 >> def
/CMapName /ContextReaderGenerated def
/CMapType 2 def
1 begincodespacerange
<00> <FF>
endcodespacerange
4 beginbfchar
<01> <8BED>
<02> <5883>
<03> <9605>
<04> <8BFB>
endbfchar
endcmap
CMapName currentdict /CMap defineresource pop
end
end
`;
const glyphs = [
  '1000 0 0 0 1000 1000 d1 0 g 100 100 800 800 re f\n',
  '1000 0 0 0 1000 1000 d1 0 g 100 100 300 800 re f 600 100 300 800 re f\n',
  '1000 0 0 0 1000 1000 d1 0 g 100 100 800 300 re f 100 600 800 300 re f\n',
  '1000 0 0 0 1000 1000 d1 0 g 100 100 m 900 900 l 700 900 l 100 300 l h f\n',
];
const cjkDocument = buildPdf('cjk-to-unicode', [
  '<< /Type /Catalog /Pages 2 0 R >>',
  '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
  '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
  '<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0] /CharProcs << /g1 6 0 R /g2 7 0 R /g3 8 0 R /g4 9 0 R >> /Encoding << /Type /Encoding /Differences [1 /g1 /g2 /g3 /g4] >> /FirstChar 1 /LastChar 4 /Widths [1000 1000 1000 1000] /ToUnicode 10 0 R /Resources << >> >>',
  stream('', cjkContent),
  ...glyphs.map((glyph) => stream('', glyph)),
  stream('', cjkCmap),
]);
fs.writeFileSync(path.join(corpusRoot, 'cjk-to-unicode.pdf'), cjkDocument, 'ascii');

const columnsContent = `BT /F1 14 Tf
50 740 Td (Left column line 1) Tj
0 -24 Td (Left column line 2) Tj
280 24 Td (Right column line 1) Tj
0 -24 Td (Right column line 2) Tj
ET
`;
const columnsDocument = buildPdf('double-column', [
  '<< /Type /Catalog /Pages 2 0 R >>',
  '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
  '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
  '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
  stream('', columnsContent),
]);
fs.writeFileSync(path.join(corpusRoot, 'double-column.pdf'), columnsDocument, 'ascii');

const imageSize = 32;
let imageHex = '';
for (let y = 0; y < imageSize; y += 1) {
  for (let x = 0; x < imageSize; x += 1) {
    const dark = x === y || x + y === imageSize - 1 || x % 8 === 0 || y % 8 === 0;
    imageHex += dark ? '202020' : 'f4f4f4';
  }
}
imageHex += '>\n';
const scanContent = 'q 300 0 0 300 50 100 cm /Im1 Do Q\n';
const scanDocument = buildPdf('image-only-scan', [
  '<< /Type /Catalog /Pages 2 0 R >>',
  '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
  '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 500] /Resources << /XObject << /Im1 5 0 R >> >> /Contents 4 0 R >>',
  stream('', scanContent),
  stream(`/Type /XObject /Subtype /Image /Width ${imageSize} /Height ${imageSize} /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /ASCIIHexDecode`, imageHex),
]);
fs.writeFileSync(path.join(corpusRoot, 'image-only-scan.pdf'), scanDocument, 'ascii');

fs.writeFileSync(
  path.join(corpusRoot, 'corrupt-truncated.pdf'),
  'not a PDF document\n',
  'ascii',
);
