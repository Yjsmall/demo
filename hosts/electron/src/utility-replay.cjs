const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

const { runReplay } = require('../../../tools/replay/engine.cjs');

function send(message) {
  process.parentPort.postMessage(message);
}

async function run() {
  const addonPath = path.resolve(process.argv[2]);
  const replayPath = path.resolve(process.argv[3]);
  const fixturePath = path.resolve(process.argv[4]);
  const outputPath = process.argv[5] ? path.resolve(process.argv[5]) : null;
  const readerNode = require(addonPath);
  const temporaryRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'context-reader-replay-'));
  let result = null;
  try {
    const summary = await runReplay({
      readerNode,
      replayPath,
      fixturePath,
      workspacePath: path.join(temporaryRoot, 'workspace'),
      outputPath,
    });
    result = { status: 'ok', ...summary };
  } finally {
    try { await readerNode.closeDocument(); } catch {}
    try { await readerNode.closeWorkspace(); } catch {}
    await fs.rm(temporaryRoot, { recursive: true, force: true, maxRetries: 20, retryDelay: 100 });
  }
  send(result);
}

run().catch((error) => {
  send({ status: 'error', message: error instanceof Error ? error.stack : String(error) });
});
