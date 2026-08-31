const fs = require('node:fs/promises');
const path = require('node:path');

class VirtualClock {
  #current = 0;

  now() { return this.#current; }

  advanceTo(value) {
    if (!Number.isSafeInteger(value) || value < this.#current) {
      throw new Error('Replay logical time must be a monotonic non-negative integer');
    }
    this.#current = value;
  }
}

class SeededRandom {
  #state;

  constructor(seed) { this.#state = seed >>> 0; }

  next() {
    let value = this.#state;
    value ^= value << 13;
    value ^= value >>> 17;
    value ^= value << 5;
    this.#state = value >>> 0;
    return this.#state;
  }
}

class ControlledExecutor {
  #clock;
  #random;
  #tasks = [];

  constructor(clock, random) {
    this.#clock = clock;
    this.#random = random;
  }

  schedule(at, action) {
    this.#tasks.push({ at, order: this.#random.next(), action });
  }

  async run() {
    this.#tasks.sort((left, right) => left.at - right.at || left.order - right.order);
    for (const task of this.#tasks) {
      this.#clock.advanceTo(task.at);
      await task.action();
    }
  }
}

function readReplay(text) {
  const records = text.split(/\r?\n/u).filter((line) => line.length > 0).map((line, index) => {
    try { return JSON.parse(line); } catch (error) {
      throw new Error(`Replay line ${index + 1} is not valid JSON: ${error.message}`);
    }
  });
  const manifest = records.shift();
  if (manifest?.kind !== 'manifest' || manifest.schemaVersion !== 1
      || !Number.isInteger(manifest.seed) || manifest.seed <= 0
      || typeof manifest.scenario !== 'string' || manifest.scenario.length === 0) {
    throw new Error('Replay manifest is invalid or unsupported');
  }
  const supported = new Set([
    'create-workspace', 'import-document', 'open-document', 'start-render',
    'cancel-job', 'expect-job-error', 'close-document', 'close-workspace',
  ]);
  for (const [index, step] of records.entries()) {
    if (step?.kind !== 'step' || !Number.isSafeInteger(step.at) || step.at < 0
        || !supported.has(step.operation)) {
      throw new Error(`Replay step ${index + 2} is invalid`);
    }
  }
  return { manifest, steps: records };
}

async function runReplay({ readerNode, replayPath, fixturePath, workspacePath, outputPath }) {
  const parsed = readReplay(await fs.readFile(replayPath, 'utf8'));
  const clock = new VirtualClock();
  const random = new SeededRandom(parsed.manifest.seed);
  const executor = new ControlledExecutor(clock, random);
  const jobs = new Map();
  const state = { document: null, cancelledJobCount: 0, workspaceClosed: false };
  const actual = [{
    kind: 'manifest',
    schemaVersion: 1,
    seed: parsed.manifest.seed,
    scenario: parsed.manifest.scenario,
  }];

  const record = (event) => actual.push({ logicalTime: clock.now(), ...event });
  for (const step of parsed.steps) {
    executor.schedule(step.at, async () => {
      record({ kind: 'request', operation: step.operation, jobId: step.jobId });
      switch (step.operation) {
        case 'create-workspace':
          await readerNode.createWorkspace(workspacePath);
          break;
        case 'import-document': {
          const imported = await readerNode.importDocument(fixturePath, step.jobId);
          state.document = imported.document;
          break;
        }
        case 'open-document':
          await readerNode.openDocument(state.document.documentId, step.jobId);
          break;
        case 'start-render': {
          if (!step.jobId) throw new Error('start-render requires jobId');
          const settled = readerNode.renderPage(0, 16, step.jobId).then(
            (value) => ({ ok: true, value }),
            (error) => ({ ok: false, error }),
          );
          jobs.set(step.jobId, settled);
          break;
        }
        case 'cancel-job': {
          if (!readerNode.cancelJob(step.jobId)) {
            throw new Error(`Replay could not cancel active Job ${step.jobId}`);
          }
          record({ kind: 'job-cancelled', jobId: step.jobId });
          break;
        }
        case 'expect-job-error': {
          const settled = await jobs.get(step.jobId);
          if (!settled || settled.ok || settled.error?.code !== step.errorCode) {
            throw new Error(`Replay Job ${step.jobId} did not fail with ${step.errorCode}`);
          }
          state.cancelledJobCount += 1;
          record({ kind: 'job-result', jobId: step.jobId, errorCode: settled.error.code });
          break;
        }
        case 'close-document':
          await readerNode.closeDocument();
          break;
        case 'close-workspace':
          await readerNode.closeWorkspace();
          state.workspaceClosed = true;
          break;
        default:
          throw new Error(`Unsupported replay operation ${step.operation}`);
      }
      record({ kind: 'result', operation: step.operation, status: 'ok' });
    });
  }
  await executor.run();
  const output = `${actual.map((record) => JSON.stringify(record)).join('\n')}\n`;
  if (outputPath) {
    await fs.mkdir(path.dirname(outputPath), { recursive: true });
    await fs.writeFile(outputPath, output, 'utf8');
  }
  return {
    scenario: parsed.manifest.scenario,
    eventCount: actual.length,
    cancelledJobCount: state.cancelledJobCount,
    workspaceClosed: state.workspaceClosed,
  };
}

module.exports = { ControlledExecutor, SeededRandom, VirtualClock, readReplay, runReplay };
