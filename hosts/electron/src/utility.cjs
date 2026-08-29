const path = require('node:path');

function send(message) {
  process.parentPort.postMessage(message);
}

try {
  const addonPath = path.resolve(process.argv[2]);
  const readerNode = require(addonPath);
  const runtimeInfo = readerNode.runtimeInfo();

  send({
    status: 'ok',
    processType: process.type,
    processVersions: {
      electron: process.versions.electron,
      node: process.versions.node,
    },
    runtimeInfo,
  });
} catch (error) {
  send({
    status: 'error',
    processType: process.type,
    message: error instanceof Error ? error.stack : String(error),
  });
}
