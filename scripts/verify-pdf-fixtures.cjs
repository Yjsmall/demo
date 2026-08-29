const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const corpusRoot = path.resolve(__dirname, '..', 'tests', 'corpus');
const manifestPath = path.join(corpusRoot, 'manifest.json');
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const failures = [];

function fail(message) {
  failures.push(message);
}

function collectPdfFiles(directory) {
  if (!fs.existsSync(directory)) {
    return [];
  }
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const entryPath = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      return collectPdfFiles(entryPath);
    }
    return entry.isFile() && path.extname(entry.name).toLowerCase() === '.pdf'
      ? [entryPath]
      : [];
  });
}

if (manifest.schema_version !== 1) {
  fail('manifest schema_version must be 1');
}
if (!Array.isArray(manifest.fixtures)) {
  fail('manifest fixtures must be an array');
}

const fixtureIds = new Set();
const listedPaths = new Set();
for (const fixture of manifest.fixtures ?? []) {
  if (typeof fixture.fixture_id !== 'string' || fixture.fixture_id.length === 0) {
    fail('fixture_id must be a non-empty string');
  } else if (fixtureIds.has(fixture.fixture_id)) {
    fail(`duplicate fixture_id: ${fixture.fixture_id}`);
  } else {
    fixtureIds.add(fixture.fixture_id);
  }

  const requiredStrings = [
    'path',
    'document_sha256',
    'origin',
    'license',
    'expected_outcome',
    'added_by_change',
  ];
  for (const field of requiredStrings) {
    if (typeof fixture[field] !== 'string' || fixture[field].length === 0) {
      fail(`${fixture.fixture_id ?? '<unknown>'}: ${field} must be a non-empty string`);
    }
  }
  if (fixture.redistributable !== true && fixture.redistributable !== false) {
    fail(`${fixture.fixture_id ?? '<unknown>'}: redistributable must be boolean`);
  }
  if (!Array.isArray(fixture.features) || fixture.features.length === 0
      || fixture.features.some((feature) => typeof feature !== 'string' || feature.length === 0)) {
    fail(`${fixture.fixture_id ?? '<unknown>'}: features must contain non-empty strings`);
  }
  if (!/^[0-9a-f]{64}$/.test(fixture.document_sha256 ?? '')) {
    fail(`${fixture.fixture_id ?? '<unknown>'}: document_sha256 must be lowercase SHA-256`);
  }

  if (typeof fixture.path !== 'string') {
    continue;
  }
  const absolutePath = path.resolve(corpusRoot, fixture.path);
  const relativePath = path.relative(corpusRoot, absolutePath).split(path.sep).join('/');
  if (relativePath.startsWith('../') || path.isAbsolute(relativePath)) {
    fail(`${fixture.fixture_id ?? '<unknown>'}: path escapes corpus root`);
    continue;
  }
  if (relativePath !== fixture.path) {
    fail(`${fixture.fixture_id ?? '<unknown>'}: path must be normalized with forward slashes`);
  }
  if (listedPaths.has(relativePath)) {
    fail(`duplicate fixture path: ${relativePath}`);
  }
  listedPaths.add(relativePath);

  if (!fs.existsSync(absolutePath) || !fs.statSync(absolutePath).isFile()) {
    fail(`${fixture.fixture_id ?? '<unknown>'}: fixture file is missing`);
    continue;
  }
  const actualHash = crypto.createHash('sha256').update(fs.readFileSync(absolutePath)).digest('hex');
  if (actualHash !== fixture.document_sha256) {
    fail(`${fixture.fixture_id ?? '<unknown>'}: SHA-256 mismatch`);
  }
}

for (const directoryName of ['generated', 'public']) {
  for (const pdfPath of collectPdfFiles(path.join(corpusRoot, directoryName))) {
    const relativePath = path.relative(corpusRoot, pdfPath).split(path.sep).join('/');
    if (!listedPaths.has(relativePath)) {
      fail(`unlisted PDF fixture: ${relativePath}`);
    }
  }
}

if (failures.length > 0) {
  for (const failure of failures) {
    console.error(`FAILED: ${failure}`);
  }
  process.exit(1);
}

console.log(`Verified ${listedPaths.size} PDF fixtures against manifest schema 1.`);
