const path = require('node:path');

const { app, utilityProcess } = require('electron');

const projectRoot = path.resolve(__dirname, '..', '..', '..');
const addonPath = path.join(projectRoot, 'build', 'node-ucrt64', 'reader_node.node');
const utilityPath = path.join(__dirname, 'utility.cjs');
const timeoutMs = 15_000;
const expectedElectronVersion = '44.1.0';
const expectedBindingNapiVersion = 8;

let child = null;
let completed = false;

function finish(exitCode, message) {
  if (completed) {
    return;
  }

  completed = true;

  if (message) {
    const output = exitCode === 0 ? process.stdout : process.stderr;
    output.write(`${message}\n`);
  }

  if (child) {
    child.kill();
  }

  app.exit(exitCode);
}

app.whenReady().then(() => {
  child = utilityProcess.fork(utilityPath, [addonPath], {
    serviceName: 'Context Reader Native Smoke',
    stdio: 'pipe',
  });

  child.stdout?.pipe(process.stdout);
  child.stderr?.pipe(process.stderr);

  const timeout = setTimeout(() => {
    finish(1, `Electron utility process timed out after ${timeoutMs} ms`);
  }, timeoutMs);

  child.on('message', (message) => {
    clearTimeout(timeout);

    if (
      message?.status === 'ok'
      && message.processType === 'utility'
      && message.processVersions?.electron === expectedElectronVersion
      && typeof message.processVersions?.node === 'string'
      && message.runtimeInfo?.version === '0.1.0'
      && message.runtimeInfo?.applicationApiVersion === 6
      && message.runtimeInfo?.bindingNapiVersion === expectedBindingNapiVersion
    ) {
      finish(
        0,
        `Electron ${message.processVersions.electron} / Node ${message.processVersions.node} utility process loaded reader_node ${message.runtimeInfo.version} with N-API ${message.runtimeInfo.bindingNapiVersion}`,
      );
      return;
    }

    finish(1, `Unexpected utility response: ${JSON.stringify(message)}`);
  });

  child.on('exit', (code) => {
    clearTimeout(timeout);
    if (!completed) {
      finish(1, `Electron utility process exited before validation with code ${code}`);
    }
  });
});

app.on('window-all-closed', () => {});
